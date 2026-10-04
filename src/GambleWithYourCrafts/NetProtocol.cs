using System.Buffers.Binary;
using System.Globalization;
using System.Text;

namespace GambleWithYourCrafts;

/// <summary>
/// Тип сетевого сообщения. Один байт в заголовке пакета.
/// </summary>
public enum MessageType : byte
{
    Hello = 1,
    Chat = 2,
    Ping = 3,
    Pong = 4,
    SyncRequest = 5,
    Presence = 6,
    VoxelEdit = 7,
    VoxelBatch = 8,
    VoxelSnapshot = 9,
    InventorySync = 10,
    BlockRequest = 11,
    CasinoBetRequest = 12,
    CasinoResult = 13,
    Bye = 14,
    FillRequest = 15,
    CraftRequest = 16,
}

/// <summary>
/// Транспорт, поверх которого работает игровая сессия.
/// Реализации: <see cref="P2PLobbyManager"/> (Steam P2P) и LoopbackNetwork (в SelfTest).
///
/// КОНТРАКТ ПОТОКОВ: <see cref="PacketReceived"/> поднимается на главном потоке.
/// Все игровые системы рассчитаны на однопоточную обработку — не вызывайте SendTo
/// из чужих потоков без нужды, а если вызываете, результат просто попадёт в очередь
/// Steam и будет отправлен на ближайшем тике.
/// </summary>
public interface INetTransport
{
    /// <summary>Являюсь ли я сейчас авторитетным хостом (владельцем лобби).</summary>
    bool IsHost { get; }

    /// <summary>SteamId локального игрока.</summary>
    ulong LocalId { get; }

    /// <summary>SteamId хоста сессии (владельца лобби). Авторитетные сообщения принимаем только от него.</summary>
    ulong HostId { get; }

    /// <summary>Участники лобби без локального игрока.</summary>
    IReadOnlyCollection<ulong> Peers { get; }

    /// <summary>Отправить пакет конкретному пиру. false — пакет не ушёл (нет сессии/пир не в лобби).</summary>
    bool SendTo(ulong peer, byte[] payload, bool reliable);

    /// <summary>Отправить пакет всем пирам лобби.</summary>
    void Broadcast(byte[] payload, bool reliable);

    /// <summary>Входящий пакет: (отправитель, сырые байты). Поднимается на главном потоке.</summary>
    event Action<ulong, byte[]>? PacketReceived;
}

/// <summary>
/// Транспорт одиночного режима: пиров нет, мы сами себе хост.
/// Нужен, чтобы игра запускалась и была полностью играбельна без Steam.
/// </summary>
public sealed class OfflineTransport : INetTransport
{
    public bool IsHost => true;

    public ulong LocalId => 0;

    public ulong HostId => 0;

    public IReadOnlyCollection<ulong> Peers => Array.Empty<ulong>();

    public bool SendTo(ulong peer, byte[] payload, bool reliable) => false;

    public void Broadcast(byte[] payload, bool reliable)
    {
        // В одиночном режиме рассылать некому.
    }

    /// <summary>Пакетов извне не бывает — обработчик никогда не вызывается.</summary>
    public event Action<ulong, byte[]>? PacketReceived
    {
        add { }
        remove { }
    }
}

/// <summary>Ошибка разбора/формирования пакета. Ловится на входе — битый пакет не должен ронять игру.</summary>
public sealed class ProtocolException : Exception
{
    public ProtocolException(string message) : base(message) { }
}

/// <summary>Запись значений в пакет (little-endian, строки — varint-длина + UTF-8).</summary>
public sealed class PacketWriter
{
    private byte[] _buffer;
    private int _length;

    public PacketWriter(int capacity = 256)
    {
        _buffer = new byte[Math.Max(16, capacity)];
    }

    public int Length => _length;

    private void Ensure(int extra)
    {
        if (_length + extra <= _buffer.Length) return;
        int size = _buffer.Length;
        while (size < _length + extra) size *= 2;
        Array.Resize(ref _buffer, size);
    }

