#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace Common::Content
{
    // WHAT KINDS OF CONTENT THIS ENGINE KNOWS, and the one spelling of each kind's name (the registry's
    // `kind` column). Where a kind lives and what extension names it is ContentPaths.hpp; this header
    // carries identity only, so the many files that name a kind stay cheap to compile.
    enum class ContentKind : std::size_t
    {
        StaticMesh, SkinnedMesh, Skeleton, Animation,
        Texture, Material, Skybox, Shader,
        CloudNoiseVolume, CloudType, CloudModellingVolume, CloudLayout,
        UITheme, ControlRig, AnimGraph, Retarget,
        FoliageType, LandscapeLayerInfo, StringTable, WorldCell,
        WorldIndex, Scene, Prefab, Redirector,
        LevelSequence, VFXSystem, Fracture, WaterWaves,
        InputAction, InputMappingContext, PhysicsAsset, Sound,
        VFXDataChannel, LandscapeGrassType, WorldParameters, WeatherPreset,
        VFXEffectType, StateTree, SubsurfaceProfile, VFXModule,
        IesProfile, SparseVolume, DataAsset,
        COUNT,
    };

    inline constexpr std::size_t CONTENT_KIND_COUNT = static_cast<std::size_t>( ContentKind::COUNT );

    // Index-matched to ContentKind; a missing name fails the size check below.
    inline constexpr std::array<std::string_view, CONTENT_KIND_COUNT> CONTENT_KIND_NAMES = { {
        "StaticMesh", "SkinnedMesh", "Skeleton", "Animation",
        "Texture", "Material", "Skybox", "Shader",
        "CloudNoiseVolume", "CloudType", "CloudModellingVolume", "CloudLayout",
        "UITheme", "ControlRig", "AnimGraph", "Retarget",
        "FoliageType", "LandscapeLayerInfo", "StringTable", "WorldCell",
        "WorldIndex", "Scene", "Prefab", "Redirector",
        "LevelSequence", "VFXSystem", "Fracture", "WaterWaves",
        "InputAction", "InputMappingContext", "PhysicsAsset", "Sound",
        "VFXDataChannel", "LandscapeGrassType", "WorldParameters", "WeatherPreset",
        "VFXEffectType", "StateTree", "SubsurfaceProfile", "VFXModule",
        "IesProfile", "SparseVolume", "DataAsset",
    } };

    [[nodiscard]] constexpr bool AllKindsNamed()
    {
        for ( const std::string_view name : CONTENT_KIND_NAMES )
            if ( name.empty() )
                return false;
        return true;
    }
    static_assert( AllKindsNamed(), "every ContentKind needs its name in CONTENT_KIND_NAMES" );

    [[nodiscard]] constexpr std::string_view KindName( ContentKind kind )
    {
        return CONTENT_KIND_NAMES[static_cast<std::size_t>( kind )];
    }
} // namespace Common::Content
