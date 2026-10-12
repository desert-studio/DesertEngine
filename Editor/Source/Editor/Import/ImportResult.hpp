#pragma once

#include <vector>
#include <optional>
#include <string>
#include <glm/glm.hpp>

#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Editor/Import/MaterialImportContract.hpp>
#include <Common/Content/AssetEnvelope.hpp>

namespace Desert::Editor
{
    // A material extracted from a source file, as the SOURCE'S OWN DICTIONARY (MAT1b adapters): the template it
    // becomes, and which of its keys fill which Property, are chosen when the .demat is written
    // (ImportManager::SerializeMaterialAsset). Name is the human-readable source material name (-> the .demat
    // filename, no handle in it). Guid is the material's one identity, stated by the .demat's header; mesh
    // submeshes reference its handle (MaterialData::HandleOf).
    struct ImportedMaterial
    {
        std::string                Name;
        SourceMaterial             Source;
        Common::Content::AssetGuid Guid;
    };

    // A light source as the file states it (aiScene::mLights), in the SOURCE's space after the import's unit
    // scaling (centimetres, before the import options' SourceToEngine): where its node stands and where it
    // shines. SourceLightsToEngine (SourceToEngine.hpp) turns it into what the import record states.
    enum class ImportedLightType
    {
        Point,
        Spot,
        Directional
    };
    struct ImportedLight
    {
        std::string          Name; // the light's node
        ImportedLightType    Type = ImportedLightType::Point;
        glm::vec3            Position{ 0.0f };               // the node's world origin
        glm::vec3            Direction{ 0.0f, -1.0f, 0.0f }; // unit, the way the light travels
        glm::vec3            Radiance{ 1.0f };               // colour times intensity / 100 (assimp's fold)
        std::optional<float> Radius;                         // cm; nullopt = no decay (FBX DecayType None)
        float                InnerConeDegrees = 0.0f;        // spot only
        float                OuterConeDegrees = 0.0f;        // spot only
    };

    struct ImportResult
    {
        std::optional<Desert::Assets::Serialization::MeshAssetData>     Mesh;
        std::optional<Desert::Assets::Serialization::SkeletonAssetData> Skeleton;
        // The file's clips as the engine holds them (ANIM 6: one Timeline::Sequence each); the import names
        // their Skeleton and writes each through BuildAssetDataFromClip (ImportManager::SerializeAnimationAsset).
        std::vector<Desert::Animation::AnimationClip>                   Animations;
        std::vector<ImportedMaterial>                                   Materials;
        // The name of the source node that placed each submesh of Mesh, one per submesh in order. A mesh the
        // source shares between nodes is one submesh PER NODE, each baked with that node's transform
        // (AssimpImporter ProcessScene). What NodeMeshSplit groups by when Combine Meshes is off (UE's default).
        std::vector<std::string> SubmeshNodes;
        // The file's light sources (UE FbxSceneImport: one light actor per FBX light), recorded in the import
        // record and placed with the source (ImportRecord.hpp `Lights`).
        std::vector<ImportedLight> Lights;
    };
} // namespace Desert::Editor