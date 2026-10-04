// ============================================================================
//  selftest.cpp — самопроверка моста без запуска игры и Minecraft.
//
//  ЧТО ИМЕННО ПРОВЕРЯЕТСЯ
//  ----------------------
//  Мост состоит из кусков, которые легко сломать незаметно: раскладка ABI,
//  кольца без блокировок, формат данных для JVM, очередь вокселей, разбор
//  профиля. Все они проверяются здесь на реальных данных:
//
//    1. ABI: размеры структур, полезные нагрузки, преобразования float;
//    2. кольцо записей: порядок, сохранность полей, переполнение;
//    3. кольцо записей под двумя потоками: ни одна запись не теряется и не
//       дублируется (то, что нельзя увидеть глазами в проде);
//    4. bulk-кольцо: перенос кадра через границу буфера (кадры-заполнители);
//    5. формат для JVM: упаковка/распаковка правок, стабильность хеша раскладки;
//    6. зеркало вокселей: спавн, удаление, бюджет кадра, предел пула,
//       идемпотентность повторной установки блока;
//    7. профиль: разбор текста, вычисление источников значений, запись в payload;
//    8. проверка раскладок хуков (то, что защищает от порчи стека).
//
//  На Windows дополнительно:
//    9.  MinHook: установка/снятие хука на живую функцию;
//   10.  поиск паттерна в собственной памяти и чтение по RVA;
//   11.  двусторонний обмен по разделяемой памяти (два моста в одном процессе);
//   12.  синтетический хост фальшивого Mono: разрешение классов/методов/полей
//        и перехват mono_runtime_invoke (см. --integration).
//
//  Запуск:  gwyfbridge_selftest.exe            — все проверки
//           gwyfbridge_selftest.exe --portable — только переносимая часть
//           gwyfbridge_selftest.exe --print-layout-hash — хеш раскладки для CI
//           gwyfbridge_selftest.exe --integration path\to\GWYF_HookEngine.dll
//
//  Код возврата 0 = всё хорошо, 1 = есть проваленные проверки.
// ============================================================================
#include "gwyfbridge/Base.h"
#include "gwyfbridge/McWireFormat.h"
#include "gwyfbridge/TargetProfile.h"
#include "gwyfbridge/VoxelWorld.h"

#if GWYF_WINDOWS
#include "gwyfbridge/Common.h"
#include "gwyfbridge/HookEngine.h"
#include "gwyfbridge/IpcBridge.h"
#include "gwyfbridge/MonoRuntime.h"
#include "gwyfbridge/PatternScan.h"

#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace gwyf;

