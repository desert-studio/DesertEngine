#pragma once
#include <Engine/Core/Serialize/WorldPartitionHLODBuilders.hpp>

#include <optional>
#include <span>

namespace Desert::Core::Rules
{
    class HLODBuilderRegistry;
} // namespace Desert::Core::Rules

namespace Desert::Landscape
{
    // THE SPLINE MESHES OF ONE LANDSCAPE SPLINE (Core::Rules::LandscapeSplineHLODRequest; UE: the spline's
    // USplineMeshComponents in its cell's HLOD): its LandscapeSpline block read as the loader reads it, every
    // segment's SplineMeshes bent by World::Landscape::BuildLandscapeSplineSegmentMeshes - the same function the
    // editor and the game draw them with (LandscapeSplineMeshDraw) - from the built-in shape or the mesh asset
    // (resolved by its handle in @p registries, read as Core::BuildHLODMesh reads a part), one world-space
    // EditMesh. Nullopt when the spline lays no mesh.
    [[nodiscard]] Common::ResultStr<std::optional<Assets::StaticMeshComponentSer>>
    BuildLandscapeSplineHLODMesh( const Core::Rules::LandscapeSplineHLODRequest& request,
                                  std::span<const Common::Utils::AssetRegistry>  registries );

    /// Sets the spline builder of @p builders (Landscape::StartupModule adds it to ModuleHooks().HLODBuilders).
    void AddLandscapeSplineHLODBuilder( Core::Rules::HLODBuilderRegistry& builders );
} // namespace Desert::Landscape
