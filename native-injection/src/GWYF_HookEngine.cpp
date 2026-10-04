// ============================================================================
//  GWYF_HookEngine.cpp — нативная DLL, которая внедряется в процесс игры
//  Gamble With Your Friends и превращает игровые события в IPC-записи.
//
//  ЧТО ИМЕННО ПЕРЕХВАТЫВАЕТСЯ
//  --------------------------
//  Игра — Unity на Mono (Assembly-CSharp.dll), поэтому вместо поиска офсетов
//  используются два уровня перехвата:
//
//   1. mode=invoke — хук на mono_runtime_invoke (экспорт Mono). Это
//      документированная точка входа для любого вызова managed-кода из
//      нативного кода, включая рефлексию, сериализацию и Mirror-сообщения.
//      Хук НЕ меняет поведение игры: мы смотрим аргументы и вызываем оригинал.
//      Совпадение цели определяем по паре (класс, метод).
//
//   2. mode=method — хук на JIT-адрес конкретного метода, полученный через
//      mono_compile_method. Быстрее и точнее (ловим и обычные вызовы, не
//      только рефлексию), но требует, чтобы сигнатура метода совпала с одной
//      из безопасных раскладок аргументов. Раскладка проверяется по
//      CIL-сигнатуре (mono_signature_get_params + mono_type_get_type) ДО
//      установки хука — при несовпадении хук не ставится вовсе.
//
//  ЧЕГО ЗДЕСЬ СОЗНАТЕЛЬНО НЕТ
//  --------------------------
//  Мы не пишем в память игры и не подменяем игровую логику: движок только
//  читает аргументы и поля и публикует их в общий регион.
//  Единственная «пишущая» часть — спавн кубов через штатные Unity-вызовы
//  (VoxelTo3DWorld.cpp), и она выполняется ТОЛЬКО на главном потоке игры.
//
//  ПОТОКИ
//  ------
//  * игровой (main) поток — Unity API, polling managed-полей, спавн объектов;
//  * поток детура mono_runtime_invoke — управляемые вызовы; здесь только
//    чтение аргументов и запись в lock-free кольцо;
//  * IPC-воркер — ожидание событий шины, разбор входящих записей.
//  Взаимодействие между ними — только через атомарные кольца без блокировок.
// ============================================================================
#include "gwyfbridge/Common.h"
#include "gwyfbridge/HookEngine.h"
#include "gwyfbridge/IpcBridge.h"
#include "gwyfbridge/McWireFormat.h"
#include "gwyfbridge/MonoRuntime.h"
#include "gwyfbridge/PatternScan.h"
#include "gwyfbridge/TargetProfile.h"
#include "gwyfbridge/VoxelWorld.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace gwyf;

namespace {

// ── Хелперы ─────────────────────────────────────────────────────────────────

/// Привести managed-строку к std::string, обрезая управляющие символы.
/// Нужно потому, что имена из runtime попадают в лог и в IPC: нефильтрованная
/// строка с "\n" или NUL внутри ломала бы и разметку файла, и текстовый протокол.
std::string SanitizeManagedText(const char* text, usize maxLength = 256) {
    if (text == nullptr) return std::string();

    std::string result;
    result.reserve(std::min<usize>(std::strlen(text), maxLength));

    for (usize index = 0; text[index] != '\0' && result.size() < maxLength; ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch >= 0x20 && ch < 0x7F) {
            result += static_cast<char>(ch);
        } else if (ch == '\n' || ch == '\r' || ch == '\t') {
            result += ' ';
        } else if (ch != '\0') {
            result += '?';  // не-ASCII (например, кириллица из Unity) — не рвём лог
        }
    }
    return result;
}

/// Длина строки с ограничением (strnlen_s есть не во всех тулчейнах).
usize BoundedLength(const char* text, usize limit) {
    if (text == nullptr) return 0;
    usize length = 0;
    while (length < limit && text[length] != '\0') ++length;
    return length;
}

/// Собрать строку из managed-объекта.
///
/// Используется диагностикой в Publish(): при включённом уровне Debug строковые
/// аргументы перехваченного метода попадают в журнал. Это единственный способ
/// увидеть, что игра реально передала в casino-метод (номер стола, имя лобби),
/// потому что в нагрузку протокола такие значения не помещаются.
std::string ManagedToString(void* object) {
    if (object == nullptr || !mono::Fn().object_to_string || !mono::Fn().string_to_utf8) return std::string();

    void* exception = nullptr;
    void* text = mono::Fn().object_to_string(object, &exception);
    if (text == nullptr || exception != nullptr) return std::string();

    char* utf8 = mono::Fn().string_to_utf8(text);
    if (utf8 == nullptr) return std::string();

    std::string result = SanitizeManagedText(utf8);
    if (mono::Fn().free_memory != nullptr) mono::Fn().free_memory(utf8);
    return result;
}

// ── Продюсер входящих правок вокселей (для IPC-воркера) ─────────────────────

struct PendingVoxel {
    ipc::PayloadVoxel voxel{};
    u64 producerId = 0;
};

/// Очередь входящих правок: пишет IPC-воркер, читает главный поток.
/// Намеренно отдельная от VoxelMirror, чтобы воркер не трогал его структуры.
struct InboundVoxelQueue {
    static constexpr u32 Capacity = 8192;
    PendingVoxel slots[Capacity]{};
    u64 head = 0;
    u64 tail = 0;
    u64 dropped = 0;

#if defined(_MSC_VER)
#define GWYF_ENGINE_BARRIER() _ReadWriteBarrier()
#else
#define GWYF_ENGINE_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

    bool Push(const PendingVoxel& item) {
        if (head - tail >= Capacity) {
            dropped++;
            return false;
        }
        slots[head % Capacity] = item;
        GWYF_ENGINE_BARRIER();
        head++;
        return true;
    }

    bool Pop(PendingVoxel& out) {
        GWYF_ENGINE_BARRIER();
        if (tail == head) return false;
        out = slots[tail % Capacity];
        tail++;
        return true;
    }
};

/// Команды от Minecraft: выполняются на главном потоке.
struct PendingCommand {
    ipc::CommandCode code = ipc::CommandCode::None;
    std::string text;
    u32 arg0 = 0;
    u32 arg1 = 0;
};

}  // namespace

namespace gwyf::engine {

// ── Правило сопоставления: одна цель профиля ────────────────────────────────

struct MatchRule {
    const profile::WatchSpec* spec = nullptr;
    std::vector<mono::ArgKind> argKinds;
    void* klass = nullptr;
    void* method = nullptr;
    void* entry = nullptr;      ///< FnPtr() — адрес нашей детур-функции
    void** originalSlot = nullptr;
    bool resolved = false;
    bool hookInstalled = false;

    /// Последнее опубликованное значение (для on_change_only у poll-целей).
    i64 lastValue = 0;
    u64 lastPollMs = 0;

    [[nodiscard]] std::string Name() const {
        return spec != nullptr ? spec->className + "." + spec->methodName + spec->fieldName : "<правило>";
    }
};

/// Внутреннее состояние движка.
class Engine {
public:
    static Engine& Instance() {
        static Engine instance;
        return instance;
    }

    // ── Жизненный цикл ──────────────────────────────────────────────────────