    public void WriteByte(byte value)
    {
        Ensure(1);
        _buffer[_length++] = value;
    }

    public void WriteBool(bool value) => WriteByte(value ? (byte)1 : (byte)0);

    public void WriteUInt16(ushort value)
    {
        Ensure(2);
        BinaryPrimitives.WriteUInt16LittleEndian(_buffer.AsSpan(_length), value);
        _length += 2;
    }

    public void WriteInt32(int value)
    {
        Ensure(4);
        BinaryPrimitives.WriteInt32LittleEndian(_buffer.AsSpan(_length), value);
        _length += 4;
    }

    public void WriteInt64(long value)
    {
        Ensure(8);
        BinaryPrimitives.WriteInt64LittleEndian(_buffer.AsSpan(_length), value);
        _length += 8;
    }

    public void WriteUInt64(ulong value)
    {
        Ensure(8);
        BinaryPrimitives.WriteUInt64LittleEndian(_buffer.AsSpan(_length), value);
        _length += 8;
    }

    /// <summary>Varint (LEB128) — компактно для счётчиков, координат и длин.</summary>
    public void WriteVarUInt(uint value)
    {
        while (value >= 0x80)
        {
            WriteByte((byte)(value | 0x80));
            value >>= 7;
        }
        WriteByte((byte)value);
    }

    /// <summary>Varint для знаковых координат (zig-zag).</summary>
    public void WriteVarInt(int value) => WriteVarUInt((uint)((value << 1) ^ (value >> 31)));

    public void WriteString(string? value)
    {
        if (string.IsNullOrEmpty(value))
        {
            WriteVarUInt(0);
            return;
        }

        byte[] bytes = Encoding.UTF8.GetBytes(value);
        WriteVarUInt((uint)bytes.Length);
        Ensure(bytes.Length);
        Buffer.BlockCopy(bytes, 0, _buffer, _length, bytes.Length);
        _length += bytes.Length;
    }

    public void WriteBytes(ReadOnlySpan<byte> value)
    {
        WriteVarUInt((uint)value.Length);
        Ensure(value.Length);
        value.CopyTo(_buffer.AsSpan(_length));
        _length += value.Length;
    }

    public byte[] ToArray()
    {
        var result = new byte[_length];
        Buffer.BlockCopy(_buffer, 0, result, 0, _length);
        return result;
    }
}

/// <summary>Чтение значений из пакета с жёсткой проверкой границ.</summary>
public sealed class PacketReader
{
    /// <summary>Верхняя граница для строк из сети — защита от мусора/подделки.</summary>
    public const int MaxStringBytes = 4096;

    private readonly byte[] _buffer;
    private readonly int _limit;
    private int _position;

    public PacketReader(byte[] buffer, int offset = 0, int? length = null)
    {
        _buffer = buffer ?? throw new ArgumentNullException(nameof(buffer));
        _position = offset;
        _limit = length.HasValue ? offset + length.Value : buffer.Length;
        if (_limit > buffer.Length) throw new ProtocolException("Длина среза больше буфера.");
    }

    public int Remaining => _limit - _position;
    public bool AtEnd => _position >= _limit;

    private void Demand(int count)
    {
        if (count < 0) throw new ProtocolException("Отрицательный размер поля.");
        if (Remaining < count) throw new ProtocolException($"Пакет короче ожидаемого (нужно {count}, есть {Remaining}).");
    }

    public byte ReadByte()
    {
        Demand(1);
        return _buffer[_position++];
    }

    public bool ReadBool() => ReadByte() != 0;

    public ushort ReadUInt16()
    {
        Demand(2);
        ushort value = BinaryPrimitives.ReadUInt16LittleEndian(_buffer.AsSpan(_position));
        _position += 2;
        return value;
    }

    public int ReadInt32()
    {
        Demand(4);
        int value = BinaryPrimitives.ReadInt32LittleEndian(_buffer.AsSpan(_position));
        _position += 4;
        return value;
    }

