using System.Text;

namespace GambleWithYourCrafts;

/// <summary>
/// Типы вокселей. Значения — это то, что реально уходит по сети в одном байте.
/// </summary>
public enum BlockId : byte
{
    Air = 0,
    Dirt = 1,
    Grass = 2,
    Stone = 3,
    Wood = 4,
    Gold = 5,
    Diamond = 6,
    CasinoTable = 7,
    Lamp = 8,
}

/// <summary>Описание типа блока: имя для консоли, глиф, цвет, ценность в блоках-единицах.</summary>
public sealed class BlockInfo
{
    public BlockInfo(BlockId id, string key, string title, char glyph, ConsoleColor color, int value, bool placeable, params string[] aliases)
    {
        Id = id;
        Key = key;
        Title = title;
        Glyph = glyph;
        Color = color;
        Value = value;
        Placeable = placeable;
        Aliases = aliases;
    }

    public BlockId Id { get; }
    /// <summary>Ключ инвентаря: он же используется в командах и в Dictionary&lt;string,int&gt;.</summary>
    public string Key { get; }
    public string Title { get; }
    public char Glyph { get; }
    public ConsoleColor Color { get; }
    /// <summary>Сколько "стоит" один блок (используется для оценки богатства). 0 — декор/инструмент.</summary>
    public int Value { get; }
    public bool Placeable { get; }
    public IReadOnlyList<string> Aliases { get; }

    /// <summary>Можно ли ставить этот блок на ставку (любой неоцениваемый блок ставить нельзя).</summary>
    public bool IsCurrency => Value > 0;

    public override string ToString() => Key;
}

/// <summary>Каталог блоков: парсинг имён (в т.ч. русских), цвета, глифы.</summary>
public static class Blocks
{
    public const string DirtKey = "dirt";
    public const string GrassKey = "grass";
    public const string StoneKey = "stone";
    public const string WoodKey = "wood";
    public const string GoldKey = "gold";
    public const string DiamondKey = "diamond";
    public const string TableKey = "table";
    public const string LampKey = "lamp";

    private static readonly BlockInfo[] Catalog =
    {
        new(BlockId.Air, "air", "Воздух", ' ', ConsoleColor.Black, 0, false, "пусто", "empty"),
        new(BlockId.Dirt, DirtKey, "Земля", '.', ConsoleColor.DarkYellow, 1, true, "земля", "ground"),
        new(BlockId.Grass, GrassKey, "Трава", '"', ConsoleColor.Green, 1, true, "трава"),
        new(BlockId.Stone, StoneKey, "Камень", '#', ConsoleColor.Gray, 2, true, "камень", "rock"),
        new(BlockId.Wood, WoodKey, "Дерево", '%', ConsoleColor.DarkRed, 3, true, "дерево", "log"),
        new(BlockId.Gold, GoldKey, "Золото", '$', ConsoleColor.Yellow, 8, true, "золото", "gold"),
        new(BlockId.Diamond, DiamondKey, "Алмаз", '*', ConsoleColor.Cyan, 25, true, "алмаз", "dia"),
        new(BlockId.CasinoTable, TableKey, "Стол казино", '@', ConsoleColor.Magenta, 0, true, "стол", "casino"),
        new(BlockId.Lamp, LampKey, "Светильник", 'o', ConsoleColor.White, 0, true, "светильник", "light"),
    };

    private static readonly Dictionary<string, BlockId> Lookup = BuildLookup();

    private static Dictionary<string, BlockId> BuildLookup()
    {
        var map = new Dictionary<string, BlockId>(StringComparer.OrdinalIgnoreCase);
        foreach (var info in Catalog)
        {
            map[info.Key] = info.Id;
            map[info.Title] = info.Id;
            foreach (string alias in info.Aliases) map[alias] = info.Id;
        }
        return map;
    }

    /// <summary>Все блоки, включая воздух (индекс = значение BlockId).</summary>
    public static IReadOnlyList<BlockInfo> All => Catalog;

    /// <summary>Только блоки, которые можно положить в мир.</summary>
    public static IEnumerable<BlockInfo> Placeable => Catalog.Where(b => b.Placeable && b.Id != BlockId.Air);

    /// <summary>Блоки, которые можно ставить на кон (все ценные).</summary>
    public static IEnumerable<BlockInfo> Currencies => Catalog.Where(b => b.IsCurrency);

    public static BlockInfo Info(BlockId id)
    {
        int index = (int)id;
        if (index < 0 || index >= Catalog.Length) throw new ArgumentOutOfRangeException(nameof(id), $"Неизвестный блок: {id}");
        return Catalog[index];
    }

