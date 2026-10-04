// ============================================================================
//  GwyfBridgeCli.cpp — gwyfbridge.exe: внедрение DLL в игру, диагностика
//  и команды к работающему мосту.
//
//  ЧТО УМЕЕТ
//  ---------
//    inject       — внедрить GWYF_HookEngine.dll в процесс игры:
//                   OpenProcess → VirtualAllocEx → WriteProcessMemory →
//                   CreateRemoteThread(LoadLibraryW) → и, если у DLL есть
//                   экспорт GWYF_Init, вызвать его вторым удалённым потоком.
//                   RVA экспорта читается из PE-файла, а не через загрузку
//                   DLL в наш процесс: запускать DllMain движка у себя нельзя.
//    status       — состояние моста и живой ли партнёр.
//    dump         — список методов/полей managed-класса (для заполнения
//                   профиля): команда уходит движку, ответ печатается.
//    scan         — поиск паттерна в модуле игры (диагностика якорей).
//    spawn-probe  — проверить создание куба в игре по координатам.
//    flush        — удалить все объекты, созданные мостом.
//    reload       — перечитать профиль.
//    watch        — печатать события из игры (ставки, столы, воксели).
//    selftest     — запустить gwyfbridge_selftest.exe рядом с этим файлом.
//
//  ЧЕСТНЫЕ ОГРАНИЧЕНИЯ
//  -------------------
//    * инжектор не обходит защиту: если игра запущена от другого пользователя
//      или с защитой от записи в память, будет честная ошибка WinAPI;
//    * команды и watch используют канал «сторона Minecraft»: не запускайте их
//      одновременно с работающим модом — продюсер кольца обязан быть один.
// ============================================================================
#include "gwyfbridge/Common.h"
#include "gwyfbridge/IpcBridge.h"
#include "gwyfbridge/McWireFormat.h"

#include <cstdio>
#include <string>
#include <vector>

#include <tlhelp32.h>

using namespace gwyf;

namespace {

// ── Разбор аргументов ───────────────────────────────────────────────────────

struct Options {
    std::string command;
    std::string target;        ///< имя процесса или путь к DLL
    u32 pid = 0;
    std::string module;        ///< для dump/scan
    std::string pattern;
    std::string text;
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u32 seconds = 10;
    bool help = false;
};

void PrintUsage() {
    std::printf(
        "gwyfbridge.exe — инжектор и диагностика моста GWYF ↔ Minecraft\n\n"
        "Использование:\n"
        "  gwyfbridge.exe inject  --pid <pid> --dll <путь к GWYF_HookEngine.dll>\n"
        "  gwyfbridge.exe inject  --process <имя.exe> --dll <путь>\n"
        "  gwyfbridge.exe status\n"
        "  gwyfbridge.exe dump    --class <Полное.Имя.Класса>\n"
        "  gwyfbridge.exe scan    --module <модуль> --pattern \"48 89 ?? 24\"\n"
        "  gwyfbridge.exe spawn-probe --x <x> --y <y> --z <z>\n"
        "  gwyfbridge.exe flush\n"
        "  gwyfbridge.exe reload\n"
        "  gwyfbridge.exe watch   [--seconds <n>]\n"
        "  gwyfbridge.exe selftest\n\n"
        "Общие ключи: --pid, --process, --dll, --module, --pattern, --class, --seconds, --verbose\n");
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];

        auto next = [&](std::string& out) {
            if (index + 1 >= argc) return false;
            out = argv[++index];
            return true;
        };

        if (arg == "--help" || arg == "-h") {
            options.help = true;
            return true;
        }
        if (arg == "--verbose") {
            log::SetLevel(log::Level::Debug);
            continue;
        }
        if (arg == "--pid") {
            std::string value;
            if (!next(value)) return false;
            options.pid = static_cast<u32>(std::stoul(value, nullptr, 0));
            continue;
        }
        if (arg == "--process" || arg == "--target") {
            if (!next(options.target)) return false;
            continue;
        }
        if (arg == "--dll") {
            if (!next(options.target)) return false;
            continue;
        }
        if (arg == "--module") {
            if (!next(options.module)) return false;
            continue;
        }
        if (arg == "--pattern") {
            if (!next(options.pattern)) return false;
            continue;
        }
        if (arg == "--class") {
            if (!next(options.text)) return false;
            continue;
        }
        if (arg == "--seconds") {
            std::string value;
            if (!next(value)) return false;
            options.seconds = static_cast<u32>(std::stoul(value));
            continue;
        }
        if (arg == "--x" || arg == "--y" || arg == "--z") {
            std::string value;
            if (!next(value)) return false;
            const i32 parsed = static_cast<i32>(std::stol(value));
            if (arg == "--x") options.x = parsed;
            if (arg == "--y") options.y = parsed;
            if (arg == "--z") options.z = parsed;
            continue;
        }