    bool Initialize() {
        if (initialized_) return true;
        if (shuttingDown_) return false;

        const u64 now = NowTickMs();
        if (now - lastInitAttemptMs_ < 500) return false;  // не спамим попытками
        lastInitAttemptMs_ = now;

        // 1. Профиль (нужен раньше лога — из него берём имя файла лога).
        LoadProfile();

        // 2. Лог.
        if (!logOpened_) {
            log::Open(profile_.logPath, log::Level::Info);
            logOpened_ = true;
            GWYF_INFO("=== GWYF_HookEngine загружен в процесс %lu (модуль %s) ===",
                      GetCurrentProcessId(), modulePath_.c_str());
            GWYF_INFO("Профиль: %s", profilePath_.c_str());
        }

        // 3. Mono.
        if (!mono::Load()) {
            initAttempts_++;
            if (initAttempts_ == 1) {
                GWYF_WARN("Mono пока недоступен — жду загрузки Unity (попытка будет повторяться).");
            }
            return false;
        }

        // 4. Сборка игры: ждём, пока Unity поднимет Assembly-CSharp.
        if (mono::FindImage(profile_.gameAssembly.c_str()) == nullptr) {
            initAttempts_++;
            if (initAttempts_ == 1 || initAttempts_ % 20 == 0) {
                GWYF_INFO("Жду загрузки сборки %s (попытка %d)…", profile_.gameAssembly.c_str(), initAttempts_);
            }
            return false;
        }

        GWYF_INFO("Runtime готов: Mono из %s, сборка %s найдена", mono::ModuleName(), profile_.gameAssembly.c_str());

        // 5. Разрешение целей профиля.
        ResolveWatches();

        // 6. Хуки.
        if (profile_.enableInvokeHook) InstallInvokeHook();
        InstallMethodHooks();

        // 7. Адаптер спавна.
        std::string adapterError;
        adapter_ = voxel::CreateAdapter(profile_.voxel, profile_.gameAssembly, &adapterError);
        if (adapter_ != nullptr && adapter_->Ready()) {
            GWYF_INFO("Адаптер спавна готов: %s", adapter_->Name());
        } else {
            GWYF_WARN("Адаптер спавна недоступен (%s). Кубы создаваться не будут, остальное работает.",
                      adapterError.c_str());
        }

        mirror_.Configure(profile_.voxel);

        // 8. Шина.
        OpenBridge();

        // 9. Воркер IPC.
        StartIpcWorker();

        initialized_ = true;
        GWYF_INFO("Инициализация завершена. %s", bridge_.IsOpen() ? bridge_.Describe().c_str() : "IPC закрыт");

        if (bridge_.IsOpen()) {
            ipc::Payload payload{};
            std::snprintf(reinterpret_cast<char*>(payload.raw), ipc::kInlinePayload,
                          "game pid %lu", GetCurrentProcessId());
            bridge_.Out().Push(ipc::RecordType::Hello, payload);
        }

        return true;
    }

    void Shutdown() {
        if (shuttingDown_) return;
        shuttingDown_ = true;

        GWYF_INFO("Остановка движка…");

        // Сначала снимаем хуки: после этого в детуры никто не войдёт.
        hook::UninstallAll();

        if (workerRunning_.load()) {
            workerStop_.store(true);
            if (workerThread_ != nullptr) {
                WaitForSingleObject(workerThread_, 2000);
                CloseHandle(workerThread_);
                workerThread_ = nullptr;
            }
            workerRunning_.store(false);
        }

        if (adapter_ != nullptr && adapter_->Ready()) {
            mirror_.DespawnAll(*adapter_);
        }

        if (bridge_.IsOpen()) {
            bridge_.Out().PushSimple(ipc::RecordType::Bye);
            bridge_.Close();
        }

        adapter_.reset();
        log::Close();
    }

    // ── Главный поток ───────────────────────────────────────────────────────

    /// Вызывается из хука оконных сообщений (то есть на главном потоке игры).
    void OnMainThreadFrame() {
        if (shuttingDown_) return;

        if (!initialized_) {
            Initialize();
            return;
        }

        const u64 now = NowTickMs();

        // 1. Команды из Minecraft.
        DrainCommands();

        // 2. Входящие правки вокселей → спавн кубов (Unity API — только здесь).
        if (adapter_ != nullptr && adapter_->Ready()) {
            PendingVoxel item;
            u32 processed = 0;
            while (processed < 4096 && inboundVoxels_.Pop(item)) {
                mirror_.PushIncoming(item.voxel);
                ++processed;
            }

            std::string error;
            mirror_.ProcessOnMainThread(*adapter_, profile_.voxel.budgetPerFrame, &error);
        }

        // 3. Периодический опрос состояния (поля managed-объектов).
        if (profile_.enableStatePolling) PollWatches(now);

        // 4. heartbeat + телеметрия.
        if (now - lastHeartbeatMs_ >= profile_.heartbeatMs) {
            lastHeartbeatMs_ = now;
            bridge_.Out().Heartbeat();
            PublishStatusLine();
        }
    }

    // ── Публикация событий из детуров ───────────────────────────────────────

    /// Найти правило, соответствующее managed-методу. Кэш только добавляется,
    /// поэтому синхронизация не нужна: указатели пишутся до publish индекса.
    MatchRule* MatchMethod(void* method, void* klass) {
        if (method == nullptr || rules_.empty()) return nullptr;

        const u32 cachedCount = matchCacheCount_.load(std::memory_order_acquire);
        for (u32 index = 0; index < cachedCount; ++index) {
            if (matchCache_[index].method == method) {
                return matchCache_[index].ruleIndex == kNoMatch ? nullptr : &rules_[matchCache_[index].ruleIndex];
            }
        }

        const char* methodNameRaw = mono::Fn().method_get_name != nullptr ? mono::Fn().method_get_name(method) : nullptr;
        const char* classNameRaw = (klass != nullptr && mono::Fn().class_get_name != nullptr) ? mono::Fn().class_get_name(klass) : nullptr;

        if (methodNameRaw == nullptr || classNameRaw == nullptr) {
            CacheMatch(method, kNoMatch);
            return nullptr;
        }

        const std::string methodName = SanitizeManagedText(methodNameRaw, 128);
        const std::string className = SanitizeManagedText(classNameRaw, 128);

        for (u32 index = 0; index < rules_.size(); ++index) {
            const MatchRule& rule = rules_[index];
            if (rule.spec == nullptr || rule.spec->className.empty()) continue;

            // Сравниваем короткое имя класса: профиль может указывать "Game.BetManager",
            // а runtime вернуть только "BetManager".
            const std::string& wanted = rule.spec->className;
            const usize dot = wanted.rfind('.');
            const std::string wantedShort = dot == std::string::npos ? wanted : wanted.substr(dot + 1);

            if (className != wantedShort && className != wanted) continue;
            if (!rule.spec->methodName.empty() && methodName != rule.spec->methodName) continue;

            CacheMatch(method, index);
            GWYF_INFO("Цель найдена: %s (method=%p)", rule.Name().c_str(), method);
            return &rules_[index];
        }

        CacheMatch(method, kNoMatch);
        return nullptr;
    }

