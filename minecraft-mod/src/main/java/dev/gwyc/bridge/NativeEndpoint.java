package dev.gwyc.bridge;

/**
 * Точка стыковки с нативным мостом через JNI.
 *
 * <p>Зачем: когда C++-мост живёт В ТОМ ЖЕ процессе, что и JVM (харнесс, тестовый
 * запуск, встроенный сервер), IPC через файл не нужен — вызовы идут напрямую.
 * Обратите внимание: JNI работает только внутри процесса. «Присоединиться» к
 * чужой JVM по JNI из другого процесса нельзя в принципе — для этого и существует
 * файловый канал (BridgeChannel).
 *
 * <p>Два направления:
 * <ul>
 *   <li>Java → C++: нативные методы {@code voxelEditFromNative} и
 *       {@code rewardFromNative}. Их реализации регистрирует C++ через
 *       {@code RegisterNatives} (см. JniBridge::RegisterNatives).</li>
 *   <li>C++ → Java: статические методы {@code onVoxelEdit} и {@code onReward},
 *       которые C++ вызывает как обычные статические методы класса
 *       (сигнатуры {@code (IIIB)V} и {@code (III)V}).</li>
 * </ul>
 *
 * <p>Если нативной библиотеки рядом нет, класс остаётся рабочим: нативные методы
 * просто не вызываются, а мод живёт на канале.
 */
public final class NativeEndpoint {

    /** Приёмники, зарегистрированные модом: они применяют события к миру. */
    private static VoxelSink voxelSink;
    private static RewardSink rewardSink;

    private static boolean libraryLoaded;

    private NativeEndpoint() {
    }

    public interface VoxelSink {
        void apply(int x, int y, int z, int blockKind);
    }

    public interface RewardSink {
        void grant(int playerId, int blockKind, int count);
    }

    public static void setSinks(VoxelSink voxel, RewardSink reward) {
        voxelSink = voxel;
        rewardSink = reward;
    }

    /**
     * Попытаться загрузить нативную библиотеку моста.
     *
     * <p>Отсутствие библиотеки — нормальная ситуация (обычная игра идёт через
     * канал), поэтому ошибка только логируется: падать из-за неё мод не должен.
     */
    public static synchronized boolean tryLoadLibrary() {
        if (libraryLoaded) {
            return true;
        }

        for (String name : new String[]{ "GWYFCraftsBridge", "gwyc_bridge" }) {
            try {
                System.loadLibrary(name);
                libraryLoaded = true;
                System.out.println("[gwyc] нативная библиотека загружена: " + name);
                return true;
            } catch (UnsatisfiedLinkError ignored) {
                // Пробуем следующее имя.
            }
        }

        System.out.println("[gwyc] нативная библиотека не найдена — работаем через канал");
        return false;
    }

    public static boolean isLibraryLoaded() {
        return libraryLoaded;
    }

    // ── Java → C++ (реализации регистрирует нативная сторона) ────────────────

    /** Правка вокселя со стороны Java. Реализация приходит из C++ (RegisterNatives). */
    public static native void voxelEditFromNative(int x, int y, int z, byte blockKind);

    /** Запрос награды со стороны Java. Реализация приходит из C++ (RegisterNatives). */
    public static native void rewardFromNative(int playerId, int blockKind, int count);

    // ── C++ → Java (вызывается нативным мостом напрямую) ─────────────────────

    /** Вызывается C++: поставить блок в мир Minecraft. */
    public static void onVoxelEdit(int x, int y, int z, byte blockKind) {
        final VoxelSink sink = voxelSink;
        if (sink != null) {
            sink.apply(x, y, z, blockKind & 0xFF);
        }
    }

    /** Вызывается C++: выдать игроку блоки за выигрыш. */
    public static void onReward(int playerId, int blockKind, int count) {
        final RewardSink sink = rewardSink;
        if (sink != null) {
            sink.grant(playerId, blockKind, count);
        }
    }

    // ── удобные обёртки для мода ─────────────────────────────────────────────

    /** Отправить правку в C++ (если библиотека загружена). */
    public static boolean sendVoxelEdit(int x, int y, int z, int blockKind) {
        if (!libraryLoaded) {
            return false;
        }
        voxelEditFromNative(x, y, z, (byte) blockKind);
        return true;
    }

    /** Отправить запрос награды в C++ (если библиотека загружена). */
    public static boolean sendReward(int playerId, int blockKind, int count) {
        if (!libraryLoaded) {
            return false;
        }
        rewardFromNative(playerId, blockKind, count);
        return true;
    }
}