    public long ReadInt64()
    {
        Demand(8);
        long value = BinaryPrimitives.ReadInt64LittleEndian(_buffer.AsSpan(_position));
        _position += 8;
        return value;
    }

    public ulong ReadUInt64()
    {
        Demand(8);
        ulong value = BinaryPrimitives.ReadUInt64LittleEndian(_buffer.AsSpan(_position));
        _position += 8;
        return value;
    }

    public uint ReadVarUInt()
    {
        uint result = 0;
        int shift = 0;
        while (true)
        {
            if (shift > 35) throw new ProtocolException("Слишком длинный varint.");
            byte b = ReadByte();
            result |= (uint)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return result;
            shift += 7;
        }
    }

    public int ReadVarInt()
    {
        uint raw = ReadVarUInt();
        return (int)(raw >> 1) ^ -(int)(raw & 1);
    }

    public string ReadString()
    {
        uint length = ReadVarUInt();
        if (length == 0) return string.Empty;
        if (length > MaxStringBytes) throw new ProtocolException($"Строка из сети слишком длинная: {length} байт.");
        Demand((int)length);
        string value = Encoding.UTF8.GetString(_buffer, _position, (int)length);
        _position += (int)length;
        return value;
    }

    public byte[] ReadBytes()
    {
        uint length = ReadVarUInt();
        if (length > (uint)Remaining) throw new ProtocolException("Массив выходит за границы пакета.");
        var result = new byte[length];
        Buffer.BlockCopy(_buffer, _position, result, 0, (int)length);
        _position += (int)length;
        return result;
    }

    public byte[] ReadRaw(int count)
    {
        Demand(count);
        var result = new byte[count];
        Buffer.BlockCopy(_buffer, _position, result, 0, count);
        _position += count;
        return result;
    }
}

/// <summary>
/// Базовое сетевое сообщение. Формат пакета:
/// [ 'G' | 'C' | version | type | тело... ]
/// </summary>
public abstract class NetMessage
{
    /// <summary>Версия протокола. Несовпадение = отказ от сессии ещё на Hello.</summary>
    public const byte ProtocolVersion = 3;

    private const byte MagicA = (byte)'G';
    private const byte MagicB = (byte)'C';

    /// <summary>Максимальный размер одного P2P-пакета. Всё, что больше, режется на фрагменты.</summary>
    public const int MaxPacketBytes = 4096;

    public abstract MessageType Type { get; }

    protected abstract void WriteBody(PacketWriter writer);

    protected abstract void ReadBody(PacketReader reader);

    public byte[] Serialize()
    {
        var writer = new PacketWriter(64);
        writer.WriteByte(MagicA);
        writer.WriteByte(MagicB);
        writer.WriteByte(ProtocolVersion);
        writer.WriteByte((byte)Type);
        WriteBody(writer);
        return writer.ToArray();
    }

    /// <summary>Разбор пакета из сети. Бросает <see cref="ProtocolException"/> на мусор.</summary>
    public static NetMessage Deserialize(byte[] data)
    {
        if (data.Length < 4) throw new ProtocolException("Пакет короче заголовка.");
        if (data[0] != MagicA || data[1] != MagicB) throw new ProtocolException("Не наш пакет (нет сигнатуры GC).");

        byte version = data[2];
        if (version != ProtocolVersion)
            throw new ProtocolException($"Версия протокола {version} != {ProtocolVersion} (у друга другая сборка игры).");

        var type = (MessageType)data[3];
        if (!Factories.TryGetValue(type, out var factory))
            throw new ProtocolException($"Неизвестный тип сообщения: {type}.");

        var reader = new PacketReader(data, 4);
        NetMessage message = factory();
        message.ReadBody(reader);
        return message;
    }

    public static string TypeName(MessageType type) => type.ToString();

