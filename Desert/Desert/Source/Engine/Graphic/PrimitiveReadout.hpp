#pragma once
/**
 * @file PrimitiveReadout.hpp
 * @brief What the renderer did with ONE entity in one frame, as values: the gameplay debugger's Rendering
 *        category (key 4) reads it.
 *
 * ── WHY THE RENDERER WRITES IT AND THE DEBUGGER ONLY READS IT ───────────────────────────────────────────────
 *
 * The debugger collects in Scene::OnUpdate, while the mesh renderer's queues are EMPTY: they fill during the
 * render phase and SceneRenderer::EndScene clears them. So the category cannot look at the queues; it asks
 * (SceneRenderer::WatchPrimitive) and the renderer answers at EndScene, before the queues are cleared, from the
 * same records every pass drew from - one frame late, as UE's collect -> replicate is. Nothing here is computed
 * twice: the LOD is MeshRenderer::ComputeLOD, the material is MeshRenderer::ShadingOf, the frustum test is
 * IsVisibleInView - the calls the passes make.
 */

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Common/Core/Math/AABB.hpp>

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    /// One draw record of the watched entity: one queue entry (a static entry is one material slot's sections).
    struct PrimitiveDrawReadout
    {
        enum class Queue : uint8_t
        {
            Static,
            Skinned,
            Generic,
            Instanced
        };
        /// The view pass that shades it: the G-buffer base pass, the forward pass, or the translucency pass.
        enum class Pass : uint8_t
        {
            BasePass,
            Forward,
            Translucency
        };

        Queue    Kind        = Queue::Static;
        uint32_t SectionSlot = 0;  // static: the material slot this draw shades with; skinned: the section index
        uint32_t Lod         = 0;  // MeshRenderer::ComputeLOD, as the passes draw it
        uint32_t LodCount    = 1;  // levels the mesh has (its LOD screen sizes)
        int      ForcedLod   = -1; // -1 = auto
        int      LodBias     = 0;
        // An ISM picks its level per instance (CollectIsmInstances): Lod is then not one value.
        bool        LodPerInstance = false;
        Pass        ShadingPass    = Pass::Forward;
        std::string MaterialInstance; // the slot instance's name; empty = none
        std::string Template;         // the surface template's shader name (or the Shader Override's)
        std::string ShadingModel;     // the template's shading model by name; empty = not a surface program
        Core::Formats::SurfaceBlendMode Blend       = Core::Formats::SurfaceBlendMode::Opaque;
        bool                            CastShadows = false;
        bool                            InFrustum = false; // the main camera's frustum, on the mesh's authored box
        Common::Math::AABB              WorldBounds;
        uint32_t                        Instances = 1; // an ISM's instance count
        uint32_t GpuPrimitive = 0; // its GpuScene primitive (0 for an ISM: one row per instance)
    };

    /// How this view-frame shadowed the sun.
    enum class SunShadowReadout : uint8_t
    {
        None,
        Cascaded,
        Virtual,
        RayTraced
    };

    struct PrimitiveReadout
    {
        uint32_t                          Entity = 0;
        uint64_t                          Frame  = 0; // the absolute frame it was filled in
        SunShadowReadout                  Sun    = SunShadowReadout::None;
        std::vector<PrimitiveDrawReadout> Draws; // empty: the renderer drew nothing of it this frame
    };
} // namespace Desert::Graphic
#endif // DESERT_DEV_INSTRUMENTS
