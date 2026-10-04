// ============================================================================
//  TargetProfile.h — декларативное описание целей внутри игры.
//
//  Идея: ВСЁ, что зависит от конкретной игры (имена классов/методов/полей,
//  сигнатуры, RVA/шаблоны), вынесено в текстовый профиль. Код при этом
//  остаётся универсальным: он умеет разрешать цели любым из способов и
//  раскладывать захваченные значения по полям IPC-записи.
//
//  Это ровно тот «технический подход из видео»: сначала реверс-инжиниринг
//  (какие классы/методы отвечают за ставки и столы), потом — конфигурация
//  моста под найденные цели, без пересборки кода.
//
//  ФОРМАТ ПРОФИЛЯ (INI-подобный)
//  ------------------------------------------------------------------
//  [bridge]
//  region=Local\GWYF_MC_BRIDGE_ABI1
//  log=gwyf_bridge.log
//  heartbeat_ms=1000
//
//  [mono]
//  module=auto
//  assembly=Assembly-CSharp
//
//  [watch.bet]                  ; имя секции произвольно, role задаёт смысл
//  mode=invoke                  ; invoke | method | export | rva | pattern
//  class=Game.BetManager        ; для invoke/method — managed-класс
//  method=PlaceBet
//  params=3
//  shape=this_i32_i32_f32       ; для mode=method: раскладка аргументов
//  event=BetPlaced
//  map=amount:arg2f,chipType:arg0,betKind:arg1
//
//  [poll.chips]
//  mode=poll
//  class=Game.GameManager
//  field=chips
//  interval_ms=250
//  event=ChipState
//  map=chips:value
//
//  [voxel]
//  mode=unity_primitive         ; unity_primitive | native_export | mono_factory
//  origin=120,3,120
//  scale=0.5
//  budget_per_frame=8
// ============================================================================
#pragma once

#include "Common.h"
#include "IpcProtocol.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gwyf::profile {

using gwyf::ipc::CommandCode;
using gwyf::ipc::Payload;
using gwyf::ipc::RecordType;

/// Как разрешается цель.
enum class ResolveMode {
    Invoke,   ///< хук mono_runtime_invoke с фильтром по классу/методу (безопасно, ABI Mono)
    Method,   ///< хук JIT-кода метода (mono_compile_method) — нужен shape
    Poll,     ///< периодическое чтение поля (без хуков вообще)
    Export,   ///< экспорт из нативного модуля (GetProcAddress)
    Rva,      ///< адрес = база модуля + RVA
    Pattern,  ///< поиск по сигнатуре байт
    Disabled,
};

/// Что делает запись профиля.
enum class WatchRole {
    Bet,        ///< постановка ставки
    BetResult,  ///< завершение раунда/выплата
    Table,      ///< появление игрового стола
    ChipState,  ///< телеметрия фишек/банка
    LobbyState, ///< телеметрия лобби
    Tick,       ///< точка дренажа очереди на главном потоке
};

/// Связка «поле IPC-записи ← источник значения».
struct Binding {
    std::string target;  ///< например "bet.amount"
    std::string source;  ///< например "arg2f", "this", "field(arg0,\"tableId\")", "value"
};

/// Описание одной цели.
struct WatchSpec {
    std::string section;      ///< имя секции профиля (для логов)
    WatchRole role = WatchRole::Bet;
    ResolveMode mode = ResolveMode::Disabled;
    RecordType event = RecordType::None;

    // managed-цель
    std::string className;    ///< "Game.BetManager" (пространство имён через точку)
    std::string methodName;
    std::string fieldName;
    int paramCount = -1;      ///< -1 = любое число параметров

    // нативная цель
    std::string moduleName;   ///< "UnityPlayer.dll", "Game.exe"
    std::string symbol;       ///< имя экспорта
    u64 rva = 0;
    std::string pattern;      ///< AOB-строка
    i32 patternOffset = 0;    ///< смещение внутри найденного места

    // раскладка аргументов для mode=method
    std::string shape;
    std::vector<std::string> argTypes;

    // поведение
    u32 intervalMs = 250;
    bool publishOnChangeOnly = true;
    std::vector<Binding> bindings;

    // диагностика
    std::string note;
};

/// Настройки спавна кубов в игре.
struct VoxelSpawnSpec {
    enum class Mode {
        Disabled,
        UnityPrimitive,  ///< GameObject.CreatePrimitive(Cube) + set_position/set_localScale
        MonoFactory,     ///< вызов метода самой игры, который создаёт объект
        NativeExport,    ///< вызов нативной функции по экспорту (и тесты)
    };