    private static readonly Dictionary<MessageType, Func<NetMessage>> Factories = new()
    {
        [MessageType.Hello] = () => new HelloMessage(),
        [MessageType.Chat] = () => new ChatMessage(),
        [MessageType.Ping] = () => new PingMessage(),
        [MessageType.Pong] = () => new PongMessage(),
        [MessageType.SyncRequest] = () => new SyncRequestMessage(),
        [MessageType.Presence] = () => new PresenceMessage(),
        [MessageType.VoxelEdit] = () => new VoxelEditMessage(),
        [MessageType.VoxelBatch] = () => new VoxelBatchMessage(),
        [MessageType.VoxelSnapshot] = () => new VoxelSnapshotMessage(),
        [MessageType.InventorySync] = () => new InventorySyncMessage(),
        [MessageType.BlockRequest] = () => new BlockRequestMessage(),
        [MessageType.CasinoBetRequest] = () => new CasinoBetRequestMessage(),
        [MessageType.CasinoResult] = () => new CasinoResultMessage(),
        [MessageType.Bye] = () => new ByeMessage(),
        [MessageType.FillRequest] = () => new FillRequestMessage(),
        [MessageType.CraftRequest] = () => new CraftRequestMessage(),
    };
}

/// <summary>Приветствие. Клиент → хост и хост → клиент (рукопожатие и обмен именами).</summary>
public sealed class HelloMessage : NetMessage
{
    public override MessageType Type => MessageType.Hello;

    public string PlayerName { get; set; } = string.Empty;
    public string ClientTag { get; set; } = string.Empty;
    public bool WantsFullState { get; set; }

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteString(PlayerName);
        writer.WriteString(ClientTag);
        writer.WriteBool(WantsFullState);
    }

    protected override void ReadBody(PacketReader reader)
    {
        PlayerName = reader.ReadString();
        ClientTag = reader.ReadString();
        WantsFullState = reader.ReadBool();
    }
}

/// <summary>Чат: клиент отправляет хосту, хост рассылает всем (единый источник порядка сообщений).</summary>
public sealed class ChatMessage : NetMessage
{
    public override MessageType Type => MessageType.Chat;

    public string SenderName { get; set; } = string.Empty;
    public string Text { get; set; } = string.Empty;

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteString(SenderName);
        writer.WriteString(Text);
    }

    protected override void ReadBody(PacketReader reader)
    {
        SenderName = reader.ReadString();
        Text = reader.ReadString();
    }
}

/// <summary>Пинг для измерения задержки до пира (эхо времени отправителя).</summary>
public sealed class PingMessage : NetMessage
{
    public override MessageType Type => MessageType.Ping;

    public long Stamp { get; set; }

    protected override void WriteBody(PacketWriter writer) => writer.WriteInt64(Stamp);

    protected override void ReadBody(PacketReader reader) => Stamp = reader.ReadInt64();
}

/// <summary>Ответ на пинг: возвращаем метку клиента, чтобы он посчитал RTT.</summary>
public sealed class PongMessage : NetMessage
{
    public override MessageType Type => MessageType.Pong;

    public long Stamp { get; set; }
    public long PeerStamp { get; set; }

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteInt64(Stamp);
        writer.WriteInt64(PeerStamp);
    }

    protected override void ReadBody(PacketReader reader)
    {
        Stamp = reader.ReadInt64();
        PeerStamp = reader.ReadInt64();
    }
}

/// <summary>Запрос полной синхронизации мира и инвентарей у хоста.</summary>
public sealed class SyncRequestMessage : NetMessage
{
    public override MessageType Type => MessageType.SyncRequest;

    protected override void WriteBody(PacketWriter writer) { }

    protected override void ReadBody(PacketReader reader) { }
}

public enum PresenceKind : byte
{
    Hello = 0,
    Joined = 1,
    Left = 2,
    Denied = 3,
}

/// <summary>Кто в сессии: хост рассылает состав (имена, роли, задержки).</summary>
public sealed class PresenceMessage : NetMessage
{
    public override MessageType Type => MessageType.Presence;

