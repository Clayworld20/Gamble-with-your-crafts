// ============================================================================
//  TargetProfile.cpp — парсер профиля и вычисление выражений привязок.
// ============================================================================
#include "gwyfbridge/TargetProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>
#include <sstream>

namespace gwyf::profile {

namespace {

std::string Trim(const std::string& text) {
    usize begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;

    usize end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;

    return text.substr(begin, end - begin);
}

std::string Lower(const std::string& text) {
    // Единая реализация из Base.h: без <algorithm>/<cctype> и без предупреждений
    // STL, которые в MSVC при /WX становятся ошибкой.
    return ToLowerAscii(text);
}

std::vector<std::string> Split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;

    for (char ch : text) {
        if (ch == '"') inQuotes = !inQuotes;
        if (ch == delimiter && !inQuotes) {
            parts.push_back(Trim(current));
            current.clear();
            continue;
        }
        current += ch;
    }
    parts.push_back(Trim(current));
    return parts;
}

/// Строка профиля "class=Game.BetManager" → ключ и значение.
bool SplitKeyValue(const std::string& line, std::string& key, std::string& value) {
    const usize position = line.find('=');
    if (position == std::string::npos) return false;
    key = Lower(Trim(line.substr(0, position)));
    value = Trim(line.substr(position + 1));
    return !key.empty();
}

/// "Game.BetManager" → namespace "Game" + имя "BetManager".
[[maybe_unused]] void SplitClassName(const std::string& full, std::string& nameSpace, std::string& name) {
    const usize position = full.rfind('.');
    if (position == std::string::npos) {
        nameSpace.clear();
        name = full;
        return;
    }
    nameSpace = full.substr(0, position);
    name = full.substr(position + 1);
}

bool ParseBool(const std::string& text) {
    const std::string lower = Lower(Trim(text));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "да" || lower == "on";
}

u32 ParseInterval(const std::string& text, u32 fallback) {
    try {
        const long long value = std::stoll(Trim(text));
        if (value <= 0) return fallback;
        return static_cast<u32>(std::min<long long>(value, 60000));
    } catch (...) {
        return fallback;
    }
}

WatchRole ParseRole(const std::string& section) {
    const std::string lower = Lower(section);
    if (lower.find("betresult") != std::string::npos || lower.find("payout") != std::string::npos) return WatchRole::BetResult;
    if (lower.find("bet") != std::string::npos) return WatchRole::Bet;
    if (lower.find("table") != std::string::npos) return WatchRole::Table;
    if (lower.find("lobby") != std::string::npos) return WatchRole::LobbyState;
    if (lower.find("chip") != std::string::npos || lower.find("bank") != std::string::npos) return WatchRole::ChipState;
    if (lower.find("tick") != std::string::npos || lower.find("update") != std::string::npos) return WatchRole::Tick;
    return WatchRole::Bet;
}

}  // namespace

bool ParseFloat3(const std::string& text, float out[3]) {
    const std::vector<std::string> parts = Split(text, ',');
    if (parts.size() != 3) return false;

    try {
        for (int index = 0; index < 3; ++index) {
            out[index] = std::stof(parts[index]);
        }
    } catch (...) {
        return false;
    }
    return true;
}

// ── Раскладки аргументов для хуков на методы ────────────────────────────────

