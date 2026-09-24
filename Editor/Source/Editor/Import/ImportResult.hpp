#pragma once

#include <vector>
#include <optional>
#include <string>

#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>

namespace Desert::Editor
{
    // A material extracted from a source file, ready to cook into a .demat. Name is the human-readable
    // source material name (-> the .demat filename, no handle in it). Data.Header's GUID is the stable external
    // handle the mesh submeshes reference.
    struct ImportedMaterial
    {
        std::string             Name;
        Assets::PBRSurfaceParams Data;
    };

    struct ImportResult
    {
        std::optional<Desert::Assets::Serialization::MeshAssetData>     Mesh;
        std::optional<Desert::Assets::Serialization::SkeletonAssetData> Skeleton;
        std::vector<Desert::Assets::Serialization::AnimationAssetData>  Animations;
        std::vector<ImportedMaterial>                                   Materials;
    };
} // namespace Desert::Editor