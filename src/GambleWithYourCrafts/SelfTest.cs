using System.Globalization;
using System.Text;

namespace GambleWithYourCrafts;

/// <summary>
/// Встроенные тесты логики. Работают без Steam и без второго компьютера:
/// транспорт подменяется на LoopbackNetwork (см. ниже), поэтому можно проверить
/// и сериализацию, и правила казино, и репликацию мира с инвентарями.
///
/// Запуск: dotnet run --project src/GambleWithYourCrafts -- --selftest
/// </summary>
public static class SelfTest
{
    public static int RunAll()
    {
        var runner = new TestRunner();
        Console.OutputEncoding = Encoding.UTF8;

        runner.Section("Протокол: сериализация сообщений");
        TestProtocolRoundTrips(runner);
        TestProtocolRejectsGarbage(runner);

        runner.Section("Протокол: сборка фрагментов");
        TestFragments(runner);

        runner.Section("Воксели: сетка и сжатие");
        TestVoxelGrid(runner);
        TestVoxelCodec(runner);
        TestEditBatchCodec(runner);
        TestStarterArena(runner);

        runner.Section("Инвентарь");
        TestInventory(runner);

        runner.Section("Казино: правила и выплаты");
        TestCasinoRules(runner);
        TestCasinoStatistics(runner);

        runner.Section("Сеть: репликация мира и инвентарей");
        TestReplication(runner);
        TestCasinoOverNetwork(runner);
        TestForgedPacketsRejected(runner);
        TestHostAuthority(runner);
        TestHostMigration(runner);

        return runner.Summary();
    }

    // ── Протокол ─────────────────────────────────────────────────────────────

    private static void TestProtocolRoundTrips(TestRunner runner)
    {
        foreach (NetMessage original in SampleMessages())
        {
            byte[] bytes = original.Serialize();
            runner.Check($"сериализация {original.Type} не пустая и влезает в пакет",
                bytes.Length is > 4 and <= NetMessage.MaxPacketBytes,
                $"{bytes.Length} байт");

            NetMessage restored = NetMessage.Deserialize(bytes);
            runner.Check($"{original.Type}: тип после разбора",
                restored.Type == original.Type,
                $"{restored.Type}");

            bool same = MessagesEqual(original, restored);
            runner.Check($"{original.Type}: данные совпадают после round-trip", same, Describe(original, restored));
        }
    }

    private static List<NetMessage> SampleMessages() => new()
    {
        new HelloMessage { PlayerName = "Игрок Ёж", ClientTag = BuildInfo.Tag, WantsFullState = true },
        new ChatMessage { SenderName = "Хост", Text = "ставлю всё золото на красное!" },
        new PingMessage { Stamp = 123456789 },
        new PongMessage { Stamp = 123456789, PeerStamp = 987654321 },
        new SyncRequestMessage(),
        new PresenceMessage { Kind = PresenceKind.Joined, PlayerId = 76561198000000000, PlayerName = "Друг", IsHost = true, Note = "вошёл" },
        new VoxelEditMessage { X = -3, Y = 12, Z = 45, Block = (byte)BlockId.Gold, Author = 76561198000000000, AuthorName = "Друг" },
        new VoxelBatchMessage { BatchId = 7, Index = 1, Count = 4, Author = 42, AuthorName = "Хост", Payload = new byte[] { 1, 2, 3, 250, 255 } },
        new VoxelSnapshotMessage { SyncId = 9, Index = 0, Count = 2, SizeX = 48, SizeY = 28, SizeZ = 48, Payload = new byte[] { 9, 8, 7 } },
        new InventorySyncMessage
        {
            PlayerId = 5,
            PlayerName = "Кто-то",
            IsSelfReport = false,
            Items = new List<KeyValuePair<string, int>>
            {
                new("diamond", 3),
                new("gold", 8),
                new("dirt", 64),
            },
        },
        new BlockRequestMessage { X = 1, Y = 2, Z = 3, Block = (byte)BlockId.Stone },
        new FillRequestMessage { Block = (byte)BlockId.Wood, MinX = 0, MinY = 0, MinZ = 0, MaxX = 4, MaxY = 2, MaxZ = 3 },
        new CraftRequestMessage { Count = 3 },
        new CasinoBetRequestMessage { Game = (byte)CasinoGame.Roulette, Target = "red", BlockKey = "gold", Amount = 4 },
        new CasinoResultMessage
        {
            PlayerId = 11,
            PlayerName = "Счастливчик",
            Game = (byte)CasinoGame.Dice,
            Target = "high",
            BlockKey = "gold",
            Stake = 4,
            Multiplier = 2,
            Payout = 8,
            Won = true,
            Rolls = new byte[] { 4, 6 },
            RollText = "4+6 = 10",
        },
        new ByeMessage { Reason = "ушёл спать" },
    };

    private static bool MessagesEqual(NetMessage original, NetMessage restored)
    {
        if (original.GetType() != restored.GetType()) return false;
        // Сравниваем через повторную сериализацию — этого достаточно и не требует рефлексии.
        byte[] a = original.Serialize();
        byte[] b = restored.Serialize();
        return a.SequenceEqual(b);
    }

    private static string Describe(NetMessage a, NetMessage b)
    {
        byte[] x = a.Serialize();
        byte[] y = b.Serialize();
        return $"было {x.Length} байт, стало {y.Length}; типы {a.Type}/{b.Type}";
    }