std::vector<std::string> ShapeKinds(const std::string& shape) {
    std::vector<std::string> kinds;
    if (shape.empty()) return kinds;

    std::string current;
    for (char ch : shape) {
        if (ch == '_') {
            if (!current.empty()) kinds.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.empty()) kinds.push_back(current);

    // Приводим короткие имена к каноническим.
    for (std::string& kind : kinds) {
        if (kind == "i32" || kind == "int" || kind == "i4") kind = "int32";
        else if (kind == "i64" || kind == "long") kind = "int64";
        else if (kind == "f32" || kind == "float") kind = "float32";
        else if (kind == "f64" || kind == "double") kind = "float64";
        else if (kind == "obj" || kind == "object" || kind == "ptr") kind = "object";
        else if (kind == "bool") kind = "bool";
        else if (kind == "i16") kind = "int16";
        else if (kind == "i8") kind = "int8";
        else kind = "unknown";
    }
    return kinds;
}

std::string SupportedShapes() {
    return "void, i32, i32_i32, i32_i32_i32, i64, f32, f32_f32, i32_f32, f32_i32, i32_i32_f32, obj, obj_i32, i32_obj";
}

std::string CheckShape(const std::vector<std::string>& declared, const std::vector<std::string>& actual) {
    if (actual.empty()) {
        // Метаданные недоступны: работаем по профилю, но об этом надо честно сказать.
        return {};
    }
    if (declared.size() != actual.size()) {
        return "в профиле " + std::to_string(declared.size()) + " аргументов, а метод принимает " +
               std::to_string(actual.size());
    }
    for (usize index = 0; index < declared.size(); ++index) {
        if (actual[index] == "unknown" || declared[index] == "unknown") continue;
        if (declared[index] != actual[index]) {
            return "аргумент " + std::to_string(index) + ": в сигнатуре " + actual[index] +
                   ", в профиле " + declared[index];
        }
    }
    return {};
}

RecordType ParseEventName(const std::string& text) {
    const std::string lower = Lower(Trim(text));
    if (lower == "betplaced" || lower == "bet") return RecordType::BetPlaced;
    if (lower == "betresolved" || lower == "payout") return RecordType::BetResolved;
    if (lower == "tablespawned" || lower == "table") return RecordType::TableSpawned;
    if (lower == "tableremoved") return RecordType::TableRemoved;
    if (lower == "chipstate" || lower == "chips") return RecordType::ChipState;
    if (lower == "lobbystate" || lower == "lobby") return RecordType::LobbyState;
    if (lower == "voxeledit" || lower == "voxel") return RecordType::VoxelEdit;
    if (lower == "blockgrant" || lower == "grant") return RecordType::BlockGrant;
    if (lower == "logline") return RecordType::LogLine;
    return RecordType::None;
}

ResolveMode ParseMode(const std::string& text) {
    const std::string lower = Lower(Trim(text));
    if (lower == "invoke") return ResolveMode::Invoke;
    if (lower == "method") return ResolveMode::Method;
    if (lower == "poll") return ResolveMode::Poll;
    if (lower == "export") return ResolveMode::Export;
    if (lower == "rva") return ResolveMode::Rva;
    if (lower == "pattern") return ResolveMode::Pattern;
    return ResolveMode::Disabled;
}

const WatchSpec* Profile::Find(WatchRole role) const {
    for (const WatchSpec& watch : watches) {
        if (watch.role == role && watch.mode != ResolveMode::Disabled) return &watch;
    }
    return nullptr;
}

std::vector<const WatchSpec*> Profile::FindAll(WatchRole role) const {
    std::vector<const WatchSpec*> result;
    for (const WatchSpec& watch : watches) {
        if (watch.role == role && watch.mode != ResolveMode::Disabled) result.push_back(&watch);
    }
    return result;
}

std::string Profile::Describe() const {
    std::string text = "Профиль: регион=" + std::string(regionName.begin(), regionName.end()) +
                       ", сборка=" + gameAssembly + ", целей=" + std::to_string(watches.size()) + "\n";

    for (const WatchSpec& watch : watches) {
        const char* mode = "disabled";
        switch (watch.mode) {
            case ResolveMode::Invoke: mode = "invoke"; break;
            case ResolveMode::Method: mode = "method"; break;
            case ResolveMode::Poll: mode = "poll"; break;
            case ResolveMode::Export: mode = "export"; break;
            case ResolveMode::Rva: mode = "rva"; break;
            case ResolveMode::Pattern: mode = "pattern"; break;
            default: break;
        }

        text += "  [" + watch.section + "] " + mode + " → " + gwyf::ipc::ToString(watch.event);
        if (!watch.className.empty()) text += " " + watch.className + "." + watch.methodName;
        if (!watch.fieldName.empty()) text += " поле " + watch.fieldName;
        if (!watch.symbol.empty()) text += " экспорт " + watch.symbol;
        text += " привязок: " + std::to_string(watch.bindings.size()) + "\n";
    }

    text += "  спавн вокселей: ";
    switch (voxel.mode) {
        case VoxelSpawnSpec::Mode::UnityPrimitive: text += "unity_primitive"; break;
        case VoxelSpawnSpec::Mode::MonoFactory: text += "mono_factory (" + voxel.factoryClass + "." + voxel.factoryMethod + ")"; break;
        case VoxelSpawnSpec::Mode::NativeExport: text += "native_export (" + voxel.symbol + ")"; break;
        default: text += "выключен"; break;
    }
    text += "\n";
    return text;
}

// ── Разбор профиля ──────────────────────────────────────────────────────────

namespace {

void ApplyBridgeSection(Profile& profile, const std::string& key, const std::string& value) {
    if (key == "region") {
        profile.regionName.assign(value.begin(), value.end());
    } else if (key == "log") {
        profile.logPath = value;
    } else if (key == "heartbeat_ms") {
        profile.heartbeatMs = ParseInterval(value, 1000);
    } else if (key == "invoke_hook") {
        profile.enableInvokeHook = ParseBool(value);
    } else if (key == "poll") {
        profile.enableStatePolling = ParseBool(value);
    } else if (key == "max_events_per_frame") {
        profile.maxEventsPerFrame = ParseInterval(value, 64);
    }
}

void ApplyVoxelSection(VoxelSpawnSpec& spec, const std::string& key, const std::string& value) {
    if (key == "mode") {
        const std::string lower = Lower(value);
        if (lower == "unity_primitive") spec.mode = VoxelSpawnSpec::Mode::UnityPrimitive;
        else if (lower == "mono_factory") spec.mode = VoxelSpawnSpec::Mode::MonoFactory;
        else if (lower == "native_export") spec.mode = VoxelSpawnSpec::Mode::NativeExport;
        else spec.mode = VoxelSpawnSpec::Mode::Disabled;
    } else if (key == "origin") {
        ParseFloat3(value, spec.origin);
    } else if (key == "scale") {
        try { spec.scale = std::stof(value); } catch (...) {}
    } else if (key == "flatten_y") {
        spec.flattenY = ParseBool(value);
    } else if (key == "budget_per_frame") {
        spec.budgetPerFrame = ParseInterval(value, 8);
    } else if (key == "pool_limit") {
        spec.poolLimit = ParseInterval(value, 4096);
    } else if (key == "despawn_on_break") {
        spec.despawnOnBreak = ParseBool(value);
    } else if (key == "factory_class") {
        spec.factoryClass = value;
    } else if (key == "factory_method") {
        spec.factoryMethod = value;
    } else if (key == "factory_arg_types") {
        spec.factoryArgTypes = Split(value, ',');
    } else if (key == "module") {
        spec.moduleName = value;
    } else if (key == "symbol") {
        spec.symbol = value;
    } else if (key == "arg_types") {
        spec.nativeArgTypes = Split(value, ',');
    }
}

/// Развернуть короткое имя поля в каноническое по роли секции.
/// В профиле удобнее писать `map=amount:arg0f`, а ApplyToPayload ожидает
/// строгие имена (`bet.amount`) — иначе опечатка в имени поля привела бы
/// к «молча ничего не публикуется». Здесь это разводится один раз.
std::string ExpandTarget(const std::string& target, WatchRole role) {
    if (target.find('.') != std::string::npos) return target;

    switch (role) {
        case WatchRole::Bet: return "bet." + target;
        case WatchRole::BetResult: return "betResult." + target;
        case WatchRole::Table: return "table." + target;
        case WatchRole::ChipState: return "chips." + target;
        case WatchRole::LobbyState: return "lobby." + target;
        default: return target;
    }
}

void ApplyWatchSection(WatchSpec& watch, const std::string& key, const std::string& value) {
    if (key == "mode") {
        watch.mode = ParseMode(value);
    } else if (key == "class") {
        watch.className = value;
    } else if (key == "method") {
        watch.methodName = value;
    } else if (key == "field") {
        watch.fieldName = value;
    } else if (key == "params") {
        try { watch.paramCount = std::stoi(value); } catch (...) {}
    } else if (key == "shape") {
        watch.shape = value;
    } else if (key == "arg_types") {
        watch.argTypes = Split(value, ',');
    } else if (key == "event") {
        watch.event = ParseEventName(value);
    } else if (key == "module") {
        watch.moduleName = value;
    } else if (key == "symbol") {
        watch.symbol = value;
    } else if (key == "rva") {
        try { watch.rva = std::stoull(value, nullptr, 0); } catch (...) {}
    } else if (key == "pattern") {
        watch.pattern = value;
    } else if (key == "pattern_offset") {
        try { watch.patternOffset = std::stoi(value); } catch (...) {}
    } else if (key == "interval_ms") {
        watch.intervalMs = ParseInterval(value, 250);
    } else if (key == "on_change_only") {
        watch.publishOnChangeOnly = ParseBool(value);
    } else if (key == "note") {
        watch.note = value;
    } else if (key == "map") {
        // map=amount:arg2f,chipType:arg0
        for (const std::string& item : Split(value, ',')) {
            const usize position = item.find(':');
            if (position == std::string::npos) continue;
            Binding binding;
            binding.target = ExpandTarget(Trim(item.substr(0, position)), watch.role);
            binding.source = Trim(item.substr(position + 1));
            if (!binding.target.empty() && !binding.source.empty()) {
                watch.bindings.push_back(std::move(binding));
            }
        }
    }
}

}  // namespace

LoadResult Load(const std::string& path, Profile& out) {
    LoadResult result;
    result.sourcePath = path;

    FILE* file = nullptr;
    file = std::fopen(path.c_str(), "rt");
    if (file == nullptr) {
        out = DefaultForGambleWithYourFriends();
        result.ok = true;
        result.usedDefaults = true;
        result.error = "файл профиля не найден — использую встроенный профиль по умолчанию";
        GWYF_WARN("Профиль %s не найден, беру встроенный. Рядом будет записан шаблон для заполнения.", path.c_str());
        return result;
    }

    Profile profile;
    profile.watches.clear();
    profile.blockNames.clear();

    std::string currentSection;
    WatchSpec currentWatch;
    bool hasWatch = false;
    int lineNumber = 0;
    char line[2048];

    auto flushWatch = [&]() {
        if (!hasWatch) return;
        if (currentWatch.event == RecordType::None) {
            // По умолчанию смысл события выводим из имени секции.
            switch (currentWatch.role) {
                case WatchRole::Bet: currentWatch.event = RecordType::BetPlaced; break;
                case WatchRole::BetResult: currentWatch.event = RecordType::BetResolved; break;
                case WatchRole::Table: currentWatch.event = RecordType::TableSpawned; break;
                case WatchRole::ChipState: currentWatch.event = RecordType::ChipState; break;
                case WatchRole::LobbyState: currentWatch.event = RecordType::LobbyState; break;
                case WatchRole::Tick: currentWatch.event = RecordType::None; break;
            }
        }
        profile.watches.push_back(currentWatch);
        currentWatch = WatchSpec{};
        hasWatch = false;
    };

    while (std::fgets(line, sizeof(line), file) != nullptr) {
        ++lineNumber;
        std::string text = Trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;

        if (text.front() == '[' && text.back() == ']') {
            flushWatch();

            currentSection = Trim(text.substr(1, text.size() - 2));
            const std::string lower = Lower(currentSection);

            if (lower.rfind("watch", 0) == 0 || lower.rfind("poll", 0) == 0 || lower.rfind("tick", 0) == 0) {
                currentWatch = WatchSpec{};
                currentWatch.section = currentSection;
                currentWatch.role = ParseRole(currentSection);
                hasWatch = true;
            }

            continue;
        }

        std::string key;
        std::string value;
        if (!SplitKeyValue(text, key, value)) {
            GWYF_WARN("Профиль, строка %d: пропускаю «%s» (нет '=')", lineNumber, text.c_str());
            continue;
        }

        const std::string lower = Lower(currentSection);

        if (lower == "bridge") {
            ApplyBridgeSection(profile, key, value);
        } else if (lower == "mono") {
            if (key == "module") profile.monoModule = value;
            else if (key == "assembly") profile.gameAssembly = value;
        } else if (lower == "voxel") {
            ApplyVoxelSection(profile.voxel, key, value);
        } else if (lower == "blocks") {
            try {
                const u32 blockId = static_cast<u32>(std::stoul(key));
                profile.blockNames[blockId] = value;
            } catch (...) {
                // Нечисловой ключ в [blocks] игнорируем.
            }
        } else if (hasWatch) {
            ApplyWatchSection(currentWatch, key, value);
        } else {
            GWYF_DEBUG("Профиль: неизвестная секция [%s], ключ %s", currentSection.c_str(), key.c_str());
        }
    }

    flushWatch();
    std::fclose(file);

    if (profile.voxel.mode == VoxelSpawnSpec::Mode::UnityPrimitive && profile.monoModule.empty()) {
        profile.monoModule = "auto";
    }

    out = std::move(profile);
    result.ok = true;
    return result;
}

Profile DefaultForGambleWithYourFriends() {
    Profile profile;

    profile.regionName = gwyf::ipc::kDefaultRegionName;
    profile.gameAssembly = "Assembly-CSharp";
    profile.logPath = "gwyf_bridge.log";

    // Ставка: ищем метод в классах, отвечающих за приём ставок.
    // Имена взяты как РАБОЧАЯ ГИПОТЕЗА — их подтверждает команда dump
    // (см. README, «Поиск целей»). Профиль правится без пересборки.
    WatchSpec bet;
    bet.section = "watch.bet";
    bet.role = WatchRole::Bet;
    bet.mode = ResolveMode::Invoke;
    bet.className = "Game.BetManager";
    bet.methodName = "PlaceBet";
    bet.paramCount = -1;
    bet.event = RecordType::BetPlaced;
    bet.note = "требует подтверждения через dump";
    bet.bindings = {
        {"bet.amount", "arg0f"},
        {"bet.chipType", "arg1"},
    };
    profile.watches.push_back(bet);

    // Стол: момент создания игрового стола.
    WatchSpec table;
    table.section = "watch.table";
    table.role = WatchRole::Table;
    table.mode = ResolveMode::Invoke;
    table.className = "Game.TableManager";
    table.methodName = "SpawnTable";
    table.paramCount = -1;
    table.event = RecordType::TableSpawned;
    table.bindings = {
        {"table.tableId", "arg0"},
        {"table.lobbyId", "arg1"},
    };
    profile.watches.push_back(table);

    // Телеметрия фишек: самое надёжное — читать поле, а не хукать.
    WatchSpec chips;
    chips.section = "poll.chips";
    chips.role = WatchRole::ChipState;
    chips.mode = ResolveMode::Poll;
    chips.className = "Game.GameManager";
    chips.fieldName = "chips";
    chips.intervalMs = 250;
    chips.event = RecordType::ChipState;
    chips.bindings = {{"chips.chips", "value"}};
    profile.watches.push_back(chips);

    // Спавн кубов по умолчанию — штатный Unity-примитив: не требует ни одного
    // адреса из реверса и работает на любом Unity-билде.
    profile.voxel.mode = VoxelSpawnSpec::Mode::UnityPrimitive;
    profile.voxel.origin[0] = 0.0f;
    profile.voxel.origin[1] = 0.0f;
    profile.voxel.origin[2] = 0.0f;
    profile.voxel.scale = 0.5f;
    profile.voxel.budgetPerFrame = 8;
    profile.voxel.poolLimit = 4096;

    profile.blockNames = {
        {1, "dirt"}, {2, "grass"}, {3, "stone"}, {4, "wood"},
        {5, "gold"}, {6, "diamond"}, {7, "table"}, {8, "lamp"},
    };

    return profile;
}

std::string TemplateText() {
    return R"(# ============================================================================
#  gwyf.profile — цели внутри Gamble With Your Friends.
#
#  Как заполнять (подробно — в native/README.md, раздел «Поиск целей»):
#    1) Соберите мост и запустите игру с внедрённой GWYF_HookEngine.dll.
#    2) Откройте Assembly-CSharp.dll в ILSpy/dnSpy и найдите классы, которые
#       принимают ставки и создают столы (ищите по словам Bet, Wager, Table, Slot).
#    3) Выполните:  gwyfbridge.exe dump --pid <pid игры> Game.BetManager
#       — команда выведет список методов с числом параметров и список полей
#       с их смещениями. Это и есть данные для секций ниже.
#    4) Впишите имена и привязки, перезапустите игру. Профиль читается заново
#       командой reload (или при следующем запуске).
#
#  Привязки (map=поле:выражение):
#    arg0..arg15  — аргумент как целое/указатель
#    arg0f        — аргумент как float
#    this         — указатель на экземпляр (для экземплярных методов)
#    field(arg0,"имя")  — прочитать поле managed-объекта по указателю из arg0
#    static("Game.GameManager","chips") — прочитать статическое поле класса
#    value        — значение, прочитанное poll-целью
# ============================================================================

[bridge]
region=Local\GWYF_MC_BRIDGE_ABI1
log=gwyf_bridge.log
heartbeat_ms=1000
invoke_hook=1
poll=1
max_events_per_frame=64

[mono]
module=auto
assembly=Assembly-CSharp

# ── Ставка ──────────────────────────────────────────────────────────────────
[watch.bet]
mode=invoke
class=Game.BetManager
method=PlaceBet
params=-1
event=BetPlaced
map=amount:arg0f,chipType:arg1

# ── Завершение раунда (если в игре есть отдельный метод выплаты) ────────────
[watch.betresult]
mode=invoke
class=Game.BetManager
method=ResolveBet
params=-1
event=BetResolved
map=payout:arg0,chipsAfter:arg1,won:arg2

# ── Появление игрового стола ────────────────────────────────────────────────
[watch.table]
mode=invoke
class=Game.TableManager
method=SpawnTable
params=-1
event=TableSpawned
map=tableId:arg0,lobbyId:arg1

# ── Телеметрия: фишки игрока (без хуков) ───────────────────────────────────
[poll.chips]
mode=poll
class=Game.GameManager
field=chips
interval_ms=250
event=ChipState
map=chips:value

# ── Точка дренажа очереди на главном потоке ────────────────────────────────
#  Нужна, чтобы создавать объекты Unity только из главного потока.
#  Укажите любой дешёвый метод, который вызывается каждый кадр.
[tick]
mode=invoke
class=Game.GameManager
method=Update
params=0

# ── Спавн кубов внутри игры ────────────────────────────────────────────────
[voxel]
mode=unity_primitive
origin=0,0,0
scale=0.5
budget_per_frame=8
pool_limit=4096
despawn_on_break=1

# ── Соответствие блоков Minecraft блокам игры ─────────────────────────────
[blocks]
1=dirt
2=grass
3=stone
4=wood
5=gold
6=diamond
7=table
8=lamp
)";
}

