// ============================================================================
//  BridgeRecords.java — разбор записей, которые нативная часть пишет в буфер.
//
//  КОНТРАКТ С НАТИВНОЙ ЧАСТЬЮ
//  --------------------------
//  Раскладка описана в native/include/gwyfbridge/McWireFormat.h. Здесь она
//  продублирована константами — и это единственное место, где дублирование
//  оправдано: у Java и C++ нет общего заголовка. Чтобы расхождение нельзя
//  было не заметить, обе стороны считают хеш FNV-1a от текстового описания
//  раскладки, и при инициализации мода хеши сравниваются (см. GwyfBridgeMod):
//  несовпадение = громкая ошибка в логе, а не «странные числа» в инвентаре.
//
//  Формат записи: 88 байт
//      [0]  int   type
//      [4]  int   flags
//      [8]  long  sourceId
//      [16] long  aux
//      [24] byte[64] payload
// ============================================================================
package dev.gwyfbridge.mc;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class BridgeRecords {

    private BridgeRecords() {
    }

    // ── Раскладка ──────────────────────────────────────────────────────────

    public static final int RECORD_STRIDE = 88;
    public static final int BULK_EDIT_STRIDE = 20;

    public static final int OFFSET_TYPE = 0;
    public static final int OFFSET_FLAGS = 4;
    public static final int OFFSET_SOURCE_ID = 8;
    public static final int OFFSET_AUX = 16;
    public static final int OFFSET_PAYLOAD = 24;

    /** Хеш раскладки, посчитанный нативной частью (см. RecordLayoutText в C++). */
    public static final int LAYOUT_HASH = 0x3F9910C9;

    // ── Типы записей (совпадают с ipc::RecordType) ─────────────────────────

    public static final int TYPE_NONE = 0;
    public static final int TYPE_HELLO = 1;
    public static final int TYPE_BYE = 2;
    public static final int TYPE_HEARTBEAT = 3;
    public static final int TYPE_BET_PLACED = 4;
    public static final int TYPE_BET_RESOLVED = 5;
    public static final int TYPE_TABLE_SPAWNED = 6;
    public static final int TYPE_TABLE_REMOVED = 7;
    public static final int TYPE_CHIP_STATE = 8;
    public static final int TYPE_LOBBY_STATE = 9;
    public static final int TYPE_VOXEL_EDIT = 10;
    public static final int TYPE_BLOCK_GRANT = 11;
    public static final int TYPE_COMMAND = 12;
    public static final int TYPE_COMMAND_ACK = 13;
    public static final int TYPE_LOG_LINE = 14;
    public static final int TYPE_BULK_SYNC = 15;
    public static final int TYPE_CUBE_SPAWNED = 16;

    // ── Действия с блоками ─────────────────────────────────────────────────

    public static final int ACTION_PLACE = 1;
    public static final int ACTION_BREAK = 2;
    public static final int ACTION_BULK_REPLACE = 3;

    // ── Коды команд ────────────────────────────────────────────────────────

    public static final int COMMAND_DUMP_CLASS = 1;
    public static final int COMMAND_SCAN_PATTERN = 2;
    public static final int COMMAND_STATUS = 3;
    public static final int COMMAND_SPAWN_PROBE = 4;
    public static final int COMMAND_RELOAD_PROFILE = 5;
    public static final int COMMAND_FLUSH_WORLD = 6;

    // ── Смещения внутри полезной нагрузки (payload) ────────────────────────

    public static final int OFFSET_BET_AMOUNT_BITS = 0;
    public static final int OFFSET_BET_CHIP_TYPE = 4;
    public static final int OFFSET_BET_BET_KIND = 8;
    public static final int OFFSET_BET_TABLE_ID = 12;
    public static final int OFFSET_BET_PLAYER_ID = 16;
    public static final int OFFSET_BET_SEAT = 24;
    public static final int OFFSET_BET_FLAGS = 28;

    public static final int OFFSET_RESULT_PAYOUT = 0;
    public static final int OFFSET_RESULT_CHIPS_AFTER = 8;
    public static final int OFFSET_RESULT_TABLE_ID = 16;
    public static final int OFFSET_RESULT_SEAT = 20;
    public static final int OFFSET_RESULT_WON = 24;
    public static final int OFFSET_RESULT_WHEEL_POCKET = 25;
    public static final int OFFSET_RESULT_DICE_A = 26;
    public static final int OFFSET_RESULT_DICE_B = 27;
    public static final int OFFSET_RESULT_PLAYER_ID = 32;

    public static final int OFFSET_TABLE_ID = 0;
    public static final int OFFSET_TABLE_LOBBY_ID = 4;
    public static final int OFFSET_TABLE_SEATS = 8;
    public static final int OFFSET_TABLE_PHASE = 12;
    public static final int OFFSET_TABLE_GAME_KIND = 16;
    public static final int OFFSET_TABLE_POS_X = 20;
    public static final int OFFSET_TABLE_POS_Y = 24;
    public static final int OFFSET_TABLE_POS_Z = 28;
    public static final int OFFSET_TABLE_MIN_BET = 32;

    public static final int OFFSET_CHIPS_CHIPS = 0;
    public static final int OFFSET_CHIPS_PENDING = 8;
    public static final int OFFSET_CHIPS_BANK = 16;
    public static final int OFFSET_CHIPS_PLAYERS = 24;
    public static final int OFFSET_CHIPS_ROUND = 28;
    public static final int OFFSET_CHIPS_PLAYER_ID = 32;

    public static final int OFFSET_VOXEL_X = 0;
    public static final int OFFSET_VOXEL_Y = 4;
    public static final int OFFSET_VOXEL_Z = 8;
    public static final int OFFSET_VOXEL_BLOCK = 12;
    public static final int OFFSET_VOXEL_ACTION = 16;
    public static final int OFFSET_VOXEL_AUTHOR = 24;

    public static final int OFFSET_GRANT_BLOCK = 0;
    public static final int OFFSET_GRANT_COUNT = 4;
    public static final int OFFSET_GRANT_PLAYER_ID = 8;
    public static final int OFFSET_GRANT_REASON = 16;

    public static final int OFFSET_COMMAND_CODE = 0;
    public static final int OFFSET_COMMAND_ARG0 = 4;
    public static final int OFFSET_COMMAND_ARG1 = 8;
    public static final int OFFSET_COMMAND_TEXT_LENGTH = 12;

    // ── Разобранная запись ─────────────────────────────────────────────────

    /** Одна запись в удобном для Java виде: разбор делается один раз. */
    public static final class Record {
        public int type;
        public int flags;
        public long sourceId;
        public long aux;
        public ByteBuffer payload;

        public Record(int type, int flags, long sourceId, long aux, ByteBuffer payload) {
            this.type = type;
            this.flags = flags;
            this.sourceId = sourceId;
            this.aux = aux;
            this.payload = payload;
        }

        public String typeName() {
            switch (type) {
                case TYPE_HELLO: return "Hello";
                case TYPE_BYE: return "Bye";
                case TYPE_HEARTBEAT: return "Heartbeat";
                case TYPE_BET_PLACED: return "BetPlaced";
                case TYPE_BET_RESOLVED: return "BetResolved";
                case TYPE_TABLE_SPAWNED: return "TableSpawned";
                case TYPE_TABLE_REMOVED: return "TableRemoved";
                case TYPE_CHIP_STATE: return "ChipState";
                case TYPE_LOBBY_STATE: return "LobbyState";
                case TYPE_VOXEL_EDIT: return "VoxelEdit";
                case TYPE_BLOCK_GRANT: return "BlockGrant";
                case TYPE_COMMAND: return "Command";
                case TYPE_COMMAND_ACK: return "CommandAck";
                case TYPE_LOG_LINE: return "LogLine";
                case TYPE_BULK_SYNC: return "BulkSync";
                case TYPE_CUBE_SPAWNED: return "CubeSpawned";
                default: return "Unknown(" + type + ")";
            }
        }

        // ── Чтение полезной нагрузки по типу события ───────────────────────

        public float betAmount() {
            return Float.intBitsToFloat(payload.getInt(OFFSET_BET_AMOUNT_BITS));
        }

        public int betTableId() {
            return payload.getInt(OFFSET_BET_TABLE_ID);
        }

        public int betChipType() {
            return payload.getInt(OFFSET_BET_CHIP_TYPE);
        }

        public int betKind() {
            return payload.getInt(OFFSET_BET_BET_KIND);
        }

        public long betPlayerId() {
            return payload.getLong(OFFSET_BET_PLAYER_ID);
        }

        public long resultPayout() {
            return payload.getLong(OFFSET_RESULT_PAYOUT);
        }

        public long resultChipsAfter() {
            return payload.getLong(OFFSET_RESULT_CHIPS_AFTER);
        }

        public boolean resultWon() {
            return payload.get(OFFSET_RESULT_WON) != 0;
        }

        public int resultWheelPocket() {
            return payload.get(OFFSET_RESULT_WHEEL_POCKET) & 0xFF;
        }

        public int tableId() {
            return payload.getInt(OFFSET_TABLE_ID);
        }

        public int tableLobbyId() {
            return payload.getInt(OFFSET_TABLE_LOBBY_ID);
        }

        public int tableSeats() {
            return payload.getInt(OFFSET_TABLE_SEATS);
        }

        public int tableGameKind() {
            return payload.getInt(OFFSET_TABLE_GAME_KIND);
        }

        public long chipsBalance() {
            return payload.getLong(OFFSET_CHIPS_CHIPS);
        }

        public long chipsPendingBet() {
            return payload.getLong(OFFSET_CHIPS_PENDING);
        }

        public long chipsBank() {
            return payload.getLong(OFFSET_CHIPS_BANK);
        }

        public int chipsPlayerCount() {
            return payload.getInt(OFFSET_CHIPS_PLAYERS);
        }

        public int voxelX() {
            return payload.getInt(OFFSET_VOXEL_X);
        }

        public int voxelY() {
            return payload.getInt(OFFSET_VOXEL_Y);
        }

        public int voxelZ() {
            return payload.getInt(OFFSET_VOXEL_Z);
        }

        public int voxelBlock() {
            return payload.getInt(OFFSET_VOXEL_BLOCK);
        }

        public int voxelAction() {
            return payload.getInt(OFFSET_VOXEL_ACTION);
        }

        public int grantBlock() {
            return payload.getInt(OFFSET_GRANT_BLOCK);
        }

        public int grantCount() {
            return payload.getInt(OFFSET_GRANT_COUNT);
        }

        public int grantReason() {
            return payload.getInt(OFFSET_GRANT_REASON);
        }

        public int commandCode() {
            return payload.getInt(OFFSET_COMMAND_CODE);
        }

        public int commandTextLength() {
            return payload.getInt(OFFSET_COMMAND_TEXT_LENGTH);
        }
    }

    /**
     * Разобрать до {@code count} записей из буфера. Буфер перематывается в начало,
     * чтобы вызывающий код мог переиспользовать его без дополнительных действий.
     */
    public static java.util.List<Record> parse(ByteBuffer buffer, int count) {
        buffer.order(ByteOrder.nativeOrder());
        buffer.position(0);

        final java.util.List<Record> records = new java.util.ArrayList<>(count);
        for (int index = 0; index < count; ++index) {
            final int base = index * RECORD_STRIDE;

            final int type = buffer.getInt(base + OFFSET_TYPE);
            final int flags = buffer.getInt(base + OFFSET_FLAGS);
            final long sourceId = buffer.getLong(base + OFFSET_SOURCE_ID);
            final long aux = buffer.getLong(base + OFFSET_AUX);

            // Копия полезной нагрузки: буфер будет перезаписан следующим опросом.
            final ByteBuffer payload = ByteBuffer.allocate(64).order(ByteOrder.nativeOrder());
            final ByteBuffer slice = buffer.duplicate().order(ByteOrder.nativeOrder());
            slice.position(base + OFFSET_PAYLOAD);
            slice.limit(base + OFFSET_PAYLOAD + 64);
            payload.put(slice);
            payload.flip();

            records.add(new Record(type, flags, sourceId, aux, payload));
        }

        return records;
    }

    /** Упаковать правку в формат bulk-пакета. */
    public static void packBulkEdit(ByteBuffer buffer, int index, int x, int y, int z, int block, int action) {
        final int base = index * BULK_EDIT_STRIDE;
        buffer.order(ByteOrder.nativeOrder());
        buffer.putInt(base, x);
        buffer.putInt(base + 4, y);
        buffer.putInt(base + 8, z);
        buffer.putInt(base + 12, block);
        buffer.putInt(base + 16, action);
    }

    /** Хеш FNV-1a от текста раскладки — та же функция, что в нативной части. */
    public static int layoutHashOf(String layoutText) {
        int hash = 0x811C9DC5;
        for (byte value : layoutText.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
            hash ^= (value & 0xFF);
            hash *= 0x01000193;
        }
        return hash;
    }
}
