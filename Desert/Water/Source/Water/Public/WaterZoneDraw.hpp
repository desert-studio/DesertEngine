#pragma once

// The water zone value WaterZoneCommand carries, apart from the water pass (WaterRenderer.hpp), so the ECS
// water system and its command do not reach the pass's pipeline and render-graph headers.
#include <Engine/Assets/Common.hpp>
#include <Water/Public/WaterQuadTree.hpp>

#include <glm/vec2.hpp>

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
        // Each body's water material (ECS::WaterBodyData::WaterMaterial), in Footprints' order.
        std::vector<Assets::AssetHandle> Materials;
        // Each body's wave set (ECS::WaterBodyData::WaterWaves; Null = a flat body) and its WaveOrigin (the
        // entity's X, Z), in Footprints' order.
        std::vector<Assets::AssetHandle> Waves;
        std::vector<glm::vec2>           WaveOrigins;
        // WATER-RIVERS-FFT R4, in Footprints' order: each body's type, its river bake (null for an ocean; the
        // same pointer as Footprints[i].Ribbon) and the Water::RiverBakeKey it was baked for (0 for an ocean).
        std::vector<Water::WaterBodyType>                         Types;
        std::vector<std::shared_ptr<const Water::RiverFlowField>> Flows;
        std::vector<uint64_t>                                     RiverKeys;
    };

    struct WaterZoneDraw
    {
        Water::WaterQuadTreeDesc Zone;
        // Never null for a submitted zone: a frame without a visible body submits no zone.
        std::shared_ptr<const WaterZoneBodies> Bodies;
        // The world's floor and shore, the very field the CPU query reads (ECS::WaterShoreFieldSource). Never
        // null: no water is drawn before the first bake.
        std::shared_ptr<const Water::WaterShoreField> Shore;
        // The scene's gravity, cm/s^2 (SceneSettings::Gravity): a river fall's water accelerates by it down the
        // sheet (Water::RiverFallSpeed), as the CPU query's does (Water::WaterBodyState::Gravity).
        float Gravity = 0.0f;
    };
} // namespace Desert::Graphic::System
