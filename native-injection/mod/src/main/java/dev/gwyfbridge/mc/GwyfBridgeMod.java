// ============================================================================
//  GwyfBridgeMod.java — клиентский вход Fabric-мода.
//
//  ЧТО ДЕЛАЕТ МОД
//  -------------
//  1. грузит нативную библиотеку gwyf_native_bridge.dll и открывает мост
//     с процессом игры (разделяемая память);
//  2. раз в тик-два забирает события из игры: выигрыш в казино, создание стола,
//     телеметрию фишек;
//  3. выигрыш превращает в блоки в креативном инвентаре;
//  4. правки блоков отправляет обратно: игра создаёт у себя настоящие объекты,
//     которые видят все игроки лобби (см. native/src/VoxelTo3DWorld.cpp).
//
//  ЧТО ЗДЕСЬ ПРИНЦИПИАЛЬНО НЕ ДЕЛАЕТСЯ
//  -----------------------------------
//  * не читается и не пишется память JVM: только JNI-вызовы своей библиотеки;
//  * не подменяется серверная логика: выдача предметов — клиентская, в
//     креативном режиме. На выделенном сервере нужны права и своя проверка;
//  * при рассинхроне раскладки (хеш не совпал) мод не «угадывает», а
//     отключает обмен и пишет причину в лог.
// ============================================================================
package dev.gwyfbridge.mc;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.loader.api.FabricLoader;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.nio.file.Files;
import java.nio.file.Path;

public final class GwyfBridgeMod implements ClientModInitializer {

    public static final String MOD_ID = "gwyfbridge";
    public static final Logger LOGGER = LoggerFactory.getLogger("GWYF-Bridge");

    /** Раз в сколько тиков опрашивать мост (1 = каждый тик, 20 т/с). */
    private static final int POLL_INTERVAL_TICKS = 1;

    /** Как часто отправлять пульс, чтобы игра видела, что мод жив (в тиках). */
    private static final int HEARTBEAT_INTERVAL_TICKS = 20;

    private static VoxelForwarder forwarder;
    private static boolean bridgeReady = false;

    @Override
    public void onInitializeClient() {
        LOGGER.info("GWYF Bridge: инициализация клиентской части");

        bridgeReady = openBridge();
        if (!bridgeReady) {
            LOGGER.error("GWYF Bridge: мост не открыт — мод работает в режиме наблюдения. Причина: {}",
                    BridgeNative.loadError());
            return;
        }

        forwarder = new VoxelForwarder();
        forwarder.registerEvents();

        final int[] tickCounter = {0};

        ClientTickEvents.END_CLIENT_TICK.register(client -> {
            tickCounter[0]++;
            if (tickCounter[0] % POLL_INTERVAL_TICKS == 0) {
                drainIncoming();
            }
            if (tickCounter[0] % HEARTBEAT_INTERVAL_TICKS == 0) {
                BridgeNative.nativeHeartbeat();
                if (!BridgeNative.nativePeerAlive()) {
                    LOGGER.warn("GWYF Bridge: игра не отвечает на пульс — проверьте, запущена ли Gamble With Your Friends "
                            + "с внедрённой GWYF_HookEngine.dll");
                }
            }
        });

        LOGGER.info("GWYF Bridge: готов. {}", BridgeNative.nativeStatus());
    }

    /** Открыть мост и проверить совместимость раскладки. */
    private static boolean openBridge() {
        final Path configDirectory = FabricLoader.getInstance().getConfigDir();
        final Path nativePath = configDirectory.resolve("gwyfbridge").resolve("gwyf_native_bridge.dll");

        String explicitPath = Files.exists(nativePath) ? nativePath.toAbsolutePath().toString() : "";
        if (explicitPath.isEmpty()) {
            LOGGER.warn("GWYF Bridge: {} не найден, пробую системный поиск библиотеки", nativePath);
        } else {
            LOGGER.info("GWYF Bridge: загружаю нативную библиотеку {}", explicitPath);
        }

        if (!BridgeNative.load(explicitPath)) {
            return false;
        }

        final int result = BridgeNative.nativeInit(BridgeNative.DEFAULT_REGION, "fabric-1.20");
        if (result != 0) {
            LOGGER.error("GWYF Bridge: nativeInit вернул {}. {}", result, BridgeNative.nativeLastError());
            return false;
        }

        // Сверка раскладки: если C++ и Java разошлись, лучше остановиться сразу.
        final String layoutText = BridgeNative.nativeRecordLayout();
        final int nativeHash = BridgeNative.nativeLayoutHash();
        final int javaHash = BridgeRecords.layoutHashOf(layoutText);

        if (nativeHash != BridgeRecords.LAYOUT_HASH || javaHash != BridgeRecords.LAYOUT_HASH) {
            LOGGER.error("GWYF Bridge: раскладка данных не совпадает! нативная #{}, java #{}, от нативной стороны #{}. "
                            + "Обмен отключён, чтобы не портить инвентарь и мир. Обновите мод и библиотеку вместе.",
                    Integer.toHexString(BridgeRecords.LAYOUT_HASH), Integer.toHexString(javaHash),
                    Integer.toHexString(nativeHash));
            BridgeNative.nativeShutdown();
            return false;
        }

        return true;
    }