    Mode mode = Mode::Disabled;

    /// Смещение мира Minecraft → координаты игры.
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float scale = 0.5f;
    bool flattenY = false;      ///< если игра плоская — игнорировать Y
    u32 budgetPerFrame = 8;     ///< сколько объектов создаём за кадр
    u32 poolLimit = 4096;       ///< предел числа живых кубов
    bool despawnOnBreak = true;

    // mode=mono_factory
    std::string factoryClass;
    std::string factoryMethod;
    std::vector<std::string> factoryArgTypes;

    // mode=native_export
    std::string moduleName;
    std::string symbol;
    std::vector<std::string> nativeArgTypes;
};

/// Полный профиль.
struct Profile {
    // [bridge]
    std::wstring regionName = gwyf::ipc::kDefaultRegionName;
    std::string logPath = "gwyf_bridge.log";
    u32 heartbeatMs = 1000;
    bool enableInvokeHook = true;
    bool enableStatePolling = true;
    u32 maxEventsPerFrame = 64;

    // [mono]
    std::string monoModule = "auto";
    std::string gameAssembly = "Assembly-CSharp";

    std::vector<WatchSpec> watches;
    VoxelSpawnSpec voxel;

    /// Соответствие «номер блока из Minecraft → имя блока игры» ([blocks]).
    std::map<u32, std::string> blockNames;

    /// Найти цель по роли (первую подходящую).
    const WatchSpec* Find(WatchRole role) const;

    /// Все цели с указанной ролью.
    std::vector<const WatchSpec*> FindAll(WatchRole role) const;

    /// Отчёт о профиле (для логов).
    std::string Describe() const;
};

/// Загрузить профиль из файла. Если файла нет — вернуть профиль по умолчанию
/// для Gamble With Your Friends и записать рядом шаблон для заполнения.
struct LoadResult {
    bool ok = false;
    bool usedDefaults = false;
    std::string error;
    std::string sourcePath;
};

LoadResult Load(const std::string& path, Profile& out);

/// Профиль по умолчанию: игра Unity Mono + Mirror, цели помечены как
/// «требуют проверки» — их надо подтвердить декомпиляцией Assembly-CSharp.dll.
Profile DefaultForGambleWithYourFriends();

/// Разбор строки вида "120,3,120" в три числа.
bool ParseFloat3(const std::string& text, float out[3]);

/// Собрать текст шаблона профиля (пишется рядом, если файла нет).
std::string TemplateText();

/// ── Применение значений к IPC-записи ────────────────────────────────────────

/// Контекст вычисления выражений профиля.
struct EvalContext {
    void* args[16]{};        ///< целочисленные/указательные аргументы
    float floatArgs[16]{};   ///< аргументы типа float
    double doubleArgs[16]{}; ///< аргументы типа double
    void* instance = nullptr;///< this (для экземплярных методов)
    i64 polledValue = 0;     ///< значение для source="value"
    u32 argCount = 0;
};

/// Вычислить выражение источника. Возвращает false, если источник недоступен.
bool EvaluateSource(const std::string& source, const EvalContext& context, i64& outInt, float& outFloat, bool& isFloat);

/// Записать значение в поле полезной нагрузки по имени цели ("bet.amount").
bool ApplyToPayload(Payload& payload, const std::string& target, i64 intValue, float floatValue, bool isFloat);

/// ── Проверка раскладки аргументов ──────────────────────────────────────────
//
//  Хук на метод — единственное место, где ошибка в типах аргументов приводит
//  не к «неверным числам», а к порче стека. Поэтому раскладка сверяется с
//  реальной CIL-сигнатурой ДО установки хука, а сама проверка вынесена в
//  переносимую функцию: её прогоняет самотест (см. tools/selftest).
//
//  declared — типы из профиля (shape="i32_i32_f32"), actual — типы из
//  метаданных (после классификации). Пустая строка = совместимо,
//  иначе возвращается причина отказа (для лога и ответа команды).

/// Проверить совместимость раскладки с фактической сигнатурой.
std::string CheckShape(const std::vector<std::string>& declared, const std::vector<std::string>& actual);

/// Типы раскладки по её имени ("i32_i32_f32" → {"int32","int32","float32"}).
std::vector<std::string> ShapeKinds(const std::string& shape);

/// Имена поддерживаемых раскладок в виде строки для диагностики.
std::string SupportedShapes();

/// Преобразовать имя события из профиля ("BetPlaced") в тип записи.
RecordType ParseEventName(const std::string& text);

/// Строка режима → enum.
ResolveMode ParseMode(const std::string& text);

}  // namespace gwyf::profile
