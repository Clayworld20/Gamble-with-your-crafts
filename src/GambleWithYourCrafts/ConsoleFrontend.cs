using System.Globalization;
using System.Text;

namespace GambleWithYourCrafts;

/// <summary>
/// Вывод в консоль: цвета (ANSI), баннер, приглушение для перенаправленного вывода.
/// </summary>
public static class Terminal
{
    private const string Esc = "\u001b[";

    /// <summary>Цвета включены? Автоматически выключаются при перенаправлении вывода и по --no-color.</summary>
    public static bool ColorEnabled { get; private set; } = true;

    public static void Initialize(bool noColor, bool verbose)
    {
        bool redirected = false;
        try
        {
            redirected = Console.IsOutputRedirected;
        }
        catch (IOException)
        {
            redirected = true;
        }

        bool envNoColor = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("NO_COLOR"));
        ColorEnabled = !noColor && !redirected && !envNoColor;

        // На Windows консоль по умолчанию в OEM-кодировке: без этого русский текст превращается в кракозябры.
        try
        {
            if (!Console.IsOutputRedirected && Console.OutputEncoding.CodePage != Encoding.UTF8.CodePage)
            {
                Console.OutputEncoding = Encoding.UTF8;
            }
        }
        catch (Exception ex) when (ex is IOException or PlatformNotSupportedException)
        {
            // Не критично: продолжаем с кодировкой по умолчанию.
        }

        if (verbose) Log.Verbose = true;

        Log.Sink = (text, color) => WriteLine($"{Timestamp()} {text}", color);
    }

    public static string Timestamp() => DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);

    /// <summary>Код ANSI для цвета консоли.</summary>
    private static int AnsiCode(ConsoleColor color) => color switch
    {
        ConsoleColor.Black => 30,
        ConsoleColor.DarkRed => 31,
        ConsoleColor.DarkGreen => 32,
        ConsoleColor.DarkYellow => 33,
        ConsoleColor.DarkBlue => 34,
        ConsoleColor.DarkMagenta => 35,
        ConsoleColor.DarkCyan => 36,
        ConsoleColor.Gray => 37,
        ConsoleColor.DarkGray => 90,
        ConsoleColor.Red => 91,
        ConsoleColor.Green => 92,
        ConsoleColor.Yellow => 93,
        ConsoleColor.Blue => 94,
        ConsoleColor.Magenta => 95,
        ConsoleColor.Cyan => 96,
        ConsoleColor.White => 97,
        _ => 39,
    };

    /// <summary>Обернуть текст ANSI-цветом (или вернуть как есть, если цвета выключены).</summary>
    public static string Colorize(string text, ConsoleColor color) =>
        ColorEnabled ? $"{Esc}{(int)AnsiCode(color)}m{text}{Esc}0m" : text;

    public static string Bold(string text) => ColorEnabled ? $"{Esc}1m{text}{Esc}22m" : text;

    public static string Dim(string text) => ColorEnabled ? $"{Esc}2m{text}{Esc}22m" : text;

    public static void Write(string text) => Console.Write(text);

    public static void WriteLine(string text = "", ConsoleColor color = ConsoleColor.Gray)
    {
        if (ColorEnabled && color != ConsoleColor.Gray) Console.WriteLine(Colorize(text, color));
        else Console.WriteLine(text);
    }

    /// <summary>Цветной глиф блока (для карты и изометрии).</summary>
    public static string BlockGlyph(BlockId block)
    {
        if (block == BlockId.Air) return Colorize("·", ConsoleColor.DarkGray);
        BlockInfo info = Blocks.Info(block);
        return Colorize(info.Glyph.ToString(), info.Color);
    }

    /// <summary>Расшифровка блоков под картой.</summary>
    public static string Legend()
    {
        var sb = new StringBuilder();
        foreach (BlockInfo block in Blocks.Placeable)
        {
            sb.Append(Colorize(block.Glyph.ToString(), block.Color)).Append('=').Append(block.Key).Append("  ");
        }
        sb.Append(Colorize("·", ConsoleColor.DarkGray)).Append("=air");
        return sb.ToString();
    }

    public static void Banner(string subtitle)
    {
        const string art = """
           ____                  _     __        __         _   _                 ____            _       __
          / ___| __ _ _ __ ___  | |__ | |_   _  \ \      __| |_   _  ___ _ __    / ___|_ __ __ _ / _| ___| |_
         | |  _ / _` | '_ ` _ \ | '_ \| | | |  \ \ /\ / /| | | |/ _ \ '__|  | |   | '__/ _` | |_/ __| __|
         | |_| | (_| | | | | | || |_) | | |_| |  \ V  V / | |_| |  __/ |     | |___| | | (_| |  _\__ \ |_
          \____|\__,_|_| |_| |_||_.__/|_|\__, |   \_/\_/   \__,_|\___|_|      \____|_|  \__,_|_| |___/\__|
                                         |___/
        """;

        WriteLine(art, ConsoleColor.DarkYellow);
        WriteLine($"  {BuildInfo.Product} v{BuildInfo.Version} — {subtitle}", ConsoleColor.Yellow);
        WriteLine($"  Протокол P2P v{NetMessage.ProtocolVersion}, транспорт: " +
                  (AppCore.SteamReady ? $"Steam P2P (AppID {AppCore.AppId})" : "локальный режим"), ConsoleColor.DarkGray);
        WriteLine();
    }
}