    public PresenceKind Kind { get; set; }
    public ulong PlayerId { get; set; }
    public string PlayerName { get; set; } = string.Empty;
    public bool IsHost { get; set; }
    public string Note { get; set; } = string.Empty;

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteByte((byte)Kind);
        writer.WriteUInt64(PlayerId);
        writer.WriteString(PlayerName);
        writer.WriteBool(IsHost);
        writer.WriteString(Note);
    }

    protected override void ReadBody(PacketReader reader)
    {
        Kind = (PresenceKind)reader.ReadByte();
        PlayerId = reader.ReadUInt64();
        PlayerName = reader.ReadString();
        IsHost = reader.ReadBool();
        Note = reader.ReadString();
    }
}

/// <summary>Одиночная правка вокселя. Хост → всем; клиенты только применяют.</summary>
public sealed class VoxelEditMessage : NetMessage
{
    public override MessageType Type => MessageType.VoxelEdit;

    public int X { get; set; }
    public int Y { get; set; }
    public int Z { get; set; }
    public byte Block { get; set; }
    public ulong Author { get; set; }
    public string AuthorName { get; set; } = string.Empty;

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteVarInt(X);
        writer.WriteVarInt(Y);
        writer.WriteVarInt(Z);
        writer.WriteByte(Block);
        writer.WriteUInt64(Author);
        writer.WriteString(AuthorName);
    }

    protected override void ReadBody(PacketReader reader)
    {
        X = reader.ReadVarInt();
        Y = reader.ReadVarInt();
        Z = reader.ReadVarInt();
        Block = reader.ReadByte();
        Author = reader.ReadUInt64();
        AuthorName = reader.ReadString();
    }
}

/// <summary>
/// Пакетная правка (команда fill, заготовки арен). Режется на фрагменты тем же
/// механизмом, что и снимок мира: BatchId + Index/Count.
/// </summary>
public sealed class VoxelBatchMessage : NetMessage
{
    public override MessageType Type => MessageType.VoxelBatch;

    public int BatchId { get; set; }
    public int Index { get; set; }
    public int Count { get; set; }
    public ulong Author { get; set; }
    public string AuthorName { get; set; } = string.Empty;
    public byte[] Payload { get; set; } = Array.Empty<byte>();

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteInt32(BatchId);
        writer.WriteVarUInt((uint)Index);
        writer.WriteVarUInt((uint)Count);
        writer.WriteUInt64(Author);
        writer.WriteString(AuthorName);
        writer.WriteBytes(Payload);
    }

    protected override void ReadBody(PacketReader reader)
    {
        BatchId = reader.ReadInt32();
        Index = (int)reader.ReadVarUInt();
        Count = (int)reader.ReadVarUInt();
        Author = reader.ReadUInt64();
        AuthorName = reader.ReadString();
        Payload = reader.ReadBytes();
    }
}

/// <summary>Фрагмент снимка мира (RLE-сжатая сетка), досылается новым игрокам и при ресинке.</summary>
public sealed class VoxelSnapshotMessage : NetMessage
{
    public override MessageType Type => MessageType.VoxelSnapshot;

    public int SyncId { get; set; }
    public int Index { get; set; }
    public int Count { get; set; }
    public ushort SizeX { get; set; }
    public ushort SizeY { get; set; }
    public ushort SizeZ { get; set; }
    public byte[] Payload { get; set; } = Array.Empty<byte>();

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteInt32(SyncId);
        writer.WriteVarUInt((uint)Index);
        writer.WriteVarUInt((uint)Count);
        writer.WriteUInt16(SizeX);
        writer.WriteUInt16(SizeY);
        writer.WriteUInt16(SizeZ);
        writer.WriteBytes(Payload);
    }

    protected override void ReadBody(PacketReader reader)
    {
        SyncId = reader.ReadInt32();
        Index = (int)reader.ReadVarUInt();
        Count = (int)reader.ReadVarUInt();
        SizeX = reader.ReadUInt16();
        SizeY = reader.ReadUInt16();
        SizeZ = reader.ReadUInt16();
        Payload = reader.ReadBytes();
        if (SizeX is < 2 or > 256 || SizeY is < 2 or > 256 || SizeZ is < 2 or > 256)
            throw new ProtocolException($"Хост прислал некорректный размер мира: {SizeX}x{SizeY}x{SizeZ}.");
    }
}