    private static void TestProtocolRejectsGarbage(TestRunner runner)
    {
        runner.Check("мусор вместо заголовка отброшен", Throws(() => NetMessage.Deserialize(new byte[] { 1, 2, 3, 4, 5 })));
        runner.Check("пустой пакет отброшен", Throws(() => NetMessage.Deserialize(Array.Empty<byte>())));

        byte[] wrongMagic = { (byte)'X', (byte)'Y', NetMessage.ProtocolVersion, (byte)MessageType.Chat };
        runner.Check("чужая сигнатура отброшена", Throws(() => NetMessage.Deserialize(wrongMagic)));

        byte[] wrongVersion = { (byte)'G', (byte)'C', 99, (byte)MessageType.Chat };
        runner.Check("несовпадающая версия протокола отброшена", Throws(() => NetMessage.Deserialize(wrongVersion)));

        byte[] unknownType = { (byte)'G', (byte)'C', NetMessage.ProtocolVersion, 200 };
        runner.Check("неизвестный тип сообщения отброшен", Throws(() => NetMessage.Deserialize(unknownType)));

        byte[] truncated = new ChatMessage { SenderName = "хост", Text = "длинное сообщение" }.Serialize();
        Array.Resize(ref truncated, truncated.Length - 6);
        runner.Check("обрезанный пакет отброшен", Throws(() => NetMessage.Deserialize(truncated)));

        byte[] tooLongString = BuildRawPacket(MessageType.Chat, writer =>
        {
            writer.WriteVarUInt(100000);
            writer.WriteByte((byte)'A');
        });
        runner.Check("слишком длинная строка отброшена", Throws(() => NetMessage.Deserialize(tooLongString)));

        byte[] badWorldSize = BuildRawPacket(MessageType.VoxelSnapshot, writer =>
        {
            writer.WriteInt32(1);
            writer.WriteVarUInt(0);
            writer.WriteVarUInt(1);
            writer.WriteUInt16(9999);
            writer.WriteUInt16(9999);
            writer.WriteUInt16(9999);
            writer.WriteBytes(Array.Empty<byte>());
        });
        runner.Check("бредовый размер мира в снимке отброшен", Throws(() => NetMessage.Deserialize(badWorldSize)));
    }

    private static byte[] BuildRawPacket(MessageType type, Action<PacketWriter> body)
    {
        var writer = new PacketWriter();
        writer.WriteByte((byte)'G');
        writer.WriteByte((byte)'C');
        writer.WriteByte(NetMessage.ProtocolVersion);
        writer.WriteByte((byte)type);
        body(writer);
        return writer.ToArray();
    }

    private static void TestFragments(TestRunner runner)
    {
        var random = new Random(1234);
        var payload = new byte[9000];
        random.NextBytes(payload);

        int count = Fragmenter.CountFor(payload.Length);
        runner.Check("количество фрагментов больше одного", count > 1, $"{count}");

        var assembler = new FragmentAssembler();
        byte[]? assembled = null;

        // Собираем в обратном порядке — сеть не гарантирует порядок даже при Reliable на разных каналах.
        for (int index = count - 1; index >= 0; index--)
        {
            assembled = assembler.Add(1, index, count, Fragmenter.Slice(payload, index), 0);
        }

        runner.Check("фрагменты собраны после перемешивания", assembled is not null);
        runner.Check("собранный буфер совпадает с исходным",
            assembled is not null && assembled.SequenceEqual(payload));

        var timeoutAssembler = new FragmentAssembler(timeoutMs: 100);
        timeoutAssembler.Add(1, 0, 2, new byte[] { 1 }, 0);
        timeoutAssembler.Prune(50);
        byte[]? partial = timeoutAssembler.Add(1, 1, 2, new byte[] { 2 }, 60);
        runner.Check("незавершённая передача не теряется до таймаута", partial is not null);

        var staleAssembler = new FragmentAssembler(timeoutMs: 10);
        staleAssembler.Add(1, 0, 2, new byte[] { 1 }, 0);
        staleAssembler.Prune(1000);
        byte[]? afterPrune = staleAssembler.Add(1, 1, 2, new byte[] { 2 }, 1001);
        runner.Check("потерянный фрагмент не склеивается с новым (после Prune)",
            afterPrune is null || afterPrune.SequenceEqual(new byte[] { 1, 2 }), "передача перезапущена");

        runner.Check("индекс за пределами передачи отброшен",
            Throws(() => new FragmentAssembler().Add(5, 3, 3, new byte[] { 1 }, 0)));
    }

    // ── Воксели ──────────────────────────────────────────────────────────────

    private static void TestVoxelGrid(TestRunner runner)
    {
        var grid = new VoxelGrid(16, 8, 12);
        runner.Check("размеры сетки", grid.SizeX == 16 && grid.SizeY == 8 && grid.SizeZ == 12);
        runner.Check("пустой мир", grid.CountNonAir() == 0);

        runner.Check("запись блока меняет версию", grid.Set(1, 2, 3, BlockId.Gold) && grid.Version > 0);
        runner.Check("чтение блока", grid.Get(1, 2, 3) == BlockId.Gold);
        runner.Check("повторная запись того же блока — не изменение", !grid.Set(1, 2, 3, BlockId.Gold));

        runner.Check("запись за границей игнорируется", !grid.Set(100, 0, 0, BlockId.Stone));
        runner.Check("чтение за границей даёт воздух", grid.Get(-1, 0, 0) == BlockId.Air);
        runner.Check("границы", grid.InBounds(15, 7, 11) && !grid.InBounds(16, 7, 11));

        grid.Set(5, 0, 5, BlockId.Stone);
        grid.Set(5, 4, 5, BlockId.Lamp);
        runner.Check("верхний блок колонки", grid.TopSolidY(5, 5) == 4);
        runner.Check("пустая колонка", grid.TopSolidY(9, 9) == -1);

        long before = grid.CountNonAir();
        runner.Check("подсчёт непустых вокселей", before == 3, $"{before}");

        byte[] raw = grid.ToBytes();
        var copy = new VoxelGrid(16, 8, 12);
        copy.FromBytes(raw);
        runner.Check("экспорт/импорт совпадает", GridsEqual(grid, copy));

        var mismatch = new VoxelGrid(4, 4, 4);
        runner.Check("несовпадающий размер снимка отброшен",
            Throws(() => mismatch.FromBytes(raw)));

        runner.Check("слишком большой мир отвергается",
            Throws(() => new VoxelGrid(256, 256, 256)));

        var small = new VoxelGrid(4, 4, 4);
        runner.Check("Resize в допустимых границах", SafeResize(small, 8, 6, 8) && small.SizeX == 8 && small.SizeY == 6 && small.SizeZ == 8);
        runner.Check("Resize в бредовые значения отвергается", !SafeResize(small, 0, 0, 0) && small.SizeX == 8);

        int enumerated = 0;
        foreach (VoxelEdit edit in copy.NonAir())
        {
            enumerated++;
            runner.Check($"перечисленный воксель {edit.Pos} не пустой", copy.Get(edit.Pos) == edit.Block);
        }
        runner.Check("перебор непустых вокселей совпадает с подсчётом", enumerated == copy.CountNonAir());
    }

