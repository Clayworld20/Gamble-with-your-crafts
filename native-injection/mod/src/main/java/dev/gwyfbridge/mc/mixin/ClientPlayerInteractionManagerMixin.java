// ============================================================================
//  ClientPlayerInteractionManagerMixin.java — точный клиентский захват правок.
//
//  Зачем миксин, если есть события Fabric API: в мультиплеере события
//  «блок сломан/поставлен» на клиенте приходят не всегда (они серверные),
//  а нам нужен ровно тот момент, когда ИГРОК сделал действие в своём мире.
//
//  require = 0 — сознательный выбор: если в выбранной версии Minecraft
//  сигнатура метода отличается, игра не упадёт, а мод продолжит работать через
//  Fabric API. Это дороже «строгого» требования цели, зато не превращает
//  обновление Minecraft в падение клиента.
// ============================================================================
package dev.gwyfbridge.mc.mixin;

import dev.gwyfbridge.mc.BridgeRecords;
import dev.gwyfbridge.mc.GwyfBridgeMod;

import net.minecraft.block.BlockState;
import net.minecraft.client.network.ClientPlayerInteractionManager;
import net.minecraft.util.math.BlockPos;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(ClientPlayerInteractionManager.class)
public class ClientPlayerInteractionManagerMixin {

    /**
     * Слом блока игроком. Срабатывает один раз на успешное действие
     * (в креативе — мгновенно, в выживании — по завершении разрушения).
     */
    @Inject(method = "breakBlock", at = @At("RETURN"), require = 0)
    private void gwyf$onBlockBroken(BlockPos pos, CallbackInfoReturnable<Boolean> info) {
        if (!GwyfBridgeMod.isBridgeReady() || !info.getReturnValueZ()) {
            return;
        }

        final BlockState state = net.minecraft.client.MinecraftClient.getInstance().world != null
                ? net.minecraft.client.MinecraftClient.getInstance().world.getBlockState(pos)
                : null;

        if (GwyfBridgeMod.forwarder() != null) {
            GwyfBridgeMod.forwarder().forwardFromMixin(pos, state, BridgeRecords.ACTION_BREAK);
        }
    }

    /**
     * Установка блока игроком. Позиция берётся из результата действия: важно
     * именно место, куда встал блок, а не точка попадания луча.
     */
    @Inject(method = "interactBlock", at = @At("RETURN"), require = 0)
    private void gwyf$onBlockPlaced(net.minecraft.client.network.ClientPlayerEntity player,
                                    net.minecraft.util.Hand hand,
                                    net.minecraft.util.hit.BlockHitResult hitResult,
                                    CallbackInfoReturnable<net.minecraft.util.ActionResult> info) {
        if (!GwyfBridgeMod.isBridgeReady() || info.getReturnValue() == null) {
            return;
        }
        if (info.getReturnValue().isAccepted() == false) {
            return;
        }

        final BlockPos pos = hitResult.getBlockPos().offset(hitResult.getSide());
        final BlockState state = net.minecraft.client.MinecraftClient.getInstance().world != null
                ? net.minecraft.client.MinecraftClient.getInstance().world.getBlockState(pos)
                : null;

        if (GwyfBridgeMod.forwarder() != null) {
            GwyfBridgeMod.forwarder().forwardFromMixin(pos, state, BridgeRecords.ACTION_PLACE);
        }
    }
}