/// <summary>Снимок креативного инвентаря игрока (ключ блока → количество).</summary>
public sealed class InventorySyncMessage : NetMessage
{
    public override MessageType Type => MessageType.InventorySync;

    public ulong PlayerId { get; set; }
    public string PlayerName { get; set; } = string.Empty;
    public bool IsSelfReport { get; set; }
    public List<KeyValuePair<string, int>> Items { get; set; } = new();

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteUInt64(PlayerId);
        writer.WriteString(PlayerName);
        writer.WriteBool(IsSelfReport);
        writer.WriteVarUInt((uint)Items.Count);
        foreach (var item in Items)
        {
            writer.WriteString(item.Key);
            writer.WriteVarUInt((uint)Math.Max(0, item.Value));
        }
    }

    protected override void ReadBody(PacketReader reader)
    {
        PlayerId = reader.ReadUInt64();
        PlayerName = reader.ReadString();
        IsSelfReport = reader.ReadBool();
        int count = (int)reader.ReadVarUInt();
        if (count > 256) throw new ProtocolException($"Слишком много позиций инвентаря: {count}.");
        Items = new List<KeyValuePair<string, int>>(count);
        for (int i = 0; i < count; i++)
        {
            string key = reader.ReadString();
            int amount = (int)reader.ReadVarUInt();
            Items.Add(new KeyValuePair<string, int>(key, amount));
        }
    }
}

/// <summary>
/// Запрос клиента поставить/сломать блок. Block = Air означает "сломать".
/// Хост проверяет границы и ресурсы, после чего рассылает VoxelEdit всем.
/// </summary>
public sealed class BlockRequestMessage : NetMessage
{
    public override MessageType Type => MessageType.BlockRequest;

    public int X { get; set; }
    public int Y { get; set; }
    public int Z { get; set; }
    public byte Block { get; set; }

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteVarInt(X);
        writer.WriteVarInt(Y);
        writer.WriteVarInt(Z);
        writer.WriteByte(Block);
    }

    protected override void ReadBody(PacketReader reader)
    {
        X = reader.ReadVarInt();
        Y = reader.ReadVarInt();
        Z = reader.ReadVarInt();
        Block = reader.ReadByte();
    }
}

/// <summary>Ставка клиента: игра, цель (red/black/high/...), тип блока-ставки и количество.</summary>
public sealed class CasinoBetRequestMessage : NetMessage
{
    public override MessageType Type => MessageType.CasinoBetRequest;

    public byte Game { get; set; }
    public string Target { get; set; } = string.Empty;
    public string BlockKey { get; set; } = string.Empty;
    public int Amount { get; set; }

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteByte(Game);
        writer.WriteString(Target);
        writer.WriteString(BlockKey);
        writer.WriteVarUInt((uint)Math.Max(0, Amount));
    }

    protected override void ReadBody(PacketReader reader)
    {
        Game = reader.ReadByte();
        Target = reader.ReadString();
        BlockKey = reader.ReadString();
        Amount = (int)reader.ReadVarUInt();
    }
}

/// <summary>Результат раунда казино — считает только хост, остальные показывают и применяют.</summary>
public sealed class CasinoResultMessage : NetMessage
{
    public override MessageType Type => MessageType.CasinoResult;

