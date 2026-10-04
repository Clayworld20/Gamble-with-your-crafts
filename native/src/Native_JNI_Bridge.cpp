// =============================================================================
//  Native_JNI_Bridge.cpp — реализации транспортов (shared memory / pipe) и JNI.
// =============================================================================
#include "Native_JNI_Bridge.h"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#   include <tlhelp32.h>
#else
#   include <dlfcn.h>
#   include <fcntl.h>
#   include <sys/stat.h>
#   include <unistd.h>
#   include <cerrno>
#endif

#if GWYC_WITH_JNI
#   include <jni.h>
#endif

namespace gwyc::bridge {

namespace {

uint64_t NowMs() noexcept
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// Собрать кадр целиком (заголовок + payload) в один буфер.
std::vector<uint8_t> BuildFrame(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize)
{
    FrameHeader header{};
    header.magic = kMagic;
    header.layoutVersion = kLayoutVersion;
    header.type = static_cast<uint8_t>(type);
    header.flags = flags;
    header.payloadSize = payloadSize;

    Crc32Stream crc;
    crc.Update(&header.type, sizeof(header.type) + sizeof(header.flags) + sizeof(header.payloadSize));
    if (payloadSize != 0 && payload != nullptr) crc.Update(payload, payloadSize);
    header.crc32 = crc.Final();

    std::vector<uint8_t> frame(sizeof(FrameHeader) + payloadSize);
    std::memcpy(frame.data(), &header, sizeof(FrameHeader));
    if (payloadSize != 0 && payload != nullptr) {
        std::memcpy(frame.data() + sizeof(FrameHeader), payload, payloadSize);
    }
    return frame;
}

/// Проверить кадр из буфера (заголовок + payload уже собраны).
bool ValidateFrame(const uint8_t* data, uint32_t totalSize) noexcept
{
    if (data == nullptr || totalSize < sizeof(FrameHeader)) return false;

    FrameHeader header{};
    std::memcpy(&header, data, sizeof(FrameHeader));
    if (header.magic != kMagic || header.layoutVersion != kLayoutVersion) return false;
    if (header.payloadSize > kMaxPayloadSize) return false;
    if (sizeof(FrameHeader) + header.payloadSize != totalSize) return false;

    Crc32Stream crc;
    crc.Update(data + offsetof(FrameHeader, type),
               sizeof(FrameHeader::type) + sizeof(FrameHeader::flags) + sizeof(FrameHeader::payloadSize));
    if (header.payloadSize != 0) crc.Update(data + sizeof(FrameHeader), header.payloadSize);
    return crc.Final() == header.crc32;
}

} // namespace

const char* StatusName(BridgeStatus status) noexcept
{
    switch (status) {
        case BridgeStatus::Ok:                return "ok";
        case BridgeStatus::NotInitialized:    return "мост не инициализирован";
        case BridgeStatus::AlreadyInitialized:return "мост уже инициализирован";
        case BridgeStatus::OpenFailed:        return "не удалось открыть канал";
        case BridgeStatus::NoPeer:            return "вторая сторона не подключилась";
        case BridgeStatus::SendFailed:        return "кадр не отправлен (нет места/канал закрыт)";
        case BridgeStatus::ReceiveFailed:     return "кадр не получен";
        case BridgeStatus::JniUnavailable:    return "JNI недоступен в этой сборке";
        case BridgeStatus::JniFailed:         return "ошибка вызова JNI";
        case BridgeStatus::LaunchFailed:      return "не удалось запустить процесс Minecraft";
    }
    return "неизвестно";
}

// ─────────────────────────────────────────────────────────────────────────────
//  SharedMemoryTransport
// ─────────────────────────────────────────────────────────────────────────────

BridgeStatus SharedMemoryTransport::Open(const BridgeConfig& config)
{
    if (m_channel.IsOpen()) return BridgeStatus::AlreadyInitialized;

    const ChannelState state = m_channel.Open(config.channelPath.c_str(), config.role, config.ringCapacity);
    if (state != ChannelState::Ok) return BridgeStatus::OpenFailed;

    if (config.peerWaitMs != 0) {
        const uint64_t deadline = NowMs() + config.peerWaitMs;
        while (!m_channel.PeerReady() && NowMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    return BridgeStatus::Ok;
}

void SharedMemoryTransport::Close() noexcept
{
    m_channel.Close();
}

bool SharedMemoryTransport::Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept
{
    return m_channel.Send(type, flags, payload, payloadSize);
}

bool SharedMemoryTransport::Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept
{
    return m_channel.Receive(type, flags, payloadOut, payloadCapacity, payloadSize);
}

// ─────────────────────────────────────────────────────────────────────────────
//  NamedPipeTransport
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32)

namespace {

std::wstring ToWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

} // namespace

NamedPipeTransport::NamedPipeTransport() noexcept = default;

NamedPipeTransport::~NamedPipeTransport() noexcept
{
    Close();
}

BridgeStatus NamedPipeTransport::Open(const BridgeConfig& config)
{
    if (m_open) return BridgeStatus::AlreadyInitialized;

    m_pipeName = config.channelPath;
    if (m_pipeName.empty()) return BridgeStatus::OpenFailed;

    // Канал должен выглядеть как \\.\pipe\<имя>
    std::string full = m_pipeName;
    if (full.rfind("\\\\.\\pipe\\", 0) != 0) full = "\\\\.\\pipe\\" + full;

    const std::wstring wide = ToWide(full);

    // Сторона «игра» создаёт канал и ждёт подключения Minecraft-процесса.
    // Message-mode: каждая запись = одно сообщение, что совпадает с моделью кадра.
    if (config.role == ChannelRole::Game) {
        HANDLE pipe = CreateNamedPipeW(wide.c_str(),
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return BridgeStatus::OpenFailed;

        // Не блокируем игровой поток: подключение ждём с таймаутом в отдельном потоке
        // вызывающего кода (BridgeDll запускает handshake в worker'е).
        if (config.peerWaitMs != 0) {
            if (ConnectNamedPipe(pipe, nullptr) == FALSE) {
                const DWORD error = GetLastError();
                if (error != ERROR_PIPE_CONNECTED) {
                    CloseHandle(pipe);
                    return BridgeStatus::NoPeer;
                }
            }
        }

        m_pipe = pipe;
    }
    else {
        // Сторона «Minecraft» подключается к уже созданному пайпу.
        const uint64_t deadline = NowMs() + (config.peerWaitMs != 0 ? config.peerWaitMs : 3000);
        HANDLE pipe = INVALID_HANDLE_VALUE;

        while (NowMs() < deadline) {
            pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;

            if (GetLastError() != ERROR_PIPE_BUSY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            WaitNamedPipeW(wide.c_str(), 100);
        }

        if (pipe == INVALID_HANDLE_VALUE) return BridgeStatus::OpenFailed;

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        m_pipe = pipe;
    }

    m_open = true;
    return BridgeStatus::Ok;
}

void NamedPipeTransport::Close() noexcept
{
    if (m_pipe != nullptr) {
        auto handle = static_cast<HANDLE>(m_pipe);
        FlushFileBuffers(handle);
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
        m_pipe = nullptr;
    }
    m_open = false;
}

bool NamedPipeTransport::IsPeerReady() const noexcept
{
    return m_open && m_pipe != nullptr;
}

bool NamedPipeTransport::Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept
{
    if (!m_open || m_pipe == nullptr) return false;

    const std::vector<uint8_t> frame = BuildFrame(type, flags, payload, payloadSize);
    DWORD written = 0;
    const BOOL ok = WriteFile(static_cast<HANDLE>(m_pipe), frame.data(),
                              static_cast<DWORD>(frame.size()), &written, nullptr);

    if (ok == FALSE || written != static_cast<DWORD>(frame.size())) {
        ++m_overruns;
        return false;
    }
    return true;
}

bool NamedPipeTransport::Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept
{
    payloadSize = 0;
    if (!m_open || m_pipe == nullptr) return false;

    // Кадр целиком в одном сообщении пайпа: читаем в буфер максимального размера.
    std::vector<uint8_t> buffer(sizeof(FrameHeader) + kMaxPayloadSize);
    DWORD available = 0;

    if (PeekNamedPipe(static_cast<HANDLE>(m_pipe), nullptr, 0, nullptr, &available, nullptr) == FALSE) return false;
    if (available < sizeof(FrameHeader)) return false;

    DWORD read = 0;
    const DWORD toRead = (available > buffer.size()) ? static_cast<DWORD>(buffer.size()) : available;
    if (ReadFile(static_cast<HANDLE>(m_pipe), buffer.data(), toRead, &read, nullptr) == FALSE || read < sizeof(FrameHeader)) return false;

    FrameHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(FrameHeader));

    const uint32_t total = static_cast<uint32_t>(sizeof(FrameHeader)) + header.payloadSize;
    if (read < total || !ValidateFrame(buffer.data(), total)) return false;

    type = static_cast<MsgType>(header.type);
    flags = header.flags;
    payloadSize = header.payloadSize;
    if (payloadSize != 0) {
        if (payloadOut == nullptr || payloadCapacity < payloadSize) return false;
        std::memcpy(payloadOut, buffer.data() + sizeof(FrameHeader), payloadSize);
    }
    return true;
}

#else   // POSIX: FIFO + сборка кадров из потока

NamedPipeTransport::NamedPipeTransport() noexcept = default;

NamedPipeTransport::~NamedPipeTransport() noexcept
{
    Close();
}

BridgeStatus NamedPipeTransport::Open(const BridgeConfig& config)
{
    if (m_open) return BridgeStatus::AlreadyInitialized;
    if (config.channelPath.empty()) return BridgeStatus::OpenFailed;

    m_pipeName = config.channelPath;

    if (config.role == ChannelRole::Game) {
        if (::mkfifo(m_pipeName.c_str(), 0666) != 0 && errno != EEXIST) return BridgeStatus::OpenFailed;
    }

    // O_RDWR не блокирует открытие, даже если вторая сторона ещё не подошла.
    m_fd = ::open(m_pipeName.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0) return BridgeStatus::OpenFailed;

    m_open = true;
    return BridgeStatus::Ok;
}

void NamedPipeTransport::Close() noexcept
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_open = false;
    m_pending.clear();
}

bool NamedPipeTransport::IsPeerReady() const noexcept
{
    return m_open;
}

bool NamedPipeTransport::Send(MsgType type, uint8_t flags, const void* payload, uint32_t payloadSize) noexcept
{
    if (!m_open || m_fd < 0) return false;

    const std::vector<uint8_t> frame = BuildFrame(type, flags, payload, payloadSize);

    // Пайп — байтовый поток: пишем с ограничением, чтобы не блокироваться навсегда.
    const size_t chunk = (frame.size() > 4096) ? 4096 : frame.size();
    const ssize_t written = ::write(m_fd, frame.data(), chunk);
    if (written <= 0) {
        ++m_overruns;
        return false;
    }

    if (static_cast<size_t>(written) < frame.size()) {
        // Дописываем остаток по частям; если пайп переполнен — сообщение потеряно.
        size_t offset = static_cast<size_t>(written);
        while (offset < frame.size()) {
            const size_t rest = frame.size() - offset;
            const size_t step = (rest > 4096) ? 4096 : rest;
            const ssize_t n = ::write(m_fd, frame.data() + offset, step);
            if (n <= 0) {
                ++m_overruns;
                return false;
            }
            offset += static_cast<size_t>(n);
        }
    }

    return true;
}

bool NamedPipeTransport::Receive(MsgType& type, uint8_t& flags, uint8_t* payloadOut, uint32_t payloadCapacity, uint32_t& payloadSize) noexcept
{
    payloadSize = 0;
    if (!m_open || m_fd < 0) return false;

    // Вычитываем всё, что есть, в накопитель.
    uint8_t chunk[4096];
    for (;;) {
        const ssize_t n = ::read(m_fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        m_pending.insert(m_pending.end(), chunk, chunk + n);
    }

    if (m_pending.size() < sizeof(FrameHeader)) return false;

    FrameHeader header{};
    std::memcpy(&header, m_pending.data(), sizeof(FrameHeader));

    const uint32_t total = static_cast<uint32_t>(sizeof(FrameHeader)) + header.payloadSize;
    if (m_pending.size() < total || !ValidateFrame(m_pending.data(), total)) {
        // Битый кадр: сдвигаемся на байт, ищем следующий.
        if (!m_pending.empty()) m_pending.erase(m_pending.begin());
        return false;
    }

    type = static_cast<MsgType>(header.type);
    flags = header.flags;
    payloadSize = header.payloadSize;
    if (payloadSize != 0) {
        if (payloadOut == nullptr || payloadCapacity < payloadSize) {
            m_pending.erase(m_pending.begin(), m_pending.begin() + total);
            return false;
        }
        std::memcpy(payloadOut, m_pending.data() + sizeof(FrameHeader), payloadSize);
    }

    m_pending.erase(m_pending.begin(), m_pending.begin() + total);
    return true;
}

#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Запуск Minecraft
// ─────────────────────────────────────────────────────────────────────────────

BridgeStatus MinecraftProcess::Launch(const BridgeConfig& config, std::string& error) noexcept
{
    if (config.javaPath.empty()) {
        error = "не указан путь к java/javaw (BridgeConfig.javaPath)";
        return BridgeStatus::LaunchFailed;
    }

    if (config.jvmArguments.empty()) {
        error = "не заданы аргументы JVM: нечего запускать (нужен -cp/-jar с игрой и загрузчиком Fabric)";
        return BridgeStatus::LaunchFailed;
    }

#if defined(_WIN32)
    std::string command = "\"" + config.javaPath + "\"";
    for (const std::string& argument : config.jvmArguments) command += " " + argument;

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    const BOOL ok = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr,
                                   config.workingDirectory.empty() ? nullptr : config.workingDirectory.c_str(),
                                   &startup, &process);
    if (ok == FALSE) {
        error = "CreateProcess не смог запустить Java (код " + std::to_string(GetLastError()) + ")";
        return BridgeStatus::LaunchFailed;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return BridgeStatus::Ok;
#else
    // POSIX: fork+exec, без оболочки (никаких system() с подстановкой строк).
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork не удался";
        return BridgeStatus::LaunchFailed;
    }

    if (pid == 0) {
        if (!config.workingDirectory.empty()) ::chdir(config.workingDirectory.c_str());

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(config.javaPath.c_str()));
        for (const std::string& argument : config.jvmArguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);

        ::execvp(config.javaPath.c_str(), argv.data());
        ::_exit(127);
    }

    return BridgeStatus::Ok;
#endif
}

bool MinecraftProcess::IsMinecraftRunning() noexcept
{
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"javaw.exe") == 0 || _wcsicmp(entry.szExeFile, L"java.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
#else
    // Linux: смотрим /proc на процесс java.
    for (int pid = 1; pid < 65536; ++pid) {
        char path[64]{};
        std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        const int fd = ::open(path, O_RDONLY);
        if (fd < 0) continue;

        char name[64]{};
        const ssize_t n = ::read(fd, name, sizeof(name) - 1);
        ::close(fd);
        if (n <= 0) continue;

        if (std::strncmp(name, "java", 4) == 0) return true;
    }
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  JNI: прямая дорога в JVM, если она живёт в нашем процессе
// ─────────────────────────────────────────────────────────────────────────────

#if GWYC_WITH_JNI

namespace {

/// Динамическая загрузка JVM: jvm.dll (Windows) / libjvm.so (Linux).
/// Компоновать бридж с JVM статически нельзя — бридж работает и без неё.
void* LoadJvmLibrary()
{
#if defined(_WIN32)
    const char* candidates[] = { "jvm.dll" };
#elif defined(__APPLE__)
    const char* candidates[] = {
        "libjvm.dylib",
        "/Library/Java/JavaVirtualMachines/current/Contents/Home/lib/server/libjvm.dylib",
    };
#else
    const char* candidates[] = {
        "libjvm.so",
        "/usr/lib/jvm/default-java/lib/server/libjvm.so",
        "/usr/lib/x86_64-linux-gnu/libjvm.so",
    };
#endif

    for (const char* candidate : candidates) {
#if defined(_WIN32)
        HMODULE library = LoadLibraryA(candidate);
        if (library != nullptr) return library;
#else
        void* library = dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
        if (library != nullptr) return library;
#endif
    }

    return nullptr;
}

std::string DescribeJavaException(JNIEnv* env)
{
    if (env == nullptr || env->ExceptionCheck() == JNI_FALSE) return "без исключения";

    jthrowable exception = env->ExceptionOccurred();
    env->ExceptionClear();
    if (exception == nullptr) return "исключение без объекта";

    jclass throwableClass = env->FindClass("java/lang/Throwable");
    if (throwableClass == nullptr) return "исключение (класс Throwable недоступен)";

    jmethodID toString = env->GetMethodID(throwableClass, "toString", "()Ljava/lang/String;");
    if (toString == nullptr) {
        env->DeleteLocalRef(throwableClass);
        return "исключение (нет Throwable.toString)";
    }

    auto* text = static_cast<jstring>(env->CallObjectMethod(exception, toString));
    std::string result = "исключение JNI";
    if (text != nullptr) {
        const char* utf8 = env->GetStringUTFChars(text, nullptr);
        if (utf8 != nullptr) {
            result = utf8;
            env->ReleaseStringUTFChars(text, utf8);
        }
        env->DeleteLocalRef(text);
    }

    env->DeleteLocalRef(throwableClass);
    env->DeleteLocalRef(exception);
    return result;
}

} // namespace

JniBridge::~JniBridge()
{
    // Отсоединяем поток, но НЕ убиваем JVM: она может принадлежать самому Minecraft.
    if (m_jvm != nullptr && m_env != nullptr) {
        auto* jvm = static_cast<JavaVM*>(m_jvm);
        jvm->DetachCurrentThread();
        m_env = nullptr;
    }
}

BridgeStatus JniBridge::AttachToRunningJvm() noexcept
{
    void* library = LoadJvmLibrary();
    if (library == nullptr) {
        m_lastError = "jvm.dll/libjvm.so не найдены";
        return BridgeStatus::JniUnavailable;
    }

#if defined(_WIN32)
    using GetCreatedFn = jint(JNICALL*)(JavaVM**, jsize, jsize*);
    auto getCreated = reinterpret_cast<GetCreatedFn>(
        reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), "JNI_GetCreatedJavaVMs")));
