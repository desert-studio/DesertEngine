#pragma once

#include <Engine/Graphic/RDG/RDGLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

#include <cstdint>
#include <initializer_list>
#include <memory>

namespace Desert::Graphic
{
    class Shader;

    // DECALS-DBUFFER R5. One bit per resource slot a scene pass binds by name onto a block when the block's
    // layout declares it (FrameGraphRefs.hpp SceneViewDetail::BindWhere, BindSceneViewInputs, BindSceneFogInputs,
    // BindVirtualShadowMapReads; the mesh draw list's GPU-scene and VG raster tables). The name and the resource
    // kind of each bit are written once, in ShaderBindingLayoutCache.cpp kSceneViewSlotTable (indexed by this
    // enum), and resolved against a layout ONCE, when the layout is derived (SceneViewSlotsOf from
    // ShaderBindingLayoutCache::Get): a pass's per-frame setup tests bits, it never compares slot names (in UE a
    // shader's parameter slots are resolved once per shader, never by name per draw).
    enum class SceneViewSlot : uint8_t
    {
        ShadowCascades,
        EnvIrradiance,
        EnvSpecular,
        BrdfLut,
        CloudShadowMap,
        ReflectionCubes,
        LocalShadowAtlas,
        SceneDepth,
        SunShadowMask,
        SunDistanceFieldShadow,
        LocalShadowMasks,
        SceneShadingWord,
        MegaLightsVolumeR,
        MegaLightsVolumeG,
        MegaLightsVolumeB,
        RainOcclusionMap,
        DBufferA,
        DBufferB,
        DBufferC,
        RadianceCacheIrradiance,
        RadianceCacheDepth,
        FogAerialPerspective,
        FogDistantSkyLight,
        FogVolume,
        // Storage buffers.
        ReflectionList,
        LightGridParams,
        LightGridCells,
        LocalShadowViews,
        MegaLightsVolumeParams,
        VsmFrameData,
        VsmPageTable,
        VsmLocalLights,
        RadianceCacheParams,
        RadianceCacheIndirection,
        RadianceCacheProbes,
        FogParams,
        GpuSceneInstances,
        GpuSceneVisibleInstances,
        GpuSceneDrawSlots,
        GpuSceneSubmeshTransforms,
        VgRasterClusters,
        VgRasterDags,
        VgRasterVisibleClusters,
        VgRasterPrimitiveDraws,
        // A storage image.
        VsmPhysicalPool,
        Count
    };
    static_assert( static_cast<uint32_t>( SceneViewSlot::Count ) <= 64, "SceneViewSlots is one 64-bit mask" );

    // The slots of SceneViewSlot a layout declares (with the kind the table names), as a mask.
    struct SceneViewSlots
    {
        uint64_t Bits = 0;

        [[nodiscard]] constexpr bool Has( SceneViewSlot slot ) const
        {
            return ( ( Bits >> static_cast<uint32_t>( slot ) ) & 1u ) != 0;
        }
        [[nodiscard]] constexpr bool HasAny( SceneViewSlots others ) const
        {
            return ( Bits & others.Bits ) != 0;
        }
        constexpr void Clear( SceneViewSlot slot )
        {
            Bits &= ~( uint64_t( 1 ) << static_cast<uint32_t>( slot ) );
        }
        [[nodiscard]] static constexpr SceneViewSlots Of( std::initializer_list<SceneViewSlot> slots )
        {
            SceneViewSlots mask;
            for ( const SceneViewSlot slot : slots )
                mask.Bits |= uint64_t( 1 ) << static_cast<uint32_t>( slot );
            return mask;
        }
    };

    // The walk of @p layout's slots against kSceneViewSlotTable: called where a layout is derived (once per
    // shader object and reload), and by a caller that holds a layout of its own (a test, a pass that edits a
    // copy of its layout) - never per block per frame on a kept layout.
    [[nodiscard]] SceneViewSlots SceneViewSlotsOf( const RDG::ShaderBindingLayout& layout );

    // A kept layout and its slot mask, derived together.
    struct SlottedLayout
    {
        std::shared_ptr<const RDG::ShaderBindingLayout> Layout;
        SceneViewSlots                                  Slots;
    };

    // The binding layout a graph node declares its block against (RDG::PassBuilder::Bindings), kept with the
    // renderer that owns the shader. Derived from the shader's reflection (Renderer::GetBindingLayout, a walk of
    // every slot) the first time it is asked for and again only when the shader is another object or recompiled
    // (a hot reload bumps Shader::GetCodeGeneration) - RDG::LayoutCache - so a node's per-frame setup hands the
    // graph a kept layout by pointer instead of re-walking the reflection. One cache per shader slot of a
    // renderer.
    //
    // Get is defined in ShaderBindingLayoutCache.cpp so this header - included by every renderer header that keeps
    // a layout, UIMaterialCache.hpp among them - does not pull Renderer.hpp into them (RenderGraphCompile
    // ShaderBindingLayoutCacheHeaderStaysLight).
    class ShaderBindingLayoutCache
    {
    public:
        [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
        Get( const std::shared_ptr<Shader>& shader )
        {
            return GetSlotted( shader ).Layout;
        }
        // The layout with its SceneViewSlots, the mask re-walked only when the layout was re-derived.
        [[nodiscard]] const SlottedLayout& GetSlotted( const std::shared_ptr<Shader>& shader );

        // The pass's parameter struct against @p shader's layout (RDG::ValidateShaderParameters), called where
        // the pipeline is created so a renamed or dropped parameter fails at load, by name.
        [[nodiscard]] Common::BoolResultStr Validate( const std::shared_ptr<Shader>&        shader,
                                                      std::span<const RDG::ShaderParameter> parameters )
        {
            if ( !shader )
                return Common::MakeError( std::string( "the pass has no shader to check its parameters against" ) );
            return RDG::ValidateShaderParameters( *Get( shader ), parameters );
        }

    private:
        RDG::LayoutCache m_Cache;
        // m_Slotted.Layout holds the layout its mask was walked from, so a re-derived layout (allocated while
        // this one is still held) never shares its address: the address compare in GetSlotted is exact.
        SlottedLayout m_Slotted;
    };

    // The layouts of a node that declares one block PER SHADER its draws record with (a mesh draw list: every
    // material executor + recording pipeline pair is a block): one kept layout per shader object
    // (RDG::BasicLayoutCacheSet), each re-derived only on that shader's reload, never per frame. Get keys on the
    // RECORDING pipeline's shader (`pipeline->GetSpecification().Shader`). DropExpired once per frame.
    class ShaderBindingLayoutSet
    {
    public:
        [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
        Get( const std::shared_ptr<Shader>& shader )
        {
            return GetSlotted( shader ).Layout;
        }
        [[nodiscard]] const SlottedLayout& GetSlotted( const std::shared_ptr<Shader>& shader )
        {
            return m_Caches.At( shader ).GetSlotted( shader );
        }

        void DropExpired()
        {
            m_Caches.DropExpired();
        }

    private:
        RDG::BasicLayoutCacheSet<ShaderBindingLayoutCache> m_Caches;
    };
} // namespace Desert::Graphic