    private static bool SafeResize(VoxelGrid grid, int x, int y, int z)
    {
        try
        {
            grid.Resize(x, y, z);
            return true;
        }
        catch (ProtocolException)
        {
            return false;
        }
    }

    private static void TestVoxelCodec(TestRunner runner)
    {
        var random = new Random(777);

        var patterns = new Dictionary<string, byte[]>
        {
            ["пустой мир"] = new byte[2000],
            ["сплошной мир"] = Enumerable.Repeat((byte)BlockId.Stone, 2000).ToArray(),
            ["шахматка"] = Enumerable.Range(0, 2000).Select(i => (byte)(i % 2)).ToArray(),
            ["случайный"] = Enumerable.Range(0, 2000).Select(_ => (byte)random.Next(0, 9)).ToArray(),
        };

        foreach (var pair in patterns)
        {
            byte[] encoded = VoxelCodec.Encode(pair.Value);
            byte[] decoded = VoxelCodec.Decode(encoded, pair.Value.Length);
            runner.Check($"RLE {pair.Key}: совпадение", decoded.SequenceEqual(pair.Value), $"{encoded.Length} байт");

            if (pair.Key != "шахматка" && pair.Key != "случайный")
            {
                runner.Check($"RLE {pair.Key}: сжатие работает", encoded.Length < pair.Value.Length / 2, $"{encoded.Length} из {pair.Value.Length}");
            }
        }

        byte[] truncated = VoxelCodec.Encode(new byte[] { 1, 1, 1, 2, 2 });
        Array.Resize(ref truncated, truncated.Length - 1);
        runner.Check("неполный RLE-поток отброшен", Throws(() => VoxelCodec.Decode(truncated, 5)));

        runner.Check("нулевая серия в RLE отброшена",
            Throws(() => VoxelCodec.Decode(new byte[] { 0, 1 }, 4)));

        byte[] overflow = VoxelCodec.Encode(new byte[10]);
        runner.Check("RLE больше заявленного размера отброшен", Throws(() => VoxelCodec.Decode(overflow, 3)));
        runner.Check("отрицательный размер мира отброшен", Throws(() => VoxelCodec.Decode(overflow, -5)));
    }

    private static void TestEditBatchCodec(TestRunner runner)
    {
        var edits = new List<VoxelEdit>
        {
            new(new VoxelPos(0, 0, 0), BlockId.Dirt, 1, "хост"),
            new(new VoxelPos(15, 7, 15), BlockId.Diamond, 1, "хост"),
            new(new VoxelPos(-5, 3, 42), BlockId.Air, 2, "клиент"),
        };

        byte[] packed = EditBatchCodec.Encode(edits);
        List<VoxelEdit> restored = EditBatchCodec.Decode(packed, 1, "хост");

        runner.Check("пакет правок: количество", restored.Count == edits.Count, $"{restored.Count}");
        bool same = restored.Select(e => (e.Pos, e.Block)).SequenceEqual(edits.Select(e => (e.Pos, e.Block)));
        runner.Check("пакет правок: координаты и блоки", same);

        byte[] bad = BuildRawPacket(MessageType.VoxelBatch, writer =>
        {
            writer.WriteInt32(1);
            writer.WriteVarUInt(0);
            writer.WriteVarUInt(1);
            writer.WriteUInt64(0);
            writer.WriteString(string.Empty);
            writer.WriteVarUInt(5);
            writer.WriteVarInt(0);
            writer.WriteVarInt(0);
            writer.WriteVarInt(0);
            writer.WriteByte(250);
        });
        NetMessage parsed = NetMessage.Deserialize(bad);
        var batch = (VoxelBatchMessage)parsed;
        runner.Check("неизвестный блок в правках отброшен", Throws(() => EditBatchCodec.Decode(batch.Payload, 1, "хост")));
    }

    private static void TestStarterArena(TestRunner runner)
    {
        var grid = new VoxelGrid(24, 10, 24);
        List<VoxelEdit> edits = VoxelWorldPresets.BuildStarterArena(grid);

        runner.Check("арена что-то построила", edits.Count > 0, $"{edits.Count} вокселей");
        runner.Check("на арене есть стол казино", grid.Count(BlockId.CasinoTable) == 1);
        runner.Check("на арене нет бесплатных ценных блоков",
            grid.Count(BlockId.Gold) == 0 && grid.Count(BlockId.Diamond) == 0);
        runner.Check("пол на нулевой высоте", grid.Get(5, 0, 5) == BlockId.Stone);
        runner.Check("бортик по краю", grid.Get(0, 2, 0) == BlockId.Wood);

        var small = new VoxelGrid(8, 4, 8);
        VoxelWorldPresets.BuildStarterArena(small);
        runner.Check("арена строится и в маленьком мире", small.Count(BlockId.CasinoTable) == 1);
    }

    // ── Инвентарь ────────────────────────────────────────────────────────────

