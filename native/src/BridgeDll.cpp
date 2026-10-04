// =============================================================================
//  BridgeDll.cpp — сам мост: экспорт C-ABI, рабочий поток, диспетчер сообщений.
//
//  МОДЕЛЬ ПОТОКОВ (главное правило, иначе игра падает):
//    * главный поток игры вызывает GwycBridge_Tick и GwycBridge_Publish*;
//    * рабочий поток занимается только каналом: забирает исходящую очередь и
//      вычитывает входящие кадры;
//    * колбэки игры ВСЕГДА поднимаются на главном потоке из GwycBridge_Tick —
//      игровые системы однопоточные, вызов из worker'а даёт гонки и падения;
//    * каналы — строго SPSC: у каждого кольца ровно один писатель и один читатель,
//      поэтому отправлять в канал имеет право только рабочий поток.
// =============================================================================
// Определение продублировано в системе сборки (CMake/vcxproj). Здесь — на случай
// ручной компиляции одним g++/cl, но без конфликта с внешним -D.
#ifndef GWYC_BRIDGE_EXPORTS
#   define GWYC_BRIDGE_EXPORTS
#endif

#include "gwyc/plugin_abi.h"

#include "GWYF_HookEngine.h"
#include "Native_JNI_Bridge.h"
#include "VoxelTo3DWorld.h"
#include "gwyc/protocol.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace gwyc;

/// Верхний предел очереди входящих: защита от роста памяти, если игра не тикает.
constexpr size_t kMaxInboundQueue = 4096;

uint64_t NowMs() noexcept
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// Исходящее сообщение: очередь ВЛАДЕЕТ байтами (никаких висячих указателей).
struct OutgoingMessage {
    MsgType type = MsgType::None;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
};

/// Входящее сообщение, ждущее разбора на главном потоке.
struct IncomingMessage {
    MsgType type = MsgType::None;
    std::vector<uint8_t> payload;
};

struct BridgeState {
    std::mutex mutex;

    bridge::BridgeConfig config;
    std::unique_ptr<bridge::ITransport> transport;
    std::unique_ptr<world::VoxelWorldSync> voxels;

    GwycGameCallbacks callbacks{};

    std::deque<OutgoingMessage> outbound;
    std::deque<IncomingMessage> inbound;

    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> peerReady{false};
    std::atomic<uint64_t> messagesSent{0};
    std::atomic<uint64_t> messagesReceived{0};
    std::atomic<uint64_t> rttMs{0};
    std::atomic<uint64_t> startedAtMs{0};
    std::atomic<uint64_t> lastPingMs{0};

    bool initialized = false;
    bool hooksInstalled = false;
};

BridgeState g_state;

void PushOutbound(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize)
{
    OutgoingMessage message;
    message.type = type;
    message.flags = flags;
    if (payloadSize != 0 && payload != nullptr) {
        message.payload.assign(static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + payloadSize);
    }

    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.outbound.push_back(std::move(message));
}

/// Лог идёт через очередь входящих → колбэк игры поднимется на её главном потоке.
void QueueLog(uint32_t level, const std::string& text)
{
    if (!g_state.initialized) return;

    MsgLog message{};
    message.level = static_cast<uint8_t>(level);
    WriteFixed(message.text, sizeof(message.text), text.c_str());

    IncomingMessage incoming;
    incoming.type = MsgType::Log;
    incoming.payload.assign(reinterpret_cast<const uint8_t*>(&message),
                            reinterpret_cast<const uint8_t*>(&message) + sizeof(message));

    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.inbound.size() >= kMaxInboundQueue) g_state.inbound.pop_front();
    g_state.inbound.push_back(std::move(incoming));
}

void QueuePeerState(bool connected)
{
    IncomingMessage incoming;
    incoming.type = connected ? MsgType::HelloAck : MsgType::Shutdown;

    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.inbound.size() >= kMaxInboundQueue) g_state.inbound.pop_front();
    g_state.inbound.push_back(std::move(incoming));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Колбэк мира: материализация куба в игре
// ─────────────────────────────────────────────────────────────────────────────