// ── Вычисление источников ───────────────────────────────────────────────────

namespace {

/// Разобрать "arg7", "arg7f", "arg7d".
bool ParseArgReference(const std::string& source, u32& index, char& kind) {
    if (source.rfind("arg", 0) != 0) return false;

    std::string digits;
    kind = 'i';

    for (usize position = 3; position < source.size(); ++position) {
        const char ch = source[position];
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            digits += ch;
        } else if (ch == 'f' || ch == 'd') {
            kind = ch;
        } else {
            return false;
        }
    }

    if (digits.empty()) return false;
    index = static_cast<u32>(std::stoul(digits));
    return index < 16;
}

/// Разобрать field(arg0,"name") / field(this,"name").
[[maybe_unused]] bool ParseFieldCall(const std::string& source, std::string& objectExpr, std::string& fieldName) {
    if (source.rfind("field(", 0) != 0) return false;
    if (source.back() != ')') return false;

    const std::string inner = source.substr(6, source.size() - 7);
    const usize comma = inner.find(',');
    if (comma == std::string::npos) return false;

    objectExpr = Trim(inner.substr(0, comma));

    fieldName = Trim(inner.substr(comma + 1));
    if (fieldName.size() >= 2 && fieldName.front() == '"' && fieldName.back() == '"') {
        fieldName = fieldName.substr(1, fieldName.size() - 2);
    }
    return !objectExpr.empty() && !fieldName.empty();
}

}  // namespace