    public static bool IsDefined(byte raw) => raw < Catalog.Length;

    /// <summary>Разбор имени блока, введённого игроком ("золото", "gold", "diamond").</summary>
    public static bool TryParse(string text, out BlockId id)
    {
        if (!string.IsNullOrWhiteSpace(text) && Lookup.TryGetValue(text.Trim(), out id)) return true;
        id = BlockId.Air;
        return false;
    }

    public static string CatalogText()
    {
        var sb = new StringBuilder();
        foreach (var block in Placeable)
        {
            sb.Append("  ").Append(block.Glyph).Append("  ").Append(block.Key.PadRight(8))
              .Append(block.Title.PadRight(14))
              .Append("ценность: ").Append(block.Value > 0 ? block.Value.ToString() : "—")
              .AppendLine();
        }
        return sb.ToString();
    }
}

/// <summary>Позиция вокселя в мировых координатах (0-based, Y — вверх).</summary>
public readonly record struct VoxelPos(int X, int Y, int Z)
{
    public override string ToString() => $"{X},{Y},{Z}";

    public static bool TryParse(string text, out VoxelPos pos)
    {
        pos = default;
        if (string.IsNullOrWhiteSpace(text)) return false;
        string[] parts = text.Split(new[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length != 3) return false;
        if (!int.TryParse(parts[0], out int x)) return false;
        if (!int.TryParse(parts[1], out int y)) return false;
        if (!int.TryParse(parts[2], out int z)) return false;
        pos = new VoxelPos(x, y, z);
        return true;
    }
}

/// <summary>Правка одного вокселя с автором — то, что транслируется по сети.</summary>
public readonly record struct VoxelEdit(VoxelPos Pos, BlockId Block, ulong Author, string AuthorName)
{
    public bool IsRemoval => Block == BlockId.Air;
}

/// <summary>
/// Трёхмерная сетка данных. Плоский массив byte (по одному байту на воксель),
/// доступ по индексу X + Z*SizeX + Y*SizeX*SizeZ.
/// </summary>
public sealed class VoxelGrid
{
    private byte[] _cells;

    public VoxelGrid(int sizeX, int sizeY, int sizeZ)
    {
        ValidateDimensions(sizeX, sizeY, sizeZ);

        SizeX = sizeX;
        SizeY = sizeY;
        SizeZ = sizeZ;
        _cells = new byte[sizeX * sizeY * sizeZ];
    }

    /// <summary>Максимальный размер мира по каждой оси.</summary>
    public const int MaxDimension = 256;

    /// <summary>Максимум вокселей в мире — защита от аллокации гигабайтов по чужому пакету.</summary>
    public const long MaxCells = 8 * 1024 * 1024;

    private static void ValidateDimensions(int sizeX, int sizeY, int sizeZ)
    {
        if (sizeX < 2 || sizeY < 2 || sizeZ < 2 || sizeX > MaxDimension || sizeY > MaxDimension || sizeZ > MaxDimension)
            throw new ProtocolException($"Размеры мира {sizeX}x{sizeY}x{sizeZ} вне допустимого диапазона 2..{MaxDimension}.");

        if ((long)sizeX * sizeY * sizeZ > MaxCells)
            throw new ProtocolException($"Мир {sizeX}x{sizeY}x{sizeZ} слишком большой.");
    }

    public int SizeX { get; private set; }
    public int SizeY { get; private set; }
    public int SizeZ { get; private set; }

    /// <summary>Версия растёт при каждом изменении — удобно для кэша рендера.</summary>
    public int Version { get; private set; }

    public int CellCount => _cells.Length;

    public bool InBounds(int x, int y, int z) => x >= 0 && y >= 0 && z >= 0 && x < SizeX && y < SizeY && z < SizeZ;

    public bool InBounds(VoxelPos pos) => InBounds(pos.X, pos.Y, pos.Z);

    public int Index(int x, int y, int z) => x + (z * SizeX) + (y * SizeX * SizeZ);

    /// <summary>Чтение блока. За границами мира возвращает воздух (удобно для рендера/подсчётов).</summary>
    public BlockId Get(int x, int y, int z)
    {
        if (!InBounds(x, y, z)) return BlockId.Air;
        return (BlockId)_cells[Index(x, y, z)];
    }

    public BlockId Get(VoxelPos pos) => Get(pos.X, pos.Y, pos.Z);

    /// <summary>Запись блока. false — координаты вне мира или блок уже такой.</summary>
    public bool Set(int x, int y, int z, BlockId block)
    {
        if (!InBounds(x, y, z)) return false;
        int index = Index(x, y, z);
        if (_cells[index] == (byte)block) return false;
        _cells[index] = (byte)block;
        Version++;
        return true;
    }

    public bool Set(VoxelPos pos, BlockId block) => Set(pos.X, pos.Y, pos.Z, block);

    /// <summary>Пересоздать сетку другого размера (например, приехал снимок мира от хоста побольше).</summary>
    public void Resize(int sizeX, int sizeY, int sizeZ)
    {
        if (sizeX == SizeX && sizeY == SizeY && sizeZ == SizeZ) return;
        ValidateDimensions(sizeX, sizeY, sizeZ);
        SizeX = sizeX;
        SizeY = sizeY;
        SizeZ = sizeZ;
        _cells = new byte[sizeX * sizeY * sizeZ];
        Version++;
    }

    public void Clear()
    {
        Array.Clear(_cells);
        Version++;
    }

    /// <summary>Самый верхний непустой воксель в колонке (для рендера и спавна). -1 — колонка пуста.</summary>
    public int TopSolidY(int x, int z)
    {
        if (x < 0 || x >= SizeX || z < 0 || z >= SizeZ) return -1;
        for (int y = SizeY - 1; y >= 0; y--)
        {
            if (Get(x, y, z) != BlockId.Air) return y;
        }
        return -1;
    }

    public long Count(BlockId block)
    {
        byte target = (byte)block;
        long total = 0;
        foreach (byte cell in _cells)
        {
            if (cell == target) total++;
        }
        return total;
    }

    public long CountNonAir()
    {
        long total = 0;
        foreach (byte cell in _cells)
        {
            if (cell != (byte)BlockId.Air) total++;
        }
        return total;
    }

    /// <summary>Перебор всех непустых вокселей (снимок массива не нужен — только чтение).</summary>
    public IEnumerable<VoxelEdit> NonAir()
    {
        int sx = SizeX, sz = SizeZ;
        for (int y = 0; y < SizeY; y++)
        {
            for (int z = 0; z < sz; z++)
            {
                for (int x = 0; x < sx; x++)
                {
                    byte cell = _cells[x + (z * sx) + (y * sx * sz)];
                    if (cell == (byte)BlockId.Air) continue;
                    yield return new VoxelEdit(new VoxelPos(x, y, z), (BlockId)cell, 0ul, string.Empty);
                }
            }
        }
    }

    /// <summary>Центр мира — точка спавна по умолчанию.</summary>
    public VoxelPos Center => new(SizeX / 2, 0, SizeZ / 2);

    /// <summary>Копия сырых данных для сериализации.</summary>
    public byte[] ToBytes() => (byte[])_cells.Clone();

    /// <summary>Загрузить сырые данные (длина должна совпадать с размером сетки).</summary>
    public void FromBytes(byte[] raw)
    {
        ArgumentNullException.ThrowIfNull(raw);
        if (raw.Length != _cells.Length)
            throw new ProtocolException($"Размер снимка мира {raw.Length} не совпадает с сеткой {_cells.Length}.");

        Buffer.BlockCopy(raw, 0, _cells, 0, raw.Length);
        Version++;
    }

    public string Describe() =>
        $"мир {SizeX}x{SizeY}x{SizeZ}, заполнено {CountNonAir()} вокселей из {CellCount}";
}

/// <summary>
/// Сжатие сетки мира: пары [varint-количество][байт-блок].
/// Мир состоит из длинных серий воздуха и камня, поэтому на реальных аренах
/// снимок ужимается в десятки раз и уходит за один-два P2P-пакета.
/// </summary>
public static class VoxelCodec
{
    /// <summary>Защита от декомпрессионных бомб: максимум вокселей в принятом снимке.</summary>
    public const int MaxDecodedCells = 512 * 512 * 512;

    public static byte[] Encode(ReadOnlySpan<byte> raw)
    {
        var writer = new PacketWriter(raw.Length / 8 + 16);
        int index = 0;
        while (index < raw.Length)
        {
            byte value = raw[index];
            int run = 1;
            while (index + run < raw.Length && raw[index + run] == value && run < int.MaxValue) run++;
            writer.WriteVarUInt((uint)run);
            writer.WriteByte(value);
            index += run;
        }
        return writer.ToArray();
    }

    public static byte[] Decode(byte[] encoded, int expectedCells)
    {
        ArgumentNullException.ThrowIfNull(encoded);
        if (expectedCells <= 0 || expectedCells > MaxDecodedCells)
            throw new ProtocolException($"Некорректный размер мира: {expectedCells}.");

        var result = new byte[expectedCells];
        var reader = new PacketReader(encoded);
        int written = 0;

        while (!reader.AtEnd)
        {
            uint run = reader.ReadVarUInt();
            byte value = reader.ReadByte();
            if (run == 0) throw new ProtocolException("Нулевая длина серии в снимке мира.");
            if (written + run > (uint)expectedCells) throw new ProtocolException("Снимок мира больше заявленного размера.");

            Array.Fill(result, value, written, (int)run);
            written += (int)run;
        }

        if (written != expectedCells)
            throw new ProtocolException($"Снимок мира неполный: {written} из {expectedCells} вокселей.");

        return result;
    }
}

/// <summary>Упаковка списка правок (команда fill, пресеты) в один буфер.</summary>
public static class EditBatchCodec
{
    public static byte[] Encode(IReadOnlyList<VoxelEdit> edits)
    {
        var writer = new PacketWriter(edits.Count * 5 + 8);
        writer.WriteVarUInt((uint)edits.Count);
        foreach (var edit in edits)
        {
            writer.WriteVarInt(edit.Pos.X);
            writer.WriteVarInt(edit.Pos.Y);
            writer.WriteVarInt(edit.Pos.Z);
            writer.WriteByte((byte)edit.Block);
        }
        return writer.ToArray();
    }

    public static List<VoxelEdit> Decode(byte[] payload, ulong author, string authorName)
    {
        ArgumentNullException.ThrowIfNull(payload);
        var reader = new PacketReader(payload);
        uint count = reader.ReadVarUInt();
        // Каждая правка занимает минимум 4 байта, поэтому больше, чем позволяет буфер, быть не может.
        if (count * 4 > payload.Length + 4) throw new ProtocolException("Некорректное число правок в пакете.");
        if (count > 100_000) throw new ProtocolException($"Слишком большой пакет правок: {count}.");

        var edits = new List<VoxelEdit>((int)count);
        for (uint i = 0; i < count; i++)
        {
            int x = reader.ReadVarInt();
            int y = reader.ReadVarInt();
            int z = reader.ReadVarInt();
            byte block = reader.ReadByte();
            if (!Blocks.IsDefined(block)) throw new ProtocolException($"Неизвестный блок в пакете правок: {block}.");
            edits.Add(new VoxelEdit(new VoxelPos(x, y, z), (BlockId)block, author, authorName));
        }
        return edits;
    }
}

/// <summary>
/// Стартовая арена: пол, бортик, четыре светильника и стол казино в центре.
/// Никаких ценных блоков — чтобы никто не мог "сломать" бесплатное золото.
/// </summary>
public static class VoxelWorldPresets
{
    public static List<VoxelEdit> BuildStarterArena(VoxelGrid grid)
    {
        ArgumentNullException.ThrowIfNull(grid);
        ulong host = 0;
        var edits = new List<VoxelEdit>();
        int sx = grid.SizeX, sz = grid.SizeZ;
        int floorGrassY = Math.Min(1, grid.SizeY - 1);
        int wallTop = Math.Min(3, grid.SizeY - 1);

        void Place(int x, int y, int z, BlockId block)
        {
            if (grid.Set(x, y, z, block))
                edits.Add(new VoxelEdit(new VoxelPos(x, y, z), block, host, string.Empty));
        }

        for (int x = 0; x < sx; x++)
        {
            for (int z = 0; z < sz; z++)
            {
                bool border = x == 0 || z == 0 || x == sx - 1 || z == sz - 1;
                Place(x, 0, z, BlockId.Stone);
                if (border)
                {
                    for (int y = 1; y <= wallTop; y++) Place(x, y, z, BlockId.Wood);
                }
                else
                {
                    Place(x, floorGrassY, z, (x + z) % 3 == 0 ? BlockId.Dirt : BlockId.Grass);
                }
            }
        }

        // Стол казино в центре — с него начинается вся азартная часть.
        int cx = sx / 2, cz = sz / 2;
        Place(cx, floorGrassY + 1, cz, BlockId.CasinoTable);

        // Четыре светильника по углам внутренней площадки.
        int inset = Math.Max(2, Math.Min(sx, sz) / 6);
        foreach (var (lx, lz) in new[] { (inset, inset), (sx - 1 - inset, inset), (inset, sz - 1 - inset), (sx - 1 - inset, sz - 1 - inset) })
        {
            if (lx <= 0 || lz <= 0 || lx >= sx - 1 || lz >= sz - 1) continue;
            Place(lx, floorGrassY + 1, lz, BlockId.Lamp);
        }

        return edits;
    }
}
