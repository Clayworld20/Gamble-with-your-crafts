// ============================================================================
//  Native_JNI_Bridge.cpp — нативная библиотека, которую загружает JVM
//  Minecraft (Fabric-мод). Это «мост» между Java-миром и разделяемой памятью,
//  через которую идёт обмен с игрой Gamble With Your Friends.
//
//  ПОЧЕМУ JNI, А НЕ ПРОСТО ПАЙП
//  ----------------------------
//  Мод живёт внутри JVM, поэтому самый дешёвый путь — не гонять байты через
//  ядро (pipe/socket), а вызвать нативный метод: JNI-вызов стоит десятки
//  наносекунд, а запись в кольцо разделяемой памяти не делает ни одной
//  системной операции. Именно отсюда берётся «мгновенно выдать блоки»:
//  выигрыш в казино → запись в кольцо игры → и в тот же тик клиент Minecraft
//  получает предмет.
//
//  ЧЕГО ЗДЕСЬ НЕТ И БЫТЬ НЕ МОЖЕТ
//  ------------------------------
//  Попытки «присоединиться» к уже запущенной JVM снаружи через JNI: у
//  внешнего процесса нет JNIEnv и он не может войти в чужую JVM. Поэтому
//  нативная библиотека грузится ИЗНУТРИ JVM (System.load), а связь с игрой
//  идёт по разделяемой памяти. Это же гарантирует, что мы не трогаем чужой
//  процесс за пределами выделенного региона.
//
//  ПОТОКИ И ПОРЯДОК
//  ----------------
//  Все методы вызываются из потоков JVM (как правило, клиентского). Нативная
//  часть защищена мьютексом: продюсер кольца обязан быть единственным, а
//  Fabric может вызвать обработчик из другого потока (например, из сетевого).
//  Мьютекс здесь допустим: это НЕ игровой поток GWYF, ждать миллисекунды
//  безопасно. В самом GWYF (GWYF_HookEngine.cpp) блокировки запрещены.
// ============================================================================
#include <jni.h>

#include "gwyfbridge/IpcBridge.h"
#include "gwyfbridge/McWireFormat.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Файл целиком описывает нативный слой моста, поэтому имена gwyf доступны
// без квалификации; имена JNI-функций обязаны быть в глобальном пространстве.
using namespace gwyf;

