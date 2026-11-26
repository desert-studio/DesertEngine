#pragma once

// The water zone value WaterZoneCommand carries, apart from the water pass (WaterRenderer.hpp), so the ECS
// water system and its command do not reach the pass's pipeline and render-graph headers.
#include <Engine/Assets/Common.hpp>
#include <Water/Public/WaterQuadTree.hpp>
#include <Water/Public/WaterWaveTable.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Desert::Water
{
    struct WaterShoreField;
} // namespace Desert::Water

namespace Desert::Graphic::System
{
    // One water zone of this frame, submitted from the ECS (ECS::WaterZoneECSSystem -> WaterZoneCommand ->
    // SceneRenderer::SubmitWaterZone): the zone and the bodies' footprints inside it. Graphic-side, no ECS types.
    // The frame's visible water bodies, as every zone of the frame draws them. Immutable once built and shared by
    // every zone's draw: the collector keeps it across frames and builds a new one only when a body changed.
    struct WaterZoneBodies
    {
        std::vector<Water::WaterQuadTreeBody> Footprints;
        std::vector<uint32_t>                 Ids; // Water::WaterBodyState::Id, in Footprints' order
        // Each body's water material (ECS::WaterBodyData::WaterMaterial), in Footprints' order.
        std::vector<Assets::AssetHandle> Materials;
        // Each body's lane in WaterZoneDraw::Waves (-1 = a flat body), in Footprints' order.
        std::vector<int32_t>   WaveLanes;
        std::vector<glm::vec2> WaveOrigins; // the entity's X, Z
        // WATER-RIVERS-FFT R4, in Footprints' order: each body's type and its river bake (null for an ocean; the
        // same pointer as Footprints[i].Ribbon, ECS::WaterRiverBakes').
        std::vector<Water::WaterBodyType>                         Types;
        std::vector<std::shared_ptr<const Water::RiverFlowField>> Flows;
        // The ECS::WaterRiverBakes::Revision these rows were built at: another revision, other rows.
        uint64_t RiverRevision = 0u;
    };

    struct WaterZoneDraw
    {
        Water::WaterQuadTreeDesc Zone;
        // Never null for a submitted zone: a frame without a visible body submits no zone.
        std::shared_ptr<const WaterZoneBodies> Bodies;
        // The world's wave lanes (ECS::WaterWaveSources): the same table the CPU queries read. Never null.
        std::shared_ptr<const Water::WaterWaveTable> Waves;
        // The world's floor and shore, the very field the CPU query reads (ECS::WaterShoreFieldSource). Never
        // null: no water is drawn before the first bake.
        std::shared_ptr<const Water::WaterShoreField> Shore;
    };
} // namespace Desert::Graphic::System
