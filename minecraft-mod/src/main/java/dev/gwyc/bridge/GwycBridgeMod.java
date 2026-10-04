package dev.gwyc.bridge;

import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.event.player.PlayerBlockBreakEvents;
import net.fabricmc.fabric.api.event.player.UseBlockCallback;
import net.minecraft.block.Block;
import net.minecraft.block.BlockState;
import net.minecraft.entity.player.PlayerEntity;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.command.CommandManager;
import net.minecraft.server.network.ServerPlayerEntity;
import net.minecraft.server.world.ServerWorld;
import net.minecraft.text.Text;
import net.minecraft.util.ActionResult;
import net.minecraft.util.Hand;
import net.minecraft.util.hit.BlockHitResult;
import net.minecraft.util.math.BlockPos;
import net.minecraft.world.World;

import java.nio.ByteBuffer;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

/**
 * Сторона Minecraft: соединяет мир игры с казино.
 *
 * <p>Что делает мод:
 * <ul>
 *   <li>поднимает канал обмена (файл в памяти) при старте сервера и закрывает при остановке;</li>
 *   <li>на каждое изменение блока игроком отправляет игре правку вокселя — та ставит
 *       «физический» куб в своём мире, и друзья видят изменение сразу;</li>
 *   <li>на выигрыш в казино получает GrantReward и выдаёт игроку блоки в креативе;</li>
 *   <li>по запросу игры отдаёт снимок региона (RLE) и принимает чужие правки.</li>
 * </ul>
 *
 * <p>Весь обмен идёт в потоке сервера (тик) — так у мода нет гонок с игровым миром,
 * а канал остаётся честным SPSC: один писатель, один читатель.
 */
public final class GwycBridgeMod implements ModInitializer {

    public static final String MOD_ID = "gwyc_bridge";
    public static final String BUILD_TAG = "fabric-1.0.0";

    private static final int WORLD_MIN = -30_000_000;
    private static final int WORLD_MAX = 30_000_000;

    private final BridgeChannel channel = new BridgeChannel();
    private GwycConfig config = new GwycConfig();

    /** Правки, накопленные за текущий тик: отправляются одной пачкой. */
    private final List<Payloads.VoxelEdit> outbox = new ArrayList<>();

    /** Отложенные проверки «поставился ли блок» — снимок состояний до действия. */
    private final List<PendingCheck> pendingChecks = new ArrayList<>();

    /** Идентификаторы игроков казино → имена игроков Minecraft (для выдачи наград). */
    private final Map<Integer, String> casinoPlayers = new HashMap<>();

    private MinecraftServer server;
    private int tickCounter;