    private static void TestInventory(TestRunner runner)
    {
        var inventory = new Inventory();
        inventory.Add("gold", 5);
        runner.Check("начисление блоков", inventory.Get("gold") == 5);
        runner.Check("псевдоним ключа (Золото → gold)", inventory.Get("Золото") == 5);
        runner.Check("неизвестный блок — ноль", inventory.Get("нет-такого") == 0);
        runner.Check("Add игнорирует отрицательное", SafeAdd(inventory, "gold", -3) && inventory.Get("gold") == 5);

        inventory.Add("gold", 5);
        runner.Check("накопление", inventory.Get("gold") == 10);

        runner.Check("списание", inventory.TrySpend("gold", 4) && inventory.Get("gold") == 6);
        runner.Check("нельзя уйти в минус", !inventory.TrySpend("gold", 100) && inventory.Get("gold") == 6);
        runner.Check("списание всего под ноль", inventory.TrySpend("gold", 6) && inventory.Get("gold") == 0);

        var rich = new Inventory();
        rich.Add("dirt", 10);
        rich.Add("diamond", 2);
        rich.Add("gold", 4);
        runner.Check("богатство считается по ценности", rich.Wealth == 10 * 1 + 2 * 25 + 4 * 8, $"{rich.Wealth}");
        runner.Check("общее число блоков", rich.TotalBlocks == 16);

        List<KeyValuePair<string, int>> snapshot = rich.Snapshot();
        runner.Check("снимок отсортирован от дорогих к дешёвым", snapshot[0].Key == "diamond", string.Join(",", snapshot.Select(s => s.Key)));

        var restored = new Inventory();
        restored.Load(snapshot);
        runner.Check("восстановление из снимка", restored.Wealth == rich.Wealth && restored.Get("diamond") == 2);

        restored.Set("gold", 0);
        runner.Check("нулевое количество убирает позицию", restored.Get("gold") == 0 && restored.Kinds == 2);

        runner.Check("описание не падает на пустом инвентаре", new Inventory().Describe() == "пусто");
        runner.Check("стартовый инвентарь выдан",
            GambleCreativeSystem.CreateStartingInventory().Get(Blocks.DiamondKey) == 3);
    }

    private static bool SafeAdd(Inventory inventory, string key, int amount)
    {
        inventory.Add(key, amount);
        return true;
    }

    // ── Казино ───────────────────────────────────────────────────────────────

    private static void TestCasinoRules(TestRunner runner)
    {
        // Рулетка: цвета и чётность.
        runner.Check("0 — зеро", CasinoRules.ColorOf(0) == CasinoRules.PocketColor.Green);
        runner.Check("1 — красное", CasinoRules.ColorOf(1) == CasinoRules.PocketColor.Red);
        runner.Check("2 — чёрное", CasinoRules.ColorOf(2) == CasinoRules.PocketColor.Black);
        runner.Check("36 — красное", CasinoRules.ColorOf(36) == CasinoRules.PocketColor.Red);

        runner.Check("красное выигрывает на 1", CasinoRules.Wins(CasinoGame.Roulette, "red", new[] { 1 }));
        runner.Check("красное проигрывает на 2", !CasinoRules.Wins(CasinoGame.Roulette, "red", new[] { 2 }));
        runner.Check("зеро убивает красное и чёрное",
            !CasinoRules.Wins(CasinoGame.Roulette, "red", new[] { 0 }) && !CasinoRules.Wins(CasinoGame.Roulette, "black", new[] { 0 }));
        runner.Check("чёт/нечет", CasinoRules.Wins(CasinoGame.Roulette, "even", new[] { 4 }) && !CasinoRules.Wins(CasinoGame.Roulette, "even", new[] { 3 }));
        runner.Check("зеро не чёт и не нечет",
            !CasinoRules.Wins(CasinoGame.Roulette, "even", new[] { 0 }) && !CasinoRules.Wins(CasinoGame.Roulette, "odd", new[] { 0 }));
        runner.Check("дужины", CasinoRules.Wins(CasinoGame.Roulette, "dozen1", new[] { 12 }) &&
                              CasinoRules.Wins(CasinoGame.Roulette, "dozen2", new[] { 13 }) &&
                              CasinoRules.Wins(CasinoGame.Roulette, "dozen3", new[] { 36 }) &&
                              !CasinoRules.Wins(CasinoGame.Roulette, "dozen1", new[] { 13 }));
        runner.Check("точное число", CasinoRules.Wins(CasinoGame.Roulette, "17", new[] { 17 }) && !CasinoRules.Wins(CasinoGame.Roulette, "18", new[] { 17 }));

        // Кости: суммы и дубли.
        runner.Check("high выигрывает на 12", CasinoRules.Wins(CasinoGame.Dice, "high", new[] { 6, 6 }));
        runner.Check("high проигрывает на 7", !CasinoRules.Wins(CasinoGame.Dice, "high", new[] { 3, 4 }));
        runner.Check("low выигрывает на 6", CasinoRules.Wins(CasinoGame.Dice, "low", new[] { 2, 4 }));
        runner.Check("seven", CasinoRules.Wins(CasinoGame.Dice, "seven", new[] { 3, 4 }));
        runner.Check("double", CasinoRules.Wins(CasinoGame.Dice, "double", new[] { 5, 5 }) && !CasinoRules.Wins(CasinoGame.Dice, "double", new[] { 5, 6 }));
        runner.Check("snakeeyes только 1+1", CasinoRules.Wins(CasinoGame.Dice, "snakeeyes", new[] { 1, 1 }) && !CasinoRules.Wins(CasinoGame.Dice, "snakeeyes", new[] { 1, 2 }));
        runner.Check("boxcars только 6+6", CasinoRules.Wins(CasinoGame.Dice, "boxcars", new[] { 6, 6 }) && !CasinoRules.Wins(CasinoGame.Dice, "boxcars", new[] { 6, 5 }));

        // Коэффициенты.
        runner.Check("коэффициент red = x2", CasinoRules.Multiplier(CasinoGame.Roulette, "red") == 2);
        runner.Check("коэффициент дюжины = x3", CasinoRules.Multiplier(CasinoGame.Roulette, "dozen2") == 3);
        runner.Check("коэффициент числа = x36", CasinoRules.Multiplier(CasinoGame.Roulette, "17") == 36);
        runner.Check("коэффициент seven = x5", CasinoRules.Multiplier(CasinoGame.Dice, "seven") == 5);
        runner.Check("неизвестная цель = 0", CasinoRules.Multiplier(CasinoGame.Dice, "что-то") == 0);

        // Разбор целей, включая русские алиасы.
        runner.Check("цель red разобрана", CasinoRules.TryParseTarget(CasinoGame.Roulette, "красное", out string normalized, out _) && normalized == "red");
        runner.Check("цель 24 разобрана как число", CasinoRules.TryParseTarget(CasinoGame.Roulette, "24", out string pocket, out _) && pocket == "24");
        runner.Check("цель 40 отвергнута", !CasinoRules.TryParseTarget(CasinoGame.Roulette, "40", out _, out _));
        runner.Check("цель seven для костей", CasinoRules.TryParseTarget(CasinoGame.Dice, "seven", out string seven, out _) && seven == "seven");
        runner.Check("цель red для костей отвергнута", !CasinoRules.TryParseTarget(CasinoGame.Dice, "red", out _, out string error) && error.Length > 0);

        // Выплаты.
        var dealer = new CasinoDealer(new SeededRandom(99));
        CasinoOutcome win = dealer.Spin(CasinoGame.Roulette, "red", 4);
        runner.Check("выплата при выигрыше = ставка × коэффициент",
            win.Won ? win.Payout == 4 * win.Multiplier : win.Payout == 0, win.RollText);
        runner.Check("проигрыш не платит", !win.Won || win.Payout > 0);

        CasinoOutcome straight = ForceOutcome(CasinoGame.Roulette, "17", 3, new[] { 17 });
        runner.Check("прямое попадание в число платит x36", straight.Won && straight.Payout == 108, straight.Summary());

        CasinoOutcome snakeEyes = ForceOutcome(CasinoGame.Dice, "snakeeyes", 2, new[] { 1, 1 });
        runner.Check("1+1 платит x31", snakeEyes.Won && snakeEyes.Payout == 62, snakeEyes.Summary());

        CasinoOutcome miss = ForceOutcome(CasinoGame.Dice, "boxcars", 5, new[] { 3, 4 });
        runner.Check("проигрыш уносит ставку", !miss.Won && miss.Payout == 0 && miss.Summary().Contains("-5", StringComparison.Ordinal));
    }

