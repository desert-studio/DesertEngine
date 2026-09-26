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
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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

    inline constexpr float kMinEV = -10.0f;
    inline constexpr float kMaxEV = 10.0f;

    // THE WINDOW'S VIEWING STATE in one struct, so that the ImGui row, the palette actions and the render
    // gate's fingerprint all read the same four fields. Never saved: it is how the file is looked at.
    struct ViewState
    {
        int        Level                                = 0;
        Projection View                                 = Projection::Sphere3D;
        float      ExposureEV                           = 0.0f;
        float      RotationDegrees                      = 0.0f; // kept in [-180, 180], the slider's range
        bool       operator==( const ViewState& ) const = default;
    };

    // What the preview's render gate is told about the fields the cube resolver samples (level, projection,
    // rotation), which the gate cannot see otherwise. EV is not here: it reaches the gate as the scene
    // setup's Exposure, which the gate already watches.
    [[nodiscard]] inline uint64_t Fingerprint( const ViewState& state )
    {
        uint32_t rotationBits = 0u;
        static_assert( sizeof( rotationBits ) == sizeof( state.RotationDegrees ) );
        std::memcpy( &rotationBits, &state.RotationDegrees, sizeof( rotationBits ) );
        return static_cast<uint64_t>( static_cast<uint32_t>( state.Level ) & 0xFFu ) |
               ( static_cast<uint64_t>( state.View ) << 8u ) | ( static_cast<uint64_t>( rotationBits ) << 16u );
    }

    // Short, stable palette names for the levels ("Level: Mip 5"): a script types these, so they carry no
    // roughness figure that would change with the chain's length.
    [[nodiscard]] inline std::string LevelActionLabel( int index, uint32_t prefilteredMips )
    {
        if ( index == DiffuseLevel( prefilteredMips ) )
            return "Level: Diffuse";
        return "Level: Mip " + std::to_string( index );
    }

    [[nodiscard]] inline float WrapDegrees( float degrees )
    {
        return std::remainder( degrees, 360.0f ); // [-180, 180]
    }

    // THE WINDOW'S MODES AS DATA (UE's TextureCube editor toolbar): the document turns each into an
    // ISubjectDocument::DocumentAction over its own ViewState, so DesertCtl and the palette reach exactly
    // what the ImGui row reaches. Only levels that exist for this chain are offered.
    struct ViewAction
    {
        std::string                       Label;
        std::function<void( ViewState& )> Apply;
    };

    [[nodiscard]] inline std::vector<ViewAction> ViewActions( uint32_t prefilteredMips )
    {
        std::vector<ViewAction> actions;
        actions.push_back( { "View: 3D", []( ViewState& s ) { s.View = Projection::Sphere3D; } } );
        actions.push_back( { "View: 2D", []( ViewState& s ) { s.View = Projection::LongLat2D; } } );
        for ( int i = 0; i < LevelCount( prefilteredMips ); ++i )
            actions.push_back( { LevelActionLabel( i, prefilteredMips ), [i]( ViewState& s ) { s.Level = i; } } );
        actions.push_back(
             { "EV +1", []( ViewState& s ) { s.ExposureEV = std::min( s.ExposureEV + 1.0f, kMaxEV ); } } );
        actions.push_back(
             { "EV -1", []( ViewState& s ) { s.ExposureEV = std::max( s.ExposureEV - 1.0f, kMinEV ); } } );
        actions.push_back( { "EV 0", []( ViewState& s ) { s.ExposureEV = 0.0f; } } );
        actions.push_back( { "Rotate +90 deg", []( ViewState& s )
                             { s.RotationDegrees = WrapDegrees( s.RotationDegrees + 90.0f ); } } );
        actions.push_back( { "Rotation 0", []( ViewState& s ) { s.RotationDegrees = 0.0f; } } );
        return actions;
    }
} // namespace Desert::Editor::SkyboxView