    /// Опубликовать событие по правилу. Вызывается из детура (managed-поток).
    void Publish(const MatchRule& rule, const profile::EvalContext& context) {
        if (!bridge_.IsOpen()) return;

        ipc::Payload payload{};
        u32 applied = 0;

        for (const profile::Binding& binding : rule.spec->bindings) {
            i64 intValue = 0;
            double floatValue = 0.0;
            bool isFloat = false;

            float floatValueF = 0.0f;
            if (!EvaluateValue(binding.source, context, intValue, floatValueF, isFloat)) {
                continue;
            }
            floatValue = floatValueF;

            if (profile::ApplyToPayload(payload, binding.target, intValue, static_cast<float>(floatValue), isFloat)) {
                ++applied;
            }
        }

        if (applied == 0) {
            static std::atomic<u32> warned{0};
            if (warned.fetch_add(1) < 8) {
                const char* eventName = ipc::ToString(rule.spec->event);
                GWYF_WARN("Событие %s: ни одна привязка не сработала — проверьте map= в профиле", eventName);
            }
        }

        // Диагностика уровня Debug: строковые аргументы метода попадают в лог.
        // Нужна при калибровке профиля под конкретную сборку игры — видно, ЧТО
        // именно пришло в метод (идентификатор стола, имя лобби, текст ошибки),
        // а не только числа. Раскладка аргументов уже посчитана на этапе
        // привязки (rule.argKinds), так что метаданные Mono здесь не читаются.
        // Вызов mono_object_to_string уводит управление в managed-код, поэтому
        // он возможен только по явному запросу: уровень Debug, не Info.
        if (log::GetLevel() <= log::Level::Debug) {
            for (usize index = 0; index < rule.argKinds.size() && index < 16; ++index) {
                if (rule.argKinds[index] != mono::ArgKind::Object) continue;
                const std::string text = ManagedToString(context.args[index]);
                if (!text.empty()) {
                    GWYF_DEBUG("%s: аргумент %zu = \"%s\"", rule.Name().c_str(), index, text.c_str());
                }
            }
        }

        bridge_.Out().Push(rule.spec->event, payload, static_cast<u64>(reinterpret_cast<u64>(context.instance)));
    }

    /// Обработка переопределённых методов (mode=method).
    void OnMethodHook(MatchRule& rule, const profile::EvalContext& context) {
        Publish(rule, context);
    }

    [[nodiscard]] bool Ready() const { return initialized_; }
    [[nodiscard]] bool ShutdownRequested() const { return shuttingDown_; }

    void RequestShutdown() { shuttingDown_ = true; }

    std::string StatusText() {
        std::string text = "GWYF_HookEngine: ";
        text += initialized_ ? "инициализирован" : "ожидает Unity/Mono";
        text += "\n  Mono: " + std::string(mono::IsLoaded() ? mono::ModuleName() : "не загружен");
        text += ", сборка: " + profile_.gameAssembly;
        text += "\n  " + (bridge_.IsOpen() ? bridge_.Describe() : std::string("IPC закрыт"));
        text += "\n  " + mirror_.Describe();
        text += "\n  " + hook::Describe();
        return text;
    }

    // ── Команды из Minecraft ────────────────────────────────────────────────

    void EnqueueCommand(const PendingCommand& command) {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (pendingCommands_.size() >= 64) {
            GWYF_WARN("Очередь команд переполнена — команда %s отброшена", ipc::ToString(command.code));
            return;
        }
        pendingCommands_.push_back(command);
    }

private:
    Engine() = default;

    static constexpr u32 kNoMatch = 0xFFFFFFFFu;

    struct CacheEntry {
        void* method = nullptr;
        u32 ruleIndex = kNoMatch;
    };

    void CacheMatch(void* method, u32 ruleIndex) {
        const u32 slot = matchCacheCount_.load(std::memory_order_relaxed);
        if (slot >= kCacheSize) return;

        matchCache_[slot].method = method;
        matchCache_[slot].ruleIndex = ruleIndex;
        matchCacheCount_.store(slot + 1, std::memory_order_release);
    }

    // ── Профиль ─────────────────────────────────────────────────────────────

    void LoadProfile() {
        char buffer[MAX_PATH]{};
        GetModuleFileNameA(moduleHandle_, buffer, MAX_PATH);
        modulePath_ = buffer;

        const usize slash = modulePath_.find_last_of("\\/");
        const std::string directory = slash == std::string::npos ? "." : modulePath_.substr(0, slash);

        char envPath[512]{};
        const DWORD envLen = GetEnvironmentVariableA("GWYF_PROFILE", envPath, sizeof(envPath));
        profilePath_ = (envLen > 0) ? std::string(envPath) : directory + "\\gwyf.profile";

        profile::LoadResult result = profile::Load(profilePath_, profile_);
        if (result.usedDefaults) {
            const std::string templateText = profile::TemplateText();
            FILE* file = nullptr;
            if (fopen_s(&file, profilePath_.c_str(), "wt") == 0 && file != nullptr) {
                std::fwrite(templateText.data(), 1, templateText.size(), file);
                std::fclose(file);
            }
        }

        if (!result.ok) {
            GWYF_ERROR("Профиль не загружен: %s", result.error.c_str());
            profile_ = profile::DefaultForGambleWithYourFriends();
        }
    }

    // ── Разрешение целей ────────────────────────────────────────────────────

    void ResolveWatches() {
        rules_.clear();
        matchCacheCount_.store(0);

        for (const profile::WatchSpec& spec : profile_.watches) {
            if (spec.mode == profile::ResolveMode::Disabled) continue;

            MatchRule rule{};
            rule.spec = &spec;

            switch (spec.mode) {
                case profile::ResolveMode::Invoke:
                case profile::ResolveMode::Method: {
                    std::string nameSpace;
                    std::string className;
                    SplitClassName(spec.className, nameSpace, className);

                    rule.klass = mono::FindClass(profile_.gameAssembly.c_str(),
                                                 nameSpace.empty() ? nullptr : nameSpace.c_str(),
                                                 className.c_str());
                    if (rule.klass == nullptr) {
                        GWYF_WARN("Цель [%s]: класс %s не найден в %s — правило отключено",
                                  spec.section.c_str(), spec.className.c_str(), profile_.gameAssembly.c_str());
                        break;
                    }

                    if (!spec.methodName.empty()) {
                        rule.method = mono::FindMethodInHierarchy(rule.klass, spec.methodName.c_str(), spec.paramCount);
                        if (rule.method == nullptr) {
                            GWYF_WARN("Цель [%s]: метод %s(%d) не найден — правило отключено",
                                      spec.section.c_str(), spec.methodName.c_str(), spec.paramCount);
                            break;
                        }
                        rule.argKinds = mono::ClassifyArgs(rule.method);
                    }

                    rule.resolved = true;
                    GWYF_INFO("Цель [%s] разрешена: %s", spec.section.c_str(),
                              mono::DescribeMethodArgs(rule.method).c_str());
                    break;
                }

                case profile::ResolveMode::Poll:
                case profile::ResolveMode::Export:
                case profile::ResolveMode::Rva:
                case profile::ResolveMode::Pattern:
                    rule.resolved = true;
                    break;

                default:
                    break;
            }

            if (rule.resolved) {
                rules_.push_back(std::move(rule));
            }
        }

        GWYF_INFO("Разрешено целей: %zu из %zu", rules_.size(), profile_.watches.size());
    }

    // ── Раскладки аргументов для mode=method ────────────────────────────────
public:
    /// Типобезопасный детур: C++ сам считает аргументы из шаблона, а раскладку
    /// мы сверяем с CIL-сигнатурой до установки хука. Так исключена классическая
    /// ошибка «вызов метода через указатель неверной сигнатуры».
    template <typename... TArgs>
    struct MethodThunk {
        using FnPtr = void (*)(void*, TArgs...);

        static inline FnPtr original = nullptr;
        static inline MatchRule* rule = nullptr;

