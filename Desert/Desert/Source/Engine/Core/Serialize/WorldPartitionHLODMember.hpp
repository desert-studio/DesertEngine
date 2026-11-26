#pragma once
// WHAT ONE RECORD GIVES ITS CELL'S HLOD (WP-HLOD-HASH) - UE AActor::IsHLODRelevant and the per-component
// UHLODBuilder::ComputeHLODHash inputs (UHLODProxy::GetCRC: the component's mesh, materials and transform, not its
// serialized text), read TYPED, once, by the one function both the Instancing pass and the saved-HLOD checksum
// call. The checksum (WorldCells::SavedHLODStatus) therefore folds exactly what the builder reads: a field the
// builder never reads (a StaticMesh's Impostor*, a Foliage block's density) cannot make an HLOD stale, and a field
// it reads cannot be missed.
//
// WHAT IS READ, in the order the Instancing pass always read it:
//   Visibility.Visible false        - the record draws nothing; it is no member at all.
//   Foliage {FoliageTypeGuid, Path} - its type's IncludeInHLOD (FoliageHLODSource): left out, or an Unreadable
//   hole
//                                     when the type does not answer.
//   PrefabPath                      - refused by name: members are read only from an expanded world.
//   a module's own builder row      - (HLODBuilderComponents) a named hole; the module states its builder's inputs
//                                     (HLODBuilderComponent::Inputs), not this file.
//   SkinnedMesh                     - a named hole.
//   StaticMesh                      - one batch {mesh, material slots, primitive, CastShadows false only} at the
//                                     record's world matrix, or the hole its block names.
//   InstancedStaticMesh             - one batch of the block's own instances (CastShadows true reset).
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core::Rules
{
    // One instanced batch a member brings to an Instancing HLOD. Key is canonical: InstanceTransforms is empty
    // (the instances live in Instances) and CastShadows is stated only when false.
    struct HLODMemberBatch
    {
        Assets::InstancedStaticMeshComponentSer           Key;
        std::optional<std::vector<std::array<float, 16>>> Instances; // absent = one instance at the record's world
    };

    // The member's typed read. Relevant = it brings a batch or its module's own builder takes it (UE
    // IsHLODRelevant: a PlayerStart, a trigger, a light, a record that is only holes make no cell want an HLOD).
    struct HLODMember
    {
        bool                         Relevant = false;
        std::vector<HLODExclusion>   Holes;                // in the order BuildInstancingHLOD names them
        const HLODBuilderComponent*  OwnBuilder = nullptr; // the module row whose builder draws this record
        std::vector<HLODMemberBatch> Batches;
        bool                         FoliageLeftOut = false;
        // Every asset key the build reads for this member (a batch's mesh and materials, a custom-shader key's
        // materials, the foliage type): a registry GUID text, else the project path. Sorted, unique.
        std::vector<std::string> Content;
        // What did not read (a Visibility or mesh block of the wrong shape); the hole it made is in Holes.
        Common::Json::Issues Issues;
    };

    // What the builder may know from outside the record (the material's shader, the foliage type's flag).
    // Empty sources are stated conditions (see CustomShaderSource, FoliageHLODSource).
    struct HLODMemberSources
    {
        CustomShaderSource CustomShader;
        FoliageHLODSource  Foliage;
    };

    // Reads @p records[index] as an HLOD member. A record that still carries a PrefabPath is refused by name
    // ("an unexpanded instance of '<path>'"): members are read only from an expanded world (ExpandedWorld).
    [[nodiscard]] Common::ResultStr<HLODMember> ReadHLODMember( std::span<const Assets::EntityData> records,
                                                                std::span<const glm::mat4>          world,
                                                                std::size_t                         index,
                                                                const HLODMemberSources&            sources );

    // THE INSTANCING HLOD of the records @p members (one cell's, in ResidencyUnitMembers order). @p world is
    // every record's world matrix (Detail::ComposeWorld over the whole file, indexed like @p records). Built from
    // ReadHLODMember of each member and nothing else: batches in the order their first source appears in
    // @p members, instances in member order. A member that does not read is an Issue, named.
    [[nodiscard]] InstancingHLOD
    BuildInstancingHLOD( std::span<const Assets::EntityData> records, std::span<const glm::mat4> world,
                         std::span<const std::size_t> members, const CustomShaderSource& customShader,
                         Common::Json::Issues& issues, const FoliageHLODSource& foliage = {} );
} // namespace Desert::Core::Rules
