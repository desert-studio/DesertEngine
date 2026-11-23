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
    struct WaterInfoGround;
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
        // Each body's TargetWaveMaskDepth (cm, UE): the depth below which the GPU waves damp, read against the
        // WaterInfo ground exactly as Water::QueryWaterBody damps the CPU's.
        std::vector<float> WaveMaskDepths;
    };

    struct WaterZoneDraw
    {
        Water::WaterQuadTreeDesc Zone;
        // Never null for a submitted zone: a frame without a visible body submits no zone.
        std::shared_ptr<const WaterZoneBodies> Bodies;
        // The zone's bottom seen from above (UE WaterInfo ground depth); null over no landscape.
        std::shared_ptr<const Water::WaterInfoGround> Ground;
    };
} // namespace Desert::Graphic::System
