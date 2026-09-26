#pragma once

// PREVIEW SCENE SETTINGS — the environment the asset-editor previews (Material Editor, Static Mesh viewer,
// the Details mesh preview) show their subject in, as UE's Preview Scene Settings.
//
// THEY BELONG TO THE EDITOR, NOT TO THE ASSET. Which HDR a person likes to judge chrome under is the same
// kind of answer as a docking layout: two people on one project hold it differently at the same moment,
// so it is one record in EditorPreferences (editor.json) — shared by every preview window, remembered
// between runs — and never a field of a material or a mesh.
//
// NO NEW BAKE. The chosen HDR lights the preview through the IBL EnvironmentCache already baked for it;
// rotation and EV reach the picture as a Graphic::SkyLook, applied where the cubes are SAMPLED, so dragging
// either costs a uniform write.
//
// Pure (no Vulkan, no asset manager): the registry lookup is passed in, so the suite
// Desert/Tests/Editor/PreviewEnvironment exercises every rule here on its own.

#include <Engine/Graphic/Environment/SkyLook.hpp>

#include <cmath>
#include <functional>
#include <optional>
#include <string>

namespace Desert::Editor::PreviewEnvironment
{
    inline constexpr float kMinEV = -10.0f;
    inline constexpr float kMaxEV = 10.0f;

    struct Settings
    {
        // The HDR skybox asset, by its project-relative path — the key a person can read in editor.json
        // and the one the registry answers FindByPath with. EMPTY means the preview's own preset sky.
        std::string Skybox;
        // Turn of the environment about the world's up axis, degrees, kept in [-180, 180].
        float RotationDegrees = 0.0f;
        // Exposure of the environment in stops: radiance is scaled by 2^EV at sampling time.
        float ExposureEV = 0.0f;
        // Draw the environment behind the subject. Off hides the backdrop only; the IBL keeps lighting.
        bool ShowEnvironment = true;
        bool ShowFloor       = true;

        bool operator==( const Settings& ) const = default;
    };

    [[nodiscard]] inline float WrapDegrees( float degrees )
    {
        return std::remainder( degrees, 360.0f );
    }

    [[nodiscard]] inline float ClampEV( float ev )
    {
        return std::fmin( std::fmax( ev, kMinEV ), kMaxEV );
    }

    // The ONE conversion from these settings to what the renderer samples the HDR with.
    [[nodiscard]] inline Graphic::SkyLook LookOf( const Settings& settings )
    {
        Graphic::SkyLook look;
        look.RotationDegrees = WrapDegrees( settings.RotationDegrees );
        look.Intensity       = std::exp2( ClampEV( settings.ExposureEV ) );
        return look;
    }

    // What the registry answers for a path: the asset's handle, or nothing when no SkyboxAsset lives there.
    using SkyboxLookup = std::function<std::optional<uint64_t>( const std::string& path )>;

    struct Resolved
    {
        std::optional<uint64_t> Skybox; // nullopt = the preset sky
        std::string             Error;  // non-empty = refused; names the path
    };

    // Resolve the stored path. An empty path is the preset sky, not an error. A path the registry does not
    // know is REFUSED with the path in the message — never silently turned into the preset sky, which is
    // how a renamed .hdr would make every preview look "fine" under the wrong light.
    [[nodiscard]] inline Resolved Resolve( const Settings& settings, const SkyboxLookup& lookup )
    {
        if ( settings.Skybox.empty() )
            return {};
        if ( auto handle = lookup( settings.Skybox ) )
            return { handle, {} };
        return { std::nullopt, "[PreviewScene] no HDR skybox asset at '" + settings.Skybox +
                                    "' (editor.json PreviewScene.Skybox); pick one in Preview Scene Settings" };
    }
} // namespace Desert::Editor::PreviewEnvironment