        static void Detour(void* self, TArgs... args) {
            MatchRule* current = rule;
            if (current != nullptr && Engine::Instance().initialized_ && !Engine::Instance().shuttingDown_) {
                profile::EvalContext context{};
                context.instance = self;
                u32 index = 0;
                (StoreArg(context, index, args), ...);
                context.argCount = index;
                Engine::Instance().OnMethodHook(*current, context);
            }

            if (original != nullptr) {
                original(self, args...);
            }
        }

    private:
        /// Разложить аргумент в контекст: целые/указатели — в args, float — в floatArgs.
        template <typename T>
        static void StoreArg(profile::EvalContext& context, u32& index, T value) {
            if (index >= 16) return;

            if constexpr (std::is_same_v<T, float>) {
                context.floatArgs[index] = value;
                context.args[index] = reinterpret_cast<void*>(static_cast<u64>(static_cast<i64>(value)));
            } else if constexpr (std::is_same_v<T, double>) {
                context.doubleArgs[index] = value;
                context.floatArgs[index] = static_cast<float>(value);
                context.args[index] = reinterpret_cast<void*>(static_cast<u64>(static_cast<i64>(value)));
            } else if constexpr (std::is_pointer_v<T>) {
                context.args[index] = const_cast<void*>(reinterpret_cast<const void*>(value));
            } else if constexpr (std::is_integral_v<T>) {
                context.args[index] = reinterpret_cast<void*>(static_cast<u64>(static_cast<i64>(value)));
            } else {
                context.args[index] = nullptr;
            }
            ++index;
        }
    };

    /// Описание поддерживаемой раскладки аргументов: сама детур-функция,
    /// слот для адреса оригинала и слот, куда движок положит указатель на правило.
    struct Thunk {
        const char* shape;
        void* detour;
        void** originalSlot;
        MatchRule** ruleSlot;
        std::vector<mono::ArgKind> kinds;
    };

    static const std::vector<Thunk>& Thunks() {
        static const std::vector<Thunk> thunks = {
            {"void", reinterpret_cast<void*>(&MethodThunk<>::Detour),
             reinterpret_cast<void**>(&MethodThunk<>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<>::rule), {}},
            {"i32", reinterpret_cast<void*>(&MethodThunk<i32>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i32>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i32>::rule), {mono::ArgKind::Int32}},
            {"i32_i32", reinterpret_cast<void*>(&MethodThunk<i32, i32>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i32, i32>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i32, i32>::rule), {mono::ArgKind::Int32, mono::ArgKind::Int32}},
            {"i32_i32_i32", reinterpret_cast<void*>(&MethodThunk<i32, i32, i32>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i32, i32, i32>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i32, i32, i32>::rule),
             {mono::ArgKind::Int32, mono::ArgKind::Int32, mono::ArgKind::Int32}},
            {"i64", reinterpret_cast<void*>(&MethodThunk<i64>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i64>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i64>::rule), {mono::ArgKind::Int64}},
            {"f32", reinterpret_cast<void*>(&MethodThunk<float>::Detour),
             reinterpret_cast<void**>(&MethodThunk<float>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<float>::rule), {mono::ArgKind::Float32}},
            {"f32_f32", reinterpret_cast<void*>(&MethodThunk<float, float>::Detour),
             reinterpret_cast<void**>(&MethodThunk<float, float>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<float, float>::rule), {mono::ArgKind::Float32, mono::ArgKind::Float32}},
            {"i32_f32", reinterpret_cast<void*>(&MethodThunk<i32, float>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i32, float>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i32, float>::rule), {mono::ArgKind::Int32, mono::ArgKind::Float32}},
            {"f32_i32", reinterpret_cast<void*>(&MethodThunk<float, i32>::Detour),
             reinterpret_cast<void**>(&MethodThunk<float, i32>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<float, i32>::rule), {mono::ArgKind::Float32, mono::ArgKind::Int32}},
            {"i32_i32_f32", reinterpret_cast<void*>(&MethodThunk<i32, i32, float>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i32, i32, float>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i32, i32, float>::rule),
             {mono::ArgKind::Int32, mono::ArgKind::Int32, mono::ArgKind::Float32}},
            {"obj", reinterpret_cast<void*>(&MethodThunk<void*>::Detour),
             reinterpret_cast<void**>(&MethodThunk<void*>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<void*>::rule), {mono::ArgKind::Object}},
            {"obj_i32", reinterpret_cast<void*>(&MethodThunk<void*, i32>::Detour),
             reinterpret_cast<void**>(&MethodThunk<void*, i32>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<void*, i32>::rule), {mono::ArgKind::Object, mono::ArgKind::Int32}},
            {"i32_obj", reinterpret_cast<void*>(&MethodThunk<i32, void*>::Detour),
             reinterpret_cast<void**>(&MethodThunk<i32, void*>::original),
             reinterpret_cast<MatchRule**>(&MethodThunk<i32, void*>::rule), {mono::ArgKind::Int32, mono::ArgKind::Object}},
        };
        return thunks;
    }

private:
    static const Thunk* FindThunk(const std::string& shape) {
        for (const Thunk& thunk : Thunks()) {
            if (shape == thunk.shape) return &thunk;
        }
        return nullptr;
    }

    static std::string SupportedShapes() {
        std::string text;
        for (const Thunk& thunk : Thunks()) {
            if (!text.empty()) text += ", ";
            text += thunk.shape;
        }
        return text;
    }

    // ── Установка хуков ─────────────────────────────────────────────────────

    void InstallInvokeHook() {
        if (!hook::Initialize()) return;

        void* target = mono::Fn().runtime_invoke_address;
        if (target == nullptr) {
            GWYF_WARN("mono_runtime_invoke недоступен — режим invoke отключён");
            return;
        }

        void* original = nullptr;
        if (!hook::Install("mono_runtime_invoke", target, reinterpret_cast<void*>(&DetourMonoInvoke), &original)) {
            return;
        }

        originalInvoke_ = original;
        GWYF_INFO("Перехват mono_runtime_invoke активен — события managed-вызовов будут видны мосту.");
    }

    void InstallMethodHooks() {
        for (MatchRule& rule : rules_) {
            if (rule.spec == nullptr) continue;
            if (rule.spec->mode != profile::ResolveMode::Method) continue;
            if (rule.method == nullptr) continue;

            const Thunk* thunk = FindThunk(rule.spec->shape);
            if (thunk == nullptr) {
                const std::string shapes = SupportedShapes();
                GWYF_ERROR("Цель [%s]: неизвестная раскладка «%s». Доступны: %s",
                           rule.spec->section.c_str(), rule.spec->shape.c_str(), shapes.c_str());
                continue;
            }

            if (!ValidateShape(*rule.spec, rule.argKinds, *thunk)) continue;

            void* entry = mono::CompileMethod(rule.method);
            if (entry == nullptr) continue;

            void* original = nullptr;
            const std::string hookName = "method:" + rule.spec->className + "." + rule.spec->methodName;
            if (!hook::Install(hookName, entry, thunk->detour, &original)) continue;

            *thunk->originalSlot = original;
            *thunk->ruleSlot = &rule;
            rule.originalSlot = thunk->originalSlot;
            rule.entry = thunk->detour;
            rule.hookInstalled = true;
        }
    }

