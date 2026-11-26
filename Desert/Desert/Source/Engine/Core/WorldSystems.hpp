#pragma once

// THE SYSTEMS OF A WORLD, ONE REGISTRY (UE: a world subsystem is registered once and every world kind that wants it
// gets it). The engine's rows are in WorldSystems.cpp, a module adds its own to ModuleHooks().WorldSystems; a host
// makes a world's systems with CreateWorldSystems and nothing else, so no host keeps a list to drift from.

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Animation
{
    class AnimationLibrary;
}

namespace Desert::Core
{
    class Scene;

    // UE EWorldType. A bit each: a registration names the kinds it runs in.
    enum class WorldKind : std::uint8_t
    {
        Game             = 1 << 0, // the packaged game's world
        Document         = 1 << 1, // an editor level document (edited, and played in place)
        Preview          = 1 << 2, // an asset preview: its panel is the only writer of a pose
        Capture          = 1 << 3, // a bake, a thumbnail, a render-texture view: draws, never plays
        DedicatedServer  = 1 << 4,
        AnimationPreview = 1 << 5, // a preview whose animation graph runs as in a level
    };
    using WorldKindMask = std::uint8_t;

    [[nodiscard]] constexpr WorldKindMask MaskOf( WorldKind kind )
    {
        return static_cast<WorldKindMask>( kind );
    }

    inline constexpr WorldKindMask kPlayedWorlds   = MaskOf( WorldKind::Game ) | MaskOf( WorldKind::Document );
    inline constexpr WorldKindMask kAnimatedWorlds = kPlayedWorlds | MaskOf( WorldKind::AnimationPreview );
    inline constexpr WorldKindMask kDrawnWorlds =
         kAnimatedWorlds | MaskOf( WorldKind::Preview ) | MaskOf( WorldKind::Capture );

    // The stages run in this order every frame.
    enum class WorldStage : std::uint8_t
    {
        Geometry,    // meshes, text
        Environment, // time of day writes the sun BEFORE the sky and the lights read it
        ModuleDraw,  // landscape, then the water over it
        Lights,
        Pose,     // animation, then attachments follow the bone
        Think,    // scripts, game mode: they set the move intent
        Simulate, // physics, then the facts it raised, then locomotion reads them
        Present,  // audio
        Sequence, // level sequences win over physics and locomotion
    };

    // What a system needs from the host; null when the host has none. A row states what it cannot run without.
    struct WorldServices
    {
        Assets::AssetManager*        Assets     = nullptr;
        Animation::AnimationLibrary* Animations = nullptr;
    };

    struct WorldSystemRegistration
    {
        const char*   Name            = "";
        WorldStage    Stage           = WorldStage::Geometry;
        std::uint16_t Order           = 0; // inside the stage; unique there
        WorldKindMask Kinds           = 0;
        bool          NeedsAssets     = false;
        bool          NeedsAnimations = false;
        void ( *Add )( Scene& scene, const WorldServices& services ) = nullptr;
    };

    // The engine's own rows (WorldSystems.cpp); StartupEngineModules calls it before any module.
    void RegisterEngineWorldSystems();

    // Every registered system @p scene's kind runs, in (Stage, Order). Refuses by name: two rows with one key, a
    // row without Add, a required service the host did not pass, a scene that already has its systems.
    [[nodiscard]] Common::BoolResultStr CreateWorldSystems( Scene& scene, const WorldServices& services );
} // namespace Desert::Core
