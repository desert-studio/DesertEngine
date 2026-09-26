#pragma once

// WHAT THE SKYBOX VIEWER CAN SHOW, as data: UE's TextureCube editor pattern (a Mip picker, a 2D/3D toggle and
// an EV exposure), mapped onto the three cubes the environment cache already holds (Graphic::Environment:
// Radiance, the GGX-prefiltered chain, Irradiance). Header-only and Vulkan-free so the level list is tested
// without a device (Tests/Editor/SkyboxViewLevels); the document only turns a Level into a cube to sample.
//
// THE LIST: entry 0 is the Radiance cube (the file as authored, and what a mirror at roughness 0 reflects);
// entries 1..N-1 are Prefiltered mips 1..N-1 (what a surface of that roughness reflects); the last entry is
// the Irradiance cube, which is a separate cube rather than a mip, so it closes the list the way UE's picker
// ends at its last level. Prefiltered mip 0 is not listed: it is the Radiance cube at a lower face size.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace Desert::Editor::SkyboxView
{
    enum class Projection
    {
        Sphere3D, // the mirror ball on the cube's own backdrop, orbited
        LongLat2D // the whole cube unwrapped onto the pane in the panorama's own layout
    };

    enum class Cube
    {
        Radiance,
        Prefiltered,
        Irradiance
    };

    struct Level
    {
        Cube  Source = Cube::Radiance;
        float Lod    = 0.0f; // the mip read with textureLod; 0 for Radiance and Irradiance
        bool  operator==( const Level& ) const = default;
    };

    // Entries in the picker for a prefiltered chain of `prefilteredMips` levels. A chain the cache has not
    // produced yet (0 mips) still lists Radiance and Diffuse: those two cubes do not depend on it.
    [[nodiscard]] constexpr int LevelCount( uint32_t prefilteredMips )
    {
        return static_cast<int>( std::max( prefilteredMips, 1u ) ) + 1;
    }

    [[nodiscard]] constexpr int DiffuseLevel( uint32_t prefilteredMips )
    {
        return LevelCount( prefilteredMips ) - 1;
    }

    // nullopt for an index outside the list: the caller names the error, nothing is clamped silently.
    [[nodiscard]] constexpr std::optional<Level> ResolveLevel( int index, uint32_t prefilteredMips )
    {
        if ( index < 0 || index >= LevelCount( prefilteredMips ) )
            return std::nullopt;
        if ( index == 0 )
            return Level{ Cube::Radiance, 0.0f };
        if ( index == DiffuseLevel( prefilteredMips ) )
            return Level{ Cube::Irradiance, 0.0f };
        return Level{ Cube::Prefiltered, static_cast<float>( index ) };
    }

    // The roughness a prefiltered mip stands for, by the rule every lit surface reads the chain with
    // (Shaders/Mesh/AmbientIBL.glslh: lod = roughness * textureQueryLevels), inverted.
    [[nodiscard]] constexpr float MipRoughness( int mip, uint32_t prefilteredMips )
    {
        return prefilteredMips == 0u ? 0.0f : static_cast<float>( mip ) / static_cast<float>( prefilteredMips );
    }

    [[nodiscard]] inline std::string LevelLabel( int index, uint32_t prefilteredMips )
    {
        const auto level = ResolveLevel( index, prefilteredMips );
        if ( !level )
            return "Level " + std::to_string( index ) + " (outside the list)";
        switch ( level->Source )
        {
            case Cube::Radiance:
                return "Mip 0 (Radiance)";
            case Cube::Irradiance:
                return "Diffuse (Irradiance)";
            case Cube::Prefiltered:
                break;
        }
        char text[64];
        std::snprintf( text, sizeof( text ), "Mip %d (roughness %.2f)", index,
                       static_cast<double>( MipRoughness( index, prefilteredMips ) ) );
        return text;
    }

    // UE's EV: each step doubles the light. Applied as the scene's exposure, i.e. at sampling time; the
    // cubes are never rebaked for it.
    [[nodiscard]] inline float ExposureFromEV( float ev )
    {
        return std::exp2( ev );
    }
} // namespace Desert::Editor::SkyboxView
