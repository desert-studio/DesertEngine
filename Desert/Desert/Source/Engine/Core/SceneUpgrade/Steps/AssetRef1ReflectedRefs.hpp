#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    // THE REFLECTED ASSET-REFERENCE FORMAT'S OWN CHAIN (Assets::kAssetRefSchemaTag), run after the scene's and
    // World Partition's.
    //   1 - STATIC MESHES AND MATERIALS BY {Guid, Path} (STATICMESH-FORMAT). The Static Mesh block's MeshGuid +
    //       MeshPath become its reflected `MeshHandle` {Guid, Path} and MaterialGuids + MaterialPaths its
    //       `MaterialSlots` [{Guid, Path}] (the GUIDs the file already states); every reflected material field
    //       stated as a bare key (LandscapeMaterial.Material, VolumetricCloud.Material,
    //       Destructible.InteriorMaterial, UIPanel.Material) becomes {Guid, Path}, the GUID the `.demat` header
    //       states (the caller's MaterialGuidLookup). Records and prefab overrides alike
    //       (MigrateReflectedAssetRefsV0ToV1).
    inline constexpr int kAssetRefVersionReflected = 1;

    // What MigrateReflectedAssetRefsV0ToV1 did to one file.
    struct ReflectedAssetRefsReport
    {
        std::size_t              MeshBlocks     = 0; // Static Mesh blocks (records and overrides) restated
        std::size_t              MaterialFields = 0; // bare material keys restated as {Guid, Path}
        std::vector<std::string> Refused;            // a material file with no header GUID, or a wrong shape
    };

    // Edits the records in place; PURE given @p materialGuid, which is asked once per distinct key.
    ReflectedAssetRefsReport MigrateReflectedAssetRefsV0ToV1( std::vector<Assets::EntityData>& entities,
                                                              const MaterialGuidLookup&        materialGuid );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunReflectedAssetRefs( UpgradeContext& context );
} // namespace Desert::Core