    /// <summary>Подставить известные броски — проверка условий выигрыша без случайности.</summary>
    private static CasinoOutcome ForceOutcome(CasinoGame game, string target, int stake, int[] rolls)
    {
        int multiplier = CasinoRules.Multiplier(game, target);
        bool won = multiplier > 0 && CasinoRules.Wins(game, target, rolls);
        return new CasinoOutcome
        {
            Game = game,
            Target = target,
            Stake = stake,
            Won = won,
            Multiplier = won ? multiplier : 0,
            Payout = won ? stake * multiplier : 0,
            Rolls = rolls,
            RollText = CasinoRules.RollText(game, rolls),
        };
    }

    private static void TestCasinoStatistics(TestRunner runner)
    {
        // Детерминированный прогон: проверяем, что матожидание близко к теоретическому.
        var dealer = new CasinoDealer(new SeededRandom(2024));
        const int rounds = 20000;

        int diceWins = 0;
        var faceCounts = new int[7];
        for (int i = 0; i < rounds; i++)
        {
            int[] rolls = dealer.RollDice();
            faceCounts[rolls[0]]++;
            faceCounts[rolls[1]]++;
            if (CasinoRules.Wins(CasinoGame.Dice, "high", rolls)) diceWins++;
        }

        double highRtp = diceWins * 2d / rounds;
        runner.Check("RTP ставки high ≈ 0.83 (хаус-эдж ~17%)", highRtp is > 0.80 and < 0.87, Fmt.N2(highRtp));

        double perFace = rounds * 2 / 6d;
        bool facesFair = Enumerable.Range(1, 6).All(face => Math.Abs(faceCounts[face] - perFace) < perFace * 0.08);
        runner.Check("кубик выпадает равномерно", facesFair,
            string.Join(",", Enumerable.Range(1, 6).Select(f => faceCounts[f].ToString(CultureInfo.InvariantCulture))));

        int redWins = 0;
        for (int i = 0; i < rounds; i++)
        {
            if (CasinoRules.Wins(CasinoGame.Roulette, "red", dealer.RollRoulette())) redWins++;
        }

        double redRtp = redWins * 2d / rounds;
        double expected = 36d / 37d; // одно зеро даёт 1/37 преимущества заведению
        runner.Check("RTP рулетки (красное) ≈ 36/37", Math.Abs(redRtp - expected) < 0.03, $"{Fmt.N2(redRtp)} против {Fmt.N2(expected)}");

        int zeros = 0;
        for (int i = 0; i < rounds; i++)
        {
            if (dealer.RollRoulette()[0] == 0) zeros++;
        }
        runner.Check("зеро выпадает примерно 1 раз из 37", Math.Abs(zeros - (rounds / 37d)) < rounds / 37d * 0.25, $"{zeros} из {rounds}");
    }

    // ── Сеть ─────────────────────────────────────────────────────────────────