#else
    auto getCreated = reinterpret_cast<jint (*)(JavaVM**, jsize, jsize*)>(dlsym(library, "JNI_GetCreatedJavaVMs"));
#endif

    if (getCreated == nullptr) {
        m_lastError = "в библиотеке JVM нет JNI_GetCreatedJavaVMs";
        return BridgeStatus::JniUnavailable;
    }

    JavaVM* jvm = nullptr;
    jsize count = 0;
    if (getCreated(&jvm, 1, &count) != JNI_OK || count == 0 || jvm == nullptr) {
        m_lastError = "уже запущенной JVM в этом процессе нет";
        return BridgeStatus::JniUnavailable;
    }

    m_jvm = jvm;
    return AttachThread();
}

BridgeStatus JniBridge::CreateJvm(const std::vector<std::string>& options) noexcept
{
    void* library = LoadJvmLibrary();
    if (library == nullptr) {
        m_lastError = "jvm.dll/libjvm.so не найдены";
        return BridgeStatus::JniUnavailable;
    }

#if defined(_WIN32)
    using CreateFn = jint(JNICALL*)(JavaVM**, void**, void*);
    auto create = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), "JNI_CreateJavaVM")));
#else
    auto create = reinterpret_cast<jint (*)(JavaVM**, void**, void*)>(dlsym(library, "JNI_CreateJavaVM"));
