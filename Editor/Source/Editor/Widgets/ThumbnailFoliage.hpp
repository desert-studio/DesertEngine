#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <string>

// A FOLIAGE TYPE'S PICTURE IS ITS MESH'S PICTURE (THM1n-5; UE: UFoliageType_InstancedStaticMesh's thumbnail is
// its Mesh rendered). A `.defoliage` owns no appearance of its own: it names a cooked static mesh by
// {Guid, Path}, and the browser photographs THAT file through the ordinary mesh route — same key, same
// freshness source, same capture — so a type and its mesh never cost two captures of one geometry.
namespace Desert::Editor::ThumbnailFoliage
{
    /// The mesh a `.defoliage`'s text names, as a path under @p assetsRoot (the cooked `.stmesh` the
    /// foliage paint tool recorded, FoliagePaintTool MeshRefOfAsset). Pure: no filesystem, no asset manager.
    /// Refuses, naming why: text that does not parse, a Prefab type (it places a prefab, it has no mesh), and
    /// a Mesh type that names no mesh yet (authored but not paintable).
    [[nodiscard]] Common::ResultStr<std::filesystem::path> MeshSourceOf( const std::string& defoliageText,
                                                                         const std::filesystem::path& assetsRoot );

    /// The same for the file at @p defoliage: read, then MeshSourceOf. An unreadable file is an error naming it.
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    ReadMeshSource( const std::filesystem::path& defoliage, const std::filesystem::path& assetsRoot );
} // namespace Desert::Editor::ThumbnailFoliage