namespace {

// ── Мини-фреймворк тестов ───────────────────────────────────────────────────

u32 g_passed = 0;
u32 g_failed = 0;
std::vector<std::string> g_failures;

void Check(bool condition, const std::string& name, const std::string& detail = {}) {
    if (condition) {
        ++g_passed;
        return;
    }

    ++g_failed;
    std::string text = "ПРОВАЛ: " + name;
    if (!detail.empty()) text += " — " + detail;
    g_failures.push_back(text);
    std::printf("%s\n", text.c_str());
    // Сброс буфера обязателен: при перенаправлении вывода в файл (CI) stdout
    // буферизуется полностью, и если процесс упадёт, сообщение о провале
    // пропадёт вместе с буфером. Именно так CI получил «SEGFAULT» без причины.
    std::fflush(stdout);
}

template <typename T>
void CheckEqual(const T& actual, const T& expected, const std::string& name) {
    if (actual == expected) {
        ++g_passed;
        return;
    }
    std::string detail = "получено " + std::to_string(actual) + ", ожидалось " + std::to_string(expected);
    Check(false, name, detail);
}

#if GWYF_WINDOWS

// ── Отчёт об аварийном завершении ───────────────────────────────────────────
//
// Тесты на Windows ловили «SegFault» без единой строки о месте падения:
// отладочных символов в CI нет, а обработчика исключений — тоже. Векторный
// обработчик ниже печатает последнюю начатую секцию и адрес сбоя, пользуясь
// только уже выделенными буферами: в момент падения heap может быть повреждён,
// поэтому здесь нет ни malloc, ни std::string, ни std::printf.

char g_currentSection[128] = "до первой секции";

/// Имя секции, в которой сейчас идёт работа (для обработчика исключений).
void RememberSection(const char* title) {
    if (title == nullptr) return;
    usize index = 0;
    for (; index + 1 < sizeof(g_currentSection) && title[index] != '\0'; ++index) {
        g_currentSection[index] = title[index];
    }
    g_currentSection[index] = '\0';
}

void ReportFatal(const char* text) {
    const usize length = std::strlen(text);
    std::fwrite(text, 1, length, stdout);
    std::fflush(stdout);
}

/// Первокаскадный обработчик: печатает секцию и код исключения, после чего
/// передаёт управление дальше — поведение процесса (и код возврата) не меняем.
LONG WINAPI FatalHandler(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char line[256];
    const int written = std::snprintf(line, sizeof(line),
                                      "\nПРОВАЛ: аварийное завершение (код 0x%08lX) в секции «%s», адрес сбоя %p\n",
                                      static_cast<unsigned long>(code), g_currentSection,
                                      info->ExceptionRecord->ExceptionAddress);
    if (written > 0) ReportFatal(line);
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif  // GWYF_WINDOWS

void Section(const char* title) {
#if GWYF_WINDOWS
    RememberSection(title);
#endif
    std::printf("\n=== %s ===\n", title);
    // См. Check(): без сброса буфера последняя начатая секция не видна в журнале
    // при аварийном завершении, и падение невозможно локализовать.
    std::fflush(stdout);
}

// ── 1. ABI ──────────────────────────────────────────────────────────────────

void TestAbi() {
    Section("ABI разделяемой памяти");

    CheckEqual<usize>(sizeof(ipc::Record), 128, "размер записи кольца");
    CheckEqual<u32>(ipc::kInlinePayload, 64, "размер полезной нагрузки");
    CheckEqual<u32>(ipc::kRecordRingCapacity, 512, "ёмкость кольца записей");
    Check(ipc::kRegionSize == sizeof(ipc::SharedHeader), "размер региона совпадает с заголовком");

    // SharedHeader — это ВЕСЬ регион (655 680 байт: два кольца записей по 64 КБ
    // и два байтовых кольца по 256 КБ), а не «шапка» из нескольких полей.
    // На стеке главного потока Windows (1 МБ) такой объект не помещается: ранее
    // здесь было `ipc::SharedHeader header{}` плюс временный объект в вызове
    // ValidateHeader, в сумме 1,25 МБ — и тест падал с 0xC00000FD ещё до печати
    // заголовка секции. Держим регион в куче, как это делает боевой код.
    auto headerStorage = std::make_unique<ipc::SharedHeader>();

    auto blankRegion = std::make_unique<ipc::SharedHeader>();
    Check(ipc::ValidateHeader(*blankRegion, nullptr) == false, "пустой заголовок отвергается");

    ipc::SharedHeader& header = *headerStorage;
    header.magic = ipc::kMagic;
    header.abiVersion = ipc::kAbiVersion;
    header.headerSize = static_cast<u32>(sizeof(ipc::SharedHeader));
    header.regionSize = static_cast<u32>(ipc::kRegionSize);
    header.gameToMc.capacity = ipc::kRecordRingCapacity;
    header.mcToGame.capacity = ipc::kRecordRingCapacity;
    header.gameToMcBulk.capacity = ipc::kBulkRingCapacity;
    header.mcToGameBulk.capacity = ipc::kBulkRingCapacity;
    Check(ipc::ValidateHeader(header, nullptr), "корректный заголовок принимается");

    header.abiVersion = ipc::kAbiVersion + 1;
    Check(!ipc::ValidateHeader(header, nullptr), "несовпадение версии ABI отвергается");

    // Полезные нагрузки: проверяем, что реальные значения доживают до байтов.
    ipc::Payload payload{};
    ipc::PayloadBet bet{};
    ipc::PayloadBet::SetAmount(bet, 12.5f);
    Check(ipc::FloatToBits(12.5f) == bet.amountBits, "float 12.5 переживает упаковку в биты");
    Check(ipc::BitsToFloat(bet.amountBits) == 12.5f, "float восстанавливается из битов");
    payload.bet = bet;
    Check(ipc::BitsToFloat(payload.bet.amountBits) == 12.5f, "нагрузка ставки сохраняет сумму");

    Check(std::strcmp(ipc::ToString(ipc::RecordType::BetPlaced), "BetPlaced") == 0, "имя события BetPlaced");
    Check(std::strcmp(ipc::ToString(ipc::CommandCode::SpawnProbe), "SpawnProbe") == 0, "имя команды SpawnProbe");
}

// ── 2. Кольцо записей ───────────────────────────────────────────────────────

void TestRecordRing() {
    Section("Кольцо записей: порядок и переполнение");

    auto* ring = new ipc::RecordRing();
    ring->capacity = ipc::kRecordRingCapacity;
    ipc::RecordProducer producer(ring, nullptr, nullptr, ipc::Producer::Game, nullptr);
    ipc::RecordConsumer consumer(ring, nullptr, nullptr);

    // Порядок и сохранность полей.
    for (u32 index = 0; index < 32; ++index) {
        ipc::Payload payload{};
        payload.bet.tableId = index;
        const bool ok = producer.Push(ipc::RecordType::BetPlaced, payload, 1000 + index, index);
        Check(ok, "запись принята в кольцо");
    }

    ipc::Record record{};
    bool orderOk = true;
    for (u32 index = 0; index < 32; ++index) {
        if (!consumer.TryPop(record)) {
            orderOk = false;
            break;
        }
        if (record.payload.bet.tableId != index || record.aux != index || record.sourceId != 1000 + index) {
            orderOk = false;
            break;
        }
    }
    Check(orderOk, "записи читаются в порядке записи с сохранением полей");

    // Переполнение: пишем больше ёмкости.
    u32 accepted = 0;
    for (u32 index = 0; index < ipc::kRecordRingCapacity + 64; ++index) {
        ipc::Payload payload{};
        if (producer.Push(ipc::RecordType::Heartbeat, payload)) ++accepted;
    }
    Check(accepted <= ipc::kRecordRingCapacity, "переполнение не приводит к перезаписи непрочитанных записей");
    Check(producer.Dropped() > 0, "счётчик потерянных записей растёт при переполнении");
    Check(ring->dropped == producer.Dropped(), "счётчик потерь в кольце совпадает с продюсером");

    consumer.Drain([](const ipc::Record&) {}, 100000);
    CheckEqual<u64>(consumer.PendingRecords(), 0, "очередь пуста после слива");

    delete ring;
}

void TestRecordRingConcurrent() {
    Section("Кольцо записей: 200 000 записей из двух потоков");

    auto* ring = new ipc::RecordRing();
    ring->capacity = ipc::kRecordRingCapacity;

    constexpr u32 kTotal = 200000;
    std::atomic<u32> received{0};
    std::atomic<u32> overlaps{0};
    std::vector<u8> seen(kTotal, 0);

    ipc::RecordProducer producer(ring, nullptr, nullptr, ipc::Producer::Minecraft, nullptr);
    ipc::RecordConsumer consumer(ring, nullptr, nullptr);

    std::atomic<bool> producerDone{false};

    std::thread producerThread([&] {
        for (u32 index = 0; index < kTotal; ++index) {
            ipc::Payload payload{};
            payload.voxel.x = static_cast<i32>(index);
            // При переполнении не ждём, а пробуем снова: тест проверяет именно
            // отсутствие потерь и дублей, а не поведение при перегрузке.
            while (!producer.Push(ipc::RecordType::VoxelEdit, payload, index)) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true);
    });

    std::thread consumerThread([&] {
        ipc::Record record{};
        u32 receivedLocal = 0;
        while (receivedLocal < kTotal) {
            if (!consumer.TryPop(record)) {
                if (producerDone.load() && consumer.PendingRecords() == 0) break;
                std::this_thread::yield();
                continue;
            }

            const u32 index = static_cast<u32>(record.payload.voxel.x);
            if (index >= kTotal) {
                overlaps.fetch_add(1);
                continue;
            }
            if (seen[index] != 0) overlaps.fetch_add(1);
            seen[index] = 1;
            ++receivedLocal;
            received.fetch_add(1);
        }
    });

    producerThread.join();
    consumerThread.join();

    CheckEqual<u32>(received.load(), kTotal, "принято все 200 000 записей");
    CheckEqual<u32>(overlaps.load(), 0, "дублей и мусора нет");

    delete ring;
}

// ── 3. Bulk-кольцо ──────────────────────────────────────────────────────────

void TestBulkRing() {
    Section("Bulk-кольцо: кадры и перенос через границу буфера");

    auto* bulk = new ipc::BulkRing();
    bulk->capacity = ipc::kBulkRingCapacity;

    ipc::RecordProducer producer(nullptr, bulk, nullptr, ipc::Producer::Game, nullptr);
    ipc::RecordConsumer consumer(nullptr, bulk, nullptr);

    const std::string first = "отчёт по столу №1: ставка 12.5, выплата 37.5";
    const std::string second(4000, 'x');

    Check(producer.PushBulk(first.data(), static_cast<u32>(first.size()), ipc::kBulkKindText), "короткий кадр записан");
    Check(producer.PushBulk(second.data(), static_cast<u32>(second.size()), ipc::kBulkKindText), "длинный кадр записан");

    std::string out;
    u32 length = 0;
    Check(consumer.TryReadBulk(out, length), "первый кадр прочитан");
    Check(out == first, "содержимое первого кадра совпадает");
    Check(consumer.TryReadBulk(out, length), "второй кадр прочитан");
    Check(out == second, "содержимое длинного кадра совпадает");
    Check(!consumer.TryReadBulk(out, length), "пустое кольцо не отдаёт мусор");

    // Заполняем так, чтобы гарантированно потребовался кадр-заполнитель:
    // сдвигаем голову почти к концу буфера короткими кадрами.
    const std::string small(37, 'y');
    u32 pushed = 0;
    while (producer.Dropped() == 0 && pushed < 6000) {
        if (!producer.PushBulk(small.data(), static_cast<u32>(small.size()), ipc::kBulkKindVoxel)) break;
        ++pushed;
    }

    // Вычитываем всё до конца: парсер обязан сам перепрыгнуть заполнители.
    u32 read = 0;
    while (consumer.TryReadBulk(out, length)) {
        ++read;
        if (read > 10000) break;
    }
    Check(read > 0, "после заполнителей читаются нормальные кадры");
    Check(read == pushed, "число прочитанных кадров совпадает с записанными до отказа: " +
                              std::to_string(pushed) + " / " + std::to_string(read));

    // Кадр больше четверти кольца отвергается: иначе продюсер способен заклинить кольцо.
    std::vector<char> tooBig(ipc::kBulkMaxFrame + 1, 'z');
    Check(!producer.PushBulk(tooBig.data(), static_cast<u32>(tooBig.size())), "слишком большой кадр отвергнут");

    delete bulk;
}

// ── 4. Формат для JVM ───────────────────────────────────────────────────────

void TestWireFormat() {
    Section("Формат обмена с JVM");

    CheckEqual<u32>(mcwire::kRecordStride, 88, "шаг записи для JVM");
    CheckEqual<u32>(mcwire::kBulkEditStride, 20, "шаг упакованной правки");
    Check(mcwire::LayoutHash() != 0, "хеш раскладки посчитан");

    // Запись целиком: пишем во все поля уникальные значения.
    ipc::Record record{};
    record.type = static_cast<u32>(ipc::RecordType::BetResolved);
    record.flags = 7;
    record.sourceId = 76561198000000000ull;
    record.aux = 42;
    ipc::PayloadBetResult result{};
    result.payout = 1500;
    result.chipsAfter = 3200;
    result.won = 1;
    result.tableId = 9;
    record.payload.betResult = result;

    std::vector<u8> buffer(mcwire::kRecordStride, 0);
    u32 written = 0;
    Check(mcwire::WriteRecord(buffer.data(), static_cast<u32>(buffer.size()), record, &written), "запись упакована");
    CheckEqual<u32>(written, mcwire::kRecordStride, "записано ровно 88 байт");

    u32 type = 0;
    u64 source = 0;
    i64 payout = 0;
    i64 chips = 0;
    std::memcpy(&type, buffer.data() + mcwire::kOffsetType, sizeof(type));
    std::memcpy(&source, buffer.data() + mcwire::kOffsetSourceId, sizeof(source));
    std::memcpy(&payout, buffer.data() + mcwire::kOffsetPayload + 0, sizeof(payout));
    std::memcpy(&chips, buffer.data() + mcwire::kOffsetPayload + 8, sizeof(chips));

    CheckEqual<u32>(type, static_cast<u32>(ipc::RecordType::BetResolved), "тип записи на своём месте");
    CheckEqual<u64>(source, record.sourceId, "sourceId на своём месте");
    CheckEqual<i64>(payout, 1500, "выплата на своём месте");
    CheckEqual<i64>(chips, 3200, "баланс на своём месте");

    // Массив правок, включая отрицательные координаты.
    std::vector<ipc::PayloadVoxel> edits(3);
    edits[0] = {-5, 70, 12, 3, static_cast<u32>(ipc::VoxelAction::Place), 111};
    edits[1] = {0, 0, 0, 1, static_cast<u32>(ipc::VoxelAction::Break), 222};
    edits[2] = {1000000, 255, -1000000, 6, static_cast<u32>(ipc::VoxelAction::Place), 333};

    std::vector<u8> packed(edits.size() * mcwire::kBulkEditStride);
    const u32 count = mcwire::PackBulkEdits(edits.data(), static_cast<u32>(edits.size()), packed.data(),
                                            static_cast<u32>(packed.size()));
    CheckEqual<u32>(count, 3, "упакованы все правки");

    u32 restored = 0;
    bool identical = true;
    mcwire::UnpackBulkEdits(packed.data(), static_cast<u32>(packed.size()), [&](const ipc::PayloadVoxel& edit) {
        const ipc::PayloadVoxel& expected = edits[restored];
        if (edit.x != expected.x || edit.y != expected.y || edit.z != expected.z || edit.block != expected.block ||
            edit.action != expected.action) {
            identical = false;
        }
        ++restored;
    });
    CheckEqual<u32>(restored, 3, "распакованы все правки");
    Check(identical, "координаты и типы блоков не искажаются (включая отрицательные)");

    std::printf("  раскладка: %s\n", mcwire::Describe().c_str());
}

// ── 5. Зеркало вокселей ─────────────────────────────────────────────────────

/// Подставной адаптер: считает вызовы и «создаёт» объекты как номера.
class FakeAdapter final : public voxel::ISpawnAdapter {
public:
    const char* Name() const override { return "fake (самотест)"; }

    bool Initialize(const profile::VoxelSpawnSpec& spec, const std::string& gameAssembly, std::string* error) override {
        (void)gameAssembly;
        (void)error;
        spec_ = spec;
        ready_ = true;
        return true;
    }

    bool Spawn(const voxel::Vec3i& position, u32 block, void** outHandle, std::string* error) override {
        (void)error;
        ++spawnCalls;
        if (failNext) {
            failNext = false;
            if (error != nullptr) *error = "подставной отказ: объект не создан";
            return false;
        }
        if (outHandle != nullptr) {
            *outHandle = reinterpret_cast<void*>(static_cast<u64>(++handleCounter));
        }
        lastPosition = position;
        lastBlock = block;
        return true;
    }

    void Despawn(void* handle) override {
        (void)handle;
        ++despawnCalls;
    }

    bool Ready() const override { return ready_; }

    u32 spawnCalls = 0;
    u32 despawnCalls = 0;
    u64 handleCounter = 0;
    bool failNext = false;
    voxel::Vec3i lastPosition{};
    u32 lastBlock = 0;

private:
    bool ready_ = false;
    profile::VoxelSpawnSpec spec_{};
};

void TestVoxelMirror() {
    Section("Зеркало вокселей: спавн, удаление, бюджет");

    profile::VoxelSpawnSpec spec{};
    spec.origin[0] = 100.0f;
    spec.origin[1] = 5.0f;
    spec.origin[2] = -50.0f;
    spec.scale = 1.0f;
    spec.budgetPerFrame = 2;
    spec.poolLimit = 8;
    spec.despawnOnBreak = true;

    // Зеркало вокселей тоже крупное (262 552 байта) — держим его в куче, чтобы
    // кадр функции оставался маленьким: стек главного потока на Windows всего 1 МБ.
    auto mirrorStorage = std::make_unique<voxel::VoxelMirror>();
    voxel::VoxelMirror& mirror = *mirrorStorage;
    mirror.Configure(spec);

    FakeAdapter adapter;
    Check(adapter.Initialize(spec, "Assembly-CSharp", nullptr), "подставной адаптер инициализирован");

    auto place = [&](i32 x, i32 y, i32 z, u32 block) {
        ipc::PayloadVoxel edit{};
        edit.x = x;
        edit.y = y;
        edit.z = z;
        edit.block = block;
        edit.action = static_cast<u32>(ipc::VoxelAction::Place);
        return mirror.PushIncoming(edit);
    };
    auto breakBlock = [&](i32 x, i32 y, i32 z) {
        ipc::PayloadVoxel edit{};
        edit.x = x;
        edit.y = y;
        edit.z = z;
        edit.action = static_cast<u32>(ipc::VoxelAction::Break);
        return mirror.PushIncoming(edit);
    };

    Check(place(1, 0, 2, 3), "правка «поставить блок» принята");
    Check(place(2, 0, 2, 5), "вторая правка принята");

    // Бюджет 2 объекта за кадр: обе правки должны уложиться.
    std::string error;
    u32 processed = mirror.ProcessOnMainThread(adapter, spec.budgetPerFrame, &error);
    CheckEqual<u32>(processed, 2, "обработаны две правки");
    CheckEqual<u32>(adapter.spawnCalls, 2, "создано ровно два объекта");
    Check(adapter.lastBlock == 5, "тип блока доходит до адаптера");

    // Проверяем преобразование координат: origin + позиция.
    float gamePosition[3]{};
    voxel::Vec3i position{1, 0, 2};
    voxel::ToGameSpace(spec, position, gamePosition);
    Check(gamePosition[0] == 101.0f && gamePosition[1] == 5.0f && gamePosition[2] == -48.0f,
          "координаты переводятся в пространство игры (origin + блок)");

    voxel::VoxelStats stats = mirror.Stats();
    CheckEqual<u32>(stats.liveObjects, 2, "в мире два живых объекта");
    CheckEqual<u64>(stats.spawned, 2, "счётчик созданных объектов");

    // Идемпотентность: повторная установка того же блока не создаёт второй объект.
    place(1, 0, 2, 3);
    mirror.ProcessOnMainThread(adapter, 4, &error);
    CheckEqual<u32>(adapter.spawnCalls, 2, "повторная установка того же блока не дублирует объект");

    // Слом блока удаляет объект.
    breakBlock(1, 0, 2);
    mirror.ProcessOnMainThread(adapter, 4, &error);
    CheckEqual<u32>(adapter.despawnCalls, 1, "слом блока удаляет объект в игре");
    CheckEqual<u32>(mirror.Stats().liveObjects, 1, "живых объектов осталось один");

    // Бюджет: 5 правок, бюджет 2 → за один проход создаётся не больше двух.
    for (i32 index = 0; index < 5; ++index) {
        place(10 + index, 1, 10, 1);
    }
    const u32 beforeSpawn = adapter.spawnCalls;
    mirror.ProcessOnMainThread(adapter, 2, &error);
    Check(adapter.spawnCalls - beforeSpawn <= 2, "бюджет кадра соблюдается (не больше двух объектов)");

    // Доспавн отложенных объектов следующими кадрами.
    u32 passes = 0;
    while (mirror.Stats().pending > 0 && passes < 10) {
        mirror.ProcessOnMainThread(adapter, 2, &error);
        ++passes;
    }
    CheckEqual<u32>(adapter.spawnCalls, beforeSpawn + 5, "все отложенные объекты досозданы");

    // Ошибка адаптера не роняет мост и учитывается в статистике.
    adapter.failNext = true;
    place(50, 50, 50, 7);
    mirror.ProcessOnMainThread(adapter, 4, &error);
    Check(mirror.Stats().failed >= 1, "отказ адаптера учтён в статистике");
    Check(error.find("отказ") != std::string::npos, "причина отказа доходит до вызывающего кода: " + error);

    // Предел пула: следующий объект не создаётся.
    for (i32 index = 0; index < 12; ++index) {
        place(100 + index, 2, 100, 2);
    }
    mirror.ProcessOnMainThread(adapter, 32, &error);
    Check(mirror.Stats().liveObjects <= spec.poolLimit, "предел числа объектов не превышается");

    const std::string description = mirror.Describe();
    Check(description.find("Воксели:") != std::string::npos, "отчёт по вокселям формируется");

    const u32 removed = mirror.DespawnAll(adapter);
    Check(removed > 0, "очистка мира удаляет объекты");
    CheckEqual<u32>(mirror.Stats().liveObjects, 0, "после очистки объектов нет");
}

// ── 6. Профиль ──────────────────────────────────────────────────────────────

void TestText() {
    Section("Строки: переход UTF-8 ↔ wide без потерь (места, где MSVC ловил C4244)");

    Check(ToWideUtf8("").empty(), "пустая узкая строка даёт пустую широкую");
    Check(ToNarrowUtf8(std::wstring_view()).empty(), "пустая широкая строка даёт пустую узкую");

    const std::string region = "Local\\GWYF_MC_BRIDGE_ABI1";
    const std::wstring wideRegion = ToWideUtf8(region);
    CheckEqual<usize>(wideRegion.size(), region.size(), "длина ASCII-строки сохраняется при переходе к wide");
    Check(ToNarrowUtf8(wideRegion) == region, "ASCII-строка переживает round-trip");

    // Имя региона/путь берутся из файла профиля и аргументов командной строки:
    // если там окажется кириллица, байты обязаны дойти без искажений.
    const std::string cyrillic = "регион-моста-№1";
    Check(ToNarrowUtf8(ToWideUtf8(cyrillic)) == cyrillic, "UTF-8 переживает round-trip");

    // Именно эта строка ловилась MSVC как C4244 в заголовке STL:
    // std::string(regionName.begin(), regionName.end()) в Profile::Describe().
    profile::Profile described = profile::DefaultForGambleWithYourFriends();
    described.regionName = ToWideUtf8("Local\\GWYF_ОПИСАНИЕ");
    const std::string report = described.Describe();
    Check(report.find("Local\\GWYF_ОПИСАНИЕ") != std::string::npos,
          "отчёт профиля содержит имя региона без искажений");
    Check(report.find("Профиль: регион=") == 0, "отчёт профиля начинается с имени региона");
}

void TestProfile() {
    Section("Профиль: разбор, источники значений, запись в нагрузку");

    const std::string text = gwyf::profile::TemplateText();
    Check(text.find("[voxel]") != std::string::npos, "шаблон профиля содержит секцию [voxel]");
    Check(text.find("map=amount:arg0f") != std::string::npos, "шаблон профиля содержит привязки ставки");

    // Раскладки аргументов.
    const std::vector<std::string> kinds = profile::ShapeKinds("i32_f32");
    CheckEqual<usize>(kinds.size(), 2, "раскладка i32_f32 разбирается на два типа");
    Check(kinds[0] == "int32" && kinds[1] == "float32", "типы раскладки распознаны верно");

    Check(profile::CheckShape({"int32", "float32"}, {"int32", "float32"}).empty(), "совпадающая раскладка принята");
    Check(!profile::CheckShape({"int32", "float32"}, {"float32", "int32"}).empty(), "перепутанные типы отвергнуты");
    Check(!profile::CheckShape({"int32"}, {"int32", "int32"}).empty(), "несовпадение числа аргументов отвергнуто");
    Check(profile::CheckShape({"int32"}, {}).empty(), "без метаданных работаем по профилю (с предупреждением)");

    // Источники значений.
    profile::EvalContext context{};
    i64 intValue = 0;
    float floatValue = 0.0f;
    bool isFloat = false;

    // Соглашение о представлении аргументов (то же, что использует детур
    // mono_runtime_invoke в движке): args[i] хранит САМО значение для целых и
    // указатель для ссылочных типов; для float дополнительно заполняется
    // floatArgs[i], потому что биты float нельзя восстановить из указателя.
    const float arg1 = 2.5f;
    context.args[0] = reinterpret_cast<void*>(static_cast<u64>(7));   // целое 7
    context.args[1] = reinterpret_cast<void*>(static_cast<u64>(static_cast<i64>(arg1)));
    context.floatArgs[1] = arg1;
    context.argCount = 2;

    Check(profile::EvaluateSource("arg0", context, intValue, floatValue, isFloat), "источник arg0 вычисляется");
    CheckEqual<i64>(intValue, 7, "значение arg0");
    Check(profile::EvaluateSource("arg1f", context, intValue, floatValue, isFloat), "источник arg1f вычисляется");
    Check(floatValue == 2.5f && isFloat, "значение float-аргумента не портится");

    // Запись в нагрузку.
    ipc::Payload payload{};
    Check(profile::ApplyToPayload(payload, "bet.amount", 0, 12.5f, true), "сумма ставки записана в нагрузку");
    Check(ipc::BitsToFloat(payload.bet.amountBits) == 12.5f, "сумма ставки читается обратно");
    Check(profile::ApplyToPayload(payload, "bet.tableId", 9, 0.0f, false), "номер стола записан");
    CheckEqual<u32>(payload.bet.tableId, 9, "номер стола в нагрузке");
    Check(profile::ApplyToPayload(payload, "voxel.block", 3, 0.0f, false), "тип блока записан");
    CheckEqual<u32>(payload.voxel.block, 3, "тип блока в нагрузке");
    Check(!profile::ApplyToPayload(payload, "нет.такого.поля", 1, 0.0f, false), "неизвестное поле отвергается");

    Check(profile::ParseEventName("BetPlaced") == ipc::RecordType::BetPlaced, "имя события разбирается");
    Check(profile::ParseEventName("BetResolved") == ipc::RecordType::BetResolved, "имя события BetResolved");
    Check(profile::ParseMode("method") == profile::ResolveMode::Method, "режим method разбирается");
    Check(profile::ParseMode("poll") == profile::ResolveMode::Poll, "режим poll разбирается");

    // Значения по умолчанию для GWYF: профиль обязан быть рабочим «из коробки».
    const profile::Profile defaults = profile::DefaultForGambleWithYourFriends();
    Check(!defaults.watches.empty(), "в профиле по умолчанию есть цели наблюдения");
    Check(defaults.voxel.poolLimit > 0, "предел пула объектов задан");
    Check(defaults.voxel.budgetPerFrame > 0, "бюджет кадра задан");
}

// ── Формальный отчёт ────────────────────────────────────────────────────────

void PrintSummary(const char* title) {
    std::printf("\n──────────────────────────────────────────────\n");
    std::printf("%s: успешно %u, провалено %u\n", title, g_passed, g_failed);
    if (!g_failures.empty()) {
        std::printf("Проваленные проверки:\n");
        for (const std::string& failure : g_failures) {
            std::printf("  * %s\n", failure.c_str());
        }
    }
    std::printf("──────────────────────────────────────────────\n");
    std::fflush(stdout);
}

#if GWYF_WINDOWS

// ── 7. MinHook ──────────────────────────────────────────────────────────────

volatile i32 g_callCount = 0;

i32 __stdcall TargetFunction(i32 a, i32 b) {
    g_callCount = g_callCount + 1;
    return a + b;
}

i32 __stdcall DetourFunction(i32 a, i32 b) {
    // Детур обязан сохранить семантику: считаем вызов и делегируем оригиналу.
    g_callCount = g_callCount + 100;
    return a * b;
}

void TestHooks() {
    Section("Хуки (MinHook): установка, детур, снятие");

    void* original = nullptr;
    Check(hook::Install("selftest:TargetFunction", reinterpret_cast<void*>(&TargetFunction),
                        reinterpret_cast<void*>(&DetourFunction), &original),
          "хук установлен");
    Check(hook::Has(reinterpret_cast<void*>(&TargetFunction)), "хук виден в списке");

    using Fn = i32(__stdcall*)(i32, i32);
    auto call = reinterpret_cast<Fn>(&TargetFunction);

    g_callCount = 0;
    const i32 result = call(3, 4);
    CheckEqual<i32>(result, 12, "вызов идёт через детур (умножение вместо сложения)");
    CheckEqual<i32>(static_cast<i32>(g_callCount), 100, "детур выполнился");

    if (original != nullptr) {
        auto originalFn = reinterpret_cast<Fn>(original);
        g_callCount = 0;
        const i32 viaTrampoline = originalFn(3, 4);
        CheckEqual<i32>(viaTrampoline, 7, "трамплин вызывает оригинальную функцию");
        CheckEqual<i32>(static_cast<i32>(g_callCount), 1, "оригинальная функция выполнилась");
    } else {
        Check(false, "трамплин получен");
    }

    Check(hook::Remove(reinterpret_cast<void*>(&TargetFunction)), "хук снят");
    g_callCount = 0;
    const i32 after = call(3, 4);
    CheckEqual<i32>(after, 7, "после снятия хука функция работает как раньше");
    CheckEqual<i32>(static_cast<i32>(g_callCount), 1, "счётчик вызовов это подтверждает");
}

// ── 8. Поиск паттернов и чтение памяти ──────────────────────────────────────

void TestPatternScan() {
    Section("Поиск паттернов и RVA");

    // Классика: сигнатура с «джокером» и точная сигнатура.
    const u8 data[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};
    const scan::Pattern wildcard = scan::Parse("48 89 ?? 24 08");
    void* found = scan::FindFirst(data, sizeof(data), wildcard);
    Check(found == data, "паттерн с джокером найден по верному смещению");

    const scan::Pattern exact = scan::Parse("48 89 74 24 10");
    void* foundExact = scan::FindFirst(data, sizeof(data), exact);
    Check(foundExact == data + 5, "точный паттерн найден по верному смещению");

    const scan::Pattern missing = scan::Parse("DE AD BE EF");
    Check(scan::FindFirst(data, sizeof(data), missing) == nullptr, "отсутствующий паттерн не находится");

    // Поиск в собственной памяти: пролог нашей функции обязан найтись.
    ModuleInfo self{};
    Check(FindModule("gwyfbridge_selftest", self), "модуль самотеста найден");
    if (self.handle != nullptr) {
        const std::string signature = hook::DumpBytes(reinterpret_cast<void*>(&TargetFunction), 8);
        Check(!signature.empty(), "байты пролога функции читаются");

        const scan::Pattern prologue = scan::Parse(signature);
        void* inModule = scan::FindInModule("gwyfbridge_selftest", prologue);
        Check(inModule != nullptr, "пролог функции находится поиском по модулю");

        void* byRva = scan::ResolveRva("gwyfbridge_selftest",
                                 reinterpret_cast<u64>(&TargetFunction) - reinterpret_cast<u64>(self.base));
        Check(byRva == reinterpret_cast<void*>(&TargetFunction), "RVA указывает на ту же функцию");
    }
}

// ── 9. Обмен через разделяемую память ───────────────────────────────────────

void TestBridge() {
    Section("Разделяемая память: два конца моста в одном процессе");

    // Уникальное имя региона, чтобы не мешать запущенной игре.
    const std::wstring region = L"Local\\GWYF_SELFTEST_" + std::to_wstring(GetCurrentProcessId());

    ipc::Bridge game;
    ipc::Bridge minecraft;

    std::string error;
    Check(game.Open(ipc::Role::Game, region, "selftest/game", true, &error), "сторона игры создала регион: " + error);
    Check(minecraft.Open(ipc::Role::Minecraft, region, "selftest/mc", false, &error),
          "сторона Minecraft подключилась: " + error);

    Check(game.IsOpen() && minecraft.IsOpen(), "оба конца моста открыты");
    Check(game.Header()->magic == ipc::kMagic, "сигнатура региона на месте");

    // Игра → Minecraft: событие выигрыша.
    ipc::Payload payload{};
    payload.betResult.payout = 250;
    payload.betResult.won = 1;
    Check(game.Out().Push(ipc::RecordType::BetResolved, payload, 4242), "событие выигрыша отправлено");

    ipc::Record received{};
    Check(minecraft.In().TryPop(received), "Minecraft получил событие");
    Check(received.type == static_cast<u32>(ipc::RecordType::BetResolved), "тип события верный");
    CheckEqual<i64>(received.payload.betResult.payout, 250, "выплата дошла без искажений");
    CheckEqual<u64>(received.sourceId, 4242, "идентификатор игрока дошёл");

    // Minecraft → игра: правка блока.
    ipc::PayloadVoxel edit{};
    edit.x = 7;
    edit.y = 64;
    edit.z = -3;
    edit.block = 2;
    edit.action = static_cast<u32>(ipc::VoxelAction::Place);

    ipc::Payload editPayload{};
    editPayload.voxel = edit;
    Check(minecraft.Out().Push(ipc::RecordType::VoxelEdit, editPayload), "правка блока отправлена");

    ipc::Record editRecord{};
    Check(game.In().TryPop(editRecord), "игра получила правку");
    CheckEqual<i32>(editRecord.payload.voxel.y, 64, "высота блока дошла верно");

    // Пульс и живость партнёра.
    game.Out().Heartbeat();
    minecraft.Out().Heartbeat();
    Check(game.PeerAlive(), "игра видит пульс Minecraft");
    Check(minecraft.PeerAlive(), "Minecraft видит пульс игры");

    const std::string description = game.Describe();
    Check(description.find("ABI=") != std::string::npos, "строка диагностики формируется");

    // Команда с длинным текстом: уходит в bulk-кольцо.
    Check(ipc::SendCommand(minecraft.Out(), ipc::CommandCode::DumpClass, "Game.BetManager"),
          "команда с текстом отправлена");
    ipc::Record command{};
    Check(game.In().TryPop(command), "команда получена игрой");
    CheckEqual<u32>(command.payload.command.textLength, 15, "длина текста команды в записи");

    std::string text;
    u32 length = 0;
    Check(game.In().TryReadBulk(text, length), "текст команды прочитан из bulk-кольца");
    Check(text == "Game.BetManager", "текст команды не искажён");

    game.Close();
    minecraft.Close();
}

// ── 10. Синтетический хост фальшивого Mono (-–integration) ──────────────────

using InitFn = BOOL(WINAPI*)();
using IsReadyFn = BOOL(WINAPI*)();
using StatusFn = const char*(WINAPI*)();
using ShutdownFn = void(WINAPI*)();

int RunIntegration(const std::string& enginePath) {
    Section("Интеграция: движок внутри синтетического хоста с фальшивым Mono");

    // 1. Определяем путь к подставному хосту Mono рядом с самотестом.
    char modulePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    std::string directory = modulePath;
    const usize slash = directory.find_last_of("\\/");
    directory = slash == std::string::npos ? "." : directory.substr(0, slash);

    const std::string fakeMonoPath = directory + "\\fake_mono_host.dll";
    SetEnvironmentVariableA("GWYF_MONO_MODULE", fakeMonoPath.c_str());

    HMODULE fakeMono = LoadLibraryA(fakeMonoPath.c_str());
    Check(fakeMono != nullptr, "подставной хост Mono загружен: " + fakeMonoPath);
    if (fakeMono == nullptr) return 1;

    // 2. Открываем «сторону Minecraft», чтобы читать события движка.
    const std::wstring region = L"Local\\GWYF_INTEGRATION_" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"GWYF_REGION_NAME", region.c_str());

    ipc::Bridge minecraft;
    std::string error;
    Check(minecraft.Open(ipc::Role::Minecraft, region, "selftest/mc", true, &error),
          "наблюдатель открыл регион: " + error);

    // 3. Загружаем сам движок (как это сделал бы инжектор внутри игры).
    HMODULE engine = LoadLibraryA(enginePath.c_str());
    Check(engine != nullptr, "GWYF_HookEngine.dll загружен: " + enginePath);
    if (engine == nullptr) return 1;

    auto init = reinterpret_cast<InitFn>(GetProcAddress(engine, "GWYF_Init"));
    auto isReady = reinterpret_cast<IsReadyFn>(GetProcAddress(engine, "GWYF_IsReady"));
    auto status = reinterpret_cast<StatusFn>(GetProcAddress(engine, "GWYF_Status"));
    auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(engine, "GWYF_Shutdown"));

