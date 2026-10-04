// =============================================================================
//  SharedChannel.h — разделяемая память (файловый маппинг) + кадрирование.
//
//  Почему файловый маппинг, а не «именованный» объект ОС: одна и та же раскладка
//  обязана работать в C++, C# и Java (MemoryMappedFile / FileChannel.map), а Java
//  не умеет открывать именованные объекты ОС. Поэтому канал — это файл, который
//  мапит каждая сторона: Win32 CreateFileMapping+MapViewOfFile, POSIX open+mmap.
//
//  Раскладка области:
//      [FileHeader][RingControlBlock A + data][RingControlBlock B + data]
//
//      кольцо A: Game  → Minecraft
//      кольцо B: Minecraft → Game
//
//  Каждый участник пишет ровно в своё кольцо и читает из чужого — это и есть
//  условие корректности SPSC без блокировок (см. RingBuffer.h).
//
//  Приём кадра: Peek заголовка → проверка magic/версии/размера → дождаться кадра
//  целиком → Read → проверка CRC → отдача. Битой кадр не «сдвигает» поток: при
//  несовпадении magic потребитель сдвигается на 1 байт и ищет следующий кадр.
// =============================================================================
#pragma once

#include "gwyc/RingBuffer.h"
#include "gwyc/protocol.h"

#include <cstdint>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   include <fcntl.h>
#   include <sys/mman.h>
#   include <sys/stat.h>
#   include <unistd.h>
#endif

namespace gwyc {

inline constexpr uint32_t kChannelMagic = 0x48435747u;   // 'GWCH'

/// Заголовок области: обе стороны проверяют его до начала работы.
#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic;                  ///< kChannelMagic
    uint16_t layoutVersion;          ///< kLayoutVersion
    uint16_t protocolVersion;        ///< kProtocolVersion
    uint32_t totalSize;              ///< размер области в байтах
    uint32_t ringCapacity;           ///< ёмкость каждого кольца (степень двойки)
    volatile uint32_t gameReady;     ///< сторона «игра» поднялась
    volatile uint32_t minecraftReady;///< сторона «Minecraft» поднялась
};
#pragma pack(pop)

static_assert(offsetof(FileHeader, ringCapacity) == 12, "FileHeader: раскладка");
static_assert(offsetof(FileHeader, gameReady) == 16, "FileHeader: раскладка");

/// Роль участника: она определяет, в какое кольцо писать и из какого читать.
enum class ChannelRole : uint32_t {
    Game = 0,
    Minecraft = 1,
};

enum class ChannelState {
    Ok = 0,
    AlreadyOpen,
    OpenFailed,
    TruncateFailed,
    MapFailed,
    LayoutMismatch,
};

[[nodiscard]] inline const char* ChannelStateName(ChannelState state) noexcept
{
    switch (state) {
        case ChannelState::Ok:             return "ok";
        case ChannelState::AlreadyOpen:    return "канал уже открыт";
        case ChannelState::OpenFailed:     return "не удалось открыть/создать файл канала";
        case ChannelState::TruncateFailed: return "не удалось выставить размер файла канала";
        case ChannelState::MapFailed:      return "не удалось замапить файл канала";
        case ChannelState::LayoutMismatch: return "раскладка канала не совпадает (другая версия сборки)";
    }
    return "неизвестно";
}

class SharedChannel {
public:
    SharedChannel() noexcept = default;
    ~SharedChannel() { Close(); }

    SharedChannel(const SharedChannel&) = delete;
    SharedChannel& operator=(const SharedChannel&) = delete;

    [[nodiscard]] ChannelState Open(const char* path, ChannelRole role, uint32_t ringCapacity = 128 * 1024) noexcept
    {
        if (m_mapped != nullptr) return ChannelState::AlreadyOpen;
        if (path == nullptr || path[0] == '\0') return ChannelState::OpenFailed;

        m_role = role;
        const uint32_t capacity = RingBuffer::NormalizeCapacity(ringCapacity);
        const uint32_t ringBytes = static_cast<uint32_t>(RingBuffer::BytesRequired(capacity));
        const uint32_t total = static_cast<uint32_t>(sizeof(FileHeader)) + ringBytes * 2u;

#if defined(_WIN32)
        m_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) { m_file = nullptr; return ChannelState::OpenFailed; }

        // Расширяем файл только при необходимости: чужой (уже созданный) канал
        // нельзя обрезать — это уничтожит заголовок второй стороны. Несовпадение
        // размеров ловится ниже проверкой header->totalSize.
        LARGE_INTEGER existingSize{};
        if (!GetFileSizeEx(m_file, &existingSize)) {
            CloseHandle(m_file); m_file = nullptr; return ChannelState::OpenFailed;
        }

        if (existingSize.QuadPart < static_cast<LONGLONG>(total)) {
            LARGE_INTEGER end{};
            end.QuadPart = total;
            if (!SetFilePointerEx(m_file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(m_file)) {
                CloseHandle(m_file); m_file = nullptr; return ChannelState::TruncateFailed;
            }
        }

        m_mapping = CreateFileMappingA(m_file, nullptr, PAGE_READWRITE, 0, total, nullptr);
        if (m_mapping == nullptr) { CloseHandle(m_file); m_file = nullptr; return ChannelState::MapFailed; }

        m_mapped = static_cast<uint8_t*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, total));
        if (m_mapped == nullptr) {
            CloseHandle(m_mapping); CloseHandle(m_file);
            m_mapping = nullptr; m_file = nullptr;
            return ChannelState::MapFailed;
        }
#else
        m_fd = ::open(path, O_RDWR | O_CREAT, 0666);
        if (m_fd < 0) return ChannelState::OpenFailed;

        // Расширяем только вверх: если файл уже больше, не обрезаем его (в нём чужой заголовок).
        struct stat info {};
        if (::fstat(m_fd, &info) != 0) { ::close(m_fd); m_fd = -1; return ChannelState::OpenFailed; }
        if (static_cast<uint32_t>(info.st_size) < total) {
            if (::ftruncate(m_fd, static_cast<off_t>(total)) != 0) {
                ::close(m_fd); m_fd = -1; return ChannelState::TruncateFailed;
            }
        }

        void* view = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (view == MAP_FAILED) { ::close(m_fd); m_fd = -1; return ChannelState::MapFailed; }
        m_mapped = static_cast<uint8_t*>(view);
#endif

        m_fileSize = total;
        auto* header = reinterpret_cast<FileHeader*>(m_mapped);
        const bool fresh = (header->magic != kChannelMagic);

        if (fresh) {
            std::memset(m_mapped, 0, total);
            header->magic = kChannelMagic;
            header->layoutVersion = kLayoutVersion;
            header->protocolVersion = kProtocolVersion;
            header->totalSize = total;
            header->ringCapacity = capacity;
        }
        else if (header->layoutVersion != kLayoutVersion || header->totalSize != total) {
            Close();
            return ChannelState::LayoutMismatch;
        }

        uint8_t* base = m_mapped + sizeof(FileHeader);
        auto* ringGameToMc = reinterpret_cast<RingControlBlock*>(base);
        auto* ringMcToGame = reinterpret_cast<RingControlBlock*>(base + ringBytes);

        if (fresh) {
            ringGameToMc->capacity = capacity;
            ringMcToGame->capacity = capacity;
            ResetRing(ringGameToMc);
            ResetRing(ringMcToGame);
        }

        RingBuffer outgoing;
        RingBuffer incoming;
        outgoing.Attach(ringGameToMc, base + sizeof(RingControlBlock), capacity);
        incoming.Attach(ringMcToGame, base + ringBytes + sizeof(RingControlBlock), capacity);

        // Minecraft пишет в своё кольцо и читает из чужого — просто меняем местами.
        m_outgoing = outgoing;
        m_incoming = incoming;
        if (role == ChannelRole::Minecraft) std::swap(m_outgoing, m_incoming);

        if (role == ChannelRole::Game) header->gameReady = 1;
        else header->minecraftReady = 1;

        m_open = true;
        return ChannelState::Ok;
    }

    void Close() noexcept
    {
        m_open = false;
#if defined(_WIN32)
        if (m_mapped != nullptr) { UnmapViewOfFile(m_mapped); m_mapped = nullptr; }
        if (m_mapping != nullptr) { CloseHandle(m_mapping); m_mapping = nullptr; }
        if (m_file != nullptr) { CloseHandle(m_file); m_file = nullptr; }
#else
        if (m_mapped != nullptr) { ::munmap(m_mapped, m_fileSize); m_mapped = nullptr; }
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
#endif
    }

    [[nodiscard]] bool IsOpen() const noexcept { return m_open; }

    /// Вторая сторона поднялась и готова принимать кадры.
    [[nodiscard]] bool PeerReady() const noexcept
    {
        if (!IsOpen()) return false;
        const auto* header = reinterpret_cast<const FileHeader*>(m_mapped);
        return (m_role == ChannelRole::Game) ? (header->minecraftReady != 0) : (header->gameReady != 0);
    }

    /// Отправить кадр. false — кадр не отправлен (нет места в кольце или слишком большой).
    ///
    /// Заголовок и payload пишутся двумя Write подряд; приёмник сначала ждёт кадр
    /// целиком (по payloadSize из заголовка), поэтому «половинок» он не увидит.
    [[nodiscard]] bool Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept
    {
        if (!IsOpen()) return false;
        if (payloadSize > kMaxPayloadSize) return false;
        if (payloadSize != 0 && payload == nullptr) return false;

        const uint32_t total = static_cast<uint32_t>(sizeof(FrameHeader)) + payloadSize;
        if (m_outgoing.WritableBytes() < total) {
            m_outgoing.NoteOverrun();
            return false;
        }

        FrameHeader header{};
        header.magic = kMagic;
        header.layoutVersion = kLayoutVersion;
        header.type = static_cast<uint8_t>(type);
        header.flags = flags;
        header.payloadSize = payloadSize;

        Crc32Stream crc;
        crc.Update(&header.type, sizeof(header.type) + sizeof(header.flags) + sizeof(header.payloadSize));
        if (payloadSize != 0) crc.Update(payload, payloadSize);
        header.crc32 = crc.Final();

        if (!m_outgoing.Write(&header, static_cast<uint32_t>(sizeof(FrameHeader)))) return false;
        if (payloadSize != 0 && !m_outgoing.Write(payload, payloadSize)) return false;
        return true;
    }

    /// Записать в кольцо произвольные байты, без формирования кадра.
    /// Нужен только тестам (проверка пересинхронизации) и дамп-инструментам.
    [[nodiscard]] bool SendRaw(const void* data, uint32_t size) noexcept
    {
        if (!IsOpen() || size == 0 || data == nullptr) return false;
        return m_outgoing.Write(data, size);
    }

    /// Шаблонная отправка POD-сообщения (единственный способ в коде бриджа).
    template <typename T>
    [[nodiscard]] bool SendPod(MsgType type, uint8_t flags, const T& payload) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "SendPod только для POD");
        return Send(type, flags, &payload, static_cast<uint32_t>(sizeof(T)));
    }

    /// Попробовать принять один кадр. false — данных/целого кадра нет либо кадр битый.
    [[nodiscard]] bool Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept
    {
        payloadSize = 0;
        if (!IsOpen()) return false;

        FrameHeader header{};
        if (!m_incoming.Peek(0, &header, static_cast<uint32_t>(sizeof(FrameHeader)))) return false;

        if (header.magic != kMagic || header.layoutVersion != kLayoutVersion || header.payloadSize > kMaxPayloadSize) {
            // Рассинхрон: ищем следующий кадр, сдвигаясь на байт за раз.
            m_incoming.Skip(1);
            ++m_resyncs;
            return false;
        }

        const uint32_t total = static_cast<uint32_t>(sizeof(FrameHeader)) + header.payloadSize;
        if (m_incoming.ReadableBytes() < total) return false;      // кадр ещё не дошёл целиком
        if (total > sizeof(m_scratch)) { m_incoming.Skip(total); return false; }

        if (m_incoming.Read(m_scratch, total) != total) return false;

        Crc32Stream crc;
        crc.Update(m_scratch + offsetof(FrameHeader, type), sizeof(FrameHeader::type) + sizeof(FrameHeader::flags) + sizeof(FrameHeader::payloadSize));
        if (header.payloadSize != 0) {
            crc.Update(m_scratch + sizeof(FrameHeader), header.payloadSize);
        }

        if (crc.Final() != header.crc32) {
            ++m_crcFailures;
            return false;                                          // битый кадр выброшен, поток не сдвинут
        }

        type = static_cast<MsgType>(header.type);
        flags = header.flags;
        payloadSize = header.payloadSize;

        if (payloadSize != 0) {
            if (payloadOut == nullptr || payloadCapacity < payloadSize) return false;
            std::memcpy(payloadOut, m_scratch + sizeof(FrameHeader), payloadSize);
        }
        return true;
    }

    /// Принять POD-сообщение ожидаемого типа.
    template <typename T>
    [[nodiscard]] bool ReceivePod(MsgType& type, T& payload) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "ReceivePod только для POD");
        uint8_t flags = 0;
        uint32_t size = 0;
        if (!Receive(type, flags, reinterpret_cast<uint8_t*>(&payload), static_cast<uint32_t>(sizeof(T)), size)) return false;
        return size == sizeof(T);
    }

    [[nodiscard]] uint32_t Overruns() const noexcept { return m_outgoing.Overruns(); }
    [[nodiscard]] uint32_t Resyncs() const noexcept { return m_resyncs; }
    [[nodiscard]] uint32_t CrcFailures() const noexcept { return m_crcFailures; }

private:
    ChannelRole m_role = ChannelRole::Game;
    bool m_open = false;
    uint32_t m_fileSize = 0;
    uint32_t m_resyncs = 0;
    uint32_t m_crcFailures = 0;

    RingBuffer m_outgoing{};
    RingBuffer m_incoming{};

    uint8_t m_scratch[sizeof(FrameHeader) + kMaxPayloadSize]{};
    uint8_t* m_mapped = nullptr;

#if defined(_WIN32)
    void* m_file = nullptr;
    void* m_mapping = nullptr;
#else
    int m_fd = -1;
#endif
};

} // namespace gwyc