        if (options.command.empty()) {
            options.command = arg;
            continue;
        }

        std::fprintf(stderr, "Неизвестный аргумент: %s\n", arg.c_str());
        return false;
    }
    return true;
}

// ── Работа с процессами ─────────────────────────────────────────────────────

u32 FindProcessByName(const std::string& name) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    const std::wstring wanted = gwyf::ToWideUtf8(name);
    u32 found = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, wanted.c_str()) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

void ListGameProcesses() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    std::printf("Процессы, похожие на игру (введите нужный pid в --pid):\n");
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring name(entry.szExeFile);
            if (name.find(L"Gamble") != std::wstring::npos || name.find(L"GWYF") != std::wstring::npos ||
                name.find(L"Unity") != std::wstring::npos) {
                std::wprintf(L"  pid %5lu  %s\n", entry.th32ProcessID, entry.szExeFile);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

// ── Разбор PE: RVA экспорта по имени ───────────────────────────────────────
//
//  Зачем вручную: чтобы вызвать GWYF_Init внутри целевого процесса, нужно
//  знать смещение экспорта в образе. Загружать DLL движка в инжектор
//  нельзя — DllMain выполнится в неподходящем процессе.

u64 FindExportRva(const std::string& dllPath, const char* exportName, std::string* error) {
    FILE* file = std::fopen(dllPath.c_str(), "rb");
    if (file == nullptr) {
        *error = "не удалось открыть " + dllPath;
        return 0;
    }

    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    std::vector<u8> image(static_cast<usize>(fileSize));
    const usize read = std::fread(image.data(), 1, image.size(), file);
    std::fclose(file);

    if (read != image.size() || image.size() < sizeof(IMAGE_DOS_HEADER)) {
        *error = "файл DLL повреждён или пуст";
        return 0;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<usize>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size()) {
        *error = "это не PE-образ";
        return 0;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        *error = "неверная подпись PE";
        return 0;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        *error = "DLL не x64 — процессу x64 нужна библиотека x64";
        return 0;
    }

    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        *error = "в DLL нет таблицы экспорта";
        return 0;
    }

    auto rvaToOffset = [&](u64 rva) -> u64 {
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (u16 index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            const IMAGE_SECTION_HEADER& section = sections[index];
            const u64 start = section.VirtualAddress;
            const u64 end = start + section.Misc.VirtualSize;
            if (rva >= start && rva < end) {
                return rva - start + section.PointerToRawData;
            }
        }
        return 0;
    };

    const u64 exportOffset = rvaToOffset(directory.VirtualAddress);
    if (exportOffset == 0 || exportOffset + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size()) {
        *error = "таблица экспорта выходит за пределы файла";
        return 0;
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image.data() + exportOffset);

    const u64 namesOffset = rvaToOffset(exports->AddressOfNames);
    const u64 functionsOffset = rvaToOffset(exports->AddressOfFunctions);
    const u64 ordinalsOffset = rvaToOffset(exports->AddressOfNameOrdinals);

    if (namesOffset == 0 || functionsOffset == 0 || ordinalsOffset == 0) {
        *error = "не удалось разобрать таблицу экспорта";
        return 0;
    }

    const auto* names = reinterpret_cast<const u32*>(image.data() + namesOffset);
    const auto* functions = reinterpret_cast<const u32*>(image.data() + functionsOffset);
    const auto* ordinals = reinterpret_cast<const u16*>(image.data() + ordinalsOffset);

    for (u32 index = 0; index < exports->NumberOfNames; ++index) {
        const u64 nameOffset = rvaToOffset(names[index]);
        if (nameOffset == 0 || nameOffset >= image.size()) continue;

        const char* name = reinterpret_cast<const char*>(image.data() + nameOffset);
        if (std::strcmp(name, exportName) != 0) continue;

        const u16 ordinal = ordinals[index];
        if (ordinal >= exports->NumberOfFunctions) break;

        return functions[ordinal];
    }

    *error = std::string("экспорт ") + exportName + " не найден";
    return 0;
}

