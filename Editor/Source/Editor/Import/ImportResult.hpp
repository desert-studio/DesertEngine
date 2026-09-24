#pragma once

#include <vector>
#include <optional>
#include <string>

#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>
#include <Common/Content/AssetEnvelope.hpp>

namespace Desert::Editor
{
    // A material extracted from a source file, ready to cook into a .demat. Name is the human-readable
    // source material name (-> the .demat filename, no handle in it). Guid is the material's one identity,
    // stated by the .demat's header; mesh submeshes reference its handle (MaterialData::HandleOf). It sits
    // beside Data, not in it, because PBRSurfaceParams is the parameter block and carries no identity.
    struct ImportedMaterial
    {
        std::string             Name;
        Assets::PBRSurfaceParams   Data;
        Common::Content::AssetGuid Guid;
        // The texture slots, by each texture asset's header GUID (MATL 3). Data's typed texture handles cannot
        // say this: a handle is a fold of a GUID, not a way back to it.
        std::vector<Assets::MaterialAssetRef> Textures;
    };

    struct ImportResult
    {
        std::optional<Desert::Assets::Serialization::MeshAssetData>     Mesh;
        std::optional<Desert::Assets::Serialization::SkeletonAssetData> Skeleton;
        std::vector<Desert::Assets::Serialization::AnimationAssetData>  Animations;
        std::vector<ImportedMaterial>                                   Materials;
    };
} // namespace Desert::Editor