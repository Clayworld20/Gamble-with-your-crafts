// =============================================================================
//  Native_JNI_Bridge.h — связь с процессом Minecraft и/или с JVM.
//
//  ТРИ ПУТИ, все реализованы и выбираются конфигом:
//
//   1) SharedMemory (по умолчанию, кроссплатформенно).
//      Файл-канал (см. SharedChannel.h): C++ пишет в своё кольцо, Java читает
//      через FileChannel.map + ByteBuffer. Работает между любыми процессами:
//      лаунчер/игра ↔ отдельно запущенный Minecraft. Именно это используется
//      в проде, потому что Minecraft Java — отдельный процесс.
//
//   2) NamedPipe (Windows).
//      Message-mode пайп: CreateNamedPipeW + ReadFile/WriteFile. Совместим с
//      .NET NamedPipeServerStream (message mode) — удобно, если с той стороны
//      C#-инструмент, а не мод.
//
//   3) JNI (GWYC_WITH_JNI).
//      Прямые вызовы Java из C++ БЕЗ IPC: JNI_GetCreatedJavaVMs находит уже
//      поднятую JVM в нашем процессе, JNI_CreateJavaVM поднимает новую (харнесс),
//      RegisterNatives отдаёт Java-классу наши callback'и. Это самый быстрый путь,
//      но он применим только когда JVM живёт в том же процессе, что и бридж.
//
//  ПОЧЕМУ НЕ «ВНЕДРЕНИЕ В JVM ЧУЖОГО ПРОЦЕССА»: JNI — это ABI внутри процесса,
//  из другого процесса он недоступен (JNIEnv указывает на структуры в адресном
//  пространстве JVM). Поэтому: либо JVM в нашем процессе (путь 3), либо IPC
//  (пути 1-2). Никакой «инжекции в чужую JVM» тут нет и быть не может.
// =============================================================================
#pragma once

#include "gwyc/protocol.h"
#include "gwyc/SharedChannel.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gwyc::bridge {

/// Как связаны процессы.
enum class TransportKind {
    SharedMemory = 0,
    NamedPipe = 1,
};

enum class BridgeStatus {
    Ok = 0,
    NotInitialized,
    AlreadyInitialized,
    OpenFailed,
    NoPeer,          ///< канал открыт, но вторая сторона ещё не поднялась
    SendFailed,
    ReceiveFailed,
    JniUnavailable,  ///< сборка без JNI или jvm.dll/libjvm.so не найдена
    JniFailed,
    LaunchFailed,
};

[[nodiscard]] const char* StatusName(BridgeStatus status) noexcept;

/// Конфигурация моста. Строки — UTF-8.
struct BridgeConfig {
    TransportKind transport = TransportKind::SharedMemory;

    /// SharedMemory: путь к файлу канала. NamedPipe (Windows): имя пайпа.
    std::string channelPath = "gwyc_bridge.channel";

    /// С какой стороны мы: игра (казино) или Minecraft.
    ChannelRole role = ChannelRole::Game;

    /// Ёмкость кольца на каждое направление.
    uint32_t ringCapacity = 128 * 1024;

    /// Запускать ли Minecraft, если он ещё не запущен.
    bool launchMinecraft = false;
    std::string javaPath;            ///< путь к javaw.exe / java (пусто = искать в PATH)
    std::vector<std::string> jvmArguments;   ///< аргументы JVM (память, модули и т.п.)
    std::string workingDirectory;    ///< рабочий каталог запускаемого Minecraft

    /// Пробовать JNI-путь до IPC (только если сборка с JNI).
    bool preferJni = false;

    /// Класс Java-мода, который принимает сообщения (для JNI-пути и регистрации нативов).
    std::string javaEndpointClass = "dev/gwyc/bridge/NativeEndpoint";

    /// Период опроса канала в рабочем потоке, мс.
    uint32_t pollIntervalMs = 2;

    /// Сколько ждать вторую сторону при открытии канала, мс (0 = не ждать).
    uint32_t peerWaitMs = 0;
};

/// Транспорт: общий интерфейс для всех трёх путей.
class ITransport {
public:
    virtual ~ITransport() = default;

    [[nodiscard]] virtual BridgeStatus Open(const BridgeConfig& config) = 0;
    virtual void Close() noexcept = 0;

    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
    [[nodiscard]] virtual bool IsPeerReady() const noexcept = 0;

    [[nodiscard]] virtual bool Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept = 0;
    [[nodiscard]] virtual bool Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept = 0;

    [[nodiscard]] virtual uint32_t Overruns() const noexcept = 0;

    /// Диагностика кадра: сколько кадров отброшено по CRC и сколько раз
    /// пришлось пересинхронизировать поток. Для пайпа это не применимо — нули.
    [[nodiscard]] virtual uint32_t CrcFailures() const noexcept { return 0; }
    [[nodiscard]] virtual uint32_t Resyncs() const noexcept { return 0; }

    [[nodiscard]] virtual const char* Name() const noexcept = 0;
};

/// Разделяемая память — основной путь.
class SharedMemoryTransport final : public ITransport {
public:
    [[nodiscard]] BridgeStatus Open(const BridgeConfig& config) override;
    void Close() noexcept override;

