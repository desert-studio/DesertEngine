#pragma once

// PER-MESH "BAKE IMPOSTOR" (WP-FAR-12F; UE ImpostorBaker run on one Static Mesh, its result saved beside the
// mesh): the Details action of a Static Mesh component bakes that mesh's octahedral impostor through the SAME
// capture Build HLODs uses for an Impostor HLOD layer (HLODApproximateBake with ONE impostor cell: Parts = this
// mesh with its material slots at one identity instance, Key = Core::HLODImpostorKey), so a mesh baked here and
// the same mesh baked by an Impostor HLOD share one key, one atlas and one card writer
// (StoreBakedImpostorCard). The atlas textures, the M_I material and the card mesh are written beside the mesh
// asset (HLODApproximateBakeCell::OutputDir), not among the built HLODs, so Delete HLODs never removes them.
// On completion the component's ImpostorMesh / ImpostorMaterial are set and ImpostorScreenSize, when unset,
// becomes kDefaultImpostorScreenSize; the entity is found again by its UUID in the scene active at that frame.
//
// The bake records into the editor's frames like Bake Material to Textures: TickImpostorBake once per frame.

#include <Common/Core/ResultStr.hpp>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::ECS
{
    class Entity;
}

namespace Desert::Editor::Baking
{
    // The screen size below which a freshly baked card is drawn, when the component states none (UE's impostor
    // LOD screen size default for a far prop).
    inline constexpr float kDefaultImpostorScreenSize = 0.05f;

    // Starts the bake of `entity`'s Static Mesh (an asset mesh: a primitive or an edited mesh has no asset to
    // bake beside). Refused by name when a bake runs, the scene is unsaved (the capture reads the saved world)
    // or the mesh is not an asset.
    [[nodiscard]] Common::BoolResultStr StartImpostorBakeOf( ::Desert::Core::Scene&      scene,
                                                             const ECS::Entity&          entity,
                                                             const Assets::AssetManager& assets );

    // One editor frame of the running bake; on completion stores the card and writes the component.
    void TickImpostorBake( const Assets::AssetManager& assets, ::Desert::Core::Scene* activeScene );

    [[nodiscard]] bool IsImpostorBakeRunning();
} // namespace Desert::Editor::Baking