    /// Сверить раскладку хука с реальной CIL-сигнатурой.
    /// Несовпадение = отказ от установки: лучше не иметь хитрого хука,
    /// чем испортить вызов метода и уронить игру.
    bool ValidateShape(const profile::WatchSpec& spec, const std::vector<mono::ArgKind>& actual, const Thunk& thunk) {
        if (actual.empty()) {
            GWYF_WARN("Цель [%s]: типы аргументов недоступны (нет экспорта mono_type_get_type) — "
                      "ставлю хук по раскладке из профиля, риск несовпадения остаётся",
                      spec.section.c_str());
            return true;
        }

        if (actual.size() != thunk.kinds.size()) {
            GWYF_ERROR("Цель [%s]: в профиле раскладка «%s» на %zu аргументов, а метод принимает %zu — хук не ставлю",
                       spec.section.c_str(), spec.shape.c_str(), thunk.kinds.size(), actual.size());
            return false;
        }

        for (usize index = 0; index < actual.size(); ++index) {
            if (actual[index] == mono::ArgKind::Unknown) continue;
            if (actual[index] != thunk.kinds[index]) {
                GWYF_ERROR("Цель [%s]: аргумент %zu в сигнатуре — %s, а раскладка «%s» ожидает %s. Хук не ставлю.",
                           spec.section.c_str(), index, mono::ArgKindName(actual[index]),
                           spec.shape.c_str(), mono::ArgKindName(thunk.kinds[index]));
                return false;
            }
        }

        return true;
    }

    // ── Опрос состояния (poll) ──────────────────────────────────────────────

    void PollWatches(u64 now) {
        if (!bridge_.IsOpen()) return;

        for (MatchRule& rule : rules_) {
            if (rule.spec == nullptr || rule.spec->mode != profile::ResolveMode::Poll) continue;
            if (rule.klass == nullptr || rule.spec->fieldName.empty()) continue;
            if (now - rule.lastPollMs < rule.spec->intervalMs) continue;
            rule.lastPollMs = now;

            void* field = mono::FindField(rule.klass, rule.spec->fieldName.c_str());
            if (field == nullptr) continue;

            i64 intValue = 0;
            double floatValue = 0.0;
            bool isFloat = false;

            // Пробуем статическое поле, потом экземплярное — так один профиль
            // работает и для синглтонов, и для компонентов на сцене.
            if (!mono::ReadStaticFieldTyped(rule.klass, field, intValue, floatValue, isFloat)) {
                void* instance = FindSingletonInstance(rule.klass);
                if (instance == nullptr) continue;
                if (!mono::ReadFieldTyped(instance, field, intValue, floatValue, isFloat)) continue;
            }

            if (rule.spec->publishOnChangeOnly && !isFloat && intValue == rule.lastValue) continue;
            rule.lastValue = intValue;

            profile::EvalContext context{};
            context.polledValue = intValue;
            context.instance = nullptr;
            context.floatArgs[0] = static_cast<float>(isFloat ? floatValue : static_cast<double>(intValue));
            context.argCount = 1;

            ipc::Payload payload{};
            u32 applied = 0;
            for (const profile::Binding& binding : rule.spec->bindings) {
                bool bound = false;
                if (binding.source == "value") {
                    bound = profile::ApplyToPayload(payload, binding.target,
                                                    isFloat ? static_cast<i64>(floatValue) : intValue,
                                                    static_cast<float>(isFloat ? floatValue : static_cast<double>(intValue)),
                                                    isFloat);
                } else {
                    i64 valueInt = 0;
                    float valueFloat = 0.0f;
                    bool valueIsFloat = false;
                    if (EvaluateValue(binding.source, context, valueInt, valueFloat, valueIsFloat)) {
                        bound = profile::ApplyToPayload(payload, binding.target, valueInt, valueFloat, valueIsFloat);
                    }
                }
                if (bound) ++applied;
            }

            if (applied > 0) {
                bridge_.Out().Push(rule.spec->event, payload);
            }
        }
    }

    /// Найти экземпляр managed-объекта для класса: перебираем статические поля
    /// класса в поисках ссылки на объект этого же типа (типичный "instance"-
    /// синглтон Unity/Mirror). Ничего не выдумываем: если не нашли — вернём nullptr.
    void* FindSingletonInstance(void* klass) {
        if (klass == nullptr || !mono::Fn().class_get_fields) return nullptr;

        for (const mono::FieldInfo& field : mono::EnumFields(klass)) {
            if (field.name != "instance" && field.name != "Instance" && field.name != "_instance" &&
                field.name != "singleton" && field.name != "Singleton" && field.name != "current") {
                continue;
            }

            i64 value = 0;
            double floatValue = 0.0;
            bool isFloat = false;
            if (!mono::ReadStaticFieldTyped(klass, field.field, value, floatValue, isFloat)) continue;
            if (value == 0) continue;

            void* object = reinterpret_cast<void*>(value);
            void* objectClass = mono::Fn().object_get_class != nullptr ? mono::Fn().object_get_class(object) : nullptr;
            if (objectClass == klass) return object;
        }

        return nullptr;
    }

    // ── Вычисление выражений из managed-мира ────────────────────────────────

    /// Разобрать источник значения: сначала простые формы (arg0/this/value),
    /// затем выражения вида field(this,"x") и static("Class","x").
    bool EvaluateValue(const std::string& source, const profile::EvalContext& context,
                       i64& intOut, float& floatOut, bool& isFloat) {
        double floatValue = 0.0;
        if (profile::EvaluateSource(source, context, intOut, floatOut, isFloat)) return true;

        if (EvaluateMonoExpression(source, context, intOut, floatValue, isFloat)) {
            floatOut = static_cast<float>(floatValue);
            return true;
        }
        return false;
    }

    bool EvaluateMonoExpression(const std::string& source, const profile::EvalContext& context,
                                i64& intOut, double& floatOut, bool& isFloat) {
        intOut = 0;
        floatOut = 0.0;
        isFloat = false;

        // field(this,"name") или field(arg0,"name")
        if (source.rfind("field(", 0) == 0 && source.back() == ')') {
            const std::string inner = source.substr(6, source.size() - 7);
            const usize comma = inner.find(',');
            if (comma == std::string::npos) return false;

            const std::string objectExpr = inner.substr(0, comma);
            std::string fieldName = inner.substr(comma + 1);
            if (fieldName.size() >= 2 && fieldName.front() == '"' && fieldName.back() == '"') {
                fieldName = fieldName.substr(1, fieldName.size() - 2);
            }
            if (fieldName.empty()) return false;

            void* object = nullptr;
            if (objectExpr == "this") {
                object = context.instance;
            } else {
                u32 index = 0;
                char kind = 'i';
                if (!ParseArgIndex(objectExpr, index, kind) || index >= context.argCount) return false;
                object = context.args[index];
                if (kind == 'f') {
                    // Аргумент-указатель приходит как float-биты — восстанавливаем.
                    const float value = context.floatArgs[index];
                    object = reinterpret_cast<void*>(static_cast<u64>(static_cast<i64>(value)));
                }
            }

            if (object == nullptr) return false;

            void* klass = mono::Fn().object_get_class != nullptr ? mono::Fn().object_get_class(object) : nullptr;
            if (klass == nullptr) return false;

            void* field = mono::FindField(klass, fieldName.c_str());
            if (field == nullptr) return false;

            return mono::ReadFieldTyped(object, field, intOut, floatOut, isFloat);
        }

        // static("Class","field")
        if (source.rfind("static(", 0) == 0 && source.back() == ')') {
            const std::string inner = source.substr(7, source.size() - 8);
            const usize comma = inner.find(',');
            if (comma == std::string::npos) return false;

            std::string className = inner.substr(0, comma);
            std::string fieldName = inner.substr(comma + 1);
            auto stripQuotes = [](std::string& text) {
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                    text = text.substr(1, text.size() - 2);
                }
            };
            stripQuotes(className);
            stripQuotes(fieldName);

            std::string nameSpace;
            std::string shortName;
            SplitClassName(className, nameSpace, shortName);

            void* klass = mono::FindClass(profile_.gameAssembly.c_str(),
                                          nameSpace.empty() ? nullptr : nameSpace.c_str(), shortName.c_str());
            if (klass == nullptr) return false;

            void* field = mono::FindField(klass, fieldName.c_str());
            if (field == nullptr) return false;

            return mono::ReadStaticFieldTyped(klass, field, intOut, floatOut, isFloat);
        }