    /** Забрать и обработать накопившиеся события игры. */
    private static void drainIncoming() {
        if (!bridgeReady || !BridgeNative.nativeIsOpen()) {
            return;
        }

        final int maxRecords = 64;
        final java.nio.ByteBuffer buffer = BridgeNative.allocateRecordBuffer(maxRecords);
        final int count = BridgeNative.nativePoll(buffer, maxRecords);
        if (count <= 0) {
            return;
        }

        for (BridgeRecords.Record record : BridgeRecords.parse(buffer, count)) {
            switch (record.type) {
                case BridgeRecords.TYPE_BET_PLACED:
                    LOGGER.debug("GWYF: ставка {} (тип {}) на столе {}",
                            record.betAmount(), record.betKind(), record.betTableId());
                    break;

                case BridgeRecords.TYPE_BET_RESOLVED:
                    onRoundResolved(record);
                    break;

                case BridgeRecords.TYPE_TABLE_SPAWNED:
                    LOGGER.info("GWYF: создан стол #{} (мест {}, игра {})",
                            record.tableId(), record.tableSeats(), record.tableGameKind());
                    break;

                case BridgeRecords.TYPE_CHIP_STATE:
                    LOGGER.debug("GWYF: фишек {}, в банке {}", record.chipsBalance(), record.chipsBank());
                    break;

                case BridgeRecords.TYPE_BLOCK_GRANT:
                    BlockGranter.grant(record.grantBlock(), record.grantCount(), record.grantReason());
                    break;

                case BridgeRecords.TYPE_COMMAND_ACK:
                    LOGGER.info("GWYF: ответ на команду, текст {} байт ({} символов)",
                            record.commandTextLength(), record.commandTextLength());
                    break;

                case BridgeRecords.TYPE_LOG_LINE:
                    LOGGER.info("GWYF: строка лога от игры");
                    break;

                case BridgeRecords.TYPE_BYE:
                    LOGGER.warn("GWYF: игра закрыла мост");
                    bridgeReady = false;
                    break;

                default:
                    break;
            }
        }
    }

    /**
     * Итог раунда: показываем игроку результат и, если он выиграл, выдаём блоки.
     * Сумма выигрыша переводится в количество блоков по курсу, настроенному
     * в BlockGranter (одна фишка = один блок по умолчанию).
     */
    private static void onRoundResolved(BridgeRecords.Record record) {
        final long payout = record.resultPayout();
        final boolean won = record.resultWon();

        if (!won || payout <= 0) {
            LOGGER.debug("GWYF: раунд завершён без выигрыша (выплата {})", payout);
            return;
        }

        LOGGER.info("GWYF: выигрыш {} фишек — выдаю блоки в креативный инвентарь", payout);
        BlockGranter.grantRewardBlocks(payout);
    }

    /** Отправить правку блока в игру (вызывается из VoxelForwarder). */
    public static boolean pushVoxelEdit(int x, int y, int z, int block, int action, long authorId) {
        if (!bridgeReady) {
            return false;
        }
        return BridgeNative.nativePushVoxel(x, y, z, block, action, authorId);
    }

    public static boolean isBridgeReady() {
        return bridgeReady;
    }

    /** Вызывается при выходе из мира: просим игру убрать созданные объекты. */
    public static void requestWorldFlush() {
        if (bridgeReady) {
            BridgeNative.nativeSendCommand(BridgeRecords.COMMAND_FLUSH_WORLD, "", 0, 0);
        }
    }

    public static VoxelForwarder forwarder() {
        return forwarder;
    }
}
