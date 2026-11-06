#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <array>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        struct SceneViewSlotName
        {
            SceneViewSlot           Slot;
            std::string_view        Name;
            RDG::ShaderResourceKind Kind;
        };

        constexpr RDG::ShaderResourceKind kTex = RDG::ShaderResourceKind::Texture;
        constexpr RDG::ShaderResourceKind kBuf = RDG::ShaderResourceKind::StorageBuffer;

        // THE name and kind of every SceneViewSlot, in the enum's order (checked below).
        constexpr std::array<SceneViewSlotName, static_cast<size_t>( SceneViewSlot::Count )> kSceneViewSlotTable{ {
             { SceneViewSlot::ShadowCascades, kSceneViewShadowCascadesName, kTex },
             { SceneViewSlot::EnvIrradiance, kSceneViewEnvIrradianceName, kTex },
             { SceneViewSlot::EnvSpecular, kSceneViewEnvSpecularName, kTex },
             { SceneViewSlot::BrdfLut, kSceneViewBrdfLutName, kTex },
             { SceneViewSlot::CloudShadowMap, kSceneViewCloudShadowMapName, kTex },
             { SceneViewSlot::ReflectionCubes, kSceneViewReflectionCubesName, kTex },
             { SceneViewSlot::LocalShadowAtlas, kSceneViewLocalShadowAtlasName, kTex },
             { SceneViewSlot::SceneDepth, kSceneViewSceneDepthName, kTex },
             { SceneViewSlot::SunShadowMask, kSceneViewSunShadowMaskName, kTex },
             { SceneViewSlot::SunDistanceFieldShadow, kSceneViewSunDistanceFieldShadowName, kTex },
             { SceneViewSlot::LocalShadowMasks, kSceneViewLocalShadowMasksName, kTex },
             { SceneViewSlot::SceneShadingWord, kSceneViewSceneShadingWordName, kTex },
             { SceneViewSlot::MegaLightsVolumeR, kMegaLightsVolumeNames[0], kTex },
             { SceneViewSlot::MegaLightsVolumeG, kMegaLightsVolumeNames[1], kTex },
             { SceneViewSlot::MegaLightsVolumeB, kMegaLightsVolumeNames[2], kTex },
             { SceneViewSlot::RainOcclusionMap, kSceneViewRainOcclusionMapName, kTex },
             { SceneViewSlot::DBufferA, kSceneViewDBufferAName, kTex },
             { SceneViewSlot::DBufferB, kSceneViewDBufferBName, kTex },
             { SceneViewSlot::DBufferC, kSceneViewDBufferCName, kTex },
             { SceneViewSlot::RadianceCacheIrradiance, kSceneViewRadianceCacheIrradianceName, kTex },
             { SceneViewSlot::RadianceCacheDepth, kSceneViewRadianceCacheDepthName, kTex },
             { SceneViewSlot::FogAerialPerspective, kSceneFogAerialPerspectiveName, kTex },
             { SceneViewSlot::FogDistantSkyLight, kSceneFogDistantSkyLightName, kTex },
             { SceneViewSlot::FogVolume, kSceneFogVolumeName, kTex },
             { SceneViewSlot::SceneColorTexture, kPassSceneColorTextureName, kTex },
             { SceneViewSlot::FrontLayerReflectionTexture, kPassFrontLayerReflectionTextureName, kTex },
             { SceneViewSlot::ReflectionList, kSceneViewReflectionListName, kBuf },
             { SceneViewSlot::LightGridParams, kLightGridParamsBlockName, kBuf },
             { SceneViewSlot::LightGridCells, kLightGridCellsBlockName, kBuf },
             { SceneViewSlot::LocalShadowViews, kLocalShadowViewsBlockName, kBuf },
             { SceneViewSlot::MegaLightsVolumeParams, kMegaLightsVolumeBlockName, kBuf },
             { SceneViewSlot::VsmFrameData, kVsmFrameDataBlockName, kBuf },
             { SceneViewSlot::VsmPageTable, kVsmPageTableBlockName, kBuf },
             { SceneViewSlot::VsmLocalLights, kVsmLocalLightsBlockName, kBuf },
             { SceneViewSlot::RadianceCacheParams, kSceneViewRadianceCacheParamsName, kBuf },
             { SceneViewSlot::RadianceCacheIndirection, kSceneViewRadianceCacheIndirectionName, kBuf },
             { SceneViewSlot::RadianceCacheProbes, kSceneViewRadianceCacheProbesName, kBuf },
             { SceneViewSlot::FogParams, kSceneFogParamsBlockName, kBuf },
             { SceneViewSlot::GpuSceneInstances, SceneResources::kGpuSceneInstances.Name, kBuf },
             { SceneViewSlot::GpuSceneVisibleInstances, SceneResources::kGpuSceneVisibleInstances.Name, kBuf },
             { SceneViewSlot::GpuSceneDrawSlots, SceneResources::kGpuSceneDrawSlots.Name, kBuf },
             { SceneViewSlot::GpuSceneSubmeshTransforms, SceneResources::kGpuSceneSubmeshTransforms.Name, kBuf },
             { SceneViewSlot::VgRasterClusters, SceneResources::kVgRasterClusters.Name, kBuf },
             { SceneViewSlot::VgRasterDags, SceneResources::kVgRasterDags.Name, kBuf },
             { SceneViewSlot::VgRasterVisibleClusters, SceneResources::kVgRasterVisibleClusters.Name, kBuf },
             { SceneViewSlot::VgRasterPrimitiveDraws, SceneResources::kVgRasterPrimitiveDraws.Name, kBuf },
             { SceneViewSlot::VisibilityBuffer, SceneResources::kVisibilityBuffer.Name, kBuf },
             { SceneViewSlot::VsmInstanceRemap, kPassVsmInstanceRemapName, kBuf },
             { SceneViewSlot::ParticleRibbonOrder, kPassParticleRibbonOrderName, kBuf },
             { SceneViewSlot::VsmPhysicalPool, kVsmPhysicalPoolName, RDG::ShaderResourceKind::StorageTexture },
        } };

        [[nodiscard]] constexpr bool TableIsInEnumOrder()
        {
            for ( size_t i = 0; i < kSceneViewSlotTable.size(); ++i )
                if ( static_cast<size_t>( kSceneViewSlotTable[i].Slot ) != i ||
                     kSceneViewSlotTable[i].Name.empty() )
                    return false;
            return true;
        }
        static_assert( TableIsInEnumOrder(), "kSceneViewSlotTable row i must name SceneViewSlot i" );
    } // namespace

    SceneViewSlots SceneViewSlotsOf( const RDG::ShaderBindingLayout& layout )
    {
        SceneViewSlots slots;
        for ( const RDG::ShaderSlot& slot : layout.Slots )
            for ( const SceneViewSlotName& row : kSceneViewSlotTable )
                if ( slot.Kind == row.Kind && slot.Name == row.Name )
                    slots.Bits |= uint64_t( 1 ) << static_cast<uint32_t>( row.Slot );
        return slots;
    }

    SceneViewSlots SceneViewSlotsOf( const RDG::OtherRouteFill& fill )
    {
        SceneViewSlots slots;
        for ( const std::string& name : fill.Slots )
            for ( const SceneViewSlotName& row : kSceneViewSlotTable )
                if ( name == row.Name )
                    slots.Bits |= uint64_t( 1 ) << static_cast<uint32_t>( row.Slot );
        return slots;
    }

    SceneViewSlots RouteFillSlotsCache::Get( const std::shared_ptr<const RDG::OtherRouteFill>& fill )
    {
        if ( fill.get() != m_Fill.get() )
        {
            m_Fill  = fill;
            m_Slots = fill ? SceneViewSlotsOf( *fill ) : SceneViewSlots{};
        }
        return m_Slots;
    }

    const SlottedLayout& ShaderBindingLayoutCache::GetSlotted( const std::shared_ptr<Shader>& shader )
    {
        const std::shared_ptr<const RDG::ShaderBindingLayout>& layout =
             m_Cache.Get( shader, shader->GetCodeGeneration(),
                          []( const Shader& s ) { return Renderer::GetBindingLayout( s ); } );
        if ( layout.get() != m_Slotted.Layout.get() )
        {
            m_Slotted.Layout = layout;
            m_Slotted.Slots  = layout ? SceneViewSlotsOf( *layout ) : SceneViewSlots{};
        }
        return m_Slotted;
    }
} // namespace Desert::Graphic
