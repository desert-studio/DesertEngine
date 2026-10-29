#pragma once

#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// THE IMPORT'S VERDICT ON A MESH'S VIRTUAL GEOMETRY FLAG (VIRT-GEOM contract §6, Wiki «VIRT-GEOM — контракт»).
//
// The cluster raster writes depth and a visibility word per pixel and has no programmable stage before step 4: a
// Masked material (its cut-out needs the material's mask per pixel) and a Translucent / Additive one (drawn by
// the translucency pass, never into depth) cannot be drawn by it. UE refuses Nanite for translucent materials the
// same way and, before 5.1, for masked ones. So the import judges every material the mesh draws - the .demat the
// project holds (MaterialAdoption.hpp: written or adopted beside the mesh), through its template's BlendMode - and
// a mesh that draws one of those stores the flag OFF, with a warning naming the mesh, the material and why. The
// file's import record keeps what the user asked for; each mesh (a node mesh, the combined one) stores its own
// verdict, so one masked node does not cost the rest of the file its clusters.
//
// The engine has no World Position Offset input: there is nothing of that kind to judge yet. A material with an
// unknown template (an instance whose shader is its parent's, a shader without an Import block) is refused too:
// a flag that stands only because its material was not read would be a guess.
namespace Desert::Editor
{
    // Why @p settings' Virtual Geometry cannot stand for @p mesh imported from @p source, whose submeshes name
    // their materials by GUID and @p named gives each GUID its slot name (the .demat beside the import); nullopt
    // when it stands or is off. Displacement Runtime is refused here too (a surface diced per frame has no cooked
    // clusters), whoever set it.
    [[nodiscard]] std::optional<std::string>
    VirtualGeometryRefusal( const Assets::MeshImportSettings&           settings,
                            const Assets::Serialization::MeshAssetData& mesh,
                            std::span<const Assets::MeshMaterialSlot> named, const std::filesystem::path& source );

    // @p settings as the mesh @p meshName stores them: Virtual Geometry cleared, and the reason logged as a
    // warning naming the mesh, when VirtualGeometryRefusal refuses it; unchanged otherwise.
    [[nodiscard]] Assets::MeshImportSettings GateVirtualGeometry( Assets::MeshImportSettings settings,
                                                                  const Assets::Serialization::MeshAssetData& mesh,
                                                                  std::span<const Assets::MeshMaterialSlot> named,
                                                                  const std::filesystem::path&              source,
                                                                  std::string_view meshName );
} // namespace Desert::Editor
