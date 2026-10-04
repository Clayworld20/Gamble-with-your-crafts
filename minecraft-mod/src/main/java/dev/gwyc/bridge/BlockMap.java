package dev.gwyc.bridge;

import net.minecraft.block.Block;
import net.minecraft.block.Blocks;
import net.minecraft.item.Item;
import net.minecraft.item.ItemStack;
import net.minecraft.item.Items;
import net.minecraft.registry.Registries;
import net.minecraft.util.Identifier;

import java.util.HashMap;
import java.util.Map;

/**
 * Соответствие типов блоков между тремя сторонами.
 *
 * <p>Числовые идентификаторы — часть протокола (см. native/include/gwyc/protocol.h,
 * enum BlockKind). Добавлять новое значение можно только увеличив LAYOUT_VERSION
 * и синхронно поправив C++, C# и Java.
 */
public final class BlockMap {

    public static final int AIR = 0;
    public static final int DIRT = 1;
    public static final int GRASS = 2;
    public static final int STONE = 3;
    public static final int WOOD = 4;
    public static final int GOLD = 5;
    public static final int DIAMOND = 6;
    public static final int TABLE = 7;
    public static final int LAMP = 8;

    public static final int MAX_KIND = LAMP;

    private static final Map<Block, Integer> BLOCK_TO_KIND = new HashMap<>();
    private static final Map<Integer, Block> KIND_TO_BLOCK = new HashMap<>();
    private static final Map<Integer, Item> KIND_TO_ITEM = new HashMap<>();

    static {
        register(DIRT, Blocks.DIRT, Items.DIRT);
        register(GRASS, Blocks.GRASS_BLOCK, Items.GRASS_BLOCK);
        register(STONE, Blocks.STONE, Items.STONE);
        register(WOOD, Blocks.OAK_PLANKS, Items.OAK_PLANKS);
        register(GOLD, Blocks.GOLD_BLOCK, Items.GOLD_BLOCK);
        register(DIAMOND, Blocks.DIAMOND_BLOCK, Items.DIAMOND_BLOCK);
        register(TABLE, Blocks.CRAFTING_TABLE, Items.CRAFTING_TABLE);
        register(LAMP, Blocks.GLOWSTONE, Items.GLOWSTONE);

        // Обратные соответствия для блоков, которые игрок ставит вручную:
        // камень/булыжник/земля и т.п. отправляются игре как свои типы.
        BLOCK_TO_KIND.putIfAbsent(Blocks.COBBLESTONE, STONE);
        BLOCK_TO_KIND.putIfAbsent(Blocks.STONE_BRICKS, STONE);
        BLOCK_TO_KIND.putIfAbsent(Blocks.DIRT_PATH, DIRT);
        BLOCK_TO_KIND.putIfAbsent(Blocks.COARSE_DIRT, DIRT);
        BLOCK_TO_KIND.putIfAbsent(Blocks.PODZOL, DIRT);
        BLOCK_TO_KIND.putIfAbsent(Blocks.OAK_LOG, WOOD);
        BLOCK_TO_KIND.putIfAbsent(Blocks.SPRUCE_PLANKS, WOOD);
        BLOCK_TO_KIND.putIfAbsent(Blocks.BIRCH_PLANKS, WOOD);
        BLOCK_TO_KIND.putIfAbsent(Blocks.GLOWSTONE, LAMP);
    }

    private BlockMap() {
    }

    private static void register(int kind, Block block, Item item) {
        KIND_TO_BLOCK.put(kind, block);
        KIND_TO_ITEM.put(kind, item);
        BLOCK_TO_KIND.put(block, kind);
    }

    /** Тип блока Minecraft → идентификатор протокола. Воздух = 0. */
    public static int toKind(Block block) {
        if (block == null || block == Blocks.AIR) {
            return AIR;
        }
        final Integer kind = BLOCK_TO_KIND.get(block);
        if (kind != null) {
            return kind;
        }

        // Незнакомый блок всё равно должен доехать до казино: отдаём его как камень
        // и пишем в лог — так игрок увидит «физический куб» вместо тишины.
        return STONE;
    }

    /** Идентификатор протокола → блок Minecraft (для установки в мир). */
    public static Block toBlock(int kind) {
        return KIND_TO_BLOCK.getOrDefault(kind, Blocks.AIR);
    }

    /** Идентификатор протокола → предмет (для наград в креативе). */
    public static Item toItem(int kind) {
        return KIND_TO_ITEM.getOrDefault(kind, Items.STONE);
    }

    public static boolean isValid(int kind) {
        return kind >= AIR && kind <= MAX_KIND;
    }

    public static String nameOf(int kind) {
        final Item item = KIND_TO_ITEM.get(kind);
        if (item == null) {
            return "воздух";
        }
        return Registries.ITEM.getId(item).toString();
    }

    public static ItemStack stackFor(int kind, int count) {
        return new ItemStack(toItem(kind), Math.max(1, Math.min(count, 64)));
    }

    /** Идентификатор типа блока в Minecraft (для резервных проверок и логов). */
    public static Identifier idOf(int kind) {
        final Block block = KIND_TO_BLOCK.get(kind);
        return (block == null) ? null : Registries.BLOCK.getId(block);
    }
}