#endif

    if (create == nullptr) {
        m_lastError = "в библиотеке JVM нет JNI_CreateJavaVM";
        return BridgeStatus::JniUnavailable;
    }

    std::vector<JavaVMOption> jvmOptions;
    jvmOptions.reserve(options.size());
    for (const std::string& option : options) {
        JavaVMOption entry{};
        entry.optionString = const_cast<char*>(option.c_str());
        entry.extraInfo = nullptr;
        jvmOptions.push_back(entry);
    }

    JavaVMInitArgs arguments{};
    arguments.version = JNI_VERSION_1_8;
    arguments.nOptions = static_cast<jint>(jvmOptions.size());
    arguments.options = jvmOptions.empty() ? nullptr : jvmOptions.data();
    arguments.ignoreUnrecognized = JNI_TRUE;

    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;
    const jint result = create(&jvm, reinterpret_cast<void**>(&env), &arguments);
    if (result != JNI_OK || jvm == nullptr) {
        m_lastError = "JNI_CreateJavaVM вернул " + std::to_string(result);
        return BridgeStatus::JniFailed;
    }

    m_jvm = jvm;
    m_env = env;
    return BridgeStatus::Ok;
}

BridgeStatus JniBridge::AttachThread() noexcept
{
    if (m_jvm == nullptr) return BridgeStatus::JniUnavailable;

    auto* jvm = static_cast<JavaVM*>(m_jvm);
    JNIEnv* env = nullptr;

    const jint result = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (result == JNI_OK && env != nullptr) {
        m_env = env;
        return BridgeStatus::Ok;
    }

    if (result != JNI_EDETACHED) {
        m_lastError = "GetEnv вернул " + std::to_string(result);
        return BridgeStatus::JniFailed;
    }

    if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
        m_lastError = "AttachCurrentThread не удался";
        return BridgeStatus::JniFailed;
    }

    m_env = env;
    return BridgeStatus::Ok;
}

