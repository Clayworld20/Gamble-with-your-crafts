// ============================================================================
//  BlockGranter.java — превращение выигрыша в казино в блоки инвентаря.
//
//  ГДЕ ЭТО РАБОТАЕТ
//  ----------------
//  Выдача — клиентская и только в креативном режиме: мод кладёт предметы в
//  инвентарь локального игрока. В одиночной игре и на собственном сервере
//  этого достаточно. На чужом сервере клиентский инвентарь не является
//  источником истины, поэтому:
//    * при включённом anti-cheat/серверной проверке предметы будут отброшены;
//    * мод никогда не пытается обойти серверную валидацию;
//    * для честной игры с друзьями используйте свой сервер/лобби и креатив.
//
//  КУРС ОБМЕНА
//  -----------
//  Одна фишка = одна группа блоков (rewardBlocksPerChip). Настраивается константой
//  ниже, потому что «курс» — вопрос договорённости между друзьями, а не логики.
// ============================================================================
package dev.gwyfbridge.mc;

import net.minecraft.client.MinecraftClient;
import net.minecraft.entity.player.PlayerEntity;
import net.minecraft.item.Item;
import net.minecraft.item.ItemStack;
import net.minecraft.item.Items;

import java.util.HashMap;
import java.util.Map;

public final class BlockGranter {

    private BlockGranter() {
    }

    /** Сколько блоков выдаётся за одну выигранную фишку. */
    public static final int REWARD_BLOCKS_PER_CHIP = 1;

    /** Ограничение на одну выдачу, чтобы выигрыш в 100000 фишек не забил инвентарь. */
    public static final int MAX_BLOCKS_PER_GRANT = 2304;

    /**
     * Соответствие «числовой тип блока → предмет Minecraft». Те же номера
     * используются в профиле игры ([blocks]) и в палитре кубов
     * (native/src/VoxelTo3DWorld.cpp), поэтому постройка в Minecraft и в игре
     * выглядит одинаково.
     */
    private static final Map<Integer, Item> BLOCK_BY_ID = new HashMap<>();

    static {
        BLOCK_BY_ID.put(1, Items.DIRT);
        BLOCK_BY_ID.put(2, Items.GRASS_BLOCK);
        BLOCK_BY_ID.put(3, Items.STONE);
        BLOCK_BY_ID.put(4, Items.OAK_LOG);
        BLOCK_BY_ID.put(5, Items.GOLD_BLOCK);
        BLOCK_BY_ID.put(6, Items.DIAMOND_BLOCK);
        BLOCK_BY_ID.put(7, Items.RED_CONCRETE);   // «стол казино»
        BLOCK_BY_ID.put(8, Items.SEA_LANTERN);    // «светильник»
        BLOCK_BY_ID.put(9, Items.EMERALD_BLOCK);
        BLOCK_BY_ID.put(10, Items.NETHERITE_BLOCK);
    }

    /** Соответствие «блок Minecraft → числовой тип для игры». */
    public static int blockIdOf(Item item) {
        for (Map.Entry<Integer, Item> entry : BLOCK_BY_ID.entrySet()) {
            if (entry.getValue() == item) {
                return entry.getKey();
            }
        }
        return 3; // камень по умолчанию: наборы блоков расширяются в одном месте
    }

    /** Выдать блоки по типу (используется событиями BlockGrant из игры). */
    public static void grant(int block, int count, int reason) {
        final Item item = BLOCK_BY_ID.getOrDefault(block, Items.STONE);
        give(item, count, "событие игры (причина " + reason + ")");
    }

    /** Выдать блоки за выигрыш в казино. */
    public static void grantRewardBlocks(long payoutChips) {
        if (payoutChips <= 0) {
            return;
        }

        // Курс намеренно нелинейный: крупный выигрыш даёт редкие блоки,
        // мелкий — обычные стройматериалы. Так выигрыш чувствуется.
        final Item item;
        if (payoutChips >= 5000) {
            item = Items.NETHERITE_BLOCK;
        } else if (payoutChips >= 1000) {
            item = Items.DIAMOND_BLOCK;
        } else if (payoutChips >= 200) {
            item = Items.GOLD_BLOCK;
        } else {
            item = Items.STONE;
        }

        long blocks = payoutChips * REWARD_BLOCKS_PER_CHIP;
        if (blocks > MAX_BLOCKS_PER_GRANT) {
            blocks = MAX_BLOCKS_PER_GRANT;
        }

        give(item, (int) blocks, "выигрыш " + payoutChips + " фишек");
    }

    /** Положить предметы в инвентарь игрока (обязательно на клиентском потоке). */
    private static void give(Item item, int count, String reason) {
        final MinecraftClient client = MinecraftClient.getInstance();
        final PlayerEntity player = client.player;

        if (player == null) {
            GwyfBridgeMod.LOGGER.warn("GWYF Bridge: некому выдавать блоки (игрок не в мире), {}", reason);
            return;
        }

        if (!player.isCreative()) {
            GwyfBridgeMod.LOGGER.warn("GWYF Bridge: игрок не в креативном режиме — выдача пропущена ({})", reason);
            return;
        }

        if (count <= 0) {
            return;
        }

        int remaining = count;
        final int maxStack = item.getMaxCount();

        while (remaining > 0) {
            final int stackSize = Math.min(remaining, maxStack);
            final ItemStack stack = new ItemStack(item, stackSize);
            if (!player.getInventory().insertStack(stack)) {
                player.dropItem(stack, false);
            }
            remaining -= stackSize;
        }

        GwyfBridgeMod.LOGGER.info("GWYF Bridge: выдано {} × {} ({})", count, item, reason);
    }
}
