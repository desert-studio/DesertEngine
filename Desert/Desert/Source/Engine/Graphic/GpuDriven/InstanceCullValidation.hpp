#pragma once

#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder;
}

// GPU-DRIVEN phase CULL — InstanceCullValidation: THE CPU ANSWER TO EVERY VISIBILITY WORD THE GPU CULL WROTE
// (DebugViewState::ValidateGpuCulling). Each frame hands the host the cull's Visibility words (and the VSM
// drawn-page mask) through a frame-in-flight readback slot, with a snapshot of the scene tables and views that
// frame culled; when the slot comes round again its fence has passed, and every (view, instance) word is
// recomputed on the CPU by InstanceCull.shader CullMain's rules over the CPU functions the draw path uses
// (KeepsInstanceAtDistance, Geometry::TransformBounds, Core::Frustum::Intersects, Geometry::SelectLODFromBounds)
// and compared on kInView + LOD. One "[GPUDriven] Cull validation" line per view: GPU = CPU is "mismatches 0".
namespace Desert::Graphic::GpuDriven
{
    class InstanceCullValidation
    {
    public:
        // Reports the slot of @p frameIndex (the words it was handed slots-in-flight frames ago), then hands it
        // this frame's @p carry.Visibility and @p vsmDirtyMask (invalid = no VSM level culled) with copies of
        // @p scene's tables and @p views. @p viewNames: one per view, the log's name of it.
        void AddFrame( RDG::Builder& graph, uint64_t frameIndex, const GpuScene& scene,
                       std::span<const CullViewInput> views, std::span<const std::string> viewNames,
                       const OcclusionCarry& carry, RDG::BufferRef vsmDirtyMask );

    private:
        struct View
        {
            GpuCullView   Gpu{};
            Core::Frustum Frustum{};
            std::string   Name;
        };

        // One frame-in-flight: the readbacks and the CPU state they were culled from.
        struct Slot
        {
            RDG::ExternalBuffer              Visibility;
            RDG::ExternalBuffer              Mask;
            std::vector<GpuScenePrimitive>   Primitives;
            std::vector<GpuSceneInstance>    Instances;
            std::vector<float>               LodScreenSizes;
            std::vector<GpuSceneWorldBounds> WorldBounds;
            std::vector<View>                Views;
            uint32_t                         InstanceCount = 0;
        };

        void Report( const Slot& slot );
        // Logged once per distinct message: the gate is a debug view held on for many frames.
        void Refuse( std::string message );

        std::vector<Slot> m_Slots;
        std::string       m_LastError;
    };
} // namespace Desert::Graphic::GpuDriven
