#pragma once
// PRIVATE to WorldCells.cpp and WorldCellsHLODStatus.cpp - included nowhere else. What one HLOD node is built from
// (HLODWorldSpec: the build builds it, the status hashes it) and the judging of a world's saved HLODs against it
// (WP-HLOD-HASH). The public answer is WorldCells::SavedHLODStatus (WorldCells.hpp).

#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODHierarchy.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODMember.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Core::WorldCells::Detail
{
    // A world cell's file name (Rules::CellStem + kCellExtension) and a parent HLOD's file stem (WorldCells.cpp).
    [[nodiscard]] std::string CellFileName( const WorldPartitionSerialized& partition,
                                            const Rules::PlannedCell&       cell );
    [[nodiscard]] std::string ParentHLODFileStem( const WorldPartitionSerialized& partition,
                                                  const Rules::HLODHierarchy&     hierarchy,
                                                  const Rules::HLODNode&          node );

    // What a build and a staleness check plan alike: the plan from the descriptors, the members and name of
    // every unit, the records by id. Refusals are CookWorld's, worded the same.
    struct HLODPlan
    {
        Rules::WorldPartitionPlan                     Plan;
        std::unordered_map<Common::UUID, std::size_t> ById;
        std::vector<std::vector<std::size_t>>         Members;
        std::vector<std::string>                      Names;
    };

    [[nodiscard]] Common::ResultStr<HLODPlan>
    PlanHLODs( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
               std::span<const Common::Utils::AssetRegistry> registries );

    // "HLOD_<cell file stem>.dhlod".
    [[nodiscard]] std::string BuiltHLODFileName( const WorldPartitionSerialized& partition,
                                                 const Rules::PlannedCell&       cell );

    // WHAT ONE HLOD NODE IS BUILT FROM (BuildPlannedHLODs builds exactly this, SavedHLODStatus hashes exactly
    // this): its members, their parts by layer, the distance it is seen from and the names its file and ids
    // go by.
    struct HLODNodeSpec
    {
        std::vector<std::size_t>         Members;
        std::vector<Rules::CellHLODPart> Parts;
        float                            MinVisibleDistance = 0.0f;
        std::string                      CookedName; // names its records' ids (HLODRecordId)
        std::string                      FileName;   // its saved .dhlod
        bool                             Wanted = false;
    };

    struct HLODWorldSpec
    {
        HLODPlan                                      Planned;
        Rules::HLODHierarchy                          Hierarchy;
        std::vector<glm::mat4>                        World;
        std::vector<HLODNodeSpec>                     Nodes;   // indexed like Hierarchy.Nodes
        std::vector<std::optional<Rules::HLODMember>> Members; // by record; read once, for cell members only
    };

    // The spec of every node of the EXPANDED @p scene: the plan (PlanHLODs), the layers' validity, the
    // hierarchy, every cell member read once (ReadHLODMember) and every node's parts. A depth-1 node is
    // wanted when a member of its cell is HLOD-relevant (UE CreateHLODActors skips a cell without one), a
    // parent when any cell beneath it is. Refused, by name, for a member that does not read and a composite
    // naming a layer its grid cannot build.
    [[nodiscard]] Common::ResultStr<HLODWorldSpec>
    SpecHLODs( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
               std::span<const Common::Utils::AssetRegistry> registries );

    // Every node of @p spec judged against @p index (null = nothing saved): fills @p status's Nodes and Orphans.
    [[nodiscard]] Common::BoolResultStr StatusOf( HLODWorldStatus& status, const HLODWorldSpec& spec,
                                                  const SceneSerialized& scene, const BuiltHLODIndex* index,
                                                  std::span<const Common::Utils::AssetRegistry> registries,
                                                  const Rules::HLODBuilderRegistry&             builders );
} // namespace Desert::Core::WorldCells::Detail