        return false;
    }

    // ── IPC ─────────────────────────────────────────────────────────────────

    void OpenBridge() {
        if (bridge_.IsOpen()) return;

        std::string error;
        if (!bridge_.Open(ipc::Role::Game, profile_.regionName, BuildTag(), true, &error)) {
            GWYF_WARN("Не удалось открыть общую память: %s. Мост работать не будет, хук продолжит собирать телеметрию.",
                      error.c_str());
        }
    }

    static std::string BuildTag() {
        return "gwyf-hookengine/1";
    }

    void StartIpcWorker() {
        if (!bridge_.IsOpen() || workerRunning_.load()) return;

        workerStop_.store(false);
        workerThread_ = CreateThread(nullptr, 0, &Engine::IpcWorkerProc, this, 0, nullptr);
        if (workerThread_ == nullptr) {
            GWYF_ERROR("Не удалось создать поток IPC-воркера (GetLastError=%lu)", GetLastError());
            return;
        }
        workerRunning_.store(true);
        GWYF_INFO("IPC-воркер запущен");
    }

    static DWORD WINAPI IpcWorkerProc(LPVOID parameter) {
        auto* self = static_cast<Engine*>(parameter);
        self->IpcWorkerLoop();
        return 0;
    }

    void IpcWorkerLoop() {
        GWYF_INFO("IPC-воркер: поток %lu запущен", GetCurrentThreadId());

        u64 lastPeerCheck = 0;

        while (!workerStop_.load()) {
            // Ждём событие до 100 мс: так мы и реагируем быстро, и умеем
            // заметить остановку/пропажу партнёра.
            if (bridge_.IsOpen()) {
                bridge_.In().Wait(100);
            } else {
                Sleep(100);
            }

            if (!bridge_.IsOpen()) continue;

            if (bridge_.StopRequested()) {
                GWYF_INFO("Получен сигнал остановки от инжектора");
                shuttingDown_ = true;
                workerStop_.store(true);
                break;
            }

            bridge_.In().Drain([this](const ipc::Record& record) {
                HandleIncoming(record);
            }, profile_.maxEventsPerFrame);

            const u64 now = NowTickMs();
            if (now - lastPeerCheck > 3000) {
                lastPeerCheck = now;
                if (!bridge_.PeerAlive() && bridge_.PeerProcessExists()) {
                    GWYF_DEBUG("Партнёр Minecraft пока не отвечает пульсом");
                }
            }
        }

        GWYF_INFO("IPC-воркер остановлен");
    }

    void HandleIncoming(const ipc::Record& record) {
        switch (static_cast<ipc::RecordType>(record.type)) {
            case ipc::RecordType::Hello: {
                const char* raw = reinterpret_cast<const char*>(record.payload.raw);
                const std::string text(raw, BoundedLength(raw, ipc::kInlinePayload));
                GWYF_INFO("Minecraft подключился: %s", SanitizeManagedText(text.c_str()).c_str());
                break;
            }

            case ipc::RecordType::Bye:
                GWYF_WARN("Minecraft отключился от моста");
                break;

            case ipc::RecordType::VoxelEdit: {
                // Массовая заливка: в bulk-кольце лежит упакованный пакет,
                // а в записи — его длина (в aux) и число элементов.
                if (record.payload.voxel.action == static_cast<u32>(ipc::VoxelAction::BulkReplace)) {
                    HandleBulkVoxels(record);
                    break;
                }

                PendingVoxel item{};
                item.voxel = record.payload.voxel;
                item.producerId = record.sourceId;
                if (!inboundVoxels_.Push(item)) {
                    GWYF_WARN("Очередь входящих вокселей переполнена — правка %d,%d,%d потеряна",
                              item.voxel.x, item.voxel.y, item.voxel.z);
                }
                break;
            }

            case ipc::RecordType::Command: {
                PendingCommand command{};
                command.code = static_cast<ipc::CommandCode>(record.payload.command.code);
                command.arg0 = record.payload.command.arg0;
                command.arg1 = record.payload.command.arg1;

                if (record.payload.command.textLength > 0) {
                    std::string text;
                    u32 length = 0;
                    if (bridge_.In().TryReadBulk(text, length)) {
                        command.text = text;
                    }
                }

                EnqueueCommand(command);
                break;
            }

            case ipc::RecordType::Heartbeat:
                break;

            default:
                GWYF_DEBUG("Необработанная запись типа %s", ipc::ToString(static_cast<ipc::RecordType>(record.type)));
                break;
        }
    }

    /// Разобрать пакет массовой заливки: bulk-кадр → отдельные правки в очередь.
    void HandleBulkVoxels(const ipc::Record& record) {
        const u32 expected = static_cast<u32>(record.aux);
        if (expected == 0) {
            GWYF_WARN("Пакет вокселей пришёл пустым");
            return;
        }

        std::string frame;
        u32 frameLength = 0;
        if (!bridge_.In().TryReadBulk(frame, frameLength)) {
            GWYF_WARN("Обещан пакет вокселей на %u элементов, но bulk-кадр не прочитан", expected);
            return;
        }

        u32 queued = 0;
        const u32 parsed = mcwire::UnpackBulkEdits(reinterpret_cast<const u8*>(frame.data()), frameLength,
                                                  [&](const ipc::PayloadVoxel& edit) {
                                                      PendingVoxel item{};
                                                      item.voxel = edit;
                                                      if (inboundVoxels_.Push(item)) ++queued;
                                                  });

        if (queued < parsed) {
            GWYF_WARN("Очередь вокселей переполнена: из %u правок пакета принято %u", parsed, queued);
        }
    }

    // ── Команды на главном потоке ───────────────────────────────────────────

    void DrainCommands() {
        std::vector<PendingCommand> commands;
        {
            std::lock_guard<std::mutex> lock(commandMutex_);
            if (pendingCommands_.empty()) return;
            commands.swap(pendingCommands_);
        }

        for (const PendingCommand& command : commands) {
            ExecuteCommand(command);
        }
    }