// ── Инжекция ────────────────────────────────────────────────────────────────

int Inject(const Options& options) {
    std::string dllPath = options.target;
    if (dllPath.empty()) {
        std::fprintf(stderr, "Укажите --dll <путь к GWYF_HookEngine.dll>\n");
        return 2;
    }

    // LoadLibraryW в чужом процессе получает только абсолютный путь.
    char absolute[MAX_PATH]{};
    if (GetFullPathNameA(dllPath.c_str(), MAX_PATH, absolute, nullptr) == 0) {
        std::fprintf(stderr, "Не удалось получить абсолютный путь к %s\n", dllPath.c_str());
        return 2;
    }
    dllPath = absolute;

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "Файл %s не найден\n", dllPath.c_str());
        return 2;
    }

    u32 pid = options.pid;
    if (pid == 0 && !options.target.empty() && options.target.find(".exe") != std::string::npos) {
        pid = FindProcessByName(options.target);
    }
    if (pid == 0) {
        std::fprintf(stderr, "Укажите --pid <pid игры> (или --process <имя.exe>)\n\n");
        ListGameProcesses();
        return 2;
    }

    std::printf("Внедряю %s в процесс %u\n", dllPath.c_str(), pid);

    UniqueHandle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                          PROCESS_VM_WRITE | PROCESS_VM_READ,
                                      FALSE, pid));
    if (!process.Valid()) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "OpenProcess не удался (код %lu). %s\n", error,
                     error == ERROR_ACCESS_DENIED
                         ? "Запустите инжектор от имени администратора и убедитесь, что разрядность совпадает."
                         : "Проверьте, что процесс ещё жив.");
        return 1;
    }

    // 1. Размещаем путь к DLL в адресном пространстве цели.
    // Ширину буфера считаем по РАСШИРЕННОЙ строке: для не-ASCII пути UTF-8
    // занимает больше байт, чем символов UTF-16, и наоборот.
    const std::wstring widePath = gwyf::ToWideUtf8(dllPath);
    const usize pathBytes = (widePath.size() + 1) * sizeof(wchar_t);

    void* remotePath = VirtualAllocEx(process.Get(), nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr) {
        std::fprintf(stderr, "VirtualAllocEx не удался (код %lu)\n", GetLastError());
        return 1;
    }

    if (!WriteProcessMemory(process.Get(), remotePath, widePath.c_str(), pathBytes, nullptr)) {
        std::fprintf(stderr, "WriteProcessMemory не удался (код %lu)\n", GetLastError());
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return 1;
    }

    // 2. CreateRemoteThread(LoadLibraryW).
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        std::fprintf(stderr, "Не удалось найти LoadLibraryW\n");
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return 1;
    }

    UniqueHandle thread(CreateRemoteThread(process.Get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr));
    if (!thread.Valid()) {
        std::fprintf(stderr, "CreateRemoteThread не удался (код %lu)\n", GetLastError());
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return 1;
    }

    if (WaitForSingleObject(thread.Get(), 20000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "Загрузка библиотеки в цель не завершилась за 20 секунд\n");
        return 1;
    }

    DWORD remoteModule = 0;
    GetExitCodeThread(thread.Get(), &remoteModule);
    if (remoteModule == 0) {
        std::fprintf(stderr, "LoadLibraryW в целевом процессе вернул 0 — библиотека не загрузилась\n");
        return 1;
    }

    std::printf("Библиотека загружена, базовый адрес в цели: 0x%08lX\n", remoteModule);

    // 3. Вызываем экспорт GWYF_Init внутри цели (если он есть).
    std::string parseError;
    const u64 initRva = FindExportRva(dllPath, "GWYF_Init", &parseError);
    if (initRva == 0) {
        std::printf("Экспорт GWYF_Init не найден (%s) — движок инициализируется сам при первом кадре.\n",
                    parseError.c_str());
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return 0;
    }

    auto remoteInit = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteModule + initRva);
    UniqueHandle initThread(CreateRemoteThread(process.Get(), nullptr, 0, remoteInit, nullptr, 0, nullptr));
    if (!initThread.Valid()) {
        std::fprintf(stderr, "Не удалось вызвать GWYF_Init (код %lu)\n", GetLastError());
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return 1;
    }

    if (WaitForSingleObject(initThread.Get(), 10000) == WAIT_OBJECT_0) {
        DWORD initResult = 0;
        GetExitCodeThread(initThread.Get(), &initResult);
        std::printf("GWYF_Init в цели вернул %lu (%s)\n", initResult,
                    initResult != 0 ? "движок готов" : "движок ещё ждёт Mono — инициализируется позже сам");
    } else {
        std::printf("GWYF_Init не успел за 10 секунд — это нормально: он повторит попытку на первом кадре.\n");
    }

    VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
    return 0;
}