void JniBridge::DetachThread() noexcept
{
    if (m_jvm == nullptr) return;
    static_cast<JavaVM*>(m_jvm)->DetachCurrentThread();
    m_env = nullptr;
}

BridgeStatus JniBridge::BindEndpoint(const std::string& slashClassName) noexcept
{
    auto* env = static_cast<JNIEnv*>(m_env);
    if (env == nullptr) {
        m_lastError = "поток не привязан к JVM (нужен AttachThread)";
        return BridgeStatus::JniFailed;
    }

    jclass local = env->FindClass(slashClassName.c_str());
    if (local == nullptr) {
        m_lastError = "класс " + slashClassName + " не найден: " + DescribeJavaException(env);
        return BridgeStatus::JniFailed;
    }

    auto* globalClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (globalClass == nullptr) {
        m_lastError = "NewGlobalRef не удался";
        return BridgeStatus::JniFailed;
    }

    m_endpointClass = globalClass;
    m_methodVoxel = env->GetStaticMethodID(globalClass, "onVoxelEdit", "(IIIB)V");
    m_methodReward = env->GetStaticMethodID(globalClass, "onReward", "(III)V");

    if (m_methodVoxel == nullptr || m_methodReward == nullptr) {
        m_lastError = "в классе нет ожидаемых статических методов onVoxelEdit/onReward";
        env->ExceptionClear();
        return BridgeStatus::JniFailed;
    }

    return BridgeStatus::Ok;
}