    void ExecuteCommand(const PendingCommand& command) {
        switch (command.code) {
            case ipc::CommandCode::DumpClass:
                ExecuteDumpClass(command);
                break;

            case ipc::CommandCode::Status:
                Reply(command, ipc::AckStatus::Ok, StatusText());
                break;

            case ipc::CommandCode::ScanPattern:
                ExecuteScanPattern(command);
                break;

            case ipc::CommandCode::SpawnProbe:
                ExecuteSpawnProbe(command);
                break;

            case ipc::CommandCode::FlushWorld: {
                const u32 removed = (adapter_ != nullptr && adapter_->Ready()) ? mirror_.DespawnAll(*adapter_) : 0;
                Reply(command, ipc::AckStatus::Ok, "удалено объектов: " + std::to_string(removed));
                break;
            }

            case ipc::CommandCode::ReloadProfile: {
                profile::Profile fresh;
                profile::LoadResult result = profile::Load(profilePath_, fresh);
                Reply(command, result.ok ? ipc::AckStatus::Ok : ipc::AckStatus::BadRequest,
                      result.ok ? "профиль перечитан (перезапустите игру, чтобы применить хуки)"
                                : "ошибка профиля: " + result.error);
                break;
            }

            default:
                Reply(command, ipc::AckStatus::NotSupported,
                      std::string("команда ") + ipc::ToString(command.code) + " не поддерживается");
                break;
        }
    }

    void ExecuteDumpClass(const PendingCommand& command) {
        if (command.text.empty()) {
            Reply(command, ipc::AckStatus::BadRequest, "укажите имя класса, например Game.BetManager");
            return;
        }

        std::string nameSpace;
        std::string className;
        SplitClassName(command.text, nameSpace, className);

        void* klass = mono::FindClass(profile_.gameAssembly.c_str(),
                                      nameSpace.empty() ? nullptr : nameSpace.c_str(), className.c_str());
        if (klass == nullptr) {
            Reply(command, ipc::AckStatus::NotFound, "класс " + command.text + " не найден");
            return;
        }

        const std::string report = mono::DescribeClass(klass);
        const std::string path = "gwyf_dump_" + className + ".txt";
        const bool saved = mono::WriteReport(path, report);

        Reply(command, ipc::AckStatus::Ok,
              std::string("dump " + command.text + (saved ? " сохранён в " + path + "\n" : "\n") + report));
    }

    /// Поиск паттерна в модуле: текст команды имеет вид «модуль|паттерн»
    /// либо просто паттерн (тогда ищем в основном исполняемом модуле игры).
    void ExecuteScanPattern(const PendingCommand& command) {
        if (command.text.empty()) {
            Reply(command, ipc::AckStatus::BadRequest, "укажите паттерн, например: --module GameAssembly.dll --pattern \"48 8B ??\"");
            return;
        }

        std::string module = ".exe";
        std::string patternText = command.text;

        const usize separator = command.text.find('|');
        if (separator != std::string::npos) {
            module = command.text.substr(0, separator);
            patternText = command.text.substr(separator + 1);
        }

        const scan::Pattern pattern = scan::Parse(patternText);
        if (pattern.bytes.empty()) {
            Reply(command, ipc::AckStatus::BadRequest, "паттерн пуст или разобран неверно");
            return;
        }

        char modulePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, modulePath, MAX_PATH);

        const void* base = nullptr;
        usize size = 0;

        if (module == ".exe") {
            base = reinterpret_cast<const void*>(GetModuleHandleA(nullptr));
            size = 64ull * 1024 * 1024;  // верхняя оценка: сканер всё равно проверяет читаемость
        } else {
            ModuleInfo info{};
            if (!FindModule(module.c_str(), info)) {
                Reply(command, ipc::AckStatus::NotFound, "модуль " + module + " не загружен в процесс");
                return;
            }
            base = info.base;
            size = info.size;
        }

        const void* found = scan::FindFirst(base, size, pattern);
        if (found == nullptr) {
            Reply(command, ipc::AckStatus::NotFound, "паттерн не найден в " + module);
            return;
        }

        char answer[256]{};
        std::snprintf(answer, sizeof(answer), "найдено по смещению 0x%llX (база модуля + 0x%llX)",
                      static_cast<unsigned long long>(reinterpret_cast<u64>(found)),
                      static_cast<unsigned long long>(reinterpret_cast<u64>(found) - reinterpret_cast<u64>(base)));
        Reply(command, ipc::AckStatus::Ok, answer);
    }

    void ExecuteSpawnProbe(const PendingCommand& command) {
        if (adapter_ == nullptr || !adapter_->Ready()) {
            Reply(command, ipc::AckStatus::NotSupported, "адаптер спавна не готов");
            return;
        }

        // Координаты передаются текстом "x,y,z" (в записи команды всего два
        // числа), а числовые arg0/arg1 служат резервным вариантом.
        i32 x = static_cast<i32>(command.arg0);
        i32 y = static_cast<i32>(command.arg1);
        i32 z = 0;

        if (!command.text.empty()) {
            if (std::sscanf(command.text.c_str(), "%d,%d,%d", &x, &y, &z) != 3) {
                Reply(command, ipc::AckStatus::BadRequest, "формат координат: x,y,z (например 4,0,8)");
                return;
            }
        }

        ipc::PayloadVoxel probe{};
        probe.x = x;
        probe.y = y;
        probe.z = z;
        probe.block = 1;
        probe.action = static_cast<u32>(ipc::VoxelAction::Place);

        mirror_.PushIncoming(probe);

        std::string error;
        const u32 processed = mirror_.ProcessOnMainThread(*adapter_, 1, &error);
        Reply(command, processed > 0 ? ipc::AckStatus::Ok : ipc::AckStatus::Rejected,
              processed > 0 ? "пробный куб создан" : "не удалось: " + error);
    }

    void Reply(const PendingCommand& command, ipc::AckStatus status, const std::string& text) {
        if (!bridge_.IsOpen()) return;

        GWYF_INFO("Команда %s → %s", ipc::ToString(command.code), text.c_str());

        // Текст уходит в bulk-кольцо, в записи — только метаданные.
        const bool bulkOk = bridge_.Out().PushBulk(text.data(), static_cast<u32>(text.size()));

        ipc::Payload payload{};
        payload.command.code = static_cast<u32>(command.code);
        payload.command.status = static_cast<u32>(status);
        payload.command.textLength = bulkOk ? static_cast<u32>(text.size()) : 0;
        bridge_.Out().Push(ipc::RecordType::CommandAck, payload);
    }

    void PublishStatusLine() {
        if (!bridge_.IsOpen()) return;

        const voxel::VoxelStats stats = mirror_.Stats();
        ipc::Payload payload{};
        payload.chips.chips = static_cast<i64>(stats.spawned);
        payload.chips.pendingBet = static_cast<i64>(stats.failed);
        payload.chips.bank = static_cast<i64>(stats.liveObjects);
        payload.chips.playerCount = static_cast<u32>(hook::Installed().size());
        payload.chips.roundIndex = static_cast<u32>(stats.dropped);
        bridge_.Out().Push(ipc::RecordType::ChipState, payload, 0, 0);
    }

    // ── Разбор строк ────────────────────────────────────────────────────────

    static void SplitClassName(const std::string& full, std::string& nameSpace, std::string& name) {
        const usize position = full.rfind('.');
        if (position == std::string::npos) {
            nameSpace.clear();
            name = full;
            return;
        }
        nameSpace = full.substr(0, position);
        name = full.substr(position + 1);
    }

    static bool ParseArgIndex(const std::string& text, u32& index, char& kind) {
        if (text.rfind("arg", 0) != 0) return false;

        std::string digits;
        kind = 'i';
        for (usize position = 3; position < text.size(); ++position) {
            const char ch = text[position];
            if (ch >= '0' && ch <= '9') {
                digits += ch;
            } else if (ch == 'f') {
                kind = 'f';
            } else {
                return false;
            }
        }
        if (digits.empty()) return false;
        index = static_cast<u32>(std::stoul(digits));
        return index < 16;
    }

    // ── Детур mono_runtime_invoke ───────────────────────────────────────────

    using InvokeFn = void* (*)(void*, void*, void**, void*);

    /// ВАЖНО: этот код выполняется на потоке, который уже находится в runtime,
    /// поэтому mono_* вызывать можно, а вот блокировки/аллокации — нельзя.
    /// Всё, что аллоцирует (std::string), используется только в ветке «цель
    /// найдена», то есть на порядки реже, чем обычные вызовы.
    static void* DetourMonoInvoke(void* method, void* object, void** params, void* exception) {
        Engine& self = Engine::Instance();

        if (self.shuttingDown_ || !self.initialized_ || self.rules_.empty() || params == nullptr) {
            return self.originalInvoke_ != nullptr
                       ? reinterpret_cast<InvokeFn>(self.originalInvoke_)(method, object, params, exception)
                       : nullptr;
        }

        // Защита от рекурсии: наши же вызовы managed-кода не должны попадать в анализ.
        static thread_local int depth = 0;
        if (depth > 0) {
            return reinterpret_cast<InvokeFn>(self.originalInvoke_)(method, object, params, exception);
        }

        ++depth;

        void* klass = mono::Fn().method_get_class != nullptr ? mono::Fn().method_get_class(method) : nullptr;
        MatchRule* rule = self.MatchMethod(method, klass);

        if (rule != nullptr) {
            profile::EvalContext context{};
            context.instance = object;

            // Типы аргументов берём из CIL-сигнатуры (а не угадываем).
            std::vector<mono::ArgKind>& kinds = rule->argKinds;
            const u32 count = static_cast<u32>(std::min<usize>(kinds.size(), 16));

            for (u32 index = 0; index < count; ++index) {
                i64 intValue = 0;
                double floatValue = 0.0;
                bool isFloat = false;
                mono::ExtractArg(params, index, kinds[index], intValue, floatValue, isFloat);

                context.args[index] = reinterpret_cast<void*>(static_cast<u64>(intValue));
                context.doubleArgs[index] = floatValue;
                context.floatArgs[index] = static_cast<float>(floatValue);
            }
            context.argCount = count;

            self.Publish(*rule, context);
        }

        void* result = reinterpret_cast<InvokeFn>(self.originalInvoke_)(method, object, params, exception);

        --depth;
        return result;
    }

    // ── Состояние ───────────────────────────────────────────────────────────

    static constexpr u32 kCacheSize = 256;

    HMODULE moduleHandle_ = nullptr;
    std::string modulePath_;
    std::string profilePath_;

    profile::Profile profile_;
    std::vector<MatchRule> rules_;

    CacheEntry matchCache_[kCacheSize]{};
    std::atomic<u32> matchCacheCount_{0};

    ipc::Bridge bridge_;
    voxel::VoxelMirror mirror_;
    std::unique_ptr<voxel::ISpawnAdapter> adapter_;

    InboundVoxelQueue inboundVoxels_;
    std::mutex commandMutex_;
    std::vector<PendingCommand> pendingCommands_;

    void* originalInvoke_ = nullptr;
    HANDLE workerThread_ = nullptr;
    std::atomic<bool> workerRunning_{false};
    std::atomic<bool> workerStop_{false};

    bool initialized_ = false;
    bool shuttingDown_ = false;
    bool logOpened_ = false;
    u64 lastInitAttemptMs_ = 0;
    u64 lastHeartbeatMs_ = 0;
    int initAttempts_ = 0;