    @Override
    public void onInitialize() {
        config = GwycConfig.load();

        ServerLifecycleEvents.SERVER_STARTED.register(startedServer -> {
            server = startedServer;
            if (config.enabled) {
                connect();
            } else {
                log("мост выключен в конфиге (" + GwycConfig.configFile() + ")");
            }
        });

        ServerLifecycleEvents.SERVER_STOPPING.register(stoppingServer -> disconnect());

        ServerTickEvents.END_SERVER_TICK.register(this::onTick);

        // Игрок сломал блок — это правка вокселя для казино.
        PlayerBlockBreakEvents.AFTER.register((world, player, pos, state, entity) -> {
            if (!isMirroring(world)) {
                return;
            }
            reportBlockChange(pos, BlockMap.AIR, player);
        });

        // Игрок кликнул по блоку: сам факт установки узнаём на следующем тике,
        // сравнив состояния двух кандидатов (место клика и соседняя клетка).
        UseBlockCallback.EVENT.register((player, world, hand, hitResult) -> {
            if (world.isClient() || hand != Hand.MAIN_HAND || !isMirroring(world)) {
                return ActionResult.PASS;
            }
            schedulePlacementCheck(world, hitResult);
            return ActionResult.PASS;
        });

        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) ->
                dispatcher.register(CommandManager.literal("gwyc")
                        .requires(source -> source.hasPermissionLevel(2))
                        .executes(context -> {
                            status(context.getSource());
                            return 1;
                        })
                        .then(CommandManager.literal("status").executes(context -> {
                            status(context.getSource());
                            return 1;
                        }))
                        .then(CommandManager.literal("reload").executes(context -> {
                            config = GwycConfig.load();
                            context.getSource().sendFeedback(
                                    () -> Text.literal("[gwyc] конфиг перечитан: " + GwycConfig.configFile()), false);

                            if (config.enabled && !channel.isOpen()) {
                                connect();
                            }
                            return 1;
                        }))
                        .then(CommandManager.literal("snapshot").executes(context -> {
                            if (!channel.isOpen()) {
                                context.getSource().sendError(Text.literal("[gwyc] канал не открыт"));
                                return 0;
                            }
                            final ServerPlayerEntity player = context.getSource().getPlayer();
                            if (player == null) {
                                context.getSource().sendError(Text.literal("[gwyc] снимок нужен от игрока: команда не из консоли"));
                                return 0;
                            }

                            final BlockPos origin = player.getBlockPos();
                            sendSnapshot(origin);
                            context.getSource().sendFeedback(
                                    () -> Text.literal("[gwyc] снимок региона отправлен из " + origin.toShortString()), false);
                            return 1;
                        }))));

        log("сторона Minecraft готова (раскладка протокола " + BridgeChannel.LAYOUT_VERSION + ")");
    }

    // ── жизненный цикл канала ────────────────────────────────────────────────

    private void connect() {
        try {
            final Path path = Paths.get(config.channelPath).toAbsolutePath();
            channel.open(path, config.ringCapacity);

            // Hello: игра сверяет раскладку и понимает, что мод на связи.
            channel.send(BridgeChannel.MSG_HELLO, BridgeChannel.FLAG_RELIABLE, Payloads.hello(BUILD_TAG));
            log("канал открыт: " + path + " (ёмкость " + channel.ringCapacity() + " Б)");
        } catch (Exception error) {
            log("не удалось открыть канал: " + error.getMessage());
        }
    }

    private void disconnect() {
        if (channel.isOpen()) {
            channel.close();
            log("канал закрыт (отправлено " + channel.messagesSent()
                    + ", принято " + channel.messagesReceived() + ")");
        }
        outbox.clear();
        pendingChecks.clear();
        casinoPlayers.clear();
        server = null;
    }

    private boolean isMirroring(World world) {
        return channel.isOpen() && world instanceof ServerWorld && !world.isClient();
    }

    // ── тик ──────────────────────────────────────────────────────────────────

    private void onTick(MinecraftServer tickingServer) {
        if (!channel.isOpen()) {
            return;
        }

        ++tickCounter;

        // 1. Разобрать входящие кадры (колбэки игры приходят сюда же).
        BridgeChannel.Frame frame;
        int processed = 0;
        while (processed < 256 && (frame = channel.receive()) != null) {
            handleFrame(frame);
            ++processed;
        }

        // 2. Дождаться проверок «поставился ли блок» (снимок делался до действия).
        runPendingChecks();

        // 3. Отправить накопленные правки в казино: одна правка — один кадр,
        //    несколько — пачкой, чтобы не забивать канал по кадру на блок.
        flushOutbox();
    }

    private void handleFrame(BridgeChannel.Frame frame) {
        switch (frame.type) {
            case BridgeChannel.MSG_PING:
                channel.send(BridgeChannel.MSG_PONG, BridgeChannel.FLAG_NONE, frame.payload);
                break;

            case BridgeChannel.MSG_HELLO:
            case BridgeChannel.MSG_HELLO_ACK:
                log("игра на связи: " + Payloads.describeHello(frame.buffer()));
                break;

            case BridgeChannel.MSG_VOXEL_EDIT: {
                if (frame.size() >= Payloads.Sizes.VOXEL_EDIT) {
                    applyVoxelEdit(Payloads.readVoxelEdit(frame.buffer()));
                }
                break;
            }

            case BridgeChannel.MSG_VOXEL_BATCH: {
                for (Payloads.VoxelEdit edit : Payloads.readVoxelBatch(frame.buffer())) {
                    applyVoxelEdit(edit);
                }
                break;
            }

            case BridgeChannel.MSG_VOXEL_SNAPSHOT: {
                handleSnapshot(frame);
                break;
            }

            case BridgeChannel.MSG_GRANT_REWARD: {
                if (frame.size() >= Payloads.Sizes.GRANT_REWARD) {
                    grantReward(Payloads.readReward(frame.buffer()));
                }
                break;
            }

            case BridgeChannel.MSG_BET_PLACED: {
                if (frame.size() >= Payloads.Sizes.BET_PLACED) {
                    announceBet(Payloads.readBetPlaced(frame.buffer()));
                }
                break;
            }

            case BridgeChannel.MSG_BET_RESOLVED: {
                if (frame.size() >= Payloads.Sizes.BET_RESOLVED) {
                    announceResult(Payloads.readBetResolved(frame.buffer()));
                }
                break;
            }

            case BridgeChannel.MSG_PLAYER_STATE: {
                if (frame.size() >= Payloads.Sizes.PLAYER_STATE) {
                    rememberPlayer(Payloads.readPlayerState(frame.buffer()));
                }
                break;
            }

            case BridgeChannel.MSG_TABLE_EVENT: {
                if (frame.size() >= Payloads.Sizes.TABLE_EVENT && config.verbose) {
                    final Payloads.TableEvent table = Payloads.readTableEvent(frame.buffer());
                    log("стол #" + table.tableId + " у " + table.owner
                            + " (" + table.seats + " мест, лимиты " + table.minBet + "–" + table.maxBet + ")");
                }
                break;
            }

            case BridgeChannel.MSG_LOG: {
                if (frame.size() >= 1) {
                    log("игра: " + BridgeChannel.readFixedString(frame.payload, 1, Math.min(160, frame.size() - 1)));
                }
                break;
            }

            case BridgeChannel.MSG_SHUTDOWN:
                log("игра закрыла канал");
                break;

            case BridgeChannel.MSG_CONSOLE_TO_MC: {
                if (frame.size() >= Payloads.Sizes.CHAT_LINE) {
                    final Payloads.ChatLine line = Payloads.readChatLine(frame.buffer());
                    broadcast("[" + line.author + "] " + line.text);
                }
                break;
            }

            default:
                break;
        }
    }

    // ── отправка правок в казино ─────────────────────────────────────────────

    private void reportBlockChange(BlockPos pos, int blockKind, PlayerEntity player) {
        if (!isInWorld(pos)) {
            return;
        }

        final Payloads.VoxelEdit edit = new Payloads.VoxelEdit();
        edit.x = pos.getX();
        edit.y = pos.getY();
        edit.z = pos.getZ();
        edit.blockKind = blockKind;
        edit.flags = 1;   // правка от локального игрока
        edit.sourceId = sourceIdOf(player);

        // Схлопываем внутри тика: игрок мог сломать и поставить один блок дважды.
        for (Payloads.VoxelEdit queued : outbox) {
            if (queued.x == edit.x && queued.y == edit.y && queued.z == edit.z) {
                queued.blockKind = edit.blockKind;
                queued.flags = edit.flags;
                return;
            }
        }

        outbox.add(edit);
    }

    private void flushOutbox() {
        if (outbox.isEmpty()) {
            return;
        }

        final boolean sent;
        if (outbox.size() == 1) {
            final Payloads.VoxelEdit edit = outbox.get(0);
            sent = channel.send(BridgeChannel.MSG_VOXEL_EDIT, BridgeChannel.FLAG_RELIABLE,
                    Payloads.voxelEdit(edit.x, edit.y, edit.z, edit.blockKind, edit.flags != 0, edit.sourceId));
        } else {
            sent = channel.send(BridgeChannel.MSG_VOXEL_BATCH, BridgeChannel.FLAG_RELIABLE,
                    Payloads.voxelBatch(outbox));
        }

        if (!sent && config.verbose && (tickCounter % 100 == 0)) {
            log("канал переполнен: " + outbox.size() + " правок не ушли (игра не читает канал?)");
        }

        outbox.clear();
    }

    // ── приём правок от казино ───────────────────────────────────────────────

    private void applyVoxelEdit(Payloads.VoxelEdit edit) {
        final ServerWorld world = overworld();
        if (world == null || !isInWorld(edit.x, edit.y, edit.z)) {
            return;
        }

        final BlockPos pos = new BlockPos(edit.x, edit.y, edit.z);
        final Block block = BlockMap.toBlock(edit.blockKind);

        // Флаг спокойной установки: не будим соседей и не запускаем обновления —
        // зеркало не должно само провоцировать игровые события.
        world.setBlockState(pos, block.getDefaultState(),
                Block.NOTIFY_LISTENERS | Block.FORCE_STATE, 0);
    }

    private void handleSnapshot(BridgeChannel.Frame frame) {
        if (frame.size() < Payloads.Sizes.VOXEL_SNAPSHOT) {
            return;
        }

        final ByteBuffer payload = frame.buffer();
        final Payloads.SnapshotHeader header = Payloads.readSnapshotHeader(payload);

        if (header.encodedSize == 0) {
            // Это запрос: игра просит снимок региона.
            sendSnapshot(new BlockPos(header.originX, header.originY, header.originZ));
            return;
        }

        final int cells = header.sizeX * header.sizeY * header.sizeZ;
        if (cells <= 0 || cells > 512 * 512 * 512) {
            log("снимок отвергнут: некорректный размер региона");
            return;
        }

        final byte[] encoded = new byte[Math.min(header.encodedSize, payload.remaining())];
        payload.get(encoded);
        final byte[] blocks = Payloads.decodeRle(encoded, cells);
        if (blocks == null) {
            log("снимок отвергнут: битый RLE-поток");
            return;
        }

        applyRegion(header, blocks);
    }

    private void sendSnapshot(BlockPos origin) {
        final ServerWorld world = overworld();
        if (world == null) {
            return;
        }

        final int size = Math.max(1, Math.min(config.maxSnapshotSize, 32));
        final byte[] blocks = readRegion(world, origin, size, size, size);

        // Порядок обхода обязан совпадать с C++: x быстрее всех, затем z, затем y.
        final byte[] encoded = Payloads.encodeRle(blocks);
        if (encoded.length > BridgeChannel.MAX_PAYLOAD_SIZE - 22) {
            log("снимок " + size + "³ не влезает в кадр (" + encoded.length + " Б) — уменьшите maxSnapshotSize");
            return;
        }

        channel.send(BridgeChannel.MSG_VOXEL_SNAPSHOT, BridgeChannel.FLAG_RELIABLE,
                Payloads.voxelSnapshot(origin.getX(), origin.getY(), origin.getZ(), size, size, size, encoded));
    }

    private byte[] readRegion(ServerWorld world, BlockPos origin, int sizeX, int sizeY, int sizeZ) {
        final byte[] blocks = new byte[sizeX * sizeY * sizeZ];
        int index = 0;

        for (int y = 0; y < sizeY; ++y) {
            for (int z = 0; z < sizeZ; ++z) {
                for (int x = 0; x < sizeX; ++x, ++index) {
                    final BlockPos pos = origin.add(x, y, z);
                    final BlockState state = world.getBlockState(pos);
                    blocks[index] = (byte) (state.isAir() ? BlockMap.AIR : BlockMap.toKind(state.getBlock()));
                }
            }
        }

        return blocks;
    }

    private void applyRegion(Payloads.SnapshotHeader header, byte[] blocks) {
        final ServerWorld world = overworld();
        if (world == null) {
            return;
        }

        int index = 0;
        for (int y = 0; y < header.sizeY; ++y) {
            for (int z = 0; z < header.sizeZ; ++z) {
                for (int x = 0; x < header.sizeX; ++x, ++index) {
                    final int kind = blocks[index] & 0xFF;
                    final BlockPos pos = new BlockPos(header.originX + x, header.originY + y, header.originZ + z);
                    if (!isInWorld(pos)) {
                        continue;
                    }

                    world.setBlockState(pos, BlockMap.toBlock(kind).getDefaultState(),
                            Block.NOTIFY_LISTENERS | Block.FORCE_STATE, 0);
                }
            }
        }

        log("применён снимок региона " + header.sizeX + "×" + header.sizeY + "×" + header.sizeZ
                + " из " + header.originX + " " + header.originY + " " + header.originZ);
    }

    // ── обнаружение установки блока ──────────────────────────────────────────

    private static final class PendingCheck {
        BlockPos[] positions;
        int[] before;
        int ticksLeft;
    }

    private void schedulePlacementCheck(World world, BlockHitResult hitResult) {
        final BlockPos clicked = hitResult.getBlockPos();
        final BlockPos neighbor = clicked.offset(hitResult.getSide());

        final PendingCheck check = new PendingCheck();
        check.positions = new BlockPos[]{ clicked, neighbor };
        check.before = new int[] { kindAt(world, clicked), kindAt(world, neighbor) };
        check.ticksLeft = 1;

        // Ставим в очередь: сравнение произойдёт на следующем тике, когда действие
        // игрока уже применено к миру.
        pendingChecks.add(check);
    }

    private int kindAt(World world, BlockPos pos) {
        final BlockState state = world.getBlockState(pos);
        return state.isAir() ? BlockMap.AIR : BlockMap.toKind(state.getBlock());
    }

    private void runPendingChecks() {
        if (pendingChecks.isEmpty()) {
            return;
        }

        final ServerWorld world = overworld();
        if (world == null) {
            pendingChecks.clear();
            return;
        }

        for (Iterator<PendingCheck> iterator = pendingChecks.iterator(); iterator.hasNext(); ) {
            final PendingCheck check = iterator.next();
            if (--check.ticksLeft > 0) {
                continue;
            }

            for (int i = 0; i < check.positions.length; ++i) {
                final BlockPos pos = check.positions[i];
                final int now = kindAt(world, pos);
                if (now != check.before[i]) {
                    reportBlockChange(pos, now, null);
                }
            }

            iterator.remove();
        }
    }

    // ── награды и события казино ─────────────────────────────────────────────

    private void grantReward(Payloads.Reward reward) {
        if (!BlockMap.isValid(reward.blockKind) || reward.blockKind == BlockMap.AIR) {
            return;
        }

        final ServerPlayerEntity player = resolvePlayer(reward.playerId);
        if (player == null) {
            log("награда для игрока #" + reward.playerId + " не выдана: игрок не найден на сервере");
            return;
        }

        final int count = Math.max(1, Math.min(reward.count, 64 * 36));   // не больше инвентаря
        int remaining = count;

        while (remaining > 0) {
            final int stackSize = Math.min(remaining, 64);
            final var stack = BlockMap.stackFor(reward.blockKind, stackSize);
            if (!player.getInventory().insertStack(stack)) {
                player.dropItem(stack, false);   // инвентарь полон — блоки падают рядом
            }
            remaining -= stackSize;
        }

        player.sendMessage(Text.literal("[казино] награда: " + count + " × "
                + BlockMap.nameOf(reward.blockKind)), false);

        log("выдано " + count + " × " + BlockMap.nameOf(reward.blockKind) + " игроку " + player.getName().getString());
    }

    private void announceBet(Payloads.BetPlaced bet) {
        rememberCasinoPlayer(bet.playerId, bet.playerName);
        broadcast("[казино] " + displayName(bet.playerId, bet.playerName)
                + " ставит " + bet.stake + " (" + bet.target + ")");
    }

    private void announceResult(Payloads.BetResolved result) {
        rememberCasinoPlayer(result.playerId, result.playerName);

        final String name = displayName(result.playerId, result.playerName);
        if (result.won != 0) {
            broadcast("[казино] " + name + " выиграл " + result.payout
                    + " (×" + result.multiplier + ", цель " + result.target + ")");
        } else {
            broadcast("[казино] " + name + " проиграл ставку " + result.stake + " (" + result.target + ")");
        }
    }

    private void rememberPlayer(Payloads.PlayerState state) {
        rememberCasinoPlayer(state.playerId, state.name);
    }

    private void rememberCasinoPlayer(int playerId, String name) {
        if (name == null || name.isEmpty()) {
            return;
        }
        casinoPlayers.put(playerId, name);
    }

    private String displayName(int playerId, String fallback) {
        final String remembered = casinoPlayers.get(playerId);
        if (remembered != null && !remembered.isEmpty()) {
            return remembered;
        }
        return (fallback == null || fallback.isEmpty()) ? ("игрок #" + playerId) : fallback;
    }

    /** Игрок казино → игрок Minecraft: по имени, иначе единственный игрок на сервере. */
    private ServerPlayerEntity resolvePlayer(int playerId) {
        if (server == null) {
            return null;
        }

        final String remembered = casinoPlayers.get(playerId);
        if (remembered != null && !remembered.isEmpty()) {
            final ServerPlayerEntity byName = server.getPlayerManager().getPlayer(remembered);
            if (byName != null) {
                return byName;
            }
        }

        if (config.rewardPlayer != null && !config.rewardPlayer.isEmpty()) {
            final ServerPlayerEntity configured = server.getPlayerManager().getPlayer(config.rewardPlayer);
            if (configured != null) {
                return configured;
            }
        }

        final List<ServerPlayerEntity> players = server.getPlayerManager().getPlayerList();
        return players.isEmpty() ? null : players.get(0);
    }

    private int sourceIdOf(PlayerEntity player) {
        if (player == null || server == null) {
            return 0;
        }
        final List<ServerPlayerEntity> players = server.getPlayerManager().getPlayerList();
        for (int i = 0; i < players.size(); ++i) {
            if (players.get(i).getUuid().equals(player.getUuid())) {
                return i + 1;   // 0 зарезервирован под «мир»
            }
        }
        return 0;
    }

    private void broadcast(String message) {
        if (server != null) {
            server.getPlayerManager().broadcast(Text.literal(message), false);
        }
        log(message);
    }

    // ── служебное ────────────────────────────────────────────────────────────

    private ServerWorld overworld() {
        return (server == null) ? null : server.getOverworld();
    }

    private static boolean isInWorld(BlockPos pos) {
        return isInWorld(pos.getX(), pos.getY(), pos.getZ());
    }

    private static boolean isInWorld(int x, int y, int z) {
        return x >= WORLD_MIN && x <= WORLD_MAX
                && z >= WORLD_MIN && z <= WORLD_MAX
                && y >= -64 && y <= 320;
    }

    private void status(net.minecraft.server.command.ServerCommandSource source) {
        final String text = channel.isOpen()
                ? channel.describe() + ", раскладка " + BridgeChannel.LAYOUT_VERSION
                : "канал не открыт (" + config.channelPath + ")";
        source.sendFeedback(() -> Text.literal("[gwyc] " + text), false);
    }

    private void log(String message) {
        if (config.verbose) {
            System.out.println("[gwyc] " + message);
        }
    }

}
