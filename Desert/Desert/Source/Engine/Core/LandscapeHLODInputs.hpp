#pragma once

// WHAT A LANDSCAPE TILE'S AND A SPLINE'S HLOD BUILDER READS (UE ULandscapeHLODBuilder::ComputeHLODHash), on the
// engine side beside the builders that read it (HLODMeshBuilder.cpp): the saved-HLOD checksum folds exactly what
// BuildLandscapeHLODMesh and the spline builder read, because both come from one read of the record. The terrain
// module registers them as its components' Rules::HLODBuilderComponent::Inputs at StartupModule; its public
// headers name no engine type.

#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <span>

namespace Desert::Core
{
    // A tile's inputs: its root's tiling and look (the LandscapeMaterial's material and the root's layer infos),
    // its coordinate and its height file; the root's place left to its TRS chain (Placed), so no composed matrix
    // enters the text. Refused by name as BuildLandscapeHLODMesh refuses the tile.
    [[nodiscard]] Common::ResultStr<Rules::HLODModuleInputs>
    LandscapeTileHLODInputs( std::span<const Assets::EntityData> records, std::span<const glm::mat4> world,
                             std::size_t index );

    // A spline's inputs: its splines as the spline builder reads them (ReadLandscapeSplinesJson), written back
    // canonical, and the static meshes they lay (by asset handle).
    [[nodiscard]] Common::ResultStr<Rules::HLODModuleInputs>
    LandscapeSplineHLODInputs( std::span<const Assets::EntityData> records, std::span<const glm::mat4> world,
                               std::size_t index );
} // namespace Desert::Core