namespace {

/// Версия, которую мы сообщаем JVM. 1.8 — минимально совместимый уровень:
/// именно его понимают все актуальные JVM, включая те, что идут с Minecraft.
constexpr jint kRequiredJniVersion = JNI_VERSION_1_8;

std::mutex g_mutex;
ipc::Bridge g_bridge;
JavaVM* g_vm = nullptr;
bool g_initialized = false;
std::string g_lastError;
u64 g_recordsRead = 0;
u64 g_editsSent = 0;
u64 g_bulkFrames = 0;
u64 g_sendFailures = 0;

/// Преобразовать jstring в std::wstring (имена объектов ядра — wide-строки).
std::wstring ToWide(JNIEnv* env, jstring text) {
    if (text == nullptr) return std::wstring();

    const jsize length = env->GetStringLength(text);
    const jchar* chars = env->GetStringChars(text, nullptr);
    if (chars == nullptr) return std::wstring();

    std::wstring result;
    result.reserve(static_cast<usize>(length));
    for (jsize index = 0; index < length; ++index) {
        result.push_back(static_cast<wchar_t>(chars[index]));
    }

    env->ReleaseStringChars(text, chars);
    return result;
}

std::string ToUtf8(JNIEnv* env, jstring text) {
    if (text == nullptr) return std::string();

    const char* utf8 = env->GetStringUTFChars(text, nullptr);
    if (utf8 == nullptr) return std::string();

    std::string result = utf8;
    env->ReleaseStringUTFChars(text, utf8);
    return result;
}

jstring FromUtf8(JNIEnv* env, const std::string& text) {
    return env->NewStringUTF(text.c_str());
}

/// Открыть регион под роль Minecraft. Сначала пробуем подключиться к уже
/// созданному региону (обычно его создаёт игра), и только если его нет —
/// создаём сами и ждём, пока игра поднимется.
bool OpenBridge(const std::wstring& regionName, std::string* error) {
    if (g_bridge.IsOpen()) return true;

    std::string openError;
    if (g_bridge.Open(ipc::Role::Minecraft, regionName, "fabric-mod/1", false, &openError)) {
        return true;
    }

    GWYF_WARN("Регион %ls не найден (%s) — создаю его со стороны Minecraft и жду запуска игры",
              regionName.c_str(), openError.c_str());

    std::string createError;
    if (g_bridge.Open(ipc::Role::Minecraft, regionName, "fabric-mod/1", true, &createError)) {
        return true;
    }

    *error = createError;
    return false;
}

/// Отправить одну правку вокселя. Вызывающий код держит g_mutex.
bool PushEdit(const ipc::PayloadVoxel& edit) {
    if (!g_bridge.IsOpen()) return false;

    ipc::Payload payload{};
    payload.voxel = edit;

    // authorId передаём отдельным полем записи: он длиннее 32 байт payload.
    if (!g_bridge.Out().Push(ipc::RecordType::VoxelEdit, payload, edit.authorId)) {
        g_sendFailures++;
        return false;
    }

    g_editsSent++;
    return true;
}

/// Отправить массив правок: кадр кладём в bulk-кольцо, а по кольцу записей
/// идёт «конверт» VoxelEdit с action=BulkReplace и числом элементов в aux.
bool PushBulkEdits(const ipc::PayloadVoxel* edits, u32 count) {
    if (!g_bridge.IsOpen() || edits == nullptr || count == 0) return false;

    std::vector<u8> frame(static_cast<usize>(count) * mcwire::kBulkEditStride);
    const u32 packed = mcwire::PackBulkEdits(edits, count, frame.data(), static_cast<u32>(frame.size()));
    if (packed == 0) return false;

    if (!g_bridge.Out().PushBulk(frame.data(), packed * mcwire::kBulkEditStride, ipc::kBulkKindVoxel)) {
        g_sendFailures++;
        return false;
    }

    ipc::Payload payload{};
    payload.voxel.action = static_cast<u32>(ipc::VoxelAction::BulkReplace);
    payload.voxel.block = edits[0].block;

    g_bulkFrames++;
    g_editsSent += packed;
    return g_bridge.Out().Push(ipc::RecordType::VoxelEdit, payload, 0, packed);
}

}  // namespace

extern "C" {

// ── Жизненный цикл библиотеки ───────────────────────────────────────────────

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_vm = vm;

    // Лог открываем рядом с процессом Minecraft: он нужен, чтобы диагностировать
    // ситуацию «мод загрузился, а игра не отвечает».
    gwyf::log::Open("gwyf_bridge_mc.log", gwyf::log::Level::Info);
    GWYF_INFO("gwyf_native_bridge загружен в JVM (%s)", mcwire::Describe().c_str());

    return kRequiredJniVersion;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_bridge.IsOpen()) {
        g_bridge.Close();
    }
    g_initialized = false;
    GWYF_INFO("gwyf_native_bridge выгружен");
    gwyf::log::Close();
}

// ── Инициализация ───────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeInit(JNIEnv* env, jclass klass, jstring regionName,
                                                                     jstring profileTag) {
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);

    const std::string requestedTag = ToUtf8(env, profileTag);
    const std::wstring region = regionName != nullptr ? ToWide(env, regionName) : std::wstring(ipc::kDefaultRegionName);

    std::string error;
    if (!OpenBridge(region, &error)) {
        g_lastError = error;
        GWYF_ERROR("nativeInit: не удалось открыть мост: %s", error.c_str());
        return -1;
    }

    g_initialized = true;
    GWYF_INFO("nativeInit: мост открыт (регион %ls, тег %s)", region.c_str(), requestedTag.c_str());

    // Приветствие игре: она по нему понимает, что мод готов.
    ipc::Payload payload{};
    std::snprintf(reinterpret_cast<char*>(payload.raw), ipc::kInlinePayload, "fabric %s",
                  requestedTag.empty() ? "mod" : requestedTag.c_str());
    g_bridge.Out().Push(ipc::RecordType::Hello, payload);
    return 0;
}