bool EvaluateSource(const std::string& source, const EvalContext& context, i64& outInt, float& outFloat, bool& isFloat) {
    isFloat = false;
    outInt = 0;
    outFloat = 0.0f;

    const std::string text = Trim(source);
    if (text.empty()) return false;

    if (text == "this") {
        outInt = reinterpret_cast<i64>(context.instance);
        return context.instance != nullptr;
    }

    if (text == "value") {
        outInt = context.polledValue;
        return true;
    }

    u32 index = 0;
    char kind = 'i';
    if (ParseArgReference(text, index, kind)) {
        if (index >= context.argCount) return false;

        if (kind == 'f') {
            outFloat = context.floatArgs[index];
            isFloat = true;
            outInt = static_cast<i64>(outFloat);
            return true;
        }
        if (kind == 'd') {
            outFloat = static_cast<float>(context.doubleArgs[index]);
            isFloat = true;
            outInt = static_cast<i64>(context.doubleArgs[index]);
            return true;
        }

        outInt = static_cast<i64>(reinterpret_cast<u64>(context.args[index]));
        return true;
    }

    // field(...) и static(...) требуют Mono — их вычисляет GWYF_HookEngine,
    // здесь возвращаем false, чтобы вызывающий код знал: нужно обратиться к runtime.
    return false;
}

