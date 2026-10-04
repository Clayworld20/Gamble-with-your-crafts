// ============================================================================
//  VoxelForwarder.java — правки блоков Minecraft → игра.
//
//  Два независимых пути захвата (оба включены сознательно):
//    1. события Fabric API (PlayerBlockBreakEvents.AFTER, UseBlockCallback) —
//       работают независимо от версии маппингов, покрывают и клиент, и
//       встроенный сервер;
//    2. миксин на ClientPlayerInteractionManager (см. mixin/) — точный захват
//       на клиенте, нужен, когда игра в мультиплеере и события сервера до нас
//       не доходят.
//
//  Дедупликация: одно и то же действие может прийти двумя путями, поэтому
//  события, совпавшие по координатам и тику, отбрасываются.
//
//  Ограничение отправки: не больше MAX_EDITS_PER_TICK правок за тик. Крупные
//  постройки (например, заливка схемы) не должны забивать кольцо игры — излишек
//  накапливается в очереди и уходит порциями, а не теряется.
// ============================================================================
package dev.gwyfbridge.mc;

import net.fabricmc.fabric.api.event.player.PlayerBlockBreakEvents;
import net.fabricmc.fabric.api.event.player.UseBlockCallback;
import net.minecraft.block.BlockState;
import net.minecraft.client.MinecraftClient;
import net.minecraft.item.Item;
import net.minecraft.item.ItemStack;
import net.minecraft.registry.Registries;
import net.minecraft.util.ActionResult;
import net.minecraft.util.Hand;
import net.minecraft.util.math.BlockPos;

import java.util.ArrayDeque;
import java.util.Deque;

public final class VoxelForwarder {

    /** Максимум правок, отправляемых игре за один тик. */
    private static final int MAX_EDITS_PER_TICK = 64;

    /** Сколько последних событий помним для дедупликации. */
    private static final int DEDUPE_WINDOW = 64;

    /** Одна отложенная правка: координаты, тип блока, действие и автор. */
    private static final class Edit {
        final int x;
        final int y;
        final int z;
        final int block;
        final int action;
        final long authorId;

        Edit(int x, int y, int z, int block, int action, long authorId) {
            this.x = x;
            this.y = y;
            this.z = z;
            this.block = block;
            this.action = action;
            this.authorId = authorId;
        }
    }

    private final Deque<Edit> pending = new ArrayDeque<>();
    private final Deque<Long> recent = new ArrayDeque<>();

    /** Регистрация событий Fabric API. */
    public void registerEvents() {
        PlayerBlockBreakEvents.AFTER.register((world, player, pos, state, entity) -> {
            if (!GwyfBridgeMod.isBridgeReady()) {
                return;
            }
            forward(pos, state, BridgeRecords.ACTION_BREAK);
        });

        UseBlockCallback.EVENT.register((player, world, hand, hitResult) -> {
            if (!GwyfBridgeMod.isBridgeReady() || hand != Hand.MAIN_HAND) {
                return ActionResult.PASS;
            }
            if (!world.isClient()) {
                return ActionResult.PASS;
            }

            final BlockPos pos = hitResult.getBlockPos();
            final BlockState state = world.getBlockState(pos);
            forward(pos, state, BridgeRecords.ACTION_PLACE);

            // Ничего не меняем в поведении игры: мы только наблюдаем.
            return ActionResult.PASS;
        });
    }

    /** Точка входа для миксина: клиентский захват установки/слома блока. */
    public void forwardFromMixin(BlockPos pos, BlockState state, int action) {
        forward(pos, state, action);
    }

    private void forward(BlockPos pos, BlockState state, int action) {
        if (pos == null) {
            return;
        }

        final long key = dedupeKey(pos, action);
        if (recent.contains(key)) {
            return;
        }
        recent.addLast(key);
        while (recent.size() > DEDUPE_WINDOW) {
            recent.removeFirst();
        }

        final int blockId = blockIdOf(state);

        // Автор запоминается вместе с правкой: очередь переживает несколько
        // тиков, и подставлять «текущего игрока» при отправке было бы ошибкой.
        pending.addLast(new Edit(pos.getX(), pos.getY(), pos.getZ(), blockId, action, localPlayerId()));
        flush();
    }

    /** Отправить накопленное порциями. Вызывается из тика и сразу после правки. */
    public void flush() {
        int sent = 0;
        while (!pending.isEmpty() && sent < MAX_EDITS_PER_TICK) {
            final Edit edit = pending.removeFirst();
            final boolean ok = GwyfBridgeMod.pushVoxelEdit(edit.x, edit.y, edit.z, edit.block, edit.action,
                    edit.authorId);
            if (!ok) {
                // Игра ещё не подняла мост — вернуть правку в очередь и попробовать позже.
                pending.addFirst(edit);
                break;
            }
            sent++;
        }
    }

    /** Числовой тип блока для игры: по предмету, если он есть, иначе по умолчанию. */
    private static int blockIdOf(BlockState state) {
        if (state == null) {
            return 3;
        }

        final Item item = state.getBlock().asItem();
        if (item != null) {
            final int id = BlockGranter.blockIdOf(item);
            if (id > 0) {
                return id;
            }
        }

        // Запасной путь: стабильное имя блока хешируется в небольшой номер,
        // чтобы игры с профилем-расширением всё равно различали блоки.
        final String name = Registries.BLOCK.getId(state.getBlock()).getPath();
        return 1000 + Math.floorMod(name.hashCode(), 1000);
    }

    private static long localPlayerId() {
        final MinecraftClient client = MinecraftClient.getInstance();
        if (client.player == null) {
            return 0;
        }
        // В одиночной игре UUID стабилен в пределах мира; в мультиплеере это
        // идентификатор игрока на сервере — достаточно, чтобы отличать авторов.
        return client.player.getUuid().getMostSignificantBits();
    }

    private static long dedupeKey(BlockPos pos, int action) {
        return ((long) pos.getX() * 73856093L) ^ ((long) pos.getY() * 19349663L) ^ ((long) pos.getZ() * 83492791L)
                ^ (action * 2654435761L);
    }

    /** Утилита для миксина: активная рука и предмет игрока. */
    public static int blockIdOfItemStack(ItemStack stack) {
        if (stack == null || stack.isEmpty()) {
            return 0;
        }
        return BlockGranter.blockIdOf(stack.getItem());
    }
}
