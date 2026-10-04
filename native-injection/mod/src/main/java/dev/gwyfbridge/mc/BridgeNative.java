// ============================================================================
//  BridgeNative.java — JNI-объявления нативной библиотеки моста.
//
//  ВАЖНО: имена методов и порядок аргументов должны совпадать с экспортами
//  в native/src/Native_JNI_Bridge.cpp. Соответствие проверяется автоматически
//  в CI: javac -h генерирует заголовок из этого класса, и скрипт
//  tools/check_jni_contract.py сверяет его с реализацией на C++.
//
//  ПОЧЕМУ JNI, А НЕ ПАЙП
//  ---------------------
//  Мод живёт внутри JVM Minecraft, а игра — отдельный процесс. Пайп означал бы
//  системный вызов на каждое событие. Здесь же JNI-вызов пишет запись прямо в
//  разделяемую память: путь «выигрыш в казино → предмет в инвентаре» укладывается
//  в доли миллисекунды.
//
//  ВНИМАНИЕ ПРО БУФЕРЫ
//  -------------------
//  Методы, принимающие ByteBuffer, требуют ПРЯМОЙ буфер
//  (ByteBuffer.allocateDirect) и порядка ByteOrder.nativeOrder(): нативная
//  сторона пишет структуры в порядке байтов машины (little-endian на x86-64).
// ============================================================================
package dev.gwyfbridge.mc;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class BridgeNative {

    private BridgeNative() {
    }

    /** Имя объекта JNI по умолчанию — то же, что в нативной части (IpcProtocol.h). */
    public static final String DEFAULT_REGION = "Local\\GWYF_MC_BRIDGE_ABI1";

    // ── Загрузка библиотеки ────────────────────────────────────────────────

    private static boolean loaded = false;
    private static String loadError = "";

    /**
     * Загружает нативную библиотеку. Возвращает true, если она доступна.
     * Путь задаёт либо аргумент, либо системное свойство gwyfbridge.native.path.
     */
    public static synchronized boolean load(String explicitPath) {
        if (loaded) {
            return true;
        }

        String path = explicitPath;
        if (path == null || path.isEmpty()) {
            path = System.getProperty("gwyfbridge.native.path", "");
        }

        try {
            if (path.isEmpty()) {
                System.loadLibrary("gwyf_native_bridge");
            } else {
                System.load(path);
            }
            loaded = true;
            loadError = "";
        } catch (UnsatisfiedLinkError error) {
            loadError = error.getMessage();
        }

        return loaded;
    }

    public static String loadError() {
        return loadError;
    }

    // ── Инициализация и состояние ──────────────────────────────────────────

    /** 0 = успех, отрицательное значение = код ошибки (см. nativeLastError). */
    public static native int nativeInit(String regionName, String profileTag);

    public static native void nativeShutdown();

    public static native boolean nativeIsOpen();

    public static native boolean nativePeerAlive();

    public static native boolean nativeHeartbeat();

    /** Строка диагностики: состояние моста, счётчики, последняя ошибка. */
    public static native String nativeStatus();

    public static native String nativeLastError();

    // ── Приём событий из игры ──────────────────────────────────────────────

    /**
     * Читает готовые записи в прямой буфер. Возвращает число записей;
     * каждая запись — BridgeRecords.RECORD_STRIDE байт.
     */
    public static native int nativePoll(ByteBuffer out, int maxRecords);

    // ── Отправка событий в игру ────────────────────────────────────────────

    /** action: 1 = поставить блок, 2 = сломать (см. BridgeRecords.ACTION_*). */
    public static native boolean nativePushVoxel(int x, int y, int z, int block, int action, long authorId);

    /**
     * Массовая отправка: упакованные правки по 20 байт (x, y, z, block, action).
     * Возвращает число реально принятых правок.
     */
    public static native int nativePushBulk(ByteBuffer packed, int count);

    /** Команда нативной части игры: dump/status/scan/flush/spawn-probe. */
    public static native boolean nativeSendCommand(int code, String text, int arg0, int arg1);

    // ── Контракт раскладки ─────────────────────────────────────────────────

    /** Текстовая раскладка структур (для логов и разбора расхождений). */
    public static native String nativeRecordLayout();

    /** Хеш раскладки: обязан совпасть с BridgeRecords.LAYOUT_HASH. */
    public static native int nativeLayoutHash();

    // ── Утилиты для вызывающего кода ───────────────────────────────────────

    /** Создать буфер нужного размера с правильным порядком байтов. */
    public static ByteBuffer allocateRecordBuffer(int maxRecords) {
        final ByteBuffer buffer = ByteBuffer.allocateDirect(maxRecords * BridgeRecords.RECORD_STRIDE);
        buffer.order(ByteOrder.nativeOrder());
        return buffer;
    }

    /** Буфер под упакованные правки вокселей. */
    public static ByteBuffer allocateBulkBuffer(int maxEdits) {
        final ByteBuffer buffer = ByteBuffer.allocateDirect(maxEdits * BridgeRecords.BULK_EDIT_STRIDE);
        buffer.order(ByteOrder.nativeOrder());
        return buffer;
    }
}
