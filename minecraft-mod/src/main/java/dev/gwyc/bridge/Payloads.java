package dev.gwyc.bridge;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Упаковка и разбор payload'ов протокола.
 *
 * <p>Порядок и размеры полей обязаны совпадать с POD-структурами C++
 * (native/include/gwyc/protocol.h). Все структуры упакованы в 1 байт, поэтому
 * заголовочного выравнивания нет — обычные последовательные put/get дают ту же
 * раскладку, если порядок байтов совпадает (nativeOrder на обеих сторонах).
 */
public final class Payloads {

    public static final int ROLE_GAME = 0;
    public static final int ROLE_MINECRAFT = 1;

    private Payloads() {
    }

    private static ByteBuffer allocate(int size) {
        return ByteBuffer.allocate(size).order(ByteOrder.nativeOrder());
    }

    private static void putFixedString(ByteBuffer buffer, String text, int capacity) {
        final byte[] bytes = (text == null) ? new byte[0] : text.getBytes(StandardCharsets.UTF_8);
        final int length = Math.min(bytes.length, capacity - 1);
        buffer.put(bytes, 0, length);

        for (int i = length; i < capacity; ++i) {
            buffer.put((byte) 0);
        }
    }

    private static String getFixedString(ByteBuffer buffer, int capacity) {
        final byte[] bytes = new byte[capacity];
        buffer.get(bytes);
        return BridgeChannel.readFixedString(bytes, 0, capacity);
    }

    // ── MsgHello (44 байта) ──────────────────────────────────────────────────

    public static byte[] hello(String buildTag) {
        final ByteBuffer buffer = allocate(44);
        buffer.putShort((short) BridgeChannel.LAYOUT_VERSION);
        buffer.putShort((short) BridgeChannel.PROTOCOL_VERSION);
        buffer.putInt(processId());
        buffer.putInt(ROLE_MINECRAFT);
        putFixedString(buffer, buildTag, 32);
        return buffer.array();
    }

    public static String describeHello(ByteBuffer payload) {
        if (payload.remaining() < 12) {
            return "Hello: короткий кадр (" + payload.remaining() + " Б)";
        }
        final int layout = Short.toUnsignedInt(payload.getShort());
        final int protocol = Short.toUnsignedInt(payload.getShort());
        final int pid = payload.getInt();
        final int role = payload.getInt();

        return "Hello: раскладка " + layout + ", протокол " + protocol
                + ", pid " + pid + ", роль " + (role == ROLE_GAME ? "игра" : "Minecraft");
    }

    private static int processId() {
        // Своей функции pid в Java нет: берём из имени JVM (или 0), это только для логов.
        return Math.abs(java.lang.management.ManagementFactory
                .getRuntimeMXBean().getName().hashCode());
    }

    // ── MsgVoxelEdit (16 байт) ───────────────────────────────────────────────

    public static byte[] voxelEdit(int x, int y, int z, int blockKind, boolean fromLocalPlayer, int sourceId) {
        final ByteBuffer buffer = allocate(16);   // Sizes.VOXEL_EDIT
        writeVoxelEdit(buffer, x, y, z, blockKind, fromLocalPlayer, sourceId);
        return buffer.array();
    }

    public static void writeVoxelEdit(ByteBuffer buffer, int x, int y, int z, int blockKind,
                                      boolean fromLocalPlayer, int sourceId) {
        buffer.putInt(x);
        buffer.putInt(y);
        buffer.putInt(z);
        buffer.put((byte) blockKind);
        buffer.put((byte) (fromLocalPlayer ? 1 : 0));
        buffer.putShort((short) sourceId);
    }

    /** Разбор одного MsgVoxelEdit из буфера, начиная с его текущей позиции. */
    public static VoxelEdit readVoxelEdit(ByteBuffer buffer) {
        final VoxelEdit edit = new VoxelEdit();
        edit.x = buffer.getInt();
        edit.y = buffer.getInt();
        edit.z = buffer.getInt();
        edit.blockKind = buffer.get() & 0xFF;
        edit.flags = buffer.get() & 0xFF;
        edit.sourceId = Short.toUnsignedInt(buffer.getShort());
        return edit;
    }

    /** Правка вокселя (зеркало MsgVoxelEdit). */
    public static final class VoxelEdit {
        public int x;
        public int y;
        public int z;
        public int blockKind;
        public int flags;
        public int sourceId;
    }

    // ── MsgVoxelBatch (8 байт заголовка + N * 16) ────────────────────────────

    public static byte[] voxelBatch(List<VoxelEdit> edits) {
        final ByteBuffer buffer = allocate(8 + edits.size() * 16);   // заголовок + N * VOXEL_EDIT
        buffer.putInt(edits.size());
        buffer.putInt(0);   // firstIndex — для фрагментации; в одной пачке всегда 0

        for (VoxelEdit edit : edits) {
            writeVoxelEdit(buffer, edit.x, edit.y, edit.z, edit.blockKind, edit.flags != 0, edit.sourceId);
        }
        return buffer.array();
    }

