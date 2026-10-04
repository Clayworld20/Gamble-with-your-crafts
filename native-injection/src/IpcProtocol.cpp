// ============================================================================
//  IpcProtocol.cpp — кольца SPSC, публикация записей и служебные функции ABI.
//
//  МОДЕЛЬ ПАМЯТИ
//  -------------
//  Целевая платформа — x86-64 (Windows). На ней:
//    * выровненные 8-байтовые чтения/записи атомарны на уровне железа;
//    * нет переупорядочивания store-store и load-load (TSO).
//  Единственная реальная опасность — перестановка компилятором, поэтому все
//  точки публикации огорожены барьером (AtomicStoreRelease / AtomicLoadAcquire
//  в Base.h). Замок-инструкции (LOCK CMPXCHG) в горячем пути не нужны:
//  инвариант SPSC «head пишет только продюсер, tail — только потребитель»
//  делает мьютекс избыточным.
//
//  Этот файл сознательно не включает windows.h: он собирается и прогоняется
//  самотестом на любой платформе (см. tools/selftest), а специфика Windows
//  осталась в IpcBridge.cpp.
// ============================================================================
#include "gwyfbridge/IpcProtocol.h"

#include <cstring>
#include <string>

namespace gwyf::ipc {

namespace {

struct BulkFrame {
    u32 length = 0;  // 0 = кадр-заполнитель: перенос на начало буфера
    u32 kind = 0;
};

/// Записать кадр-заполнитель до конца буфера и вернуть новую позицию записи.
u64 WritePaddingFrame(BulkRing* bulk, u64 head, u32 offset) {
    const u32 tailSpace = bulk->capacity - offset;
    BulkFrame pad{};
    pad.length = 0;
    pad.kind = 0;

    std::memcpy(bulk->data + offset, &pad, sizeof(pad));
    AtomicStoreRelease(&bulk->head, head + tailSpace);
    bulk->pushed++;
    return head + tailSpace;
}

}  // namespace

// ── Продюсер ────────────────────────────────────────────────────────────────

bool RecordProducer::Push(RecordType type, const Payload& payload, u64 sourceId, u64 aux) {
    if (ring_ == nullptr) return false;

    const u32 mask = ring_->capacity - 1;
    const u64 head = ring_->head;                            // пишем его только мы
    const u64 tail = AtomicLoadAcquire(&ring_->tail);

    if (head - tail >= ring_->capacity) {
        // Переполнение: запись теряется, но игровой поток НИКОГДА не блокируется.
        ring_->dropped++;
        dropped_++;
        return false;
    }

    Record& slot = ring_->records[head & mask];
    slot.type = static_cast<u32>(type);
    slot.producer = static_cast<u32>(who_);
    slot.sequence = ++sequence_;
    slot.timestampMs = NowUnixMs();
    slot.sourceId = sourceId;
    slot.aux = aux;
    slot.payload = payload;

    // Публикация: содержимое записи должно стать видимым до нового head.
    AtomicStoreRelease(&ring_->head, head + 1);
    ring_->pushed++;

    // Авто-сброс события: один сигнал = одна готовая запись.
    SignalWake(wakeEvent_);
    return true;
}

bool RecordProducer::PushSimple(RecordType type) {
    Payload empty{};
    return Push(type, empty);
}

bool RecordProducer::PushBulk(const void* data, u32 size, u32 kind) {
    if (bulk_ == nullptr || data == nullptr) return false;
    if (size == 0 || size > kBulkMaxFrame) {
        GWYF_WARN("PushBulk: некорректный размер кадра %u (максимум %u)", size, kBulkMaxFrame);
        return false;
    }

    const u32 mask = bulk_->capacity - 1;
    u64 head = bulk_->head;
    const u64 tail = AtomicLoadAcquire(&bulk_->tail);
    u32 offset = static_cast<u32>(head) & mask;
    const u32 used = static_cast<u32>(head - tail);

    // Хвост буфера: если заголовок кадра туда не влезает — пишем заполнитель.
    if (bulk_->capacity - offset < kBulkFrameHeader) {
        if (used + (bulk_->capacity - offset) > bulk_->capacity) {
            bulk_->dropped++;
            dropped_++;
            return false;
        }
        head = WritePaddingFrame(bulk_, head, offset);
        offset = static_cast<u32>(head) & mask;
    }

    const u32 needed = kBulkFrameHeader + size;
    if (bulk_->capacity - offset < needed) {
        // Кадр целиком не влезает в хвост — заполнитель до конца и пишем с начала.
        if (used + (bulk_->capacity - offset) > bulk_->capacity) {
            bulk_->dropped++;
            dropped_++;
            return false;
        }
        head = WritePaddingFrame(bulk_, head, offset);
        offset = static_cast<u32>(head) & mask;
    }

    if (used + needed > bulk_->capacity) {
        bulk_->dropped++;
        dropped_++;
        return false;
    }

    BulkFrame frame{};
    frame.length = size;
    frame.kind = kind;
    std::memcpy(bulk_->data + offset, &frame, sizeof(frame));
    std::memcpy(bulk_->data + offset + kBulkFrameHeader, data, size);

    AtomicStoreRelease(&bulk_->head, head + needed);
    bulk_->pushed++;

    SignalWake(wakeEvent_);
    return true;
}

void RecordProducer::Heartbeat() {
    if (heartbeat_ != nullptr) {
        *heartbeat_ = NowUnixMs();
    }
}

// ── Потребитель ─────────────────────────────────────────────────────────────

bool RecordConsumer::TryPop(Record& out) {
    if (ring_ == nullptr) return false;

    const u32 mask = ring_->capacity - 1;
    const u64 tail = ring_->tail;  // пишем его только мы
    const u64 head = AtomicLoadAcquire(&ring_->head);

    if (tail == head) return false;

    out = ring_->records[tail & mask];
    AtomicStoreRelease(&ring_->tail, tail + 1);
    ring_->popped++;
    return true;
}

u32 RecordConsumer::Drain(const std::function<void(const Record&)>& handler, u32 maxRecords) {
    u32 processed = 0;
    Record record{};
    while (processed < maxRecords && TryPop(record)) {
        ++processed;
        if (handler) handler(record);
    }
    return processed;
}

bool RecordConsumer::Wait(u32 timeoutMs) {
    return WaitOnHandle(waitEvent_, timeoutMs);
}

bool RecordConsumer::TryReadBulk(std::string& out, u32& frameLength) {
    if (bulk_ == nullptr) return false;

    for (u32 attempt = 0; attempt < 8; ++attempt) {
        const u32 mask = bulk_->capacity - 1;
        const u64 tail = bulk_->tail;
        const u64 head = AtomicLoadAcquire(&bulk_->head);

        if (head == tail) return false;

        const u32 offset = static_cast<u32>(tail) & mask;

        // Остаток буфера без полного заголовка: пропускаем хвост.
        if (bulk_->capacity - offset < kBulkFrameHeader) {
            const u32 skip = bulk_->capacity - offset;
            AtomicStoreRelease(&bulk_->tail, tail + skip);
            bulk_->popped++;
            continue;
        }

        BulkFrame frame{};
        std::memcpy(&frame, bulk_->data + offset, sizeof(frame));

        if (frame.length == 0) {
            // Заполнитель — переносим чтение на начало буфера.
            const u32 skip = bulk_->capacity - offset;
            AtomicStoreRelease(&bulk_->tail, tail + skip);
            bulk_->popped++;
            continue;
        }

        const u32 needed = kBulkFrameHeader + frame.length;
        if (bulk_->capacity - offset < needed || needed > kBulkRingCapacity) {
            GWYF_WARN("Bulk-кольцо: кадр длиной %u не влезает — буфер пересинхронизирован", frame.length);
            AtomicStoreRelease(&bulk_->tail, head);
            bulk_->dropped++;
            return false;
        }

        out.assign(reinterpret_cast<const char*>(bulk_->data + offset + kBulkFrameHeader), frame.length);
        frameLength = frame.length;

        AtomicStoreRelease(&bulk_->tail, tail + needed);
        bulk_->popped++;
        return true;
    }

    return false;
}

u64 RecordConsumer::Head() const {
    if (ring_ == nullptr) return 0;
    return AtomicLoadAcquire(&ring_->head);
}

u64 RecordConsumer::Tail() const {
    return ring_ == nullptr ? 0 : ring_->tail;
}

u32 RecordConsumer::Dropped() const {
    return ring_ == nullptr ? 0 : ring_->dropped;
}

u64 RecordConsumer::PendingRecords() const {
    if (ring_ == nullptr) return 0;
    return AtomicLoadAcquire(&ring_->head) - ring_->tail;
}

// ── Служебные функции ABI ───────────────────────────────────────────────────

const char* ToString(RecordType type) {
    switch (type) {
        case RecordType::None: return "None";
        case RecordType::Hello: return "Hello";
        case RecordType::Bye: return "Bye";
        case RecordType::Heartbeat: return "Heartbeat";
        case RecordType::BetPlaced: return "BetPlaced";
        case RecordType::BetResolved: return "BetResolved";
        case RecordType::TableSpawned: return "TableSpawned";
        case RecordType::TableRemoved: return "TableRemoved";
        case RecordType::ChipState: return "ChipState";
        case RecordType::LobbyState: return "LobbyState";
        case RecordType::VoxelEdit: return "VoxelEdit";
        case RecordType::BlockGrant: return "BlockGrant";
        case RecordType::Command: return "Command";
        case RecordType::CommandAck: return "CommandAck";
        case RecordType::LogLine: return "LogLine";
        case RecordType::BulkSync: return "BulkSync";
        case RecordType::CubeSpawned: return "CubeSpawned";
        default: return "Unknown";
    }
}

const char* ToString(CommandCode code) {
    switch (code) {
        case CommandCode::None: return "None";
        case CommandCode::DumpClass: return "DumpClass";
        case CommandCode::ScanPattern: return "ScanPattern";
        case CommandCode::Status: return "Status";
        case CommandCode::SpawnProbe: return "SpawnProbe";
        case CommandCode::ReloadProfile: return "ReloadProfile";
        case CommandCode::FlushWorld: return "FlushWorld";
        default: return "Unknown";
    }
}

Record MakeRecord(RecordType type, Producer producer, u64 sequence, u64 sourceId, u64 aux) {
    Record record{};
    record.type = static_cast<u32>(type);
    record.producer = static_cast<u32>(producer);
    record.sequence = sequence;
    record.timestampMs = NowUnixMs();
    record.sourceId = sourceId;
    record.aux = aux;
    return record;
}

bool ValidateHeader(const SharedHeader& header, std::string* error) {
    auto fail = [error](const std::string& text) {
        if (error != nullptr) *error = text;
        return false;
    };

    if (header.magic != kMagic) {
        return fail("неверная сигнатура региона: по этому имени лежит чужая разделяемая память");
    }
    if (header.abiVersion != kAbiVersion) {
        return fail("несовпадение версии ABI: компоненты собраны из разных ревизий моста (" +
                    std::to_string(header.abiVersion) + " против " + std::to_string(kAbiVersion) + ")");
    }
    if (header.headerSize != sizeof(SharedHeader)) {
        return fail("размер ABI-заголовка не совпадает (" + std::to_string(header.headerSize) + " против " +
                    std::to_string(sizeof(SharedHeader)) + ") — пересоберите обе стороны");
    }
    if (header.gameToMc.capacity != kRecordRingCapacity || header.mcToGame.capacity != kRecordRingCapacity) {
        return fail("ёмкость колец не совпадает с ожидаемой");
    }
    if (header.gameToMcBulk.capacity != kBulkRingCapacity || header.mcToGameBulk.capacity != kBulkRingCapacity) {
        return fail("ёмкость bulk-колец не совпадает с ожидаемой");
    }
    return true;
}

bool SendCommand(RecordProducer& producer, CommandCode code, std::string_view text, u32 arg0, u32 arg1) {
    Payload payload{};
    payload.command.code = static_cast<u32>(code);
    payload.command.arg0 = arg0;
    payload.command.arg1 = arg1;

    if (!text.empty()) {
        // Текст уходит в bulk-кольцо; в записи остаются признак и длина.
        // Партнёр читает bulk сразу после получения записи Command.
        if (!producer.PushBulk(text.data(), static_cast<u32>(text.size()), kBulkKindText)) {
            GWYF_WARN("SendCommand: в bulk-кольце нет места под текст команды %s", ToString(code));
            return false;
        }
        payload.command.textLength = static_cast<u32>(text.size());
    }

    return producer.Push(RecordType::Command, payload);
}

}  // namespace gwyf::ipc