bool ApplyToPayload(Payload& payload, const std::string& target, i64 intValue, float floatValue, bool isFloat) {
    auto asInt = [&](i64 fallback = 0) -> i64 { return isFloat ? static_cast<i64>(floatValue) : intValue; };
    auto asFloat = [&](float fallback = 0.0f) -> float { return isFloat ? floatValue : static_cast<float>(intValue); };

    if (target == "bet.amount") { ipc::PayloadBet::SetAmount(payload.bet, asFloat()); return true; }
    if (target == "bet.chipType") { payload.bet.chipType = static_cast<u32>(asInt()); return true; }
    if (target == "bet.betKind") { payload.bet.betKind = static_cast<u32>(asInt()); return true; }
    if (target == "bet.tableId") { payload.bet.tableId = static_cast<u32>(asInt()); return true; }
    if (target == "bet.playerId") { payload.bet.playerId = static_cast<u64>(asInt()); return true; }
    if (target == "bet.seat") { payload.bet.seat = static_cast<u32>(asInt()); return true; }
    if (target == "bet.flags") { payload.bet.flags = static_cast<u32>(asInt()); return true; }

    if (target == "betResult.payout") { payload.betResult.payout = asInt(); return true; }
    if (target == "betResult.chipsAfter") { payload.betResult.chipsAfter = asInt(); return true; }
    if (target == "betResult.won") { payload.betResult.won = static_cast<u8>(asInt() != 0 ? 1 : 0); return true; }
    if (target == "betResult.tableId") { payload.betResult.tableId = static_cast<u32>(asInt()); return true; }
    if (target == "betResult.seat") { payload.betResult.seat = static_cast<u32>(asInt()); return true; }
    if (target == "betResult.playerId") { payload.betResult.playerId = static_cast<u64>(asInt()); return true; }
    if (target == "betResult.wheelPocket") { payload.betResult.wheelPocket = static_cast<u8>(asInt()); return true; }
    if (target == "betResult.diceA") { payload.betResult.diceA = static_cast<u8>(asInt()); return true; }
    if (target == "betResult.diceB") { payload.betResult.diceB = static_cast<u8>(asInt()); return true; }

    if (target == "table.tableId") { payload.table.tableId = static_cast<u32>(asInt()); return true; }
    if (target == "table.lobbyId") { payload.table.lobbyId = static_cast<u32>(asInt()); return true; }
    if (target == "table.seats") { payload.table.seats = static_cast<u32>(asInt()); return true; }
    if (target == "table.phase") { payload.table.phase = static_cast<u32>(asInt()); return true; }
    if (target == "table.gameKind") { payload.table.gameKind = static_cast<u32>(asInt()); return true; }
    if (target == "table.minBet") { payload.table.minBet = asInt(); return true; }
    if (target == "table.posX") { payload.table.posX = static_cast<i32>(asInt()); return true; }
    if (target == "table.posY") { payload.table.posY = static_cast<i32>(asInt()); return true; }
    if (target == "table.posZ") { payload.table.posZ = static_cast<i32>(asInt()); return true; }

    if (target == "chips.chips") { payload.chips.chips = asInt(); return true; }
    if (target == "chips.pendingBet") { payload.chips.pendingBet = asInt(); return true; }
    if (target == "chips.bank") { payload.chips.bank = asInt(); return true; }
    if (target == "chips.playerCount") { payload.chips.playerCount = static_cast<u32>(asInt()); return true; }
    if (target == "chips.roundIndex") { payload.chips.roundIndex = static_cast<u32>(asInt()); return true; }
    if (target == "chips.playerId") { payload.chips.playerId = static_cast<u64>(asInt()); return true; }

    if (target == "voxel.x") { payload.voxel.x = static_cast<i32>(asInt()); return true; }
    if (target == "voxel.y") { payload.voxel.y = static_cast<i32>(asInt()); return true; }
    if (target == "voxel.z") { payload.voxel.z = static_cast<i32>(asInt()); return true; }
    if (target == "voxel.block") { payload.voxel.block = static_cast<u32>(asInt()); return true; }
    if (target == "voxel.action") { payload.voxel.action = static_cast<u32>(asInt()); return true; }
    if (target == "voxel.authorId") { payload.voxel.authorId = static_cast<u64>(asInt()); return true; }

    if (target == "grant.block") { payload.grant.block = static_cast<u32>(asInt()); return true; }
    if (target == "grant.count") { payload.grant.count = static_cast<u32>(asInt()); return true; }
    if (target == "grant.playerId") { payload.grant.playerId = static_cast<u64>(asInt()); return true; }
    if (target == "grant.reason") { payload.grant.reason = static_cast<u32>(asInt()); return true; }

    GWYF_DEBUG("Неизвестное поле привязки: %s", target.c_str());
    return false;
}

}  // namespace gwyf::profile