    public static List<VoxelEdit> readVoxelBatch(ByteBuffer payload) {
        final List<VoxelEdit> edits = new ArrayList<>();
        if (payload.remaining() < 8) {   // Sizes.VOXEL_BATCH_HEADER
            return edits;
        }

        final int count = payload.getInt();
        payload.getInt();   // firstIndex

        final int available = payload.remaining() / 16;
        final int real = Math.min(count, available);
        for (int i = 0; i < real; ++i) {
            edits.add(readVoxelEdit(payload));
        }
        return edits;
    }

    // ── MsgVoxelSnapshot (22 байта) ──────────────────────────────────────────

    public static byte[] voxelSnapshot(int originX, int originY, int originZ,
                                       int sizeX, int sizeY, int sizeZ, byte[] encoded) {
        final byte[] body = (encoded == null) ? new byte[0] : encoded;
        final ByteBuffer buffer = allocate(22 + body.length);

        buffer.putInt(originX);
        buffer.putInt(originY);
        buffer.putInt(originZ);
        buffer.putShort((short) sizeX);
        buffer.putShort((short) sizeY);
        buffer.putShort((short) sizeZ);
        buffer.putInt(body.length);
        buffer.put(body);

        return buffer.array();
    }

    /** Запрос снимка у мода: encodedSize = 0 (см. C++ GwycBridge_RequestRegionSnapshot). */
    public static byte[] voxelSnapshotRequest(int originX, int originY, int originZ,
                                              int sizeX, int sizeY, int sizeZ) {
        return voxelSnapshot(originX, originY, originZ, sizeX, sizeY, sizeZ, new byte[0]);
    }

    public static SnapshotHeader readSnapshotHeader(ByteBuffer payload) {
        final SnapshotHeader header = new SnapshotHeader();
        header.originX = payload.getInt();
        header.originY = payload.getInt();
        header.originZ = payload.getInt();
        header.sizeX = Short.toUnsignedInt(payload.getShort());
        header.sizeY = Short.toUnsignedInt(payload.getShort());
        header.sizeZ = Short.toUnsignedInt(payload.getShort());
        header.encodedSize = payload.getInt();
        return header;
    }

    public static final class SnapshotHeader {
        public int originX;
        public int originY;
        public int originZ;
        public int sizeX;
        public int sizeY;
        public int sizeZ;
        public int encodedSize;
    }

    // ── MsgGrantReward (12 байт) ─────────────────────────────────────────────

    public static byte[] grantReward(int playerId, int blockKind, int count, int reason) {
        final ByteBuffer buffer = allocate(12);   // Sizes.GRANT_REWARD
        buffer.putInt(playerId);
        buffer.putShort((short) blockKind);
        buffer.putInt(count);
        buffer.put((byte) reason);
        buffer.put((byte) 0);
        return buffer.array();
    }

    public static Reward readReward(ByteBuffer payload) {
        final Reward reward = new Reward();
        reward.playerId = payload.getInt();
        reward.blockKind = Short.toUnsignedInt(payload.getShort());
        reward.count = payload.getInt();
        reward.reason = payload.get() & 0xFF;
        payload.get();   // reserved
        return reward;
    }

    public static final class Reward {
        public int playerId;
        public int blockKind;
        public int count;
        public int reason;
    }

    // ── MsgBetPlaced (53 байта) / MsgBetResolved (68 байт) ───────────────────

    /** Размеры payload'ов: должны совпадать с static_assert в protocol.h. */
    public static final class Sizes {
        public static final int HELLO = 44;
        public static final int PING_PONG = 16;
        public static final int LOG = 161;
        public static final int VOXEL_EDIT = 16;
        public static final int VOXEL_BATCH_HEADER = 8;
        public static final int VOXEL_SNAPSHOT = 22;
        public static final int PLAYER_STATE = 48;
        public static final int BET_PLACED = 53;
        public static final int BET_RESOLVED = 68;
        public static final int TABLE_EVENT = 58;
        public static final int GRANT_REWARD = 12;
        public static final int CHAT_LINE = 185;

        private Sizes() {
        }
    }

    public static BetPlaced readBetPlaced(ByteBuffer payload) {
        final BetPlaced bet = new BetPlaced();
        bet.playerId = payload.getInt();
        bet.stake = payload.getLong();
        bet.game = payload.get() & 0xFF;
        bet.target = getFixedString(payload, 16);
        bet.playerName = getFixedString(payload, 24);
        return bet;
    }

    public static final class BetPlaced {
        public int playerId;
        public long stake;
        public int game;
        public String target = "";
        public String playerName = "";
    }

