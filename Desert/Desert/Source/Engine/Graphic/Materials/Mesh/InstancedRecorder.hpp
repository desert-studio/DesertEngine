#pragma once

#include <cstdint>

namespace Desert::Graphic
{
    // ── WHICH MATERIAL RECORDS AN INSTANCED BATCH ───────────────────────────────────────────────────
    //
    // A hardware-instanced draw is ONE `vkCmdDrawIndexed`, so it binds ONE descriptor set, so it draws
    // with ONE material's samplers. The per-instance differences that a batch CAN carry are the ones
    // that ride a buffer: the model matrix (InstanceTransforms) and the surface row (Materials[],
    // named by a push constant). A texture is neither — it is a descriptor.
    //
    // WHAT THIS EXISTS TO STOP, MEASURED. MeshRenderer used to record every auto-batched group and every
    // Instanced Static Mesh with ONE material it built for itself (`m_StaticInstancedMaterial` /
    // `m_InstancedGBufferMaterial`). That material has no `.demat` behind it, so MaterialFactory never
    // ran over it and every 2D sampler it declares holds the shader schema's default — a 1x1 WHITE
    // image. The consequence is a texture channel that is loaded, resident and drawn by nothing:
    //
    //   * on the 50 179-entity world scene, whose floor is 1024 cubes sharing `M_CheckerFloor.demat`,
    //     swapping that material's `u_AlbedoTexture` for a completely different image moved 0 of
    //     560 560 pixels, at all three elevations;
    //   * `UVTiling` 500 -> 1 moved 0 pixels too, because tiling the lookup of a 1x1 image is a no-op;
    //   * `AlbedoColor` -> red moved 77.78 % of the frame, because a colour rides the Materials[] row
    //     and a row IS per instance.
    //
    // So the material "worked" by every knob a reviewer is likely to try, and the failure looked like
    // aliasing (`Docs/World/WORLD_SCENE.md` §6 said so in writing) rather than like a lost binding.
    //
    // AND REBINDING THE SHARED MATERIAL PER BATCH IS NOT THE FIX. `VulkanMaterialBackend::ApplyTexture2D`
    // refuses a second write to the same (frame x slot x set) and calls `ReportSwallowedRebind`: a
    // descriptor set written twice in one frame would have the LAST batch's textures under every draw
    // recorded before it. One material per texture set is the only shape Vulkan allows here, which is
    // the same conclusion TerrainRenderer reached for its splat maps.
    //
    // The answer is therefore a PROPERTY OF THE GROUP, not of the renderer, and it is this enum.
    enum class InstancedRecorder : uint8_t
    {
        // The (Instanced x pass) sibling of the group's own `.demat`, built by MaterialFactory from the
        // same asset — same parameters, same textures, by construction rather than by anyone copying.
        AssetVariant,

        // The renderer's own spare. Correct for a group whose material came from NO asset — in practice
        // MeshECSSystem's default, standing in for a mesh whose slot did not resolve. It has no textures
        // to lose, which is exactly why it is safe here and nowhere else.
        RendererSpare,

        // This group cannot be instanced in this pass at all (the engine has no shader for the cell).
        // The caller must fall back to per-object draws, or REFUSE OUT LOUD if it has no per-object
        // path — an ISM is one entity carrying N transforms and has none. What it must not do is reach
        // for the spare: that draws the surface with a white 1x1 in every sampler and says nothing.
        None,
    };

    // @p groupHasAsset — MaterialService::Owns(group): does a `.demat` stand behind this material.
    // @p assetVariantBuilt — did MaterialService::GetVariant(group, Instanced, pass) answer non-null.
    //
    // Deliberately takes two bools and not two pointers: the decision is the whole of what was wrong,
    // and a decision expressed over pointers cannot be tested without a Vulkan device.
    constexpr InstancedRecorder SelectInstancedRecorder( bool groupHasAsset, bool assetVariantBuilt )
    {
        if ( !groupHasAsset )
            return InstancedRecorder::RendererSpare;

        return assetVariantBuilt ? InstancedRecorder::AssetVariant : InstancedRecorder::None;
    }
} // namespace Desert::Graphic
