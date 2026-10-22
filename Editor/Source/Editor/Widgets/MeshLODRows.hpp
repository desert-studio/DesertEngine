#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Geometry/Mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

// THE LOD ROWS OF A MESH, shared by the two places that show a mesh's LOD chain: the Details panel's Level of
// Detail section and the Modeling panel's LOD Manager (UE 5.8 LODManagerTool: one row per level, its triangle
// count and screen size). One drawing, so the two can never disagree about what a level holds.
namespace Desert::Core
{
    class Scene;
}
namespace Desert::ECS
{
    class Entity;
}

namespace Desert::Editor::MeshLODRows
{
    // The chain's length: the longest chain of any submesh (a shorter one repeats its last level).
    std::size_t LevelCount( const ::Desert::Mesh& mesh );
    // Triangles drawn at `level`, summed over the submeshes (a submesh past its chain draws its last level).
    std::uint32_t Triangles( const ::Desert::Mesh& mesh, std::size_t level );
    // The level `scene`'s active camera draws `entity` at — the renderer's own policy (Geometry::SelectLOD), so a
    // marker cannot disagree with what is on screen. Empty without a scene or camera.
    std::optional<std::size_t> DrawingLevel( const Core::Scene* scene, const ECS::Entity& entity,
                                             const ::Desert::Mesh& mesh, int forcedLOD, int lodBias );
    // One table row per level: LOD, triangles, screen size; `marked` (the level on screen) carries `markLabel`.
    void DrawLevels( const ::Desert::Mesh& mesh, std::optional<std::size_t> marked, const char* markLabel );
    // THE PER-LOD SCREEN SIZES OF THE MESH (UE Static Mesh editor, LOD Settings: "Auto Compute LOD Distances"
    // + each LOD's "Screen Size"). Their home is the record of the source the mesh was imported from
    // (ImportRecordData::LodScreenSize); the drawn mesh is updated as the value is dragged, the record written
    // when the edit ends. A mesh no record speaks for shows its computed sizes, read-only.
    void DrawScreenSizes( const Assets::AssetHandle& meshHandle, ::Desert::Mesh& mesh );
} // namespace Desert::Editor::MeshLODRows