/// <summary>Текстовый рендер воксельного мира: срез по высоте и изометрия.</summary>
public static class Renderer
{
    /// <summary>Срез мира на высоте y (вид сверху), с координатной линейкой.</summary>
    public static string Slice(VoxelGrid grid, int y, int radius = -1, int? centerX = null, int? centerZ = null)
    {
        ArgumentNullException.ThrowIfNull(grid);
        y = Math.Clamp(y, 0, grid.SizeY - 1);

        int window = radius > 0 ? Math.Min(radius, 48) : Math.Min(grid.SizeX, 48);
        int centerXValue = centerX ?? grid.SizeX / 2;
        int centerZValue = centerZ ?? grid.SizeZ / 2;

        int x0 = Math.Max(0, centerXValue - window);
        int x1 = Math.Min(grid.SizeX - 1, centerXValue + window);
        int z0 = Math.Max(0, centerZValue - window);
        int z1 = Math.Min(grid.SizeZ - 1, centerZValue + window);

        var sb = new StringBuilder();
        sb.AppendLine($"Срез мира y={y} (x {x0}..{x1}, z {z0}..{z1}). «·» — пусто.");

        // Линейка по X: десятки и единицы.
        sb.Append("      ");
        for (int x = x0; x <= x1; x++) sb.Append(x % 10 == 0 ? (x / 10 % 10).ToString(CultureInfo.InvariantCulture) : " ");
        sb.AppendLine();
        sb.Append("      ");
        for (int x = x0; x <= x1; x++) sb.Append((x % 10).ToString(CultureInfo.InvariantCulture));
        sb.AppendLine();

        for (int z = z0; z <= z1; z++)
        {
            sb.Append(z.ToString(CultureInfo.InvariantCulture).PadLeft(4)).Append("  ");
            for (int x = x0; x <= x1; x++)
            {
                sb.Append(Terminal.BlockGlyph(grid.Get(x, y, z)));
            }
            sb.AppendLine();
        }

        sb.AppendLine();
        sb.AppendLine(Terminal.Legend());
        sb.Append($"Вокселей в мире: {grid.CountNonAir()} из {grid.CellCount}.");
        return sb.ToString();
    }