namespace {

/// Нативные callback'и, которые получает Java-класс через RegisterNatives.
/// Сигнатуры обязаны совпадать с Java-объявлениями (JNI-конвенция именования).
void JNICALL NativeOnVoxelEditToJava(JNIEnv*, jclass, jint x, jint y, jint z, jbyte blockId);
void JNICALL NativeOnRewardToJava(JNIEnv*, jclass, jint playerId, jint blockKind, jint count);

/// Указатель на приёмник, который Java-вызовы пересылают в бридж.
/// Один процесс — один бридж, поэтому статическая переменная здесь уместна.
void (*g_javaVoxelSink)(int32_t, int32_t, int32_t, uint8_t) = nullptr;
void (*g_javaRewardSink)(int32_t, int32_t, int32_t) = nullptr;

void JNICALL NativeOnVoxelEditToJava(JNIEnv*, jclass, jint x, jint y, jint z, jbyte blockId)
{
    if (g_javaVoxelSink != nullptr) g_javaVoxelSink(x, y, z, static_cast<uint8_t>(blockId));
}

void JNICALL NativeOnRewardToJava(JNIEnv*, jclass, jint playerId, jint blockKind, jint count)
{
    if (g_javaRewardSink != nullptr) g_javaRewardSink(playerId, blockKind, count);
}

} // namespace