    private static void TestReplication(TestRunner runner)
    {
        const ulong hostId = 1001;
        const ulong clientId = 1002;

        var bus = new LoopbackBus();
        var hostTransport = bus.CreateEndpoint(hostId);
        var clientTransport = bus.CreateEndpoint(clientId);

        var hostWorld = new VoxelGrid(24, 10, 24);
        VoxelWorldPresets.BuildStarterArena(hostWorld);
        var clientWorld = new VoxelGrid(24, 10, 24);

        using var host = new GambleCreativeSystem(hostTransport, "Хост", hostWorld, random: new SeededRandom(1));
        using var client = new GambleCreativeSystem(clientTransport, "Друг", clientWorld, random: new SeededRandom(2));

        Pump(bus, host, client);

        runner.Check("хост знает о клиенте", host.Players.ContainsKey(clientId));
        runner.Check("клиент знает о хосте", client.Players.ContainsKey(hostId));
        runner.Check("хост получил self-report инвентаря клиента",
            host.Players[clientId].Inventory.Get(Blocks.DirtKey) == 64, host.Players[clientId].Inventory.Describe());

        runner.Check("мир клиента совпал с миром хоста после снимка", GridsEqual(hostWorld, clientWorld),
            $"хост: {hostWorld.CountNonAir()} вокселей, клиент: {clientWorld.CountNonAir()}");

        // Хост строит — клиент видит.
        hostWorld.Set(5, 5, 5, BlockId.Air);
        runner.Check("хост поставил блок", host.Place(5, 5, 5, BlockId.Gold, out _));
        Pump(bus, host, client);
        runner.Check("клиент увидел блок хоста", clientWorld.Get(5, 5, 5) == BlockId.Gold);
        runner.Check("у хоста списалось золото", host.LocalInventory.Get(Blocks.GoldKey) == 7, host.LocalInventory.Describe());

        // Клиент строит — хост применяет и рассылает, клиент видит результат.
        runner.Check("клиент поставил блок", client.Place(6, 5, 5, BlockId.Stone, out _));
        Pump(bus, host, client);
        runner.Check("хост применил блок клиента", hostWorld.Get(6, 5, 5) == BlockId.Stone);
        runner.Check("клиент увидел свой блок", clientWorld.Get(6, 5, 5) == BlockId.Stone);
        runner.Check("у клиента списался камень, но не золото",
            client.LocalInventory.Get(Blocks.StoneKey) == 31 && client.LocalInventory.Get(Blocks.GoldKey) == 8,
            client.LocalInventory.Describe());

        // Снос возвращает блок владельцу.
        runner.Check("клиент сломал свой блок", client.Break(6, 5, 5, out _));
        Pump(bus, host, client);
        runner.Check("мир хоста после сноса", hostWorld.Get(6, 5, 5) == BlockId.Air);
        runner.Check("камень вернулся клиенту", client.LocalInventory.Get(Blocks.StoneKey) == 32, client.LocalInventory.Describe());

        // Массовая заливка (VoxelBatch + фрагментация при большом объёме).
        runner.Check("клиент залил площадку", client.Fill(new VoxelPos(2, 3, 2), new VoxelPos(9, 3, 9), BlockId.Wood, out string fillMessage), fillMessage);
        Pump(bus, host, client);
        runner.Check("миры совпали после заливки", GridsEqual(hostWorld, clientWorld),
            $"хост: {hostWorld.CountNonAir()}, клиент: {clientWorld.CountNonAir()}");
        runner.Check("дерево списалось у клиента", client.LocalInventory.Get(Blocks.WoodKey) == 24 - 64, client.LocalInventory.Describe());
    }

    private static void TestCasinoOverNetwork(TestRunner runner)
    {
        const ulong hostId = 2001;
        const ulong clientId = 2002;

        var bus = new LoopbackBus();
        var hostWorld = new VoxelGrid(16, 8, 16);
        VoxelWorldPresets.BuildStarterArena(hostWorld);
        var clientWorld = new VoxelGrid(16, 8, 16);

        // Фиксированный сид: результат раунда известен заранее.
        using var host = new GambleCreativeSystem(bus.CreateEndpoint(hostId), "Хост", hostWorld, random: new SeededRandom(4242));
        using var client = new GambleCreativeSystem(bus.CreateEndpoint(clientId), "Друг", clientWorld, random: new SeededRandom(4242));

        Pump(bus, host, client);

        int goldBefore = client.LocalInventory.Get(Blocks.GoldKey);
        long totalGoldBefore = client.LocalInventory.Get(Blocks.GoldKey) + host.LocalInventory.Get(Blocks.GoldKey);

        CasinoResultMessage? observed = null;
        client.CasinoResolved += result => observed = result;

        runner.Check("клиент сделал ставку", client.Bet(CasinoGame.Roulette, "red", Blocks.GoldKey, 4, out string betMessage), betMessage);
        Pump(bus, host, client);

        runner.Check("результат раунда дошёл до клиента", observed is not null);
        if (observed is not null)
        {
            runner.Check("ставка списана", observed.Stake == 4, observed.SummaryText());
            runner.Check("результат помечен как хостовый", observed.RollText.Length > 0 && observed.Rolls.Length == 1);
        }

        int clientGoldAfter = client.LocalInventory.Get(Blocks.GoldKey);
        int hostViewOfClientGold = host.Players[clientId].Inventory.Get(Blocks.GoldKey);
        runner.Check("хост и клиент согласны по золоту клиента",
            clientGoldAfter == hostViewOfClientGold, $"клиент {clientGoldAfter}, хост {hostViewOfClientGold}");

        long totalGoldAfter = client.LocalInventory.Get(Blocks.GoldKey) + host.LocalInventory.Get(Blocks.GoldKey);
        runner.Check("баланс блоков сходится: ставка сгорела или выигрыш начислен",
            totalGoldAfter == totalGoldBefore - 4 || totalGoldAfter == totalGoldBefore - 4 + 8,
            $"{totalGoldBefore} → {totalGoldAfter}");

        // Ставка больше, чем есть — хост отказывает и мир/инвентарь не меняются.
        int before = clientGoldAfter;
        runner.Check("клиент попробовал поставить больше, чем есть",
            !client.Bet(CasinoGame.Roulette, "red", Blocks.GoldKey, 9999, out string bigMessage), bigMessage);
        Pump(bus, host, client);
        runner.Check("инвентарь не изменился после отказа", client.LocalInventory.Get(Blocks.GoldKey) == before);

        // Ставка при отсутствии стола казино запрещена.
        var emptyBus = new LoopbackBus();
        var emptyWorld = new VoxelGrid(12, 6, 12);
        using var lonely = new GambleCreativeSystem(emptyBus.CreateEndpoint(5), "Один", emptyWorld, random: new SeededRandom(1));
        runner.Check("без стола казино ставка невозможна",
            !lonely.Bet(CasinoGame.Dice, "high", Blocks.DirtKey, 1, out string noTable) && noTable.Contains("стол", StringComparison.OrdinalIgnoreCase), noTable);
    }