// ── Обмен с игрой ───────────────────────────────────────────────────────────

/// Открыть мост со стороны Minecraft. Создавать регион нельзя: он принадлежит
/// игре, иначе мы получим два независимых «моста», которые не видят друг друга.
bool OpenObserver(ipc::Bridge& bridge, std::string* error) {
    if (!bridge.Open(ipc::Role::Minecraft, ipc::kDefaultRegionName, "cli/1", false, error)) {
        return false;
    }

    if (bridge.Header()->mcPid != 0 && bridge.Header()->mcPid != GetCurrentProcessId()) {
        std::printf("ВНИМАНИЕ: канал Minecraft уже использовался процессом %u. "
                    "Не запускайте gwyfbridge.exe одновременно с модом — продюсер кольца обязан быть один.\n",
                    bridge.Header()->mcPid);
    }
    return true;
}

int SendCommand(const Options& options, ipc::CommandCode code, const std::string& text, u32 timeoutSeconds) {
    ipc::Bridge bridge;
    std::string error;
    if (!OpenObserver(bridge, &error)) {
        std::fprintf(stderr, "Мост недоступен: %s\nЗапущена ли игра с внедрённой GWYF_HookEngine.dll?\n",
                     error.c_str());
        return 1;
    }

    ipc::Record record{};
    while (bridge.In().TryPop(record)) {
        // Чистим залежавшееся, чтобы ответ не потерялся среди старых событий.
    }

    if (!ipc::SendCommand(bridge.Out(), code, text, static_cast<u32>(options.x), static_cast<u32>(options.y))) {
        std::fprintf(stderr, "Не удалось отправить команду\n");
        return 1;
    }

    std::printf("Команда %s отправлена, жду ответ...\n", ipc::ToString(code));

    const u64 deadline = NowTickMs() + timeoutSeconds * 1000ull;
    while (NowTickMs() < deadline) {
        bridge.In().Wait(200);

        if (!bridge.In().Drain([](const ipc::Record&) {}, 64)) continue;

        ipc::Record answer{};
        while (bridge.In().TryPop(answer)) {
            if (answer.type != static_cast<u32>(ipc::RecordType::CommandAck)) continue;

            std::string body;
            u32 length = 0;
            if (answer.payload.command.textLength > 0 && bridge.In().TryReadBulk(body, length)) {
                std::printf("%s\n", body.c_str());
            } else {
                std::printf("Ответ получен (текст не приложен)\n");
            }

            return answer.payload.command.status == 0 ? 0 : 1;
        }
    }

    std::fprintf(stderr, "Ответ не пришёл за %u с.\n", timeoutSeconds);
    return 1;
}