    /// <summary>
    /// Изометрическая ASCII-проекция: каждый столбец мира показывает свой верхний блок.
    /// Дальние столбцы рисуются первыми, ближние перекрывают их — простой painter's algorithm.
    /// </summary>
    public static string Isometric(VoxelGrid grid, VoxelPos center, int radius = 12)
    {
        ArgumentNullException.ThrowIfNull(grid);
        radius = Math.Clamp(radius, 2, 24);

        int x0 = Math.Max(0, center.X - radius);
        int x1 = Math.Min(grid.SizeX - 1, center.X + radius);
        int z0 = Math.Max(0, center.Z - radius);
        int z1 = Math.Min(grid.SizeZ - 1, center.Z + radius);

        int width = (x1 - x0) + (z1 - z0) + 3;
        int height = (x1 - x0 + z1 - z0) / 2 + grid.SizeY + 4;

        if (width > 220 || height > 200)
        {
            return "Слишком большой регион для изометрии — уменьшите radius (максимум 24).";
        }

        var buffer = new string[height, width];
        for (int row = 0; row < height; row++)
            for (int col = 0; col < width; col++)
                buffer[row, col] = " ";

        int offsetX = z1 - z0 + 1;
        int offsetY = grid.SizeY;

        // painter's algorithm: сначала дальние (x+z мало), потом ближние.
        for (int depth = x0 + z0; depth <= x1 + z1; depth++)
        {
            for (int x = x0; x <= x1; x++)
            {
                int z = depth - x;
                if (z < z0 || z > z1) continue;

                int top = grid.TopSolidY(x, z);
                if (top < 0) continue;

                BlockId block = grid.Get(x, top, z);
                if (block == BlockId.Air) continue;

                int isoX = (x - x0) - (z - z0) + offsetX;
                int isoY = ((x - x0) + (z - z0)) / 2 - top + offsetY;
                if (isoX < 0 || isoX >= width || isoY < 0 || isoY >= height) continue;

                buffer[isoY, isoX] = Terminal.BlockGlyph(block);

                // Толщина: показываем блок под верхним, чтобы постройки не выглядели плоскими.
                int below = top - 1;
                if (below >= 0 && isoY + 1 < height && buffer[isoY + 1, isoX] == " ")
                {
                    BlockId under = grid.Get(x, below, z);
                    if (under != BlockId.Air) buffer[isoY + 1, isoX] = Terminal.BlockGlyph(under);
                }
            }
        }

        var sb = new StringBuilder();
        sb.AppendLine($"Изометрия вокруг {center}, радиус {radius} (вид сверху-сбоку). Смотрите на стол казино — «@».");
        int firstRow = height, lastRow = -1;
        for (int row = 0; row < height; row++)
        {
            bool empty = true;
            for (int col = 0; col < width; col++)
            {
                if (buffer[row, col] != " ") { empty = false; break; }
            }
            if (empty) continue;
            firstRow = Math.Min(firstRow, row);
            lastRow = Math.Max(lastRow, row);
        }

        if (lastRow < 0)
        {
            sb.AppendLine("В этом регионе пусто — постройте что-нибудь (place) или подвиньте центр.");
            return sb.ToString();
        }

        for (int row = firstRow; row <= lastRow; row++)
        {
            var line = new StringBuilder(width);
            int lastCol = width - 1;
            while (lastCol >= 0 && buffer[row, lastCol] == " ") lastCol--;
            for (int col = 0; col <= lastCol; col++) line.Append(buffer[row, col]);
            sb.AppendLine(line.ToString());
        }

        sb.AppendLine(Terminal.Legend());
        return sb.ToString();
    }

    /// <summary>Компактная индикация инвентаря для HUD.</summary>
    public static string InventoryLine(Inventory inventory)
    {
        ArgumentNullException.ThrowIfNull(inventory);
        var sb = new StringBuilder();
        foreach (var pair in inventory.Snapshot())
        {
            string glyph = Blocks.TryParse(pair.Key, out BlockId id)
                ? Terminal.Colorize(Blocks.Info(id).Glyph.ToString(), Blocks.Info(id).Color)
                : pair.Key;
            sb.Append(glyph).Append('x').Append(pair.Value.ToString(CultureInfo.InvariantCulture)).Append(' ');
        }
        return sb.Length == 0 ? Terminal.Dim("(пусто)") : sb.ToString().TrimEnd();
    }
}

/// <summary>Разбор строки команды с поддержкой кавычек: say "привет всем".</summary>
public static class CommandLine
{
    public static List<string> Split(string line)
    {
        var result = new List<string>();
        if (string.IsNullOrWhiteSpace(line)) return result;

        var current = new StringBuilder();
        bool inQuotes = false;

        foreach (char ch in line)
        {
            if (ch == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }

            if (!inQuotes && char.IsWhiteSpace(ch))
            {
                if (current.Length > 0)
                {
                    result.Add(current.ToString());
                    current.Clear();
                }
                continue;
            }

            current.Append(ch);
        }

        if (current.Length > 0) result.Add(current.ToString());
        return result;
    }
}