    Check(init != nullptr && status != nullptr && shutdown != nullptr, "экспорты движка на месте");
    if (init == nullptr || status == nullptr) return 1;

    // 4. Инициализация: движок должен найти фальшивый Mono, сборку и цели профиля.
    for (int attempt = 0; attempt < 20; ++attempt) {
        init();
        if (isReady != nullptr && isReady()) break;
        Sleep(100);
    }

    Check(isReady != nullptr && isReady(), "движок инициализировался на подставном хосте");

    const std::string statusText = status();
    std::printf("%s\n", statusText.c_str());
    Check(statusText.find("GWYF_HookEngine") != std::string::npos, "статус движка читается");

    // 5. Дёргаем фальшивый runtime так, как это делала бы игра: вызов ставки.
    using FakeInvoke = void*(*)(const char*, void**, const char*);
    auto fakeInvoke = reinterpret_cast<FakeInvoke>(GetProcAddress(fakeMono, "FakeMono_InvokeByName"));
    Check(fakeInvoke != nullptr, "фальшивый invoke найден");

    if (fakeInvoke != nullptr) {
        void* args[2] = {nullptr, nullptr};
        i64 amount = 0;
        float betAmount = 5.0f;
        args[0] = reinterpret_cast<void*>(static_cast<u64>(76561198000000001ull));
        args[1] = reinterpret_cast<void*>(static_cast<u64>(static_cast<i64>(betAmount)));
        (void)amount;

        fakeInvoke("Game.BetManager", args, "PlaceBet");

        // Читаем кольцо: движок должен опубликовать событие ставки.
        u64 deadline = NowTickMs() + 3000;
        bool eventSeen = false;
        while (NowTickMs() < deadline && !eventSeen) {
            ipc::Record record{};
            while (minecraft.In().TryPop(record)) {
                if (record.type == static_cast<u32>(ipc::RecordType::BetPlaced)) {
                    eventSeen = true;
                    std::printf("  получено событие BetPlaced, сумма-биты=%u\n", record.payload.bet.amountBits);
                }
            }
            Sleep(20);
        }

        Check(eventSeen, "событие ставки дошло от движка к стороне Minecraft");
    }