JNIEXPORT void JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeShutdown(JNIEnv* env, jclass klass) {
    (void)env;
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_bridge.IsOpen()) {
        g_bridge.Out().PushSimple(ipc::RecordType::Bye);
        g_bridge.Close();
    }
    g_initialized = false;
}

JNIEXPORT jboolean JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeIsOpen(JNIEnv* env, jclass klass) {
    (void)env;
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bridge.IsOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativePeerAlive(JNIEnv* env, jclass klass) {
    (void)env;
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bridge.PeerAlive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeHeartbeat(JNIEnv* env, jclass klass) {
    (void)env;
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_bridge.IsOpen()) return JNI_FALSE;

    g_bridge.Out().Heartbeat();
    g_bridge.Out().PushSimple(ipc::RecordType::Heartbeat);
    return JNI_TRUE;
}

// ── Приём данных из игры ────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativePoll(JNIEnv* env, jclass klass, jobject byteBuffer,
                                                                     jint maxRecords) {
    (void)klass;

    if (byteBuffer == nullptr || maxRecords <= 0) return 0;

    auto* output = static_cast<u8*>(env->GetDirectBufferAddress(byteBuffer));
    if (output == nullptr) {
        g_lastError = "nativePoll: буфер должен быть прямым (ByteBuffer.allocateDirect)";
        return 0;
    }

    const jlong capacity = env->GetDirectBufferCapacity(byteBuffer);
    const u32 maxByCapacity = static_cast<u32>(capacity / static_cast<jlong>(mcwire::kRecordStride));
    u32 limit = static_cast<u32>(maxRecords);
    if (limit > maxByCapacity) limit = maxByCapacity;
    if (limit == 0) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_bridge.IsOpen()) {
        // Пытаемся восстановить соединение: игра могла запуститься позже мода
        // или регион мог быть создан заново.
        if (g_initialized) {
            std::string error;
            const std::wstring region = std::wstring(ipc::kDefaultRegionName);
            if (!OpenBridge(region, &error)) return 0;
        } else {
            return 0;
        }
    }

    u32 written = 0;
    ipc::Record record{};

    while (written < limit && g_bridge.In().TryPop(record)) {
        u32 recordBytes = 0;
        if (!mcwire::WriteRecord(output + static_cast<usize>(written) * mcwire::kRecordStride,
                                 mcwire::kRecordStride, record, &recordBytes)) {
            break;
        }
        ++written;
        ++g_recordsRead;

        // VoxelEdit с BulkReplace: следом в bulk-кольце лежит пакет правок,
        // читаем его сразу, чтобы Java получила согласованную пару.
        if (record.type == static_cast<u32>(ipc::RecordType::VoxelEdit) &&
            record.payload.voxel.action == static_cast<u32>(ipc::VoxelAction::BulkReplace)) {
            // Java получает только «конверт»; полезную нагрузку читает
            // следующая версия протокола (см. комментарий в BridgeRecords.java).
        }
    }

    return static_cast<jint>(written);
}

// ── Отправка правок в игру ─────────────────────────────────────────────────