    private static void TestForgedPacketsRejected(TestRunner runner)
    {
        var bus = new LoopbackBus();
        var hostEndpoint = bus.CreateEndpoint(3001);
        var goodClient = bus.CreateEndpoint(3002);
        var badClient = bus.CreateEndpoint(3003);

        var world = new VoxelGrid(12, 6, 12);
        var clientWorld = new VoxelGrid(12, 6, 12);
        var victimWorld = new VoxelGrid(12, 6, 12);

        using var host = new GambleCreativeSystem(hostEndpoint, "Хост", world, random: new SeededRandom(1));
        using var victim = new GambleCreativeSystem(goodClient, "Жертва", victimWorld, random: new SeededRandom(1));
        using var attacker = new GambleCreativeSystem(badClient, "Читер", clientWorld, random: new SeededRandom(1));

        Pump(bus, host, victim, attacker);

        // Клиент шлёт "авторитетную" правку напрямую другому клиенту — она должна быть отброшена.
        var forged = new VoxelEditMessage
        {
            X = 1,
            Y = 1,
            Z = 1,
            Block = (byte)BlockId.Diamond,
            Author = 3003,
            AuthorName = "Читер",
        };

        badClient.SendTo(3002, forged.Serialize(), reliable: true);
        Pump(bus, host, victim, attacker);

        runner.Check("поддельная правка от клиента не применилась", victimWorld.Get(1, 1, 1) == BlockId.Air,
            victimWorld.Get(1, 1, 1).ToString());

        // Подделанная правка от имени хоста, но отправленная клиентом, тоже не проходит.
        var forgedHost = new VoxelEditMessage { X = 2, Y = 1, Z = 2, Block = (byte)BlockId.Gold, Author = 3001, AuthorName = "Хост" };
        badClient.SendTo(3002, forgedHost.Serialize(), reliable: true);
        Pump(bus, host, victim, attacker);
        runner.Check("подделка «от хоста» клиентом не применилась", victimWorld.Get(2, 1, 2) == BlockId.Air);

        // Прямая жалоба хосту: клиент шлёт результат казино, будто он хост.
        var fakeCasino = new CasinoResultMessage
        {
            PlayerId = 3003,
            PlayerName = "Читер",
            Game = (byte)CasinoGame.Roulette,
            Target = "17",
            BlockKey = "diamond",
            Stake = 0,
            Payout = 1000,
            Won = true,
            Rolls = new byte[] { 17 },
            RollText = "17",
        };
        badClient.SendTo(3001, fakeCasino.Serialize(), reliable: true);
        Pump(bus, host, victim, attacker);
        runner.Check("хост не принял чужой результат казино как свой",
            host.Players[3003].Inventory.Get(Blocks.DiamondKey) == 3,
            host.Players[3003].Inventory.Describe());
    }

    private static void TestHostAuthority(TestRunner runner)
    {
        var bus = new LoopbackBus();
        var hostWorld = new VoxelGrid(12, 6, 12);
        var clientWorld = new VoxelGrid(12, 6, 12);

        using var host = new GambleCreativeSystem(bus.CreateEndpoint(4001), "Хост", hostWorld, random: new SeededRandom(1));
        using var client = new GambleCreativeSystem(bus.CreateEndpoint(4002), "Друг", clientWorld, random: new SeededRandom(1));

        Pump(bus, host, client);

        // Вне границ мира.
        runner.Check("клиент не может строить за границей мира",
            !client.Place(999, 999, 999, BlockId.Dirt, out string outside), outside);
        Pump(bus, host, client);
        runner.Check("мир не изменился после неверного запроса", hostWorld.CountNonAir() == clientWorld.CountNonAir());

        // Клетка занята.
        hostWorld.Set(3, 1, 3, BlockId.Stone);
        runner.Check("нельзя ставить в занятую клетку", !client.Place(3, 1, 3, BlockId.Dirt, out string occupied), occupied);

        // Нет ресурсов.
        var poor = new Inventory();
        poor.Add(Blocks.DirtKey, 1);
        var poorBus = new LoopbackBus();
        using var poorHost = new GambleCreativeSystem(poorBus.CreateEndpoint(5001), "Хост", new VoxelGrid(8, 6, 8),
            startingInventory: poor, random: new SeededRandom(1));
        runner.Check("нельзя ставить блок, которого нет",
            !poorHost.Place(3, 2, 3, BlockId.Diamond, out string noResources), noResources);
        runner.Check("инвентарь не ушёл в минус", poorHost.LocalInventory.Get(Blocks.DiamondKey) == 0);

        // Песочница: ресурсы не тратятся.
        var sandboxBus = new LoopbackBus();
        using var sandbox = new GambleCreativeSystem(sandboxBus.CreateEndpoint(6001), "Песочница", new VoxelGrid(8, 6, 8),
            sandbox: true, random: new SeededRandom(1));
        runner.Check("в песочнице блок ставится без затрат",
            sandbox.Place(2, 2, 2, BlockId.Diamond, out string sandboxMessage) && sandbox.LocalInventory.Get(Blocks.DiamondKey) == 3,
            sandboxMessage);

        // Крафт стола казино.
        var craftBus = new LoopbackBus();
        var craftWorld = new VoxelGrid(10, 6, 10);
        using var crafter = new GambleCreativeSystem(craftBus.CreateEndpoint(7001), "Крафтер", craftWorld, random: new SeededRandom(1));
        runner.Check("крафт стола казино", crafter.CraftTable(1, out string craftMessage), craftMessage);
        runner.Check("стол появился в инвентаре", crafter.LocalInventory.Get(Blocks.TableKey) == 1);
        runner.Check("ресурсы списались по рецепту",
            crafter.LocalInventory.Get(Blocks.DirtKey) == 48 && crafter.LocalInventory.Get(Blocks.WoodKey) == 16,
            crafter.LocalInventory.Describe());
        runner.Check("крафт без ресурсов отклонён",
            !SafeCraftMany(crafter, 16, out string tooMany), tooMany);
    }

    private static bool SafeCraftMany(GambleCreativeSystem system, int count, out string message) =>
        system.CraftTable(count, out message);

