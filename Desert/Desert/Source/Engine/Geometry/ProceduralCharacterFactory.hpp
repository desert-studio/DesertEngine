#pragma once

#include <Common/Content/AssetEnvelope.hpp> // AssetGuid
#include <Common/Core/ResultStr.hpp>

#include <Engine/Assets/Serialization/Mesh.hpp> // MeshAssetData

#include <array>
#include <filesystem>
#include <string_view>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Geometry
{
    // THE BUILT-IN HUMANOID IS ENGINE CONTENT, NOT A RUNTIME MESH. As UE's /Engine mannequin, the character is
    // three kinds of asset under Resources/Engine/Meshes/Skinned/: Humanoid.skeleton (the rig), Humanoid.skmesh
    // (the body) and one `.anim` per locomotion clip — each with a GUID its header states. A spawned character
    // is an ordinary entity that REFERENCES them (SkinnedMesh by the mesh's GUID, Animation by a clip's), so a
    // Play snapshot, a save and a load carry it like any imported character.
    //
    // This factory is the GENERATOR of the mesh and the clips, run ONCE by a person (the editor command
    // "Generate Humanoid Engine Assets") and its output committed; nothing reaches it at runtime. The body is
    // tapered capsules and joint spheres, each rigid-skinned to one bone, laid over the bind pose the skeleton
    // file states — a greybox mannequin, not sculpted art. The GUIDs are fixed below, so a regeneration writes
    // the same identities and every reference already authored keeps resolving.
    inline constexpr Common::Content::AssetGuid kHumanoidMeshGuid{ 0x6d3a1f0e2b8c4e57ULL, 0x9a41c7d25e0b3f18ULL };

    // One locomotion clip of the humanoid: its asset name (the clip's Name, and the file's stem suffix) and GUID.
    struct HumanoidClipAsset
    {
        std::string_view           Name;
        Common::Content::AssetGuid Guid;
    };
    inline constexpr std::array<HumanoidClipAsset, 4> kHumanoidClips = { {
         { "Idle", { 0x6d3a1f0e2b8c4e57ULL, 0x9a41c7d25e0b3f21ULL } },
         { "Walk", { 0x6d3a1f0e2b8c4e57ULL, 0x9a41c7d25e0b3f22ULL } },
         { "Run", { 0x6d3a1f0e2b8c4e57ULL, 0x9a41c7d25e0b3f23ULL } },
         { "Jump", { 0x6d3a1f0e2b8c4e57ULL, 0x9a41c7d25e0b3f24ULL } },
    } };

    // The runtime handle of the humanoid mesh: its GUID folded (HandleForGuid), as a scene load resolves it.
    [[nodiscard]] inline Assets::AssetHandle HumanoidMeshHandle()
    {
        return Assets::AssetHandle{ static_cast<uint64_t>( Common::Content::HandleForGuid( kHumanoidMeshGuid ) ) };
    }

    // The clip a spawned humanoid plays until something else (a graph, a Character Controller) chooses.
    inline constexpr std::string_view kHumanoidDefaultClip = "Walk";

    // Engine-content paths (mount relative, like HumanoidSkeletonFile) of the mesh and of one clip by name.
    std::filesystem::path HumanoidMeshFile();
    std::filesystem::path HumanoidClipFile( std::string_view clipName );

    class ProceduralCharacterFactory
    {
    public:
        // The body over @p skeleton's component-space bind, stating @p skeletonGuid as its skeleton and
        // kHumanoidMeshGuid as its own identity. A bone the body names that the rig lacks is refused by name.
        [[nodiscard]] static Common::ResultStr<Assets::Serialization::MeshAssetData>
        BuildHumanoidMesh( const Animation::Skeleton& skeleton, const Common::Content::AssetGuid& skeletonGuid );

        // Reads Humanoid.skeleton, builds the mesh and every clip of kHumanoidClips over it, and writes them
        // beside the skeleton (HumanoidMeshFile, HumanoidClipFile). Returns the paths written; any failure is
        // refused naming the file, and nothing is written after it.
        [[nodiscard]] static Common::ResultStr<std::vector<std::filesystem::path>> WriteEngineAssets();
    };
} // namespace Desert::Geometry
