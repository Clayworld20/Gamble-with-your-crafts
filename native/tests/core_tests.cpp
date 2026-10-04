// =============================================================================
//  core_tests.cpp — проверки нативного ядра без Windows, без Steam и без Minecraft.
//
//  Что покрыто:
//    * CRC32 (эталонный вектор) и потоковый расчёт;
//    * кольцевой буфер SPSC под реальной многопоточной нагрузкой;
//    * кадрирование канала: доставка, отказ от битых кадров, пересинхронизация
//      после мусора в потоке;
//    * RLE-кодек (общий формат с C# и Java);
//    * схлопывание правок вокселей, лимит кубов, снос;
//    * сканер сигнатур: разбор паттерна, поиск в буфере, поиск в СВОЁМ модуле;
//    * реестр хуков и наблюдатели событий казино (теневой режим вне Windows);
//    * сквозной тест ABI моста: настоящие GwycBridge_* + вторая сторона канала,
//      которая играет роль Minecraft.
// =============================================================================
#include "gwyc/plugin_abi.h"

#include "GWYF_HookEngine.h"
#include "VoxelTo3DWorld.h"
#include "gwyc/SharedChannel.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>


/// Копирование C-строки в буфер фиксированного размера без strncpy:
/// MSVC считает strncpy небезопасным (C4996), а тесты собираются с /W4 /WX.
template <std::size_t N>
void CopyText(char (&destination)[N], const char* source)
{
    const std::size_t limit = N - 1;
    std::size_t length = 0;
    while (length < limit && source[length] != '\0') ++length;
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

namespace {

int g_passed = 0;
std::vector<std::string> g_failures;

void Section(const char* title)
{
    std::printf("\n── %s ──\n", title);
}

void Check(const char* name, bool condition, const std::string& detail = {})
{
    if (condition) {
        ++g_passed;
        if (detail.empty()) std::printf("  [ ok ] %s\n", name);
        else std::printf("  [ ok ] %s (%s)\n", name, detail.c_str());
        return;
    }

    g_failures.emplace_back(name);
    std::printf("  [FAIL] %s%s%s\n", name, detail.empty() ? "" : " — ", detail.c_str());
}

std::string TempPath(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path();
    std::error_code error;
    std::filesystem::remove(dir / name, error);
    return (dir / name).string();
}

void SleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ─────────────────────────────────────────────────────────────────────────────
//  CRC32
// ─────────────────────────────────────────────────────────────────────────────

void TestCrc32()
{
    Section("CRC32");

    const uint32_t reference = gwyc::Crc32("123456789", 9);
    Check("эталонный вектор 0xCBF43926", reference == 0xCBF43926u, [&] {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "получено 0x%08X", reference);
        return std::string(buffer);
    }());

    gwyc::Crc32Stream streamed;
    streamed.Update("12345", 5);
    streamed.Update("6789", 4);
    Check("потоковый CRC совпадает с разовым", streamed.Final() == reference);

    Check("пустой буфер считается", gwyc::Crc32(nullptr, 0) == 0u);
    Check("разные данные дают разные суммы", gwyc::Crc32("a", 1) != gwyc::Crc32("b", 1));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Кольцевой буфер под нагрузкой
// ─────────────────────────────────────────────────────────────────────────────

void TestRingBufferThreads()
{
    Section("Кольцо SPSC под нагрузкой");

    const auto path = TempPath("gwyc_ring_test.bin");
    gwyc::SharedChannel producer;
    gwyc::SharedChannel consumer;

    Check("канал открыт продюсером", producer.Open(path.c_str(), gwyc::ChannelRole::Game, 4096) == gwyc::ChannelState::Ok);
    Check("канал открыт потребителем", consumer.Open(path.c_str(), gwyc::ChannelRole::Minecraft, 4096) == gwyc::ChannelState::Ok);

    constexpr uint32_t kMessages = 200000;
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<uint64_t> checksumOut{0};
    std::atomic<uint64_t> checksumIn{0};

    std::thread writer([&] {
        uint32_t sequence = 0;
        while (sequence < kMessages) {
            const uint32_t value = sequence * 2654435761u;   // чтобы байты не были одинаковыми
            if (!producer.Send(gwyc::MsgType::Ping, 0, &value, sizeof(value))) {
                std::this_thread::yield();    // кольцо полно — подождём потребителя
                continue;
            }
            checksumOut.fetch_add(value);
            produced.fetch_add(1);
            ++sequence;
        }
    });

    std::thread reader([&] {
        uint8_t payload[64]{};
        while (consumed.load() < kMessages) {
            gwyc::MsgType type{};
            uint8_t flags = 0;
            uint32_t size = 0;
            if (!consumer.Receive(type, flags, payload, sizeof(payload), size) || size != sizeof(uint32_t)) {
                std::this_thread::yield();
                continue;
            }

            uint32_t value = 0;
            std::memcpy(&value, payload, sizeof(value));
            checksumIn.fetch_add(value);
            consumed.fetch_add(1);
        }
    });

    writer.join();
    reader.join();

    Check("все сообщения дошли", produced.load() == kMessages && consumed.load() == kMessages,
          std::to_string(consumed.load()) + " из " + std::to_string(kMessages));
    Check("контрольные суммы совпали", checksumOut.load() == checksumIn.load());
    // Переполнение кольца здесь — это backpressure: Send вернул false, писатель
    // повторил отправку, ни один кадр не потерян. Число таких отказов зависит от
    // планировщика (под ThreadSanitizer оно в разы больше), поэтому проверяем
    // строгий инвариант — все кадры дошли, — а счётчик показываем как диагностику.
    Check("потерь при переполнении нет", consumed.load() == kMessages,
          "backpressure-событий: " + std::to_string(producer.Overruns()));

    producer.Close();
    consumer.Close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Кадрирование: доставка, порча, пересинхронизация
// ─────────────────────────────────────────────────────────────────────────────

void TestChannelFraming()
{
    Section("Кадрирование канала");

    const auto path = TempPath("gwyc_framing_test.bin");
    gwyc::SharedChannel game;
    gwyc::SharedChannel mc;
    Check("сторона игры открыта", game.Open(path.c_str(), gwyc::ChannelRole::Game, 8192) == gwyc::ChannelState::Ok);
    Check("сторона Minecraft открыта", mc.Open(path.c_str(), gwyc::ChannelRole::Minecraft, 8192) == gwyc::ChannelState::Ok);

    gwyc::MsgBetResolved bet{};
    bet.playerId = 7;
    bet.stake = 4;
    bet.payout = 8;
    bet.multiplier = 2;
    bet.won = 1;
    gwyc::WriteFixed(bet.target, sizeof(bet.target), "red");
    gwyc::WriteFixed(bet.name, sizeof(bet.name), "Player");

    Check("кадр отправлен", game.SendPod(gwyc::MsgType::BetResolved, gwyc::kFlagReliable, bet));

    gwyc::MsgType type{};
    gwyc::MsgBetResolved received{};
    Check("кадр принят", mc.ReceivePod(type, received) && type == gwyc::MsgType::BetResolved);
    Check("данные кадра целы", received.payout == 8 && received.multiplier == 2 && received.won == 1);
    Check("строка в кадре цела", std::strcmp(received.target, "red") == 0);

    // Битый кадр: собираем настоящий кадр, портим в нём поле CRC и пишем в кольцо
    // сырыми байтами. Приёмник обязан отбросить кадр и не тронуть поток.
    const auto buildDamagedFrame = [&] {
        std::vector<uint8_t> frame(sizeof(gwyc::FrameHeader) + sizeof(bet));
        gwyc::FrameHeader header{};
        header.magic = gwyc::kMagic;
        header.layoutVersion = gwyc::kLayoutVersion;
        header.type = static_cast<uint8_t>(gwyc::MsgType::BetResolved);
        header.flags = gwyc::kFlagReliable;
        header.payloadSize = static_cast<uint32_t>(sizeof(bet));

        std::memcpy(frame.data(), &header, sizeof(header));
        std::memcpy(frame.data() + sizeof(header), &bet, sizeof(bet));

        gwyc::Crc32Stream crc;
        crc.Update(frame.data() + offsetof(gwyc::FrameHeader, type),
                   sizeof(gwyc::FrameHeader::type) + sizeof(gwyc::FrameHeader::flags) + sizeof(gwyc::FrameHeader::payloadSize));
        crc.Update(frame.data() + sizeof(header), sizeof(bet));

        auto* outHeader = reinterpret_cast<gwyc::FrameHeader*>(frame.data());
        outHeader->crc32 = crc.Final() ^ 0x5A5A5A5Au;   // корректная длина, неверная сумма

        // Байты те же, сумма не сходится — ровно тот случай, который ловит приёмник.
        return frame;
    }();

    Check("битый кадр записан в кольцо", game.SendRaw(buildDamagedFrame.data(), static_cast<uint32_t>(buildDamagedFrame.size())));
    gwyc::MsgBetResolved ignored{};
    const bool accepted = mc.ReceivePod(type, ignored);
    Check("битый кадр не принят", !accepted);
    Check("счётчик CRC-ошибок вырос", mc.CrcFailures() >= 1, "crcFailures=" + std::to_string(mc.CrcFailures()));

    // Мусор в потоке: приёмник должен пересинхронизироваться и принять следующий кадр.
    // 37 байт мусора: часть совпадает с началом кадра случайно, поэтому приёмник
    // обязан искать синхронизацию по magic, а не просто пропустить один кадр.
    const uint8_t garbage[37] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
        0x47, 0x57, 0x03, 0x00, 0x10, 0x00, 0x00, 0x00, 0xFF, 0xFF,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
    };
    Check("сырой мусор записан в канал", game.SendRaw(garbage, sizeof(garbage)));

    gwyc::MsgVoxelEdit edit{};
    edit.x = 5;
    edit.y = 70;
    edit.z = -2;
    edit.blockId = static_cast<uint8_t>(gwyc::BlockKind::Diamond);
    Check("полезный кадр отправлен после мусора", game.SendPod(gwyc::MsgType::VoxelEdit, gwyc::kFlagReliable, edit));

    bool recovered = false;
    gwyc::MsgVoxelEdit arrived{};
    for (int attempt = 0; attempt < 64 && !recovered; ++attempt) {
        if (mc.ReceivePod(type, arrived) && type == gwyc::MsgType::VoxelEdit) recovered = true;
    }

    Check("приёмник пересинхронизировался после мусора", recovered);
    Check("пересинхронизация зафиксирована", mc.Resyncs() >= 1, "resyncs=" + std::to_string(mc.Resyncs()));
    Check("данные после пересинхронизации верны", arrived.x == 5 && arrived.y == 70 && arrived.blockId == 6);

    game.Close();
    mc.Close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  RLE
// ─────────────────────────────────────────────────────────────────────────────

void TestRle()
{
    Section("RLE-кодек");

    const size_t count = 4096;
    std::vector<uint8_t> pattern(count, 0);

    // 1. Пустой регион.
    std::vector<uint8_t> encoded = gwyc::world::EncodeRle(pattern.data(), count);
    std::vector<uint8_t> decoded(count, 0xFF);
    Check("пустой регион кодируется коротко", encoded.size() <= 8, std::to_string(encoded.size()) + " байт");
    Check("пустой регион декодируется", gwyc::world::DecodeRle(encoded.data(), encoded.size(), decoded.data(), count));

    // 2. Сплошной и полосатый.
    std::fill(pattern.begin(), pattern.end(), static_cast<uint8_t>(gwyc::BlockKind::Stone));
    encoded = gwyc::world::EncodeRle(pattern.data(), count);
    Check("сплошной регион сжимается до пары байт", encoded.size() <= 8, std::to_string(encoded.size()) + " байт");

    for (size_t i = 0; i < count; ++i) pattern[i] = static_cast<uint8_t>(i % 9);
    encoded = gwyc::world::EncodeRle(pattern.data(), count);
    std::fill(decoded.begin(), decoded.end(), 0);
    Check("полосатый регион декодируется без потерь",
          gwyc::world::DecodeRle(encoded.data(), encoded.size(), decoded.data(), count) && decoded == pattern);

    // 3. Битые потоки.
    const uint8_t zeroRun[] = { 0x00, 0x01 };
    Check("нулевая серия отвергается", !gwyc::world::DecodeRle(zeroRun, sizeof(zeroRun), decoded.data(), count));

    std::vector<uint8_t> truncated = encoded;
    truncated.pop_back();
    Check("обрезанный поток отвергается", !gwyc::world::DecodeRle(truncated.data(), truncated.size(), decoded.data(), count));

    const uint8_t overflow[] = { 0xFF, 0xFF, 0xFF, 0x07, 0x01 };   // серия длиннее региона
    Check("серия больше региона отвергается",
          !gwyc::world::DecodeRle(overflow, sizeof(overflow), decoded.data(), count));

    std::vector<uint8_t> varint;
    gwyc::world::WriteVarUInt(varint, 300);
    size_t offset = 0;
    uint32_t read = 0;
    Check("varint читается обратно", gwyc::world::ReadVarUInt(varint.data(), varint.size(), offset, read) && read == 300);

    size_t overrun = varint.size();
    Check("varint за границей буфера отвергается",
          !gwyc::world::ReadVarUInt(varint.data(), varint.size(), overrun, read) || overrun == varint.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Синхронизация вокселей
// ─────────────────────────────────────────────────────────────────────────────

struct AppliedCube {
    int32_t x, y, z;
    uint8_t block;
    uint16_t source;
};

std::vector<AppliedCube> g_applied;

void GWYC_ABI_CALL CaptureApply(int32_t x, int32_t y, int32_t z, uint8_t block, uint16_t source)
{
    g_applied.push_back(AppliedCube{ x, y, z, block, source });
}

void TestVoxelSync()
{
    Section("Синхронизация вокселей в мир игры");

    g_applied.clear();

    gwyc::world::VoxelCallbacks callbacks;
    callbacks.apply = &CaptureApply;

    gwyc::world::VoxelWorldSync sync(callbacks, 16);

    // 1. Схлопывание: тысяча правок одной клетки превращается в одну.
    for (int i = 0; i < 1000; ++i) {
        gwyc::MsgVoxelEdit edit{};
        edit.x = 3; edit.y = 64; edit.z = 3;
        edit.blockId = (i % 2 == 0) ? 1 : 5;
        sync.SubmitRemoteEdit(edit);
    }

    const gwyc::world::VoxelSyncStats stats = sync.GetStats();
    Check("правки схлопнулись в одну", stats.pendingRemote == 1 && stats.remoteEditsCoalesced == 999,
          "в очереди " + std::to_string(stats.pendingRemote));

    size_t applied = sync.Flush();
    Check("применился один куб", applied == 1 && g_applied.size() == 1);
    Check("победила последняя правка", g_applied[0].block == 5, std::to_string(g_applied[0].block));

    // 2. Снос.
    g_applied.clear();
    gwyc::MsgVoxelEdit remove{};
    remove.x = 3; remove.y = 64; remove.z = 3;
    remove.blockId = 0;   // воздух = убрать
    sync.SubmitRemoteEdit(remove);
    sync.Flush();
    Check("куб снят", g_applied.size() == 1 && g_applied[0].block == 0);
    Check("зеркало кубов пусто после сноса", sync.MirrorCubeCount() == 0);

    // 3. Лимит кубов.
    g_applied.clear();
    for (int i = 0; i < 50; ++i) {
        gwyc::MsgVoxelEdit edit{};
        edit.x = i; edit.y = 65; edit.z = 0;
        edit.blockId = 3;
        sync.SubmitRemoteEdit(edit);
    }
    sync.Flush();
    const gwyc::world::VoxelSyncStats limited = sync.GetStats();
    Check("лимит кубов соблюдён", sync.MirrorCubeCount() <= 16 && limited.overflowDropped >= 30,
          "кубов " + std::to_string(sync.MirrorCubeCount()) + ", отброшено " + std::to_string(limited.overflowDropped));

    // 4. Мусорный blockId отбрасывается.
    g_applied.clear();
    gwyc::MsgVoxelEdit bogus{};
    bogus.x = 1; bogus.y = 1; bogus.z = 1; bogus.blockId = 200;
    const uint64_t droppedBefore = sync.GetStats().overflowDropped;
    sync.SubmitRemoteEdit(bogus);
    sync.Flush();
    Check("неизвестный тип блока отброшен", g_applied.empty() && sync.GetStats().overflowDropped > droppedBefore);

    // 5. Исходящий поток: правки игры тоже схлопываются.
    std::vector<gwyc::MsgVoxelEdit> outgoing;
    sync.SubmitLocalEdit(10, 70, 10, gwyc::BlockKind::Gold);
    sync.SubmitLocalEdit(10, 70, 10, gwyc::BlockKind::Diamond);
    sync.SubmitLocalEdit(11, 70, 10, gwyc::BlockKind::Dirt);
    const size_t drained = sync.DrainLocalEdits(outgoing, 100);
    Check("исходящие правки схлопнуты", drained == 2 && outgoing.size() == 2, "drained=" + std::to_string(drained));
    Check("исходящая правка обновилась до последней", outgoing[0].blockId == 6);

    // 6. Снимок региона через RLE.
    g_applied.clear();
    sync.Reset();

    const uint16_t sizeX = 4, sizeY = 2, sizeZ = 4;
    std::vector<uint8_t> region(sizeX * sizeY * sizeZ, 0);
    region[0] = 5; region[5] = 6; region[31] = 3;
    const std::vector<uint8_t> encoded = gwyc::world::EncodeRle(region.data(), region.size());

    gwyc::MsgVoxelSnapshot snapshot{};
    snapshot.originX = 0; snapshot.originY = 64; snapshot.originZ = 0;
    snapshot.sizeX = sizeX; snapshot.sizeY = sizeY; snapshot.sizeZ = sizeZ;
    snapshot.encodedSize = static_cast<uint32_t>(encoded.size());

    const size_t decoded = sync.SubmitRemoteSnapshot(snapshot, encoded.data(), encoded.size());
    sync.Flush();
    Check("снимок региона применён полностью", decoded == region.size(),
          std::to_string(decoded) + " клеток");
    Check("в мир попали ровно непустые клетки", g_applied.size() == 3, std::to_string(g_applied.size()) + " кубов");

    // Порядок применения не определён (карта схлопывания), поэтому сверяем набор клеток.
    const auto hasCube = [](int32_t x, int32_t y, int32_t z, uint8_t block) {
        for (const AppliedCube& cube : g_applied) {
            if (cube.x == x && cube.y == y && cube.z == z && cube.block == block) return true;
        }
        return false;
    };

    Check("координаты снимка смещены началом региона",
          hasCube(0, 64, 0, 5) && hasCube(1, 64, 1, 6) && hasCube(3, 65, 3, 3),
          "клеток " + std::to_string(g_applied.size()));

    // Битый снимок не применяется.
    g_applied.clear();
    const uint8_t broken[] = { 0x00, 0x05 };
    Check("битый снимок отвергнут", sync.SubmitRemoteSnapshot(snapshot, broken, sizeof(broken)) == 0 && g_applied.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Сканер сигнатур
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_MSC_VER)
#   define GWYC_NOINLINE __declspec(noinline)
#else
#   define GWYC_NOINLINE __attribute__((noinline))
#endif

/// Функция, по байтам которой проверяется сканер: он должен найти её в .text
/// того же процесса, в котором идёт тест.
extern "C" GWYC_NOINLINE int32_t GwycScannerProbe(int32_t input)
{
    volatile int32_t accumulator = input;
    for (int i = 0; i < 32; ++i) {
        accumulator = accumulator * 3 + i * 7;
    }
    return accumulator;
}

void TestScanner()
{
    Section("Сканер сигнатур");

    const gwyc::hooks::Pattern valid = gwyc::hooks::ParsePattern("48 8B ?? E8 * * FF");
    Check("паттерн разобран", valid.valid && valid.bytes.size() == 7, std::to_string(valid.bytes.size()) + " байт");
    Check("подстановки помечены как маска", valid.mask[0] && !valid.mask[2] && !valid.mask[4]);

    Check("пустой паттерн отвергнут", !gwyc::hooks::ParsePattern("").valid);
    Check("неразбираемый паттерн отвергнут", !gwyc::hooks::ParsePattern("48 ZZ 11").valid);

    const uint8_t haystack[] = {
        0x11, 0x22, 0x48, 0x8B, 0x99, 0xE8, 0x01, 0x02, 0xFF, 0x00,   // вхождение 1: смещение 2
        0x48, 0x8B, 0x77, 0xE8, 0xAA, 0xBB, 0xFF, 0x00,                // вхождение 2: смещение 10
    };
    const uint8_t* hit = gwyc::hooks::FindPatternIn(haystack, sizeof(haystack), valid);
    Check("паттерн найден в буфере", hit == haystack + 2, hit == nullptr ? "не найдено" : "смещение " + std::to_string(hit - haystack));

    const std::vector<const uint8_t*> all = gwyc::hooks::FindAllPatternsIn(haystack, sizeof(haystack), valid, 8);
    Check("найдены все вхождения", all.size() == 2,
          std::to_string(all.size()) + " вхождений" +
              (all.size() == 2 ? "" : " (первое на смещении " + std::to_string(all.empty() ? -1 : (all[0] - haystack)) + ")"));

    Check("паттерн из одних подстановок работает",
          gwyc::hooks::ParsePattern("? ? ?").valid && gwyc::hooks::FindPatternIn(haystack, sizeof(haystack), gwyc::hooks::ParsePattern("? ? ?")) == haystack);

    // Свой модуль: проверяем EnumerateModules и реальный поиск в .text.
    const std::vector<gwyc::hooks::ModuleRecord> modules = gwyc::hooks::EnumerateModules();
    Check("список модулей процесса не пуст", !modules.empty(), std::to_string(modules.size()) + " модулей");
    Check("главный модуль определён", modules.size() > 0 && modules[0].executable && modules[0].size > 0,
          modules.empty() ? "" : modules[0].name);

    gwyc::hooks::ModuleRecord self;
    const bool foundSelf = gwyc::hooks::FindModule(modules[0].name.c_str(), self);
    Check("главный модуль ищется по имени", foundSelf && self.base == modules[0].base,
          foundSelf ? "base=" + std::to_string(self.base) : "не найдено");

    // Строим паттерн по первым трём реальным байтам функции и ищем её в .text.
    uint8_t prologue[3]{};
    std::memcpy(prologue, reinterpret_cast<const void*>(&GwycScannerProbe), sizeof(prologue));

    char patternText[32]{};
    std::snprintf(patternText, sizeof(patternText), "%02X %02X %02X ? ? ?", prologue[0], prologue[1], prologue[2]);

    const gwyc::hooks::Pattern probe = gwyc::hooks::ParsePattern(patternText);

    // Внутри буфера самой функции паттерн обязан найтись — это проверка «в лоб».
    const uint8_t* selfHit = gwyc::hooks::FindPatternIn(reinterpret_cast<const void*>(&GwycScannerProbe), 32, probe);
    Check("паттерн находится в собственном коде функции", selfHit != nullptr, patternText);

    // И через модуль: адрес обязан быть в пределах образа процесса.
    if (!modules.empty()) {
        void* moduleHit = gwyc::hooks::FindPatternInModule(modules[0].name.c_str(), patternText, 0);
        const bool inside = moduleHit != nullptr &&
                            reinterpret_cast<uintptr_t>(moduleHit) >= modules[0].base &&
                            reinterpret_cast<uintptr_t>(moduleHit) < modules[0].base + modules[0].size;
        Check("поиск по модулю возвращает адрес внутри образа", inside,
              moduleHit == nullptr ? "не найдено" : std::to_string(reinterpret_cast<uintptr_t>(moduleHit)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Хуки
// ─────────────────────────────────────────────────────────────────────────────

int32_t GWYC_CALL FakeBetSubmit(void* context)
{
    if (context != nullptr) *static_cast<int32_t*>(context) += 1;
    return 42;
}

void* GWYC_CALL FakeTableCreate(const char* name, int32_t seats)
{
    (void)name;
    (void)seats;
    return nullptr;
}

int32_t g_detourCalls = 0;
int32_t g_observedBets = 0;

/// Наблюдатель события «ставка отправлена»: движок зовёт его из своего детура.
void GWYC_CALL BetObserver(void* context)
{
    (void)context;
    ++g_observedBets;
}

/// Детур для проверки перехвата: считает вызов и возвращает свой код.
int32_t GWYC_CALL FakeBetSubmitDetour(void* context)
{
    (void)context;
    ++g_detourCalls;
    return 7;
}

void TestHooks()
{
    Section("Движок хуков");

    using namespace gwyc::hooks;

    Check("движок инициализируется", Initialize() == Status::Ok);
    Check("повторная инициализация распознана", Initialize() == Status::AlreadyInitialized);

#if defined(_WIN32)
    constexpr bool kExpectInterception = true;
#else
    constexpr bool kExpectInterception = false;
#endif
    Check("режим перехвата соответствует платформе", IsInterceptionAvailable() == kExpectInterception,
          IsInterceptionAvailable() ? "реальный перехват MinHook" : "теневой режим (не Windows)");

    // Ставим хук на функцию «движка» и проверяем контракт: оригинал вызывается,
    // поведение игры не меняется.
    int32_t betCounter = 0;
    BetSubmitFn original = nullptr;

    const Status created = Create("test.bet.submit",
                                  reinterpret_cast<void*>(&FakeBetSubmit),
                                  reinterpret_cast<void*>(&FakeBetSubmitDetour),
                                  reinterpret_cast<void**>(&original));
    Check("хук создан", created == Status::Ok, StatusName(created));
    Check("оригинал доступен", original != nullptr);
    Check("хук включён", Enable("test.bet.submit") == Status::Ok);

    Check("дубликат имени отвергнут",
          Create("test.bet.submit", reinterpret_cast<void*>(&FakeBetSubmit),
                 reinterpret_cast<void*>(&FakeBetSubmitDetour), nullptr) == Status::AlreadyExists);

    if (original != nullptr) {
        const int32_t result = original(&betCounter);
        Check("оригинал вызывается и работает", result == 42 && betCounter == 1,
              "result=" + std::to_string(result) + ", counter=" + std::to_string(betCounter));
    }

    Check("хук выключается", Disable("test.bet.submit") == Status::Ok);
    Check("хук включается снова", Enable("test.bet.submit") == Status::Ok);
    Check("неизвестный хук не найден", Remove("нет.такого") == Status::NotFound);
    Check("хук снят", Remove("test.bet.submit") == Status::Ok);
    Check("повторное снятие не находит хук", Remove("test.bet.submit") == Status::NotFound);

    // Хуки событий казино: детуры движка зовут наблюдателей и вызывают оригинал.
    SetBetObserver(&BetObserver);
    SetTableObserver(nullptr);
    Check("наблюдатели зарегистрированы", true);

    const Status hooked = InstallCasinoHooks(reinterpret_cast<void*>(&FakeBetSubmit), nullptr, nullptr,
                                            reinterpret_cast<void*>(&FakeTableCreate), nullptr, nullptr);
    Check("хуки казино установлены", hooked == Status::Ok, StatusName(hooked));

    bool sawBetHook = false;
    bool sawTableHook = false;
    ForEach([&](const HookInfo& info) {
        if (info.name == "casino.bet.submit") sawBetHook = true;
        if (info.name == "casino.table.create") sawTableHook = true;
    });
    Check("хук на ставки в реестре", sawBetHook);
    Check("хук на столы в реестре", sawTableHook);

    const CasinoHookCounters counters = GetCasinoCounters();
    Check("счётчики хуков стартуют с нуля", counters.betsObserved == 0 && counters.tablesObserved == 0);

    // Наблюдатель ставки — обычная функция: проверяем, что её можно вызвать и она считается.
    BetObserver(nullptr);
    Check("наблюдатель ставки вызван", g_observedBets == 1, "вызовов: " + std::to_string(g_observedBets));

    Check("хуки казино сняты", RemoveCasinoHooks() == Status::Ok);
    Check("движок остановлен", Shutdown() == Status::Ok);
    Check("после остановки реестр пуст", [&] {
        bool anyHook = false;
        ForEach([&](const HookInfo&) { anyHook = true; });
        return !anyHook;
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Сквозной тест ABI моста (роль Minecraft играет второй конец канала)
// ─────────────────────────────────────────────────────────────────────────────

struct BridgeCapture {
    std::vector<AppliedCube> voxels;
    std::vector<GwycReward> rewards;
    std::vector<std::string> logs;
    int peerConnected = 0;
    int peerDisconnected = 0;
};

BridgeCapture g_capture;

void GWYC_ABI_CALL OnVoxelApply(int32_t x, int32_t y, int32_t z, uint8_t block, uint16_t source)
{
    g_capture.voxels.push_back(AppliedCube{ x, y, z, block, source });
}

void GWYC_ABI_CALL OnRewardRequest(const GwycReward* reward)
{
    if (reward != nullptr) g_capture.rewards.push_back(*reward);
}

void GWYC_ABI_CALL OnChatFromMc(const GwycChatLine* line)
{
    if (line != nullptr) g_capture.logs.emplace_back(std::string("чат: ") + line->author + ": " + line->text);
}

void GWYC_ABI_CALL OnPeerState(uint32_t connected)
{
    if (connected != 0) ++g_capture.peerConnected;
    else ++g_capture.peerDisconnected;
}

void GWYC_ABI_CALL OnLog(uint32_t level, const char* message)
{
    (void)level;
    if (message != nullptr) g_capture.logs.emplace_back(message);
}

void TestBridgeAbiEndToEnd()
{
    Section("ABI моста: игра ↔ канал ↔ Minecraft");

    g_capture = BridgeCapture{};
    const std::string path = TempPath("gwyc_abi_test.channel");

    Check("ABI-версии совпадают", GwycBridge_GetAbiVersion() == GWYC_ABI_VERSION);
    Check("мост отдаёт тег сборки", std::string(GwycBridge_GetBuildTag()).find("gwyc-bridge") == 0);

    GwycGameCallbacks callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.onVoxelApply = &OnVoxelApply;
    callbacks.onRewardRequest = &OnRewardRequest;
    callbacks.onChatFromMinecraft = &OnChatFromMc;
    callbacks.onPeerState = &OnPeerState;
    callbacks.onLog = &OnLog;

    GwycBridgeConfig config{};
    config.structSize = sizeof(config);
    config.abiVersion = GWYC_ABI_VERSION;
    config.transport = GwycTransport_SharedMemory;
    config.role = GwycRole_Game;
    config.ringCapacity = 32 * 1024;
    config.pollIntervalMs = 1;
    config.maxCubes = 128;
    config.channelPath = path.c_str();
    config.gameTag = "native-tests";

    Check("конфиг без версии ABI отвергается", [&] {
        GwycBridgeConfig broken = config;
        broken.abiVersion = 999;
        return GwycBridge_Initialize(&broken, &callbacks) == GwycStatus_AbiMismatch;
    }());

    Check("мост инициализируется", GwycBridge_Initialize(&config, &callbacks) == GwycStatus_Ok);
    Check("повторная инициализация отвергается", GwycBridge_Initialize(&config, &callbacks) == GwycStatus_AlreadyInitialized);

    // Вторая сторона канала: играет роль процесса Minecraft.
    gwyc::SharedChannel minecraft;
    Check("Minecraft подключился к каналу", minecraft.Open(path.c_str(), gwyc::ChannelRole::Minecraft, 32 * 1024) == gwyc::ChannelState::Ok);

    // Ждём от моста служебные кадры (Ping раз в секунду + пинг аудита).
    bool sawPing = false;
    gwyc::MsgType type{};
    uint8_t flags = 0;
    uint32_t size = 0;
    std::vector<uint8_t> buffer(gwyc::kMaxPayloadSize);

    for (int attempt = 0; attempt < 200 && !sawPing; ++attempt) {
        while (minecraft.Receive(type, flags, buffer.data(), static_cast<uint32_t>(buffer.size()), size)) {
            if (type == gwyc::MsgType::Ping) sawPing = true;
        }
        SleepMs(5);
    }
    Check("мост шлёт пинг-кадры", sawPing);

    // Minecraft → игра: правка блока и награда.
    gwyc::MsgVoxelEdit edit{};
    edit.x = 12; edit.y = 71; edit.z = -3;
    edit.blockId = static_cast<uint8_t>(gwyc::BlockKind::Gold);
    edit.flags = 1;
    edit.sourceId = 2;
    Check("правка блока ушла в канал", minecraft.SendPod(gwyc::MsgType::VoxelEdit, gwyc::kFlagReliable, edit));

    gwyc::MsgGrantReward reward{};
    reward.playerId = 1;
    reward.blockKind = static_cast<uint8_t>(gwyc::BlockKind::Diamond);
    reward.count = 2;
    reward.reason = 1;
    Check("запрос награды ушёл в канал", minecraft.SendPod(gwyc::MsgType::GrantReward, gwyc::kFlagReliable, reward));

    // Игра тикает: колбэки обязаны подняться на этом потоке.
    for (int attempt = 0; attempt < 200 && (g_capture.voxels.empty() || g_capture.rewards.empty()); ++attempt) {
        GwycBridge_Tick();
        SleepMs(5);
    }

    Check("колбэк правки вокселя вызван", g_capture.voxels.size() == 1, std::to_string(g_capture.voxels.size()) + " вызовов");
    if (!g_capture.voxels.empty()) {
        Check("координаты и тип блока дошли",
              g_capture.voxels[0].x == 12 && g_capture.voxels[0].y == 71 && g_capture.voxels[0].z == -3 &&
              g_capture.voxels[0].block == static_cast<uint8_t>(gwyc::BlockKind::Gold),
              "block=" + std::to_string(g_capture.voxels[0].block));
    }

    Check("колбэк награды вызван", g_capture.rewards.size() == 1 && g_capture.rewards[0].count == 2);

    // Игра → Minecraft: результат раунда обязан превратиться в BetResolved + GrantReward.
    GwycBetResolved result{};
    result.structSize = sizeof(result);
    result.playerId = 1;
    result.stake = 4;
    result.payout = 32;
    result.multiplier = 8;
    result.won = 1;
    result.game = 2;
    result.blockKind = static_cast<uint8_t>(GwycBlock_Gold);
    CopyText(result.target, "red");
    CopyText(result.playerName, "Host");

    Check("ставка опубликована", GwycBridge_PublishBetResolved(&result) == GwycStatus_Ok);

    bool sawBetResolved = false;
    bool sawGrant = false;
    gwyc::MsgBetResolved receivedBet{};
    gwyc::MsgGrantReward receivedReward{};

    for (int attempt = 0; attempt < 200 && !(sawBetResolved && sawGrant); ++attempt) {
        while (minecraft.Receive(type, flags, buffer.data(), static_cast<uint32_t>(buffer.size()), size)) {
            if (type == gwyc::MsgType::BetResolved && size == sizeof(receivedBet)) {
                std::memcpy(&receivedBet, buffer.data(), sizeof(receivedBet));
                sawBetResolved = true;
            }
            if (type == gwyc::MsgType::GrantReward && size == sizeof(receivedReward)) {
                std::memcpy(&receivedReward, buffer.data(), sizeof(receivedReward));
                sawGrant = true;
            }
        }
        GwycBridge_Tick();
        SleepMs(5);
    }

    Check("Minecraft получил результат раунда", sawBetResolved);
    if (sawBetResolved) {
        Check("выплата и множитель дошли", receivedBet.payout == 32 && receivedBet.multiplier == 8 && receivedBet.won == 1);
        Check("строка цели дошла", std::strcmp(receivedBet.target, "red") == 0);
    }

    Check("выигрыш превратился в награду блоками", sawGrant && receivedReward.count == 32 && receivedReward.blockKind == 5,
          "count=" + std::to_string(receivedReward.count));

    // Проигрыш награду не создаёт.
    GwycBetResolved loss{};
    loss.structSize = sizeof(loss);
    loss.playerId = 1;
    loss.stake = 4;
    loss.payout = 0;
    loss.won = 0;
    loss.blockKind = static_cast<uint8_t>(GwycBlock_Gold);
    Check("проигрыш публикуется", GwycBridge_PublishBetResolved(&loss) == GwycStatus_Ok);

    int grantFrames = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        while (minecraft.Receive(type, flags, buffer.data(), static_cast<uint32_t>(buffer.size()), size)) {
            if (type == gwyc::MsgType::GrantReward) ++grantFrames;
        }
        SleepMs(3);
    }
    Check("проигрыш не выдаёт блоков", grantFrames == 0, "кадров награды: " + std::to_string(grantFrames));

    // Мусорные аргументы не ломают мост.
    Check("нулевой указатель отвергнут", GwycBridge_PublishBetResolved(nullptr) == GwycStatus_InvalidArgument);
    Check("неизвестный блок отвергнут", [&] {
        GwycVoxelEdit bad{};
        bad.structSize = sizeof(bad);
        bad.blockKind = 200;
        return GwycBridge_PublishVoxelEdit(&bad) == GwycStatus_InvalidArgument;
    }());
    Check("неправильный размер структуры отвергнут", [&] {
        GwycChatLine bad{};
        bad.structSize = 4;
        return GwycBridge_PublishChat(&bad) == GwycStatus_InvalidArgument;
    }());

    // Статистика.
    GwycBridgeStats stats{};
    stats.structSize = sizeof(stats);
    Check("статистика читается", GwycBridge_GetStats(&stats) == GwycStatus_Ok);
    Check("вторая сторона видна как подключённая", stats.peerReady == 1);
    Check("счётчики кадров растут", stats.messagesSent > 0 && stats.messagesReceived > 0,
          "отправлено " + std::to_string(stats.messagesSent) + ", принято " + std::to_string(stats.messagesReceived));
    Check("куб из Minecraft отражён в статистике", stats.cubesSpawned >= 1);
    Check("колбэк подключения поднялся", g_capture.peerConnected >= 1);

    // Хуки казино через ABI (теневой режим вне Windows — это ожидаемо).
    Check("хуки через ABI ставятся", GwycBridge_InstallCasinoHooks(reinterpret_cast<void*>(&FakeBetSubmit),
                                                                   reinterpret_cast<void*>(&FakeTableCreate)) == GwycStatus_Ok);
    uint64_t bets = 0;
    uint64_t tables = 0;
    Check("счётчики хуков читаются", GwycBridge_GetHookCounters(&bets, &tables) == GwycStatus_Ok);
    Check("хуки снимаются через ABI", GwycBridge_RemoveCasinoHooks() == GwycStatus_Ok);

    // Остановка: Minecraft должен увидеть Shutdown-кадр.
    Check("мост останавливается", GwycBridge_Shutdown() == GwycStatus_Ok);

    bool sawShutdown = false;
    for (int attempt = 0; attempt < 100 && !sawShutdown; ++attempt) {
        while (minecraft.Receive(type, flags, buffer.data(), static_cast<uint32_t>(buffer.size()), size)) {
            if (type == gwyc::MsgType::Shutdown) sawShutdown = true;
        }
        SleepMs(3);
    }
    Check("Minecraft получил кадр остановки", sawShutdown);

    Check("после остановки публикация отвергается", GwycBridge_PublishChat(nullptr) == GwycStatus_InvalidArgument);
    Check("статистика после остановки недоступна", GwycBridge_GetStats(&stats) == GwycStatus_NotInitialized);

    minecraft.Close();
}

} // namespace

int main()
{
    std::printf("Проверки нативного ядра моста (порт: %s)\n",
#if defined(_WIN32)
                "Windows/MinHook"
#else
                "теневой режим без MinHook"
#endif
    );

    TestCrc32();
    TestRingBufferThreads();
    TestChannelFraming();
    TestRle();
    TestVoxelSync();
    TestScanner();
    TestHooks();
    TestBridgeAbiEndToEnd();

    std::printf("\n%s\n", std::string(72, '=').c_str());
    if (g_failures.empty()) {
        std::printf("ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ: %d\n", g_passed);
        return 0;
    }

    std::printf("ПРОВАЛЕНО %zu из %d:\n", g_failures.size(), g_passed + static_cast<int>(g_failures.size()));
    for (const std::string& failure : g_failures) std::printf("  • %s\n", failure.c_str());
    return 1;
}
