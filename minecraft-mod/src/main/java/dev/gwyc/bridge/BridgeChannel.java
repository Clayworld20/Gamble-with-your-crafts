package dev.gwyc.bridge;

import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.zip.CRC32;

/**
 * Канал обмена с нативным мостом (C++): файл, отображённый в память.
 *
 * <p>Раскладка области совпадает с native/include/gwyc/SharedChannel.h:
 * <pre>
 *   [FileHeader 24 байта]
 *   [RingControlBlock A 20 байт][данные A (capacity байт)]   — игра  → Minecraft
 *   [RingControlBlock B 20 байт][данные B (capacity байт)]   — Minecraft → игра
 * </pre>
 *
 * <p>Каждое кольцо — SPSC: пишет ровно один процесс, читает ровно один. Мод
 * пишет в кольцо B и читает из A.
 *
 * <p>Видимость данных между процессами обеспечивают acquire/release-доступы
 * через VarHandle: это тот же контракт, что у std::atomic с memory_order_acquire
 * и release в C++. Без него JIT вправе переставить записи payload и индекса.
 *
 * <p>Формат кадра (14 байт заголовка + payload) и CRC32 (IEEE) совпадают с
 * C++ и C# до байта — см. native/include/gwyc/protocol.h.
 */
public final class BridgeChannel implements AutoCloseable {

    // ── Константы протокола (совпадают с protocol.h) ────────────────────────
    public static final int LAYOUT_VERSION = 3;
    public static final int PROTOCOL_VERSION = 1;
    public static final int MAGIC = 0x5747;
    public static final int MAX_PAYLOAD_SIZE = 64 * 1024;

    public static final int MSG_HELLO = 0x01;
    public static final int MSG_HELLO_ACK = 0x02;
    public static final int MSG_PING = 0x03;
    public static final int MSG_PONG = 0x04;
    public static final int MSG_LOG = 0x05;
    public static final int MSG_VOXEL_EDIT = 0x10;
    public static final int MSG_VOXEL_BATCH = 0x11;
    public static final int MSG_VOXEL_SNAPSHOT = 0x12;
    public static final int MSG_PLAYER_STATE = 0x20;
    public static final int MSG_BET_PLACED = 0x21;
    public static final int MSG_BET_RESOLVED = 0x22;
    public static final int MSG_TABLE_EVENT = 0x30;
    public static final int MSG_GRANT_REWARD = 0x40;
    public static final int MSG_CONSOLE_TO_MC = 0x41;
    public static final int MSG_CONSOLE_TO_GAME = 0x42;
    public static final int MSG_SHUTDOWN = 0x7F;

    public static final int FLAG_NONE = 0;
    public static final int FLAG_RELIABLE = 1;

    private static final int FILE_HEADER_SIZE = 24;
    private static final int RING_CONTROL_SIZE = 20;
    private static final int FRAME_HEADER_SIZE = 14;
    private static final long CHANNEL_MAGIC = 0x48435747L;   // 'GWCH'

    // Смещения внутри FileHeader.
    private static final int OFF_MAGIC = 0;
    private static final int OFF_LAYOUT = 4;
    private static final int OFF_PROTOCOL = 6;
    private static final int OFF_TOTAL_SIZE = 8;
    private static final int OFF_RING_CAPACITY = 12;
    private static final int OFF_GAME_READY = 16;
    private static final int OFF_MC_READY = 20;

    // Смещения внутри RingControlBlock.
    private static final int RING_TAIL = 0;
    private static final int RING_HEAD = 4;
    private static final int RING_OVERRUNS = 8;

