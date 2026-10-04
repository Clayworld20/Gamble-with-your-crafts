// ============================================================================
//  IpcBridge.cpp — Windows-часть моста: регион разделяемой памяти, события,
//  проверка живости партнёра.
//
//  Регион создаётся один раз (обычно его создаёт сторона игры, когда движок
//  стартует первым). Вторая сторона открывает существующий объект и сверяет
//  ABI: если версии разъехались, подключение отклоняется с понятной ошибкой —
//  это лучше, чем молча разобрать чужие байты как свои структуры.
//
//  Все объекты ядра живут в RAII-обёртках (UniqueHandle/MappedFile из Common.h),
//  поэтому аварийный выход из функции не оставляет открытых хендлов.
// ============================================================================
#include "gwyfbridge/IpcBridge.h"

#include <cstring>

namespace gwyf::ipc {

Bridge::~Bridge() {
    Close();
}

bool Bridge::Open(Role role, const std::wstring& regionName, const std::string& targetTag,
                  bool createIfMissing, std::string* error) {
    auto fail = [error](const char* text) {
        if (error != nullptr) *error = text;
        GWYF_ERROR("%s", text);
        return false;
    };

    role_ = role;

    bool created = false;
    if (createIfMissing) {
        if (!region_.Open(regionName, kRegionSize, true)) {
            return fail("не удалось создать регион разделяемой памяти");
        }
        created = region_.Created();
    } else {
        if (!region_.Open(regionName, kRegionSize, false)) {
            return fail("регион разделяемой памяти не найден — партнёр ещё не запущен");
        }
    }

    header_ = static_cast<SharedHeader*>(region_.Data());
    if (header_ == nullptr) return fail("отображение региона вернуло nullptr");

    if (created) {
        // Инициализация региона: кольца, версии, ёмкости.
        std::memset(header_, 0, sizeof(SharedHeader));
        header_->magic = kMagic;
        header_->abiVersion = kAbiVersion;
        header_->headerSize = static_cast<u32>(sizeof(SharedHeader));
        header_->regionSize = static_cast<u32>(kRegionSize);
        header_->bridgeVersion = kAbiVersion;

        header_->gameToMc.capacity = kRecordRingCapacity;
        header_->mcToGame.capacity = kRecordRingCapacity;
        header_->gameToMcBulk.capacity = kBulkRingCapacity;
        header_->mcToGameBulk.capacity = kBulkRingCapacity;

        std::strncpy(header_->targetTag, targetTag.c_str(), sizeof(header_->targetTag) - 1);
    } else {
        // Гонка одновременного старта: даём инициатору мгновение на инициализацию.
        for (int attempt = 0; attempt < 50 && header_->magic != kMagic; ++attempt) {
            Sleep(2);
        }

        std::string problem;
        if (!ValidateHeader(*header_, &problem)) {
            if (error != nullptr) *error = problem;
            GWYF_ERROR("Регион несовместим: %s", problem.c_str());
            return false;
        }
    }

    // События: по одному на направление + отдельное событие остановки.
    eventGameToMc_.Reset(CreateEventW(nullptr, FALSE, FALSE, kEventGameToMc));
    eventMcToGame_.Reset(CreateEventW(nullptr, FALSE, FALSE, kEventMcToGame));
    eventStop_.Reset(CreateEventW(nullptr, TRUE, FALSE, kEventStop));
    if (!eventGameToMc_.Valid() || !eventMcToGame_.Valid() || !eventStop_.Valid()) {
        return fail("не удалось создать объекты синхронизации");
    }

    const u32 pid = GetCurrentProcessId();
    if (role == Role::Game) {
        header_->gamePid = pid;
        header_->gameHeartbeatMs = NowUnixMs();
        if (header_->gameStartedMs == 0) header_->gameStartedMs = NowUnixMs();

        // Игра продюсирует канал «игра → Minecraft», значит будит потребителя
        // на стороне Minecraft (eventGameToMc).
        out_ = RecordProducer(&header_->gameToMc, &header_->gameToMcBulk, eventGameToMc_.Get(),
                              Producer::Game, &header_->gameHeartbeatMs);
        in_ = RecordConsumer(&header_->mcToGame, &header_->mcToGameBulk, eventMcToGame_.Get());
    } else {
        header_->mcPid = pid;
        header_->mcHeartbeatMs = NowUnixMs();
        if (header_->mcStartedMs == 0) header_->mcStartedMs = NowUnixMs();

        out_ = RecordProducer(&header_->mcToGame, &header_->mcToGameBulk, eventMcToGame_.Get(),
                              Producer::Minecraft, &header_->mcHeartbeatMs);
        in_ = RecordConsumer(&header_->gameToMc, &header_->gameToMcBulk, eventGameToMc_.Get());
    }

    GWYF_INFO("IPC открыт: роль %s, регион %s, targetTag=%s, ABI=%u",
              role == Role::Game ? "игра" : "minecraft", created ? "создан нами" : "открыт существующий",
              targetTag.c_str(), header_->abiVersion);
    return true;
}

void Bridge::Close() {
    if (header_ != nullptr && out_.Valid()) {
        Payload payload{};
        std::memcpy(payload.raw, "bye", 3);
        out_.Push(RecordType::Bye, payload);
    }

    eventGameToMc_.Reset();
    eventMcToGame_.Reset();
    eventStop_.Reset();
    header_ = nullptr;
    out_ = RecordProducer{};
    in_ = RecordConsumer{};
}

bool Bridge::PeerAlive() const {
    if (header_ == nullptr) return false;

    const u64 now = NowUnixMs();
    const u64 peerHeartbeat = (role_ == Role::Game) ? header_->mcHeartbeatMs : header_->gameHeartbeatMs;
    if (peerHeartbeat == 0) return false;

    return (now - peerHeartbeat) < kPeerTimeoutMs;
}

bool Bridge::PeerProcessExists() const {
    if (header_ == nullptr) return false;

    const u32 peerPid = (role_ == Role::Game) ? header_->mcPid : header_->gamePid;
    if (peerPid == 0) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peerPid);
    if (process == nullptr) {
        // Отказ в доступе означает, что процесс существует (просто мы не можем
        // его расспросить) — в отличие от «нет такого процесса».
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }

    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

bool Bridge::StopRequested() const {
    return eventStop_.Valid() && WaitForSingleObject(eventStop_.Get(), 0) == WAIT_OBJECT_0;
}

std::string Bridge::Describe() const {
    if (header_ == nullptr) return "IPC закрыт";

    char buffer[512]{};
    const u64 g2m = header_->gameToMc.head - header_->gameToMc.tail;
    const u64 m2g = header_->mcToGame.head - header_->mcToGame.tail;
    const bool peerAlive = PeerAlive();

    std::snprintf(buffer, sizeof(buffer),
                  "ABI=%u pid(игра)=%u pid(minecraft)=%u в очереди G→MC=%llu M→G=%llu "
                  "потери (G→MC=%u M→G=%u) пульс партнёра: %s",
                  header_->abiVersion, header_->gamePid, header_->mcPid,
                  static_cast<unsigned long long>(g2m), static_cast<unsigned long long>(m2g),
                  header_->gameToMc.dropped, header_->mcToGame.dropped,
                  peerAlive ? "есть" : "нет");
    return buffer;
}

}  // namespace gwyf::ipc
