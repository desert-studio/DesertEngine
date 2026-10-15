#pragma once

// CLOTH ASSET DATA — what a piece of simulated clothing IS, independent of who simulates it.
//
// UE counterpart: UClothingAssetBase / UClothingAssetCommon and its FClothLODDataCommon ->
// FClothPhysicalMeshData (ClothingSystemRuntimeCommon/Public/ClothingAsset.h, ClothPhysicalMeshData.h),
// plus UChaosClothConfig for the tuning and the physics-asset spheres/capsules for the colliders. Taken as a
// PATTERN, not a port: the data is plain std/glm, every length is in centimetres (1 unit = 1 cm, as in UE),
// and the weight maps are named fields instead of UE's generic "weight map by target" table.
//
// Divergences on purpose:
//   * A FIXED vertex is not a separate list. It is a vertex whose MaxDistance is 0 — exactly UE's rule
//     (a zero max distance pins the particle to its skinned position). A separate list would be a second
//     source of truth that could disagree with the map.
//   * Colliders name bones by string, like SocketAttachmentComponent::BoneName; they are resolved against
//     the wearer's skeleton when a simulation is created, because the same garment is worn by different
//     characters that share a skeleton layout, not a skeleton object.
//
// NOT HERE (by task):
//   * CLO1 — the asset TYPE (AssetBase subclass, file format, importer from the skinned mesh section, asset
//     registry entry) and the Jolt SoftBody backend. Nothing in this header is serialized or reflected yet,
//     so no scene/asset format and no census moves with CLO0.
//   * CLO1 — the painting tool for the weight maps.

#include <Common/Core/AssetHandle.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Desert::Physics::Cloth
{
    // Which part of which skinned mesh this cloth replaces while it simulates (UE: the clothing asset is
    // bound to one section of one LOD of a USkeletalMesh; the section's render vertices are then driven by
    // the simulated mesh instead of by skinning).
    struct ClothMeshBinding
    {
        Common::AssetHandle SkinnedMesh; // the mesh asset whose section is replaced
        uint32_t            SectionIndex = 0;
        uint32_t            LodIndex     = 0;
    };

    // The simulation mesh, in the skinned mesh's reference pose (mesh space, cm). UE: FClothPhysicalMeshData.
    struct ClothPhysicalMesh
    {
        std::vector<glm::vec3> Positions;
        std::vector<glm::vec3> Normals;
        std::vector<uint32_t>  TriangleIndices; // 3 per triangle

        // Skinning of the simulation vertices, used for the fixed vertices and as the anchor the max
        // distance is measured from. Indices point into ClothingAsset::UsedBoneNames.
        std::vector<std::array<uint32_t, 4>> BoneIndices;
        std::vector<std::array<float, 4>>    BoneWeights;
    };

    // Per-vertex maps, one entry per ClothPhysicalMesh vertex (UE: MaxDistances, BackstopDistances,
    // BackstopRadiuses weight maps).
    struct ClothVertexMaps
    {
        // How far (cm) the particle may move from its skinned position. 0 = fixed to the skin.
        std::vector<float> MaxDistance;
        // Backstop: a sphere of BackstopRadius (cm) placed BackstopDistance (cm) behind the skinned vertex
        // along its inverted normal; the particle may not enter it. Stops cloth sinking into the body.
        std::vector<float> BackstopDistance;
        std::vector<float> BackstopRadius;
    };

    [[nodiscard]] inline bool IsFixedVertex( const ClothVertexMaps& maps, uint32_t vertex )
    {
        return maps.MaxDistance[vertex] == 0.0f;
    }

    // Solver tuning (UE: UChaosClothConfig, reduced to what a position-based soft body can honour).
    // Stiffnesses are normalised [0, 1] like UE's; lengths are cm; time is seconds.
    struct ClothConfig
    {
        float EdgeStiffness    = 1.0f;
        float BendingStiffness = 0.5f;
        float LinearDamping    = 0.01f;
        // Multiplies the world gravity the step context carries; the cloth does not own a gravity value,
        // so it cannot disagree with the physics world's.
        float GravityScale = 1.0f;
        // Aerodynamics: how strongly the wind velocity in the step context moves the cloth (UE Drag/Lift).
        float    DragCoefficient    = 0.035f;
        float    LiftCoefficient    = 0.035f;
        float    CollisionThickness = 1.0f; // cm
        float    Friction           = 0.8f;
        float    DensityKgPerM2     = 0.35f;
        uint32_t SolverIterations   = 4;
    };

    // Colliders that follow bones (UE: the spheres/capsules of the character's physics asset).
    struct ClothSphereCollider
    {
        std::string BoneName;
        glm::vec3   Offset = { 0.0f, 0.0f, 0.0f }; // cm, bone space
        float       Radius = 0.0f;                 // cm
    };

    // Tapered capsule between two bone-space points (UE's "tapered capsule" collider).
    struct ClothCapsuleCollider
    {
        std::string BoneName;
        glm::vec3   PointA  = { 0.0f, 0.0f, 0.0f }; // cm, bone space
        glm::vec3   PointB  = { 0.0f, 0.0f, 0.0f };
        float       RadiusA = 0.0f; // cm
        float       RadiusB = 0.0f;
    };

    using ClothCollider = std::variant<ClothSphereCollider, ClothCapsuleCollider>;

    struct ClothingAsset
    {
        std::string                Name;
        ClothMeshBinding           Binding;
        ClothPhysicalMesh          Mesh;
        ClothVertexMaps            Maps;
        ClothConfig                Config;
        std::vector<ClothCollider> Colliders;
        std::vector<std::string>   UsedBoneNames; // index space of ClothPhysicalMesh::BoneIndices
    };
} // namespace Desert::Physics::Cloth