    shutdown();
    minecraft.Close();
    FreeLibrary(engine);
    return 0;
}

#endif  // GWYF_WINDOWS

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);

    const bool portableOnly = std::find(arguments.begin(), arguments.end(), "--portable") != arguments.end();
    const bool printHash = std::find(arguments.begin(), arguments.end(), "--print-layout-hash") != arguments.end();

    if (printHash) {
        // Используется проверкой контракта в CI: Java-константа обязана совпасть.
        std::printf("%08X\n", mcwire::LayoutHash());
        return 0;
    }

    log::SetLevel(log::Level::Warn);
#if GWYF_WINDOWS
    // Отчёт об аварии ставим до первой секции: иначе падение в тесте остаётся
    // без объяснения (в CI нет отладочных символов и дампа).
    ::AddVectoredExceptionHandler(1, &FatalHandler);
#endif
    std::printf("gwyfbridge selftest — проверка моста GWYF ↔ Minecraft\n");
    std::fflush(stdout);

    TestAbi();
    TestRecordRing();
    TestRecordRingConcurrent();
    TestBulkRing();
    TestWireFormat();
    TestVoxelMirror();
    TestText();
    TestProfile();

#if GWYF_WINDOWS
    if (!portableOnly) {
        TestHooks();
        TestPatternScan();
        TestBridge();
    }
#else
    (void)portableOnly;
#endif

    // Интеграционный прогон: движок внутри синтетического хоста.
    int integrationResult = 0;
    const auto integrationArg = std::find(arguments.begin(), arguments.end(), "--integration");
    if (integrationArg != arguments.end() && std::next(integrationArg) != arguments.end()) {
#if GWYF_WINDOWS
        integrationResult = RunIntegration(*std::next(integrationArg));
#else
        std::printf("\n(интеграционный прогон доступен только на Windows)\n");
#endif
    }

    PrintSummary(integrationResult == 0 ? "Итог" : "Итог (интеграция провалена)");
    return (g_failed == 0 && integrationResult == 0) ? 0 : 1;
}