    public ulong PlayerId { get; set; }
    public string PlayerName { get; set; } = string.Empty;
    public byte Game { get; set; }
    public string Target { get; set; } = string.Empty;
    public string BlockKey { get; set; } = string.Empty;
    public int Stake { get; set; }
    public int Multiplier { get; set; }
    public int Payout { get; set; }
    public bool Won { get; set; }
    public byte[] Rolls { get; set; } = Array.Empty<byte>();
    public string RollText { get; set; } = string.Empty;

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteUInt64(PlayerId);
        writer.WriteString(PlayerName);
        writer.WriteByte(Game);
        writer.WriteString(Target);
        writer.WriteString(BlockKey);
        writer.WriteVarUInt((uint)Math.Max(0, Stake));
        writer.WriteVarUInt((uint)Math.Max(0, Multiplier));
        writer.WriteVarUInt((uint)Math.Max(0, Payout));
        writer.WriteBool(Won);
        writer.WriteBytes(Rolls);
        writer.WriteString(RollText);
    }

    protected override void ReadBody(PacketReader reader)
    {
        PlayerId = reader.ReadUInt64();
        PlayerName = reader.ReadString();
        Game = reader.ReadByte();
        Target = reader.ReadString();
        BlockKey = reader.ReadString();
        Stake = (int)reader.ReadVarUInt();
        Multiplier = (int)reader.ReadVarUInt();
        Payout = (int)reader.ReadVarUInt();
        Won = reader.ReadBool();
        Rolls = reader.ReadBytes();
        RollText = reader.ReadString();
    }

    /// <summary>Короткая строка результата для логов/экрана.</summary>
    public string SummaryText() => Won
        ? $"+{Payout} (x{Multiplier})"
        : (Stake == 0 ? "без ставки" : $"-{Stake}");
}

/// <summary>
/// Запрос клиента залить объём блоком (команда fill). Хост сам пересчитывает
/// пересечение с миром, проверяет ресурсы и рассылает VoxelBatch.
/// </summary>
public sealed class FillRequestMessage : NetMessage
{
    public override MessageType Type => MessageType.FillRequest;

    public byte Block { get; set; }
    public int MinX { get; set; }
    public int MinY { get; set; }
    public int MinZ { get; set; }
    public int MaxX { get; set; }
    public int MaxY { get; set; }
    public int MaxZ { get; set; }

    protected override void WriteBody(PacketWriter writer)
    {
        writer.WriteByte(Block);
        writer.WriteVarInt(MinX);
        writer.WriteVarInt(MinY);
        writer.WriteVarInt(MinZ);
        writer.WriteVarInt(MaxX);
        writer.WriteVarInt(MaxY);
        writer.WriteVarInt(MaxZ);
    }

    protected override void ReadBody(PacketReader reader)
    {
        Block = reader.ReadByte();
        MinX = reader.ReadVarInt();
        MinY = reader.ReadVarInt();
        MinZ = reader.ReadVarInt();
        MaxX = reader.ReadVarInt();
        MaxY = reader.ReadVarInt();
        MaxZ = reader.ReadVarInt();
    }
}

/// <summary>Запрос клиента скрафтить стол казино. Рецепт проверяет хост.</summary>
public sealed class CraftRequestMessage : NetMessage
{
    public override MessageType Type => MessageType.CraftRequest;

    public int Count { get; set; }

    protected override void WriteBody(PacketWriter writer) => writer.WriteVarUInt((uint)Math.Max(0, Count));

    protected override void ReadBody(PacketReader reader) => Count = (int)reader.ReadVarUInt();
}

/// <summary>Вежливое прощание (выход из лобби).</summary>
public sealed class ByeMessage : NetMessage
{
    public override MessageType Type => MessageType.Bye;

    public string Reason { get; set; } = string.Empty;

    protected override void WriteBody(PacketWriter writer) => writer.WriteString(Reason);

    protected override void ReadBody(PacketReader reader) => Reason = reader.ReadString();
}

