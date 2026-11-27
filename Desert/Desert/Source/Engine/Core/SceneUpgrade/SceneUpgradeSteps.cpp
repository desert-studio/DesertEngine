#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <Engine/Core/SceneUpgrade/SceneUpgrade.hpp>
#include <Engine/Core/SceneUpgrade/Steps/AssetRef1ReflectedRefs.hpp>
#include <Engine/Core/SceneUpgrade/Steps/Light1Rotation.hpp>
#include <Engine/Core/SceneUpgrade/Steps/PostProcess1Bloom.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V71VolumetricFog.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V72TracedRequest.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V73ComposedGI.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V74FilmicDefault.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V75GrassRasterPath.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V76LandscapeSplineEntities.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V77TubeSourceLength.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V78ExposureBiasOnly.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V79UnbuiltLightmaps.hpp>
#include <Engine/Core/SceneUpgrade/Steps/V80WaterBodyRetiredKeys.hpp>
#include <Engine/Core/SceneUpgrade/Steps/WP13EntityHLODLayer.hpp>
#include <Engine/Core/SceneUpgrade/Steps/WP14ImpostorLayers.hpp>

#include <array>

namespace Desert::Core
{
    namespace
    {
        // ADDING A STEP: one file under Steps/ and one row here, at the end of its chain. The static_asserts
        // below refuse a head (kSceneVersion, ...) that no row reaches and a row that skips or repeats a number.
        constexpr std::array kUpgradeSteps{
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionVolumetricFog, "VolumetricFog", &RunVolumetricFog },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionTracedRequest, "TracedRequest", &RunTracedRequest },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionComposedGI, "ComposedGI", &RunComposedGI },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionFilmicDefault, "FilmicDefault", &RunFilmicDefault },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionGrassRasterPath, "GrassRasterPath",
                          &RunGrassRasterPath },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionLandscapeSplineEntities, "LandscapeSplineEntities",
                          &RunLandscapeSplineEntities },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionTubeSourceLength, "TubeSourceLength",
                          &RunTubeSourceLength },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionExposureBiasOnly, "ExposureBiasOnly",
                          &RunExposureBiasOnly },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionUnbuiltLightmaps, "UnbuiltLightmaps",
                          &RunUnbuiltLightmaps },
             UpgradeStep{ UpgradeChain::Scene, kSceneVersionWaterBodyRetiredKeys, "WaterBodyRetiredKeys",
                          &RunWaterBodyRetiredKeys },
             UpgradeStep{ UpgradeChain::WorldPartition, kWorldPartitionVersionEntityHLODLayer, "EntityHLODLayer",
                          &RunEntityHLODLayer },
             UpgradeStep{ UpgradeChain::WorldPartition, kWorldPartitionVersionImpostorLayers, "ImpostorLayers",
                          &RunImpostorLayers },
             UpgradeStep{ UpgradeChain::AssetRef, kAssetRefVersionReflected, "ReflectedAssetRefs",
                          &RunReflectedAssetRefs },
             UpgradeStep{ UpgradeChain::Light, kLightVersionRotation, "LightRotation", &RunLightRotation },
             UpgradeStep{ UpgradeChain::PostProcess, kPostProcessVersionBloom, "PostProcessBloom",
                          &RunPostProcessBloom },
        };

        // The post-process chain runs after the light chain: its numbers are its tag's.
        constexpr std::array kChainOrder{ UpgradeChain::Scene, UpgradeChain::WorldPartition, UpgradeChain::AssetRef,
                                          UpgradeChain::Light, UpgradeChain::PostProcess };

        // The last row's number of @p chain (its floor when it has none).
        constexpr int LastStepOf( const UpgradeChain chain )
        {
            int last = UpgradeFloorOf( chain );
            for ( const UpgradeStep& step : kUpgradeSteps )
                if ( step.Chain == chain )
                    last = step.To;
            return last;
        }

        // The rows are grouped by chain in kChainOrder, and each chain's rows run floor + 1, floor + 2, ... with
        // no gap and no repeat.
        constexpr bool ChainsAreContiguous()
        {
            std::size_t row = 0;
            for ( const UpgradeChain chain : kChainOrder )
            {
                int expected = UpgradeFloorOf( chain ) + 1;
                for ( ; row < kUpgradeSteps.size() && kUpgradeSteps[row].Chain == chain; ++row, ++expected )
                    if ( kUpgradeSteps[row].To != expected )
                        return false;
            }
            return row == kUpgradeSteps.size();
        }
    } // namespace

    static_assert( ChainsAreContiguous(), "the upgrade table's rows must be grouped by chain (scene, world "
                                          "partition, asset references, lights, post process), each from its floor + 1 without "
                                          "a gap or a repeat (SceneUpgradeSteps.cpp)" );
    static_assert( LastStepOf( UpgradeChain::Scene ) == kSceneVersion,
                   "Core::kSceneVersion moved without its step: add Steps/V<N><Name>.{hpp,cpp} and its row here" );
    static_assert( LastStepOf( UpgradeChain::WorldPartition ) == kWorldPartitionVersion,
                   "World Partition's version moved without its step: add its file under Steps/ and its row here" );
    static_assert( LastStepOf( UpgradeChain::AssetRef ) == kAssetRefVersion,
                   "the asset-reference version moved without its step: add its file under Steps/ and its row here" );
    static_assert( LastStepOf( UpgradeChain::Light ) == kLightVersion,
                   "the light version moved without its step: add its file under Steps/ and its row here" );
    static_assert( LastStepOf( UpgradeChain::PostProcess ) == kPostProcessVersion,
                   "the post-process version moved without its step: add its file under Steps/ and its row here" );

    std::span<const UpgradeStep> UpgradeSteps()
    {
        return kUpgradeSteps;
    }
} // namespace Desert::Core