    [[nodiscard]] bool IsOpen() const noexcept override { return m_channel.IsOpen(); }
    [[nodiscard]] bool IsPeerReady() const noexcept override { return m_channel.PeerReady(); }

    [[nodiscard]] bool Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept override;
    [[nodiscard]] bool Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept override;

    [[nodiscard]] uint32_t Overruns() const noexcept override { return m_channel.Overruns(); }
    [[nodiscard]] uint32_t CrcFailures() const noexcept override { return m_channel.CrcFailures(); }
    [[nodiscard]] uint32_t Resyncs() const noexcept override { return m_channel.Resyncs(); }
    [[nodiscard]] const char* Name() const noexcept override { return "shared-memory"; }

private:
    SharedChannel m_channel;
};

/// Именованный канал: Windows — message-mode пайп, POSIX — FIFO с разбором кадра.
class NamedPipeTransport final : public ITransport {
public:
    NamedPipeTransport() noexcept;
    ~NamedPipeTransport() noexcept override;

    [[nodiscard]] BridgeStatus Open(const BridgeConfig& config) override;
    void Close() noexcept override;

    [[nodiscard]] bool IsOpen() const noexcept override { return m_open; }
    [[nodiscard]] bool IsPeerReady() const noexcept override;

    [[nodiscard]] bool Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept override;
    [[nodiscard]] bool Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept override;

    [[nodiscard]] uint32_t Overruns() const noexcept override { return m_overruns; }
    [[nodiscard]] const char* Name() const noexcept override { return "named-pipe"; }

private:
    bool m_open = false;
    uint32_t m_overruns = 0;
    std::string m_pipeName;
    std::vector<uint8_t> m_pending;   ///< накопленный поток (POSIX-пайп не хранит границы)

#if defined(_WIN32)
    void* m_pipe = nullptr;           ///< HANDLE
#else
    int m_fd = -1;
#endif
};

/// Запуск отдельного процесса Minecraft (когда он ещё не запущен).
class MinecraftProcess {
public:
    /// Запустить java/javaw с переданными аргументами. Процесс НЕ привязывается к бриджу.
    [[nodiscard]] static BridgeStatus Launch(const BridgeConfig& config, std::string& error) noexcept;

    /// Имя процесса, который мы ищем среди запущенных (для диагностики).
    [[nodiscard]] static bool IsMinecraftRunning() noexcept;
};

#if GWYC_WITH_JNI

/// Прямой путь: JVM в нашем процессе.
class JniBridge {
public:
    ~JniBridge();

    /// Найти уже поднятую JVM (JNI_GetCreatedJavaVMs) и привязаться к ней.
    [[nodiscard]] BridgeStatus AttachToRunningJvm() noexcept;

    /// Поднять новую JVM внутри текущего процесса (харнесс/тесты, без Minecraft).
    [[nodiscard]] BridgeStatus CreateJvm(const std::vector<std::string>& options) noexcept;

    /// Привязать текущий поток к JVM (обязательно для вызовов из рабочего потока).
    [[nodiscard]] BridgeStatus AttachThread() noexcept;
    void DetachThread() noexcept;

    /// Найти класс-эндпоинт и закешировать jclass + jmethodID.
    [[nodiscard]] BridgeStatus BindEndpoint(const std::string& slashClassName) noexcept;

    /// Отдать Java-классу наши нативные callback'и (RegisterNatives).
    [[nodiscard]] BridgeStatus RegisterNatives() noexcept;

    [[nodiscard]] bool IsAttached() const noexcept { return m_jvm != nullptr; }

    /// Вызов dev.gwyc.bridge.NativeEndpoint.onVoxelEdit(int,int,int,byte)
    [[nodiscard]] BridgeStatus CallVoxelEdit(int32_t x, int32_t y, int32_t z, uint8_t blockId) noexcept;

    /// Вызов dev.gwyc.bridge.NativeEndpoint.onReward(int playerId, int blockKind, int count)
    [[nodiscard]] BridgeStatus CallReward(int32_t playerId, int32_t blockKind, int32_t count) noexcept;

    /// Последняя ошибка (описание исключения или проблемы окружения JVM).
    [[nodiscard]] const std::string& LastError() const noexcept { return m_lastError; }

private:
    void* m_jvm = nullptr;            ///< JavaVM*
    void* m_env = nullptr;            ///< JNIEnv* (для привязанного потока)
    void* m_endpointClass = nullptr;  ///< jclass (global ref)
    void* m_methodVoxel = nullptr;    ///< jmethodID
    void* m_methodReward = nullptr;   ///< jmethodID
    std::string m_lastError;
};

#endif // GWYC_WITH_JNI

#if GWYC_WITH_JNI
/// Подключить приёмники Java → C++: их вызовут нативные методы, зарегистрированные
/// в JVM через RegisterNatives (см. Native_JNI_Bridge.cpp).
void SetJavaCallbacks(void (*voxelSink)(int32_t, int32_t, int32_t, uint8_t),
                      void (*rewardSink)(int32_t, int32_t, int32_t)) noexcept;
#endif

/// Собрать транспорт по конфигу.
[[nodiscard]] std::unique_ptr<ITransport> MakeTransport(const BridgeConfig& config);

} // namespace gwyc::bridge