JNIEXPORT jboolean JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativePushVoxel(JNIEnv* env, jclass klass, jint x,
                                                                              jint y, jint z, jint block, jint action,
                                                                              jlong authorId) {
    (void)env;
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_bridge.IsOpen()) return JNI_FALSE;

    ipc::PayloadVoxel edit{};
    edit.x = x;
    edit.y = y;
    edit.z = z;
    edit.block = static_cast<u32>(block);
    edit.action = static_cast<u32>(action);
    edit.authorId = static_cast<u64>(authorId);

    return PushEdit(edit) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativePushBulk(JNIEnv* env, jclass klass, jobject packed,
                                                                         jint count) {
    (void)klass;

    if (packed == nullptr || count <= 0) return 0;

    const auto* data = static_cast<const u8*>(env->GetDirectBufferAddress(packed));
    if (data == nullptr) {
        g_lastError = "nativePushBulk: буфер должен быть прямым (ByteBuffer.allocateDirect)";
        return 0;
    }

    const jlong capacity = env->GetDirectBufferCapacity(packed);
    const u32 available = static_cast<u32>(capacity / static_cast<jlong>(mcwire::kBulkEditStride));
    u32 total = static_cast<u32>(count);
    if (total > available) total = available;

    std::vector<ipc::PayloadVoxel> edits;
    edits.reserve(total);

    for (u32 index = 0; index < total; ++index) {
        const u8* entry = data + static_cast<usize>(index) * mcwire::kBulkEditStride;
        ipc::PayloadVoxel edit{};
        std::memcpy(&edit.x, entry + 0, sizeof(edit.x));
        std::memcpy(&edit.y, entry + 4, sizeof(edit.y));
        std::memcpy(&edit.z, entry + 8, sizeof(edit.z));
        std::memcpy(&edit.block, entry + 12, sizeof(edit.block));
        std::memcpy(&edit.action, entry + 16, sizeof(edit.action));
        edits.push_back(edit);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_bridge.IsOpen()) return 0;

    u32 sent = 0;
    // Режем пакет по вместимости кадра bulk-кольца.
    while (sent < total) {
        const u32 chunk = (total - sent) > mcwire::kBulkEditMaxPerFrame ? mcwire::kBulkEditMaxPerFrame : (total - sent);
        if (!PushBulkEdits(edits.data() + sent, chunk)) break;
        sent += chunk;
    }

    return static_cast<jint>(sent);
}

/// Отправить команду игре (dump/status/scan/flush/spawn-probe).
JNIEXPORT jboolean JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeSendCommand(JNIEnv* env, jclass klass, jint code,
                                                                                jstring text, jint arg0, jint arg1) {
    (void)klass;

    const std::string body = ToUtf8(env, text);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_bridge.IsOpen()) return JNI_FALSE;

    return ipc::SendCommand(g_bridge.Out(), static_cast<ipc::CommandCode>(code), body, static_cast<u32>(arg0),
                            static_cast<u32>(arg1))
               ? JNI_TRUE
               : JNI_FALSE;
}

// ── Диагностика ─────────────────────────────────────────────────────────────

JNIEXPORT jstring JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeStatus(JNIEnv* env, jclass klass) {
    (void)klass;

    std::lock_guard<std::mutex> lock(g_mutex);

    std::string text;
    text += g_bridge.IsOpen() ? g_bridge.Describe() : std::string("мост закрыт");
    text += "\n  " + mcwire::Describe();

    char counters[256]{};
    std::snprintf(counters, sizeof(counters),
                  "\n  получено записей %llu, отправлено правок %llu (пакетов %llu), отказов отправки %llu",
                  static_cast<unsigned long long>(g_recordsRead), static_cast<unsigned long long>(g_editsSent),
                  static_cast<unsigned long long>(g_bulkFrames), static_cast<unsigned long long>(g_sendFailures));
    text += counters;

    if (!g_lastError.empty()) {
        text += "\n  последняя ошибка: " + g_lastError;
    }

    return FromUtf8(env, text);
}

JNIEXPORT jstring JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeRecordLayout(JNIEnv* env, jclass klass) {
    (void)klass;
    return FromUtf8(env, mcwire::RecordLayoutText());
}

JNIEXPORT jint JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeLayoutHash(JNIEnv* env, jclass klass) {
    (void)env;
    (void)klass;
    return static_cast<jint>(mcwire::LayoutHash());
}

JNIEXPORT jstring JNICALL Java_dev_gwyfbridge_mc_BridgeNative_nativeLastError(JNIEnv* env, jclass klass) {
    (void)klass;
    return FromUtf8(env, g_lastError);
}

}  // extern "C"