void GWYC_ABI_CALL VoxelApplyToGame(int32_t x, int32_t y, int32_t z, uint8_t blockKind, uint16_t sourceId)
{
    // Всегда вызывается из главного потока (VoxelWorldSync::Flush в GwycBridge_Tick).
    if (g_state.callbacks.onVoxelApply != nullptr) {
        g_state.callbacks.onVoxelApply(x, y, z, blockKind, sourceId);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Хуки казино: наблюдатели получают контекст ставки от движка перехвата
// ─────────────────────────────────────────────────────────────────────────────

void GWYC_ABI_CALL OnBetObserved(void* betContext)
{
    (void)betContext;
    // Раскладка структуры ставки — знание владельца процесса (игры): именно он
    // публикует ставку через GwycBridge_PublishBetPlaced, вызывая этот колбэк из
    // своего обработчика. Мост только считает события (см. GwycBridge_GetHookCounters).
}

void GWYC_ABI_CALL OnTableObserved(const char* tableName, int32_t seats)
{
    if (tableName == nullptr) return;

    MsgTableEvent event{};
    event.tableId = static_cast<uint32_t>(hooks::GetCasinoCounters().tablesObserved);
    event.seats = static_cast<uint8_t>(seats < 0 ? 0 : (seats > 255 ? 255 : seats));
    event.state = 1;
    WriteFixed(event.owner, sizeof(event.owner), tableName);

    PushOutbound(MsgType::TableEvent, kFlagReliable, &event, static_cast<uint32_t>(sizeof(event)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Рабочий поток: канал — единственный его владелец
// ─────────────────────────────────────────────────────────────────────────────

void PumpOutbound()
{
    std::deque<OutgoingMessage> batch;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.outbound.empty()) return;
        batch.swap(g_state.outbound);
    }

    for (const OutgoingMessage& message : batch) {
        if (g_state.transport == nullptr) break;

        const bool sent = g_state.transport->Send(message.type, message.flags,
                                                  message.payload.empty() ? nullptr : message.payload.data(),
                                                  static_cast<uint32_t>(message.payload.size()));
        if (sent) g_state.messagesSent.fetch_add(1, std::memory_order_relaxed);
    }
}

void WorkerLoop()
{
    std::vector<uint8_t> buffer(sizeof(FrameHeader) + kMaxPayloadSize);

    while (g_state.running.load(std::memory_order_relaxed)) {
        PumpOutbound();

        MsgType type = MsgType::None;
        uint8_t flags = 0;
        uint32_t size = 0;

        while (g_state.transport != nullptr &&
               g_state.transport->Receive(type, flags, buffer.data(), static_cast<uint32_t>(buffer.size()), size)) {

            g_state.messagesReceived.fetch_add(1, std::memory_order_relaxed);

            if (!g_state.peerReady.load(std::memory_order_relaxed)) {
                g_state.peerReady.store(true, std::memory_order_relaxed);
                QueueLog(GwycLogLevel_Info, "вторая сторона на связи (Minecraft)");
                QueuePeerState(true);
            }

            switch (type) {
                // Служебное обслуживаем прямо здесь: состояние игры не нужно.
                case MsgType::Ping: {
                    MsgPingPong ping{};
                    if (size == sizeof(ping)) {
                        std::memcpy(&ping, buffer.data(), sizeof(ping));
                        MsgPingPong pong = ping;
                        pong.sendTimeMs = NowMs();
                        PushOutbound(MsgType::Pong, kFlagNone, &pong, static_cast<uint32_t>(sizeof(pong)));
                    }
                    break;
                }

                case MsgType::Pong: {
                    MsgPingPong pong{};
                    if (size == sizeof(pong)) {
                        std::memcpy(&pong, buffer.data(), sizeof(pong));
                        const uint64_t now = NowMs();
                        g_state.rttMs.store(now >= pong.sendTimeMs ? now - pong.sendTimeMs : 0, std::memory_order_relaxed);
                    }
                    break;
                }

                case MsgType::Hello:
                case MsgType::HelloAck: {
                    MsgHello hello{};
                    if (size == sizeof(hello)) {
                        std::memcpy(&hello, buffer.data(), sizeof(hello));
                        if (hello.layoutVersion != kLayoutVersion) {
                            QueueLog(GwycLogLevel_Error, "Minecraft собран под другую раскладку протокола — обмен остановлен");
                        }
                    }
                    break;
                }

                default: {
                    IncomingMessage incoming;
                    incoming.type = type;
                    incoming.payload.assign(buffer.begin(), buffer.begin() + size);

                    std::lock_guard<std::mutex> lock(g_state.mutex);
                    if (g_state.inbound.size() >= kMaxInboundQueue) g_state.inbound.pop_front();
                    g_state.inbound.push_back(std::move(incoming));
                    break;
                }
            }
        }

        // Пинг раз в секунду: HUD игры показывает задержку до Minecraft.
        const uint64_t now = NowMs();
        if (now - g_state.lastPingMs.load(std::memory_order_relaxed) > 1000) {
            g_state.lastPingMs.store(now, std::memory_order_relaxed);

            MsgPingPong ping{};
            ping.seq = g_state.messagesSent.load(std::memory_order_relaxed);
            ping.sendTimeMs = now;
            PushOutbound(MsgType::Ping, kFlagNone, &ping, static_cast<uint32_t>(sizeof(ping)));
        }

        const uint32_t interval = (g_state.config.pollIntervalMs == 0) ? 2u : g_state.config.pollIntervalMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }

    // Перед выходом отдаём всё, что осталось в очереди (канал ещё наш).
    PumpOutbound();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Диспетчер входящих (главный поток)
// ─────────────────────────────────────────────────────────────────────────────

void DispatchIncoming(const IncomingMessage& message)
{
    switch (message.type) {
        case MsgType::VoxelEdit: {
            MsgVoxelEdit edit{};
            if (message.payload.size() == sizeof(edit) && g_state.voxels) {
                std::memcpy(&edit, message.payload.data(), sizeof(edit));
                g_state.voxels->SubmitRemoteEdit(edit);
            }
            break;
        }

        case MsgType::VoxelBatch: {
            MsgVoxelBatch header{};
            if (message.payload.size() >= sizeof(header) && g_state.voxels) {
                std::memcpy(&header, message.payload.data(), sizeof(header));

                const size_t available = (message.payload.size() - sizeof(header)) / sizeof(MsgVoxelEdit);
                const size_t count = (static_cast<size_t>(header.count) < available) ? header.count : available;

                const auto* edits = reinterpret_cast<const MsgVoxelEdit*>(message.payload.data() + sizeof(header));
                g_state.voxels->SubmitRemoteBatch(edits, count);
            }
            break;
        }

        case MsgType::VoxelSnapshot: {
            MsgVoxelSnapshot header{};
            if (message.payload.size() > sizeof(header) && g_state.voxels) {
                std::memcpy(&header, message.payload.data(), sizeof(header));
                const size_t applied = g_state.voxels->SubmitRemoteSnapshot(header,
                                                                            message.payload.data() + sizeof(header),
                                                                            message.payload.size() - sizeof(header));
                if (applied == 0) QueueLog(GwycLogLevel_Warn, "снимок региона не применён (битый RLE или запрос)");
            }
            break;
        }

        case MsgType::GrantReward: {
            MsgGrantReward reward{};
            if (message.payload.size() == sizeof(reward) && g_state.callbacks.onRewardRequest != nullptr) {
                std::memcpy(&reward, message.payload.data(), sizeof(reward));

                GwycReward abi{};
                abi.structSize = sizeof(abi);
                abi.playerId = reward.playerId;
                abi.blockKind = static_cast<uint8_t>(reward.blockKind);
                abi.reason = reward.reason;
                abi.count = reward.count;

                g_state.callbacks.onRewardRequest(&abi);
            }
            break;
        }

        case MsgType::ConsoleToGame: {
            MsgChatLine line{};
            if (message.payload.size() == sizeof(line) && g_state.callbacks.onChatFromMinecraft != nullptr) {
                std::memcpy(&line, message.payload.data(), sizeof(line));

                GwycChatLine abi{};
                abi.structSize = sizeof(abi);
                abi.channel = line.channel;
                std::memcpy(abi.author, line.author, sizeof(abi.author));
                std::memcpy(abi.text, line.text, sizeof(abi.text));

                g_state.callbacks.onChatFromMinecraft(&abi);
            }
            break;
        }

        case MsgType::HelloAck: {
            if (g_state.callbacks.onPeerState != nullptr) g_state.callbacks.onPeerState(1);
            break;
        }

        case MsgType::Shutdown: {
            if (g_state.callbacks.onPeerState != nullptr) g_state.callbacks.onPeerState(0);
            break;
        }

        case MsgType::Log: {
            MsgLog log{};
            if (message.payload.size() == sizeof(log) && g_state.callbacks.onLog != nullptr) {
                std::memcpy(&log, message.payload.data(), sizeof(log));
                g_state.callbacks.onLog(log.level, log.text);
            }
            break;
        }

        default:
            break;
    }
}

/// Отправить всё, что накопилось, вне рабочего потока. Законно только когда
/// рабочий поток остановлен (при закрытии): иначе получается два писателя в кольцо,
/// а это ломает контракт SPSC.
void DrainOutboundDirect()
{
    std::deque<OutgoingMessage> batch;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        batch.swap(g_state.outbound);
    }

    for (const OutgoingMessage& message : batch) {
        if (g_state.transport == nullptr) break;
        // Send помечен nodiscard: результат обязан быть использован (иначе MSVC
        // ругается C4834, а сборка идёт с /WX). Считаем доставленное так же,
        // как рабочий поток.
        const bool sent = g_state.transport->Send(message.type, message.flags,
                                                  message.payload.empty() ? nullptr : message.payload.data(),
                                                  static_cast<uint32_t>(message.payload.size()));
        if (sent) g_state.messagesSent.fetch_add(1, std::memory_order_relaxed);
    }
}

int32_t EnqueueForSend(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize)
{
    if (!g_state.initialized) return GwycStatus_NotInitialized;
    if (payloadSize > kMaxPayloadSize) return GwycStatus_InvalidArgument;

    PushOutbound(type, flags, payload, payloadSize);
    return GwycStatus_Ok;
}

} // namespace

// =============================================================================
//  Экспорт
// =============================================================================

uint32_t GWYC_ABI_CALL GwycBridge_GetAbiVersion(void)
{
    return GWYC_ABI_VERSION;
}

const char* GWYC_ABI_CALL GwycBridge_GetBuildTag(void)
{
    return "gwyc-bridge 1.0.0 (layout 3, proto 1)";
}

int32_t GWYC_ABI_CALL GwycBridge_Initialize(const GwycBridgeConfig* config, const GwycGameCallbacks* callbacks)
{
    try {
        if (config == nullptr) return GwycStatus_InvalidArgument;
        if (config->structSize < sizeof(GwycBridgeConfig)) return GwycStatus_InvalidArgument;
        if (config->abiVersion != GWYC_ABI_VERSION) return GwycStatus_AbiMismatch;

        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            if (g_state.initialized) return GwycStatus_AlreadyInitialized;
        }

        g_state.config = bridge::BridgeConfig{};
        g_state.config.channelPath = (config->channelPath != nullptr) ? config->channelPath : "gwyc_bridge.channel";
        g_state.config.role = (config->role == GwycRole_Minecraft) ? ChannelRole::Minecraft : ChannelRole::Game;
        g_state.config.ringCapacity = (config->ringCapacity >= 4096) ? config->ringCapacity : 128u * 1024u;
        g_state.config.pollIntervalMs = (config->pollIntervalMs != 0) ? config->pollIntervalMs : 2;
        g_state.config.peerWaitMs = config->peerWaitMs;
        g_state.config.transport = (config->transport == GwycTransport_NamedPipe)
                                       ? bridge::TransportKind::NamedPipe
                                       : bridge::TransportKind::SharedMemory;

        g_state.callbacks = GwycGameCallbacks{};
        g_state.callbacks.structSize = sizeof(GwycGameCallbacks);
        if (callbacks != nullptr && callbacks->structSize >= sizeof(GwycGameCallbacks)) {
            g_state.callbacks = *callbacks;
        }

        g_state.transport = bridge::MakeTransport(g_state.config);
        if (g_state.transport == nullptr) return GwycStatus_InternalError;

        const bridge::BridgeStatus opened = g_state.transport->Open(g_state.config);
        if (opened != bridge::BridgeStatus::Ok) {
            g_state.transport.reset();
            return GwycStatus_ChannelOpenFailed;
        }

        world::VoxelCallbacks voxelCallbacks;
        voxelCallbacks.apply = &VoxelApplyToGame;
        voxelCallbacks.trace = nullptr;

        const size_t maxCubes = (config->maxCubes != 0) ? config->maxCubes : 8192;
        g_state.voxels = std::make_unique<world::VoxelWorldSync>(voxelCallbacks, maxCubes);

        g_state.startedAtMs.store(NowMs(), std::memory_order_relaxed);
        g_state.lastPingMs.store(0, std::memory_order_relaxed);
        g_state.peerReady.store(false, std::memory_order_relaxed);
        g_state.initialized = true;

        if (g_state.callbacks.onLog != nullptr) {
            // Первое сообщение отдаём напрямую: Tick ещё не вызывался, очередь пуста,
            // а разработчику важно увидеть старт моста сразу.
            const std::string text = std::string("мост поднят: ") + g_state.transport->Name() +
                                     ", канал " + g_state.config.channelPath;
            g_state.callbacks.onLog(GwycLogLevel_Info, text.c_str());
        }

        g_state.running.store(true, std::memory_order_relaxed);
        g_state.worker = std::thread(WorkerLoop);

        return GwycStatus_Ok;
    }
    catch (const std::exception&) {
        return GwycStatus_InternalError;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_Shutdown(void)
{
    try {
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            if (!g_state.initialized) return GwycStatus_NotInitialized;
        }

        if (g_state.hooksInstalled) {
            // При выходе статус снятия хука уже ни на что не влияет: логируем факт.
            const hooks::Status removed = hooks::RemoveCasinoHooks();
            (void)removed;
            g_state.hooksInstalled = false;
        }

        const hooks::Status hooksDown = hooks::Shutdown();
        (void)hooksDown;

        // Прощание и остановка: сначала останавливаем рабочего (иначе получится
        // второй писатель в кольцо), затем сами отдаём остаток очереди.
        MsgLog goodbye{};
        goodbye.level = 0;
        WriteFixed(goodbye.text, sizeof(goodbye.text), "мост закрывается");
        PushOutbound(MsgType::Shutdown, kFlagReliable, &goodbye, static_cast<uint32_t>(sizeof(goodbye)));

        g_state.running.store(false, std::memory_order_relaxed);
        if (g_state.worker.joinable()) g_state.worker.join();

        DrainOutboundDirect();

        if (g_state.transport != nullptr) g_state.transport->Close();

        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.inbound.clear();
            g_state.outbound.clear();
            g_state.voxels.reset();
            g_state.transport.reset();
            g_state.initialized = false;
            g_state.peerReady.store(false, std::memory_order_relaxed);
        }

        return GwycStatus_Ok;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_Tick(void)
{
    try {
        if (!g_state.initialized) return GwycStatus_NotInitialized;

        // 1. Входящие → колбэки игры. Здесь и только здесь мы входим в игровые системы.
        for (;;) {
            IncomingMessage message;
            {
                std::lock_guard<std::mutex> lock(g_state.mutex);
                if (g_state.inbound.empty()) break;
                message = std::move(g_state.inbound.front());
                g_state.inbound.pop_front();
            }

            DispatchIncoming(message);
        }

        // 2. Накопленные правки мира материализуются в мир игры.
        if (g_state.voxels) g_state.voxels->Flush();

        return GwycStatus_Ok;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

// ── публикация событий (игровой поток) ──────────────────────────────────────

int32_t GWYC_ABI_CALL GwycBridge_PublishVoxelEdit(const GwycVoxelEdit* edit)
{
    try {
        if (edit == nullptr || edit->structSize < sizeof(GwycVoxelEdit)) return GwycStatus_InvalidArgument;
        if (!IsValidBlockId(edit->blockKind)) return GwycStatus_InvalidArgument;

        MsgVoxelEdit message{};
        message.x = edit->x;
        message.y = edit->y;
        message.z = edit->z;
        message.blockId = edit->blockKind;
        message.flags = edit->fromLocalPlayer;
        message.sourceId = edit->sourceId;

        return EnqueueForSend(MsgType::VoxelEdit, kFlagReliable, &message, static_cast<uint32_t>(sizeof(message)));
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_PublishBetPlaced(const GwycBetPlaced* bet)
{
    try {
        if (bet == nullptr || bet->structSize < sizeof(GwycBetPlaced)) return GwycStatus_InvalidArgument;

        MsgBetPlaced message{};
        message.playerId = bet->playerId;
        message.stake = bet->stake;
        message.game = bet->game;
        std::memcpy(message.target, bet->target, sizeof(message.target));
        std::memcpy(message.name, bet->playerName, sizeof(message.name));

        return EnqueueForSend(MsgType::BetPlaced, kFlagReliable, &message, static_cast<uint32_t>(sizeof(message)));
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_PublishBetResolved(const GwycBetResolved* result)
{
    try {
        if (result == nullptr || result->structSize < sizeof(GwycBetResolved)) return GwycStatus_InvalidArgument;

        MsgBetResolved message{};
        message.playerId = result->playerId;
        message.stake = result->stake;
        message.payout = result->payout;
        message.multiplier = result->multiplier;
        message.won = result->won;
        message.game = result->game;
        message.blockType = result->blockKind;
        std::memcpy(message.target, result->target, sizeof(message.target));
        std::memcpy(message.name, result->playerName, sizeof(message.name));

        const int32_t queued = EnqueueForSend(MsgType::BetResolved, kFlagReliable, &message, static_cast<uint32_t>(sizeof(message)));
        if (queued != GwycStatus_Ok) return queued;

        // Выигрыш сразу превращается в награду для Minecraft: в креативе появляются
        // те самые блоки, которыми играли на столе.
        if (result->won != 0 && result->payout > 0) {
            MsgGrantReward reward{};
            reward.playerId = result->playerId;
            reward.blockKind = result->blockKind;
            reward.count = static_cast<int32_t>(result->payout > 100000 ? 100000 : result->payout);
            reward.reason = 0;

            return EnqueueForSend(MsgType::GrantReward, kFlagReliable, &reward, static_cast<uint32_t>(sizeof(reward)));
        }

        return GwycStatus_Ok;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_PublishTableEvent(const GwycTableEvent* table)
{
    try {
        if (table == nullptr || table->structSize < sizeof(GwycTableEvent)) return GwycStatus_InvalidArgument;

        MsgTableEvent message{};
        message.tableId = table->tableId;
        message.x = table->x;
        message.y = table->y;
        message.z = table->z;
        message.minBet = table->minBet;
        message.maxBet = table->maxBet;
        message.seats = table->seats;
        message.state = table->state;
        std::memcpy(message.owner, table->ownerName, sizeof(message.owner));

        return EnqueueForSend(MsgType::TableEvent, kFlagReliable, &message, static_cast<uint32_t>(sizeof(message)));
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_PublishPlayerState(const GwycPlayerState* state)
{
    try {
        if (state == nullptr || state->structSize < sizeof(GwycPlayerState)) return GwycStatus_InvalidArgument;

        MsgPlayerState message{};
        message.playerId = state->playerId;
        message.chips = state->chips;
        message.netWorth = state->netWorth;
        message.lobbyState = state->lobbyState;
        std::memcpy(message.name, state->playerName, sizeof(message.name));

        return EnqueueForSend(MsgType::PlayerState, kFlagNone, &message, static_cast<uint32_t>(sizeof(message)));
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_PublishChat(const GwycChatLine* line)
{
    try {
        if (line == nullptr || line->structSize < sizeof(GwycChatLine)) return GwycStatus_InvalidArgument;

        MsgChatLine message{};
        message.channel = line->channel;
        std::memcpy(message.author, line->author, sizeof(message.author));
        std::memcpy(message.text, line->text, sizeof(message.text));

        return EnqueueForSend(MsgType::ConsoleToMc, kFlagReliable, &message, static_cast<uint32_t>(sizeof(message)));
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_RequestRegionSnapshot(int32_t x, int32_t y, int32_t z, uint16_t sizeX, uint16_t sizeY, uint16_t sizeZ)
{
    try {
        if (sizeX == 0 || sizeY == 0 || sizeZ == 0) return GwycStatus_InvalidArgument;

        MsgVoxelSnapshot request{};
        request.originX = x;
        request.originY = y;
        request.originZ = z;
        request.sizeX = sizeX;
        request.sizeY = sizeY;
        request.sizeZ = sizeZ;
        request.encodedSize = 0;   // 0 = это запрос, а не данные

        return EnqueueForSend(MsgType::VoxelSnapshot, kFlagReliable, &request, static_cast<uint32_t>(sizeof(request)));
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_GetStats(GwycBridgeStats* outStats)
{
    try {
        if (outStats == nullptr || outStats->structSize < sizeof(GwycBridgeStats)) return GwycStatus_InvalidArgument;
        if (!g_state.initialized) return GwycStatus_NotInitialized;

        outStats->transport = static_cast<uint32_t>(g_state.config.transport);
        outStats->peerReady = (g_state.transport != nullptr && g_state.transport->IsPeerReady()) ? 1u : 0u;
        outStats->overruns = (g_state.transport != nullptr) ? g_state.transport->Overruns() : 0u;
        outStats->crcFailures = (g_state.transport != nullptr) ? g_state.transport->CrcFailures() : 0u;
        outStats->resyncs = (g_state.transport != nullptr) ? g_state.transport->Resyncs() : 0u;
        outStats->messagesSent = g_state.messagesSent.load(std::memory_order_relaxed);
        outStats->messagesReceived = g_state.messagesReceived.load(std::memory_order_relaxed);
        outStats->rttMs = g_state.rttMs.load(std::memory_order_relaxed);
        outStats->uptimeMs = NowMs() - g_state.startedAtMs.load(std::memory_order_relaxed);

        if (g_state.voxels) {
            const world::VoxelSyncStats stats = g_state.voxels->GetStats();
            outStats->cubesSpawned = stats.cubesSpawned;
            outStats->cubesRemoved = stats.cubesRemoved;
            outStats->editsDropped = stats.overflowDropped;
        }

        return GwycStatus_Ok;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_InstallCasinoHooks(void* betSubmitTarget, void* tableCreateTarget)
{
    try {
        if (!g_state.initialized) return GwycStatus_NotInitialized;
        if (betSubmitTarget == nullptr && tableCreateTarget == nullptr) return GwycStatus_InvalidArgument;

        const hooks::Status hooksUp = hooks::Initialize();
        if (hooksUp != hooks::Status::Ok && hooksUp != hooks::Status::AlreadyInitialized) return GwycStatus_InternalError;

        hooks::SetBetObserver(&OnBetObserved);
        hooks::SetTableObserver(&OnTableObserved);

        // Детуры не передаём: движок использует свои, которые зовут наблюдателей
        // и всё равно вызывают оригинал — поведение игры не меняется.
        const hooks::Status status = hooks::InstallCasinoHooks(betSubmitTarget, nullptr, nullptr,
                                                             tableCreateTarget, nullptr, nullptr);
        if (status != hooks::Status::Ok && status != hooks::Status::AlreadyInitialized) {
            return GwycStatus_InternalError;
        }

        g_state.hooksInstalled = true;
        QueueLog(GwycLogLevel_Info, hooks::IsInterceptionAvailable()
                                        ? "хуки казино установлены (перехват активен)"
                                        : "хуки казино зарегистрированы в теневом режиме (не Windows)");
        return GwycStatus_Ok;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_RemoveCasinoHooks(void)
{
    try {
        const hooks::Status status = hooks::RemoveCasinoHooks();
        g_state.hooksInstalled = false;

        const bool ok = (status == hooks::Status::Ok) || (status == hooks::Status::NotFound) ||
                        (status == hooks::Status::NotInitialized);
        return ok ? GwycStatus_Ok : GwycStatus_InternalError;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

int32_t GWYC_ABI_CALL GwycBridge_GetHookCounters(uint64_t* bets, uint64_t* tables)
{
    try {
        const hooks::CasinoHookCounters counters = hooks::GetCasinoCounters();
        if (bets != nullptr) *bets = counters.betsObserved;
        if (tables != nullptr) *tables = counters.tablesObserved;
        return GwycStatus_Ok;
    }
    catch (...) {
        return GwycStatus_InternalError;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Windows: точка входа DLL. DllMain обязан быть скучным: только сохранение
//  handle модуля, никаких потоков, загрузок и блокировок (loader lock!).
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32)
#   include <windows.h>

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }

    return TRUE;
}
#endif