int Watch(const Options& options) {
    ipc::Bridge bridge;
    std::string error;
    if (!OpenObserver(bridge, &error)) {
        std::fprintf(stderr, "Мост недоступен: %s\n", error.c_str());
        return 1;
    }

    std::printf("Наблюдаю события %u с (Ctrl+C для выхода)\n", options.seconds);
    const u64 deadline = NowTickMs() + options.seconds * 1000ull;
    u64 count = 0;

    while (NowTickMs() < deadline) {
        bridge.In().Wait(250);

        bridge.In().Drain([&](const ipc::Record& record) {
            ++count;
            const auto type = static_cast<ipc::RecordType>(record.type);

            switch (type) {
                case ipc::RecordType::BetPlaced:
                    std::printf("[BetPlaced] сумма %.*f, стол %u, игрок %llu\n", 2,
                                ipc::BitsToFloat(record.payload.bet.amountBits), record.payload.bet.tableId,
                                static_cast<unsigned long long>(record.payload.bet.playerId));
                    break;

                case ipc::RecordType::BetResolved:
                    std::printf("[BetResolved] выплата %lld, баланс %lld, выигрыш %u\n",
                                static_cast<long long>(record.payload.betResult.payout),
                                static_cast<long long>(record.payload.betResult.chipsAfter),
                                record.payload.betResult.won);
                    break;

                case ipc::RecordType::TableSpawned:
                    std::printf("[TableSpawned] стол %u, мест %u\n", record.payload.table.tableId,
                                record.payload.table.seats);
                    break;

                case ipc::RecordType::VoxelEdit:
                    std::printf("[VoxelEdit] %d,%d,%d блок %u действие %u\n", record.payload.voxel.x,
                                record.payload.voxel.y, record.payload.voxel.z, record.payload.voxel.block,
                                record.payload.voxel.action);
                    break;

                case ipc::RecordType::ChipState:
                    std::printf("[ChipState] фишек %lld, банк %lld\n",
                                static_cast<long long>(record.payload.chips.chips),
                                static_cast<long long>(record.payload.chips.bank));
                    break;

                case ipc::RecordType::CommandAck:
                    std::printf("[CommandAck] статус %u\n", record.payload.command.status);
                    break;

                default:
                    std::printf("[%s]\n", ipc::ToString(type));
                    break;
            }
        }, 256);
    }

    std::printf("Событий получено: %llu\n", static_cast<unsigned long long>(count));
    return 0;
}

int RunSelftest() {
    char modulePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    std::string directory = modulePath;
    const usize slash = directory.find_last_of("\\/");
    directory = slash == std::string::npos ? "." : directory.substr(0, slash);

    const std::string selftestPath = directory + "\\gwyfbridge_selftest.exe";
    if (GetFileAttributesA(selftestPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "Не найден %s — соберите цель gwyfbridge_selftest.\n", selftestPath.c_str());
        return 1;
    }

    std::printf("Запускаю %s\n\n", selftestPath.c_str());

    std::string commandLine = "\"" + selftestPath + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "Не удалось запустить самотест (код %lu)\n", GetLastError());
        return 1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    return static_cast<int>(exitCode);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options) || options.help || options.command.empty()) {
        PrintUsage();
        return options.help ? 0 : 2;
    }

    log::SetLevel(log::Level::Info);

    if (options.command == "inject") return Inject(options);
    if (options.command == "status") return SendCommand(options, ipc::CommandCode::Status, "", 5);
    if (options.command == "dump") {
        if (options.text.empty()) {
            std::fprintf(stderr, "Укажите --class <Полное.Имя.Класса>\n");
            return 2;
        }
        return SendCommand(options, ipc::CommandCode::DumpClass, options.text, 10);
    }
    if (options.command == "scan") {
        if (options.pattern.empty()) {
            std::fprintf(stderr, "Укажите --pattern \"48 89 ?? 24\"\n");
            return 2;
        }
        const std::string text = options.module.empty() ? options.pattern : options.module + "|" + options.pattern;
        return SendCommand(options, ipc::CommandCode::ScanPattern, text, 15);
    }
    if (options.command == "spawn-probe") {
        char coordinates[64]{};
        std::snprintf(coordinates, sizeof(coordinates), "%d,%d,%d", options.x, options.y, options.z);
        return SendCommand(options, ipc::CommandCode::SpawnProbe, coordinates, 10);
    }
    if (options.command == "flush") return SendCommand(options, ipc::CommandCode::FlushWorld, "", 15);
    if (options.command == "reload") return SendCommand(options, ipc::CommandCode::ReloadProfile, "", 5);
    if (options.command == "watch") return Watch(options);
    if (options.command == "selftest") return RunSelftest();

    std::fprintf(stderr, "Неизвестная команда: %s\n\n", options.command.c_str());
    PrintUsage();
    return 2;
}