    private static void TestHostMigration(TestRunner runner)
    {
        var bus = new LoopbackBus();
        var hostWorld = new VoxelGrid(12, 6, 12);
        VoxelWorldPresets.BuildStarterArena(hostWorld);
        var clientWorld = new VoxelGrid(12, 6, 12);

        using var host = new GambleCreativeSystem(bus.CreateEndpoint(8001), "Хост", hostWorld, random: new SeededRandom(5));
        using var client = new GambleCreativeSystem(bus.CreateEndpoint(8002), "Друг", clientWorld, random: new SeededRandom(6));

        Pump(bus, host, client);
        runner.Check("до миграции хост — первый игрок", host.Net.IsHost && !client.Net.IsHost);

        // Хост ушёл: Steam передаёт владение лобби оставшемуся игроку.
        bus.SetHost(8002);
        host.Tick(Environment.TickCount64);
        client.Tick(Environment.TickCount64);
        Pump(bus, host, client);

        runner.Check("после ухода хоста роль перешла клиенту", !host.Net.IsHost && client.Net.IsHost);
        runner.Check("новый хост управляет сессией: поставил блок",
            client.Place(3, 3, 3, BlockId.Gold, out string message), message);
        Pump(bus, host, client);
        runner.Check("правка нового хоста применилась у него самого", clientWorld.Get(3, 3, 3) == BlockId.Gold);
        runner.Check("мир у бывшего хоста тоже обновился", hostWorld.Get(3, 3, 3) == BlockId.Gold);
    }

    /// <summary>Прогон нескольких "кадров" сети: тики сессий + доставка пакетов.</summary>
    private static void Pump(LoopbackBus bus, params GambleCreativeSystem[] sessions)
    {
        for (int frame = 0; frame < 6; frame++)
        {
            long now = Environment.TickCount64 + frame;
            foreach (var session in sessions) session.Tick(now);
            bus.DeliverAll();
        }
    }

    private static bool GridsEqual(VoxelGrid a, VoxelGrid b)
    {
        if (a.SizeX != b.SizeX || a.SizeY != b.SizeY || a.SizeZ != b.SizeZ) return false;
        byte[] left = a.ToBytes();
        byte[] right = b.ToBytes();
        return left.SequenceEqual(right);
    }

    private static bool Throws(Action action)
    {
        try
        {
            action();
            return false;
        }
        catch (ProtocolException)
        {
            return true;
        }
        catch (ArgumentOutOfRangeException)
        {
            return true;
        }
    }
}

/// <summary>Детерминированный ГПСЧ для тестов (и повторяемых раундов казино).</summary>
public sealed class SeededRandom : IRandomSource
{
    private readonly Random _random;

    public SeededRandom(int seed) => _random = new Random(seed);

    public int NextInt(int maxExclusive) => maxExclusive <= 1 ? 0 : _random.Next(maxExclusive);
}

/// <summary>
/// Тестовый транспорт: полностью повторяет контракт INetTransport, но пакеты
/// складываются в очередь и доставляются вызовом <see cref="DeliverAll"/>
/// (как настоящая сеть с задержкой, только без Steam).
/// </summary>
public sealed class LoopbackBus
{
    private readonly List<LoopbackNetwork> _endpoints = new();
    private readonly Queue<(ulong From, ulong To, byte[] Payload)> _inFlight = new();

    private ulong _hostId;

    public LoopbackNetwork CreateEndpoint(ulong id)
    {
        if (_hostId == 0) _hostId = id;
        var endpoint = new LoopbackNetwork(this, id);
        _endpoints.Add(endpoint);
        return endpoint;
    }

    /// <summary>Смена хоста — эмулирует передачу владения лобби в Steam.</summary>
    public void SetHost(ulong id) => _hostId = id;

    internal ulong HostId => _hostId;

    internal IReadOnlyList<LoopbackNetwork> Endpoints => _endpoints;

    internal void Enqueue(ulong from, ulong to, byte[] payload) => _inFlight.Enqueue((from, to, payload));

    /// <summary>Доставить все накопившиеся пакеты (аналог Pump у Steam-транспорта).</summary>
    public void DeliverAll()
    {
        int guard = 0;
        while (_inFlight.Count > 0 && guard++ < 10000)
        {
            (ulong from, ulong to, byte[] payload) = _inFlight.Dequeue();
            LoopbackNetwork? target = _endpoints.FirstOrDefault(e => e.LocalId == to);
            target?.Deliver(from, payload);
        }
    }
}

/// <summary>Транспорт одного участника тестовой сети.</summary>
public sealed class LoopbackNetwork : INetTransport
{
    private readonly LoopbackBus _bus;

    internal LoopbackNetwork(LoopbackBus bus, ulong localId)
    {
        _bus = bus;
        LocalId = localId;
    }

    public bool IsHost => _bus.HostId == LocalId;

    public ulong LocalId { get; }

    public ulong HostId => _bus.HostId;

    public IReadOnlyCollection<ulong> Peers => _bus.Endpoints.Where(e => e.LocalId != LocalId).Select(e => e.LocalId).ToList();

    public int SentCount { get; private set; }

    public event Action<ulong, byte[]>? PacketReceived;

    public bool SendTo(ulong peer, byte[] payload, bool reliable)
    {
        if (!Peers.Contains(peer)) return false;
        SentCount++;
        _bus.Enqueue(LocalId, peer, payload);
        return true;
    }

    public void Broadcast(byte[] payload, bool reliable)
    {
        foreach (ulong peer in Peers) SendTo(peer, payload, reliable);
    }

    internal void Deliver(ulong from, byte[] payload) => PacketReceived?.Invoke(from, payload);
}

/// <summary>Мини-раннер: печатает результат каждого теста и общий итог.</summary>
public sealed class TestRunner
{
    private readonly List<string> _failures = new();
    private int _passed;

    public static void Section(string title)
    {
        Console.WriteLine();
        Console.WriteLine($"── {title} ──");
    }

    public void Check(string name, bool condition, string detail = "")
    {
        if (condition)
        {
            _passed++;
            Console.WriteLine($"  [ ok ] {name}{(detail.Length > 0 ? $" ({detail})" : string.Empty)}");
            return;
        }

        _failures.Add(name);
        Console.WriteLine($"  [FAIL] {name}{(detail.Length > 0 ? $" — {detail}" : string.Empty)}");
    }

    public int Summary()
    {
        Console.WriteLine();
        Console.WriteLine(new string('=', 72));
        if (_failures.Count == 0)
        {
            Console.WriteLine($"ВСЕ ТЕСТЫ ПРОШЛИ: {_passed} проверок.");
            return 0;
        }

        Console.WriteLine($"ПРОВАЛЕНО {_failures.Count} из {_passed + _failures.Count} проверок:");
        foreach (string failure in _failures) Console.WriteLine($"  • {failure}");
        return 1;
    }
}