void SetJavaCallbacks(void (*voxelSink)(int32_t, int32_t, int32_t, uint8_t),
                      void (*rewardSink)(int32_t, int32_t, int32_t)) noexcept
{
    g_javaVoxelSink = voxelSink;
    g_javaRewardSink = rewardSink;
}

BridgeStatus JniBridge::RegisterNatives() noexcept
{
    auto* env = static_cast<JNIEnv*>(m_env);
    if (env == nullptr || m_endpointClass == nullptr) {
        m_lastError = "эндпоинт не привязан (нужен BindEndpoint)";
        return BridgeStatus::JniFailed;
    }

    // Java-класс объявляет:
    //     static native void voxelEditFromNative(int x, int y, int z, byte blockId);
    //     static native void rewardFromNative(int playerId, int blockKind, int count);
    // Регистрируем вместо них свои C++-функции — это и есть мост Java → C++.
    JNINativeMethod methods[] = {
        { const_cast<char*>("voxelEditFromNative"), const_cast<char*>("(IIIB)V"), reinterpret_cast<void*>(&NativeOnVoxelEditToJava) },
        { const_cast<char*>("rewardFromNative"), const_cast<char*>("(III)V"), reinterpret_cast<void*>(&NativeOnRewardToJava) },
    };

    const jint result = env->RegisterNatives(static_cast<jclass>(m_endpointClass), methods, 2);
    if (result != JNI_OK) {
        m_lastError = "RegisterNatives вернул " + std::to_string(result) + ": " + DescribeJavaException(env);
        return BridgeStatus::JniFailed;
    }

    return BridgeStatus::Ok;
}