    public static BetResolved readBetResolved(ByteBuffer payload) {
        final BetResolved result = new BetResolved();
        result.playerId = payload.getInt();
        result.stake = payload.getLong();
        result.payout = payload.getLong();
        result.multiplier = payload.getInt();
        result.won = payload.get() & 0xFF;
        result.game = payload.get() & 0xFF;
        result.blockKind = payload.get() & 0xFF;
        payload.get();   // reserved
        result.target = getFixedString(payload, 16);
        result.playerName = getFixedString(payload, 24);
        return result;
    }

    public static final class BetResolved {
        public int playerId;
        public long stake;
        public long payout;
        public int multiplier;
        public int won;
        public int game;
        public int blockKind;
        public String target = "";
        public String playerName = "";
    }

    // ── MsgPlayerState (48 байт) / MsgTableEvent (58 байт) ───────────────────

    public static PlayerState readPlayerState(ByteBuffer payload) {
        final PlayerState state = new PlayerState();
        state.playerId = payload.getInt();
        state.chips = payload.getLong();
        state.netWorth = payload.getLong();
        state.lobbyState = payload.get() & 0xFF;
        payload.get();
        payload.get();
        payload.get();
        state.name = getFixedString(payload, 24);
        return state;
    }

    public static final class PlayerState {
        public int playerId;
        public long chips;
        public long netWorth;
        public int lobbyState;
        public String name = "";
    }

    public static TableEvent readTableEvent(ByteBuffer payload) {
        final TableEvent event = new TableEvent();
        event.tableId = payload.getInt();
        event.x = payload.getInt();
        event.y = payload.getInt();
        event.z = payload.getInt();
        event.minBet = payload.getLong();
        event.maxBet = payload.getLong();
        event.seats = payload.get() & 0xFF;
        event.state = payload.get() & 0xFF;
        event.owner = getFixedString(payload, 24);
        return event;
    }

    public static final class TableEvent {
        public int tableId;
        public int x;
        public int y;
        public int z;
        public long minBet;
        public long maxBet;
        public int seats;
        public int state;
        public String owner = "";
    }

    // ── MsgChatLine (185 байт) ──────────────────────────────────────────────

    public static byte[] chatLine(int channel, String author, String text) {
        final ByteBuffer buffer = allocate(185);   // Sizes.CHAT_LINE (без выравнивания)
        buffer.put((byte) channel);
        putFixedString(buffer, author, 24);
        putFixedString(buffer, text, 160);
        return buffer.array();
    }

    public static ChatLine readChatLine(ByteBuffer payload) {
        final ChatLine line = new ChatLine();
        line.channel = payload.get() & 0xFF;
        line.author = getFixedString(payload, 24);
        line.text = getFixedString(payload, 160);
        return line;
    }

    public static final class ChatLine {
        public int channel;
        public String author = "";
        public String text = "";
    }

    // ── MsgLog (161 байт) ────────────────────────────────────────────────────

    public static byte[] log(int level, String text) {
        final ByteBuffer buffer = allocate(161);   // Sizes.LOG
        buffer.put((byte) level);
        putFixedString(buffer, text, 160);
        return buffer.array();
    }

    // ── RLE (общий формат с C++ и C#) ────────────────────────────────────────

    /** Сжать массив блоков в поток [varint run][uint8 block]. */
    public static byte[] encodeRle(byte[] blocks) {
        final java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(blocks.length / 4 + 16);

        int index = 0;
        while (index < blocks.length) {
            final byte value = blocks[index];
            int run = 1;
            while (index + run < blocks.length && blocks[index + run] == value) {
                ++run;
            }

            writeVarUInt(out, run);
            out.write(value & 0xFF);
            index += run;
        }

        return out.toByteArray();
    }

    /** Развернуть RLE-поток. Возвращает null, если поток битый. */
    public static byte[] decodeRle(byte[] encoded, int expectedCount) {
        final byte[] out = new byte[expectedCount];
        int offset = 0;
        int written = 0;

        while (offset < encoded.length) {
            int shift = 0;
            int run = 0;
            while (true) {
                if (offset >= encoded.length || shift > 35) {
                    return null;
                }
                final int value = encoded[offset++] & 0xFF;
                run |= (value & 0x7F) << shift;
                if ((value & 0x80) == 0) {
                    break;
                }
                shift += 7;
            }

            if (run == 0 || offset >= encoded.length) {
                return null;
            }
            if (written + run > expectedCount) {
                return null;
            }

            final byte block = encoded[offset++];
            java.util.Arrays.fill(out, written, written + run, block);
            written += run;
        }

        return (written == expectedCount) ? out : null;
    }

    public static void writeVarUInt(java.io.ByteArrayOutputStream out, int value) {
        int remaining = value;
        while ((remaining & ~0x7F) != 0) {
            out.write((remaining & 0x7F) | 0x80);
            remaining >>>= 7;
        }
        out.write(remaining);
    }
}