    /**
     * Доступ к int внутри ByteBuffer с семантикой acquire/release —
     * аналог std::atomic&lt;uint32_t&gt; на другой стороне канала.
     */
    private static final VarHandle INT_VIEW =
            MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.nativeOrder());

    private MappedByteBuffer mapping;
    private int ringCapacity;
    private int ringAOffset;      // управляющий блок кольца A
    private int ringAData;        // данные кольца A
    private int ringBOffset;      // управляющий блок кольца B
    private int ringBData;        // данные кольца B
    private boolean open;

    // Диагностика
    private long messagesSent;
    private long messagesReceived;
    private long crcFailures;
    private long resyncs;

    public boolean isOpen() {
        return open;
    }

    public int ringCapacity() {
        return ringCapacity;
    }

    public long messagesSent() {
        return messagesSent;
    }

    public long messagesReceived() {
        return messagesReceived;
    }

    public long crcFailures() {
        return crcFailures;
    }

    public long resyncs() {
        return resyncs;
    }

    /**
     * Открыть (или создать) канал и подключиться в роли Minecraft.
     *
     * @param channelPath путь к файлу канала — тот же, что у игры
     * @param capacity    ёмкость кольца; должна совпадать с настройкой игры
     */
    public synchronized void open(Path channelPath, int capacity) throws IOException {
        if (open) {
            return;
        }

        if (capacity <= 0) {
            capacity = 128 * 1024;
        }
        final int normalized = normalizeCapacity(capacity);
        final int ringBytes = RING_CONTROL_SIZE + normalized;
        final int totalSize = FILE_HEADER_SIZE + ringBytes * 2;

        if (channelPath.getParent() != null) {
            Files.createDirectories(channelPath.getParent());
        }

        boolean fresh;
        try (FileChannel file = FileChannel.open(channelPath,
                StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE)) {

            final long existingSize = file.size();
            fresh = existingSize < FILE_HEADER_SIZE;

            // Файл никогда не обрезаем: если игра создала канал с другой ёмкостью,
            // мы обязаны сообщить об ошибке, а не затереть её заголовок.
            if (existingSize < totalSize) {
                file.position(totalSize - 1L);
                file.write(ByteBuffer.wrap(new byte[]{ 0 }));
            }

            mapping = file.map(FileChannel.MapMode.READ_WRITE, 0, Math.max(existingSize, totalSize));
            mapping.order(ByteOrder.nativeOrder());
        }

        ringCapacity = normalized;
        ringAOffset = FILE_HEADER_SIZE;
        ringAData = ringAOffset + RING_CONTROL_SIZE;
        ringBOffset = ringAOffset + ringBytes;
        ringBData = ringBOffset + RING_CONTROL_SIZE;

        final long magic = (long) (int) getU32(OFF_MAGIC);
        if (magic != CHANNEL_MAGIC) {
            if (!fresh) {
                throw new IOException("Файл " + channelPath + " занят чем-то другим: это не канал моста."
                        + " Укажите в конфиге мода другой путь или удалите файл.");
            }

            // Создаём заголовок: до нас канал никто не открывал.
            putU32(OFF_MAGIC, (int) CHANNEL_MAGIC);
            putU16(OFF_LAYOUT, LAYOUT_VERSION);
            putU16(OFF_PROTOCOL, PROTOCOL_VERSION);
            putU32(OFF_TOTAL_SIZE, totalSize);
            putU32(OFF_RING_CAPACITY, normalized);
            putU32(ringAOffset + RING_TAIL, 0);
            putU32(ringAOffset + RING_HEAD, 0);
            putU32(ringAOffset + RING_OVERRUNS, 0);
            putU32(ringBOffset + RING_TAIL, 0);
            putU32(ringBOffset + RING_HEAD, 0);
            putU32(ringBOffset + RING_OVERRUNS, 0);
        } else {
            final int layout = getU16(OFF_LAYOUT);
            if (layout != LAYOUT_VERSION) {
                throw new IOException("Канал создан другой версией раскладки: " + layout
                        + " вместо " + LAYOUT_VERSION + ". Обновите оба компонента.");
            }
            final int fileCapacity = (int) getU32(OFF_RING_CAPACITY);
            if (fileCapacity != normalized) {
                throw new IOException("Ёмкость кольца в канале (" + fileCapacity
                        + ") не совпадает с настройкой моста (" + normalized + ")");
            }
        }

        putU32(OFF_MC_READY, 1);   // сообщаем игре, что сторона Minecraft поднялась
        open = true;
    }

    @Override
    public synchronized void close() {
        if (!open) {
            return;
        }

        // Прощание: игра увидит аккуратное закрытие и поймёт, что мод ушёл.
        send(MSG_SHUTDOWN, FLAG_RELIABLE, new byte[0]);
        putU32(OFF_MC_READY, 0);

        open = false;
        mapping = null;
    }

    /** Игра уже подняла свой конец канала. */
    public boolean isGameReady() {
        return open && getU32(OFF_GAME_READY) != 0;
    }

    private static int normalizeCapacity(int requested) {
        int capacity = 1024;
        final int limit = 64 * 1024 * 1024;
        while (capacity < requested && capacity < limit) {
            capacity <<= 1;
        }
        return capacity;
    }

    // ── send ────────────────────────────────────────────────────────────────

    /** Отправить кадр в игру. */
    public synchronized boolean send(int type, int flags, byte[] payload) {
        if (!open) {
            return false;
        }

        final byte[] body = (payload == null) ? new byte[0] : payload;
        if (body.length > MAX_PAYLOAD_SIZE) {
            return false;
        }

        final byte[] frame = new byte[FRAME_HEADER_SIZE + body.length];
        writeU16(frame, 0, MAGIC);
        writeU16(frame, 2, LAYOUT_VERSION);
        frame[4] = (byte) type;
        frame[5] = (byte) flags;
        writeU32(frame, 6, body.length);

        final CRC32 crc = new CRC32();
        crc.update(frame, 4, 6);           // type + flags + payloadSize
        crc.update(body, 0, body.length);
        writeU32(frame, 10, (int) crc.getValue());

        System.arraycopy(body, 0, frame, FRAME_HEADER_SIZE, body.length);

        if (!ringWrite(ringBOffset, ringBData, frame)) {
            return false;
        }

        ++messagesSent;
        return true;
    }

    /** Удобная отправка POD-структуры (поле в порядке байтов хоста, как в C++). */
    public boolean sendStruct(int type, int flags, ByteBuffer struct) {
        final byte[] bytes = new byte[struct.remaining()];
        struct.duplicate().get(bytes);
        return send(type, flags, bytes);
    }

    private boolean ringWrite(int controlOffset, int dataOffset, byte[] frame) {
        final int tail = getU32(controlOffset + RING_TAIL);
        final int head = getU32(controlOffset + RING_HEAD);
        final int used = tail - head;
        final int writable = (ringCapacity - 1) - used;

        if (frame.length > writable) {
            // Кольцо полно: инкрементируем счётчик в разделяемой памяти и уходим.
            putU32(controlOffset + RING_OVERRUNS, (int) getU32(controlOffset + RING_OVERRUNS) + 1);
            return false;
        }

        final int offset = tail & (ringCapacity - 1);
        final int firstChunk = Math.min(ringCapacity - offset, frame.length);
        final int secondChunk = frame.length - firstChunk;

        for (int i = 0; i < firstChunk; ++i) {
            mapping.put(dataOffset + offset + i, frame[i]);
        }
        for (int i = 0; i < secondChunk; ++i) {
            mapping.put(dataOffset + i, frame[firstChunk + i]);
        }

        // release: данные кадра обязаны стать видимыми до публикации нового tail.
        INT_VIEW.setRelease(mapping, controlOffset + RING_TAIL, tail + frame.length);
        return true;
    }

    // ── receive ─────────────────────────────────────────────────────────────

    /** Один принятый кадр. */
    public static final class Frame {
        public final int type;
        public final int flags;
        public final byte[] payload;

        Frame(int type, int flags, byte[] payload) {
            this.type = type;
            this.flags = flags;
            this.payload = payload;
        }

        public int size() {
            return payload.length;
        }

        public ByteBuffer buffer() {
            return ByteBuffer.wrap(payload).order(ByteOrder.nativeOrder());
        }
    }

    /** Принять очередной кадр или вернуть null, если данных нет. */
    public synchronized Frame receive() {
        if (!open) {
            return null;
        }

        final int controlOffset = ringAOffset;
        final int tail = getU32(controlOffset + RING_TAIL);
        final int head = getU32(controlOffset + RING_HEAD);

        if (tail - head < FRAME_HEADER_SIZE) {
            return null;
        }

        final byte[] header = new byte[FRAME_HEADER_SIZE];
        ringRead(ringAData, head, header, 0, FRAME_HEADER_SIZE);

        final int magic = readU16(header, 0);
        final int layout = readU16(header, 2);
        final int payloadSize = readU32(header, 6);

        if (magic != MAGIC || layout != LAYOUT_VERSION || payloadSize > MAX_PAYLOAD_SIZE) {
            // Рассинхрон потока: сдвигаемся на байт и ищем следующий кадр.
            advanceHead(controlOffset, head + 1);
            ++resyncs;
            return null;
        }

        final int total = FRAME_HEADER_SIZE + payloadSize;
        if (tail - head < total) {
            return null;   // кадр ещё не пришёл целиком
        }

        final byte[] frame = new byte[total];
        ringRead(ringAData, head, frame, 0, total);

        final CRC32 crc = new CRC32();
        crc.update(frame, 4, 6);
        crc.update(frame, FRAME_HEADER_SIZE, payloadSize);
        final int expected = readU32(frame, 10);

        advanceHead(controlOffset, head + total);

        if ((int) crc.getValue() != expected) {
            ++crcFailures;
            return null;
        }

        ++messagesReceived;

        final byte[] payload = new byte[payloadSize];
        System.arraycopy(frame, FRAME_HEADER_SIZE, payload, 0, payloadSize);
        return new Frame(frame[4] & 0xFF, frame[5] & 0xFF, payload);
    }

    private void ringRead(int dataOffset, int ringPosition, byte[] destination, int destinationOffset, int length) {
        final int offset = ringPosition & (ringCapacity - 1);
        final int firstChunk = Math.min(ringCapacity - offset, length);
        final int secondChunk = length - firstChunk;

        for (int i = 0; i < firstChunk; ++i) {
            destination[destinationOffset + i] = mapping.get(dataOffset + offset + i);
        }
        for (int i = 0; i < secondChunk; ++i) {
            destination[destinationOffset + firstChunk + i] = mapping.get(dataOffset + i);
        }
    }

    private void advanceHead(int controlOffset, int newHead) {
        INT_VIEW.setRelease(mapping, controlOffset + RING_HEAD, newHead);
    }

    // ── доступ к разделяемой памяти ────────────────────────────────────────

    /** Чтение с семантикой acquire (см. комментарий к INT_VIEW). */
    private int getU32(int offset) {
        return (int) INT_VIEW.getAcquire(mapping, offset);
    }

    private void putU32(int offset, int value) {
        INT_VIEW.setRelease(mapping, offset, value);
    }

    private int getU16(int offset) {
        return Short.toUnsignedInt(mapping.getShort(offset));
    }

    private void putU16(int offset, int value) {
        mapping.putShort(offset, (short) value);
    }

    private static int readU16(byte[] data, int offset) {
        return (data[offset] & 0xFF) | ((data[offset + 1] & 0xFF) << 8);
    }

    private static int readU32(byte[] data, int offset) {
        return (data[offset] & 0xFF)
                | ((data[offset + 1] & 0xFF) << 8)
                | ((data[offset + 2] & 0xFF) << 16)
                | ((data[offset + 3] & 0xFF) << 24);
    }

    private static void writeU16(byte[] data, int offset, int value) {
        data[offset] = (byte) (value & 0xFF);
        data[offset + 1] = (byte) ((value >>> 8) & 0xFF);
    }

    private static void writeU32(byte[] data, int offset, int value) {
        data[offset] = (byte) (value & 0xFF);
        data[offset + 1] = (byte) ((value >>> 8) & 0xFF);
        data[offset + 2] = (byte) ((value >>> 16) & 0xFF);
        data[offset + 3] = (byte) ((value >>> 24) & 0xFF);
    }

    /** Дамп состояния канала для команды /gwyc status. */
    public String describe() {
        if (!open) {
            return "канал закрыт";
        }
        final int head = (int) getU32(ringAOffset + RING_HEAD);
        final int tail = (int) getU32(ringAOffset + RING_TAIL);
        final int mcTail = (int) getU32(ringBOffset + RING_TAIL);
        final int mcHead = (int) getU32(ringBOffset + RING_HEAD);

        return String.format(
                "канал: ёмкость %d Б, игра %s, отправлено %d, принято %d, CRC-ошибок %d, пересинхронизаций %d"
                        + " (входящих в буфере %d Б, исходящих %d Б)",
                ringCapacity, isGameReady() ? "на связи" : "молчит",
                messagesSent, messagesReceived, crcFailures, resyncs, tail - head, mcTail - mcHead);
    }

    /** Заголовок строки фиксированной длины: UTF-8 с обрезкой по нулю. */
    public static String readFixedString(byte[] data, int offset, int capacity) {
        int length = 0;
        while (length < capacity && data[offset + length] != 0) {
            ++length;
        }
        return new String(data, offset, length, StandardCharsets.UTF_8);
    }
}