BridgeStatus JniBridge::CallVoxelEdit(int32_t x, int32_t y, int32_t z, uint8_t blockId) noexcept
{
    auto* env = static_cast<JNIEnv*>(m_env);
    if (env == nullptr || m_endpointClass == nullptr || m_methodVoxel == nullptr) return BridgeStatus::JniFailed;

    env->CallStaticVoidMethod(static_cast<jclass>(m_endpointClass), static_cast<jmethodID>(m_methodVoxel),
                              x, y, z, static_cast<jbyte>(blockId));

    if (env->ExceptionCheck() == JNI_TRUE) {
        m_lastError = DescribeJavaException(env);
        return BridgeStatus::JniFailed;
    }
    return BridgeStatus::Ok;
}

BridgeStatus JniBridge::CallReward(int32_t playerId, int32_t blockKind, int32_t count) noexcept
{
    auto* env = static_cast<JNIEnv*>(m_env);
    if (env == nullptr || m_endpointClass == nullptr || m_methodReward == nullptr) return BridgeStatus::JniFailed;

    env->CallStaticVoidMethod(static_cast<jclass>(m_endpointClass), static_cast<jmethodID>(m_methodReward),
                              playerId, blockKind, count);

    if (env->ExceptionCheck() == JNI_TRUE) {
        m_lastError = DescribeJavaException(env);
        return BridgeStatus::JniFailed;
    }
    return BridgeStatus::Ok;
}

#endif // GWYC_WITH_JNI

// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ITransport> MakeTransport(const BridgeConfig& config)
{
    switch (config.transport) {
        case TransportKind::NamedPipe:
            return std::make_unique<NamedPipeTransport>();
        case TransportKind::SharedMemory:
        default:
            return std::make_unique<SharedMemoryTransport>();
    }
}

} // namespace gwyc::bridge