/// <summary>
/// Сборщик фрагментов: накапливает куски снимка мира/батча и собирает их в один буфер.
/// Один экземпляр обслуживает несколько параллельных передач (ключ — идентификатор передачи).
/// </summary>
public sealed class FragmentAssembler
{
    private sealed class Pending
    {
        public byte[]?[] Parts = Array.Empty<byte[]>();
        public int Received;
        public long StartedAtMs;
    }

    private readonly Dictionary<int, Pending> _pending = new();
    private readonly long _timeoutMs;

    public FragmentAssembler(long timeoutMs = 15000) => _timeoutMs = timeoutMs;

    /// <summary>Забыть устаревшие передачи (иначе память течёт на брошенных синках).</summary>
    public void Prune(long nowMs)
    {
        if (_pending.Count == 0) return;
        var stale = new List<int>();
        foreach (var pair in _pending)
        {
            if (nowMs - pair.Value.StartedAtMs > _timeoutMs) stale.Add(pair.Key);
        }
        foreach (int key in stale) _pending.Remove(key);
    }

    public void Reset() => _pending.Clear();

    /// <summary>
    /// Добавить фрагмент. Возвращает собранный буфер, когда пришли все части; иначе null.
    /// </summary>
    public byte[]? Add(int transferId, int index, int count, byte[] payload, long nowMs)
    {
        if (count <= 0 || count > 4096) throw new ProtocolException($"Некорректное число фрагментов: {count}.");
        if (index < 0 || index >= count) throw new ProtocolException($"Некорректный индекс фрагмента: {index}.");

        if (!_pending.TryGetValue(transferId, out var state) || state.Parts.Length != count)
        {
            state = new Pending { Parts = new byte[]?[count], StartedAtMs = nowMs };
            _pending[transferId] = state;
        }

        if (state.Parts[index] is null)
        {
            state.Parts[index] = payload;
            state.Received++;
        }

        if (state.Received < count) return null;

        int total = 0;
        foreach (var part in state.Parts) total += part?.Length ?? 0;

        var result = new byte[total];
        int offset = 0;
        foreach (var part in state.Parts)
        {
            if (part is null) return null;
            Buffer.BlockCopy(part, 0, result, offset, part.Length);
            offset += part.Length;
        }

        _pending.Remove(transferId);
        return result;
    }
}

/// <summary>Разбиение большого буфера на фрагменты, которые влезают в один P2P-пакет.</summary>
public static class Fragmenter
{
    /// <summary>Полезная нагрузка одного фрагмента снимка (заголовок + varint-поля оставляют запас).</summary>
    public const int ChunkSize = NetMessage.MaxPacketBytes - 256;

    public static int CountFor(int totalBytes) => Math.Max(1, (totalBytes + ChunkSize - 1) / ChunkSize);

    public static byte[] Slice(byte[] source, int index)
    {
        int count = CountFor(source.Length);
        if (index < 0 || index >= count) throw new ArgumentOutOfRangeException(nameof(index));

        int offset = index * ChunkSize;
        int length = Math.Min(ChunkSize, source.Length - offset);
        var slice = new byte[length];
        Buffer.BlockCopy(source, offset, slice, 0, length);
        return slice;
    }
}

/// <summary>Счётчик передачи: выдаёт идентификаторы синков/батчей (монотонно, без повторов в сессии).</summary>
public sealed class TransferIdSource
{
    private int _next;

    public int Next()
    {
        int id = Interlocked.Increment(ref _next);
        if (id <= 0) id = Interlocked.Exchange(ref _next, 1);
        return id;
    }
}

/// <summary>Мелкие утилиты форматирования для логов/консоли (инвариантная культура — важно для тестов).</summary>
public static class Fmt
{
    public static string I(int value) => value.ToString(CultureInfo.InvariantCulture);

    public static string L(long value) => value.ToString(CultureInfo.InvariantCulture);

    public static string Pct(double ratio) => (ratio * 100d).ToString("0.0", CultureInfo.InvariantCulture) + "%";

    public static string N2(double value) => value.ToString("0.##", CultureInfo.InvariantCulture);
}