public:
    void SetModuleHandle(HMODULE handle) { moduleHandle_ = handle; }
};

// ── Хук главного потока ───────────────────────────────────────────────────
//
//  Нам нужен гарантированный вызов на главном потоке игры: Unity API
//  (создание GameObject, установка позиции) можно вызывать только оттуда.
//  Unity держит цикл сообщений Windows, поэтому WH_GETMESSAGE — самый
//  надёжный «пульс главного потока», доступный нативному коду извне.

HHOOK g_messageHook = nullptr;
std::atomic<u64> g_mainThreadId{0};
std::atomic<u64> g_frameCount{0};

LRESULT CALLBACK MainThreadHookProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        g_frameCount.fetch_add(1, std::memory_order_relaxed);
        Engine::Instance().OnMainThreadFrame();
    }
    return CallNextHookEx(g_messageHook, code, wparam, lparam);
}

/// Запасной вариант: если хук сообщений поставить не удалось (игра не крутит
/// цикл сообщений), поднимаем поток-наблюдатель. Он НЕ трогает Unity API,
/// поэтому безопасен: работают события ввода-вывода и телеметрия, а спавн
/// кубов остаётся недоступным (об этом пишем в лог).
DWORD WINAPI FallbackPumpProc(LPVOID) {
    GWYF_WARN("Хук главного потока недоступен — работаю в режиме наблюдения "
              "(события и телеметрия работают, создание кубов в игре — нет).");

    while (!Engine::Instance().ShutdownRequested()) {
        Sleep(50);
        Engine::Instance().OnMainThreadFrame();
    }
    return 0;
}

}  // namespace gwyf::engine

// ============================================================================
//  Точки входа DLL
// ============================================================================

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(instance);
            auto& engine = gwyf::engine::Engine::Instance();
            engine.SetModuleHandle(instance);

            // Лог и профиль откроются в Initialize (нужно, чтобы Mono уже был).
            gwyf::log::Open("", gwyf::log::Level::Info);

            // Ставим хук главного потока сразу: он же будет триггером инициализации,
            // когда Unity поднимет Mono и загрузит Assembly-CSharp.
            gwyf::engine::g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, gwyf::engine::MainThreadHookProc,
                                                           nullptr, GetCurrentThreadId());
            if (gwyf::engine::g_messageHook == nullptr) {
                GWYF_WARN("SetWindowsHookExW(WH_GETMESSAGE) не удался (GetLastError=%lu) — включаю запасной режим", GetLastError());
                CreateThread(nullptr, 0, gwyf::engine::FallbackPumpProc, nullptr, 0, nullptr);
            } else {
                gwyf::engine::g_mainThreadId.store(GetCurrentThreadId());
            }

            return TRUE;
        }

        case DLL_PROCESS_DETACH: {
            // Не делаем тяжёлых операций, если процесс завершается сам:
            // huки и потоки уже не нужны, а вызовы Unity могут упасть.
            if (reserved == nullptr) {
                gwyf::engine::Engine::Instance().Shutdown();
            } else {
                gwyf::log::Close();
            }
            return TRUE;
        }

        default:
            return TRUE;
    }
}

// ── Экспортируемые функции для инжектора ────────────────────────────────────

extern "C" __declspec(dllexport) BOOL WINAPI GWYF_Init(void) {
    return gwyf::engine::Engine::Instance().Initialize() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void WINAPI GWYF_Shutdown(void) {
    gwyf::engine::Engine::Instance().Shutdown();
}

extern "C" __declspec(dllexport) BOOL WINAPI GWYF_IsReady(void) {
    return gwyf::engine::Engine::Instance().Ready() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) const char* WINAPI GWYF_Version(void) {
    return "GWYF_HookEngine 1.0 (ABI 1)";
}

/// Диагностика: текстовая сводка состояния ядра.
extern "C" __declspec(dllexport) const char* WINAPI GWYF_Status(void) {
    static thread_local std::string text;
    text = gwyf::engine::Engine::Instance().StatusText();
    return text.c_str();
}
