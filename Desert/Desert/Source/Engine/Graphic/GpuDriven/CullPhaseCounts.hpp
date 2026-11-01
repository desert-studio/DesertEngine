#pragma once

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

// GPU-DRIVEN PASSES — CullPhaseCounts: HOW MANY INSTANCES EACH BINNED VIEW DREW IN EACH OCCLUSION PHASE
// (DesertCtl `state views.drawBinViews`). The draw bins' cull Visibility words (OcclusionCarry::Visibility, after
// phase 2 marked its finds) reach the host through a frame-in-flight readback slot, the pattern of
// InstanceCullValidation; when the slot comes round again its fence has passed and the words are counted per view:
// phase 1 = in view and not held back by its phase-1 HZB (CullVisibility::kInView without kOccluded: last frame's
// HZB for a two-phase view, this frame's for the view culled once after it - the deferred depth prepass), phase 2
// = found visible in this frame's HZB (kPostVisible). LastCounts is the newest frame read, slots-in-flight late.
namespace Desert::Graphic::GpuDriven
{
    struct CullPhaseViewCount
    {
        std::string Name;
        uint32_t    Phase1 = 0;
        uint32_t    Phase2 = 0;
        // In view, held back by its phase-1 HZB and not found visible in this frame's: what the occlusion took
        // out (Phase1 + Phase2 + Occluded = the frustum-only count).
        uint32_t Occluded = 0;
    };

    // One cull dispatch of the frame whose views are counted: its carry's Visibility words, a name per view in the
    // carry's view order.
    struct CullPhaseSource
    {
        std::span<const std::string> ViewNames;
        OcclusionCarry               Carry;
    };

    class CullPhaseCounts
    {
    public:
        // Counts the slot of @p frameIndex (the words it was handed slots-in-flight frames ago), then hands it
        // this frame's @p sources: every cull dispatch of the draw bins (phase 1's views, then the depth prepass
        // view's own), counted as one list of views in that order. A source whose names do not match its carry is
        // refused (logged once per message) and the frame hands nothing.
        void AddFrame( RDG::Builder& graph, uint64_t frameIndex, std::span<const CullPhaseSource> sources );
        // A frame that culls no bins: nothing is handed, and the counts read so far are forgotten.
        void                                                 Forget();
        [[nodiscard]] const std::vector<CullPhaseViewCount>& LastCounts() const
        {
            return m_Last;
        }

    private:
        struct SlotPart
        {
            RDG::ExternalBuffer      Visibility;
            std::vector<std::string> Names;
            uint32_t                 InstanceCount = 0;
        };
        struct Slot
        {
            std::vector<SlotPart> Parts; // one per CullPhaseSource, in order; empty: not handed a frame
        };
        void Count( const Slot& slot );

        std::vector<Slot>               m_Slots;
        std::vector<CullPhaseViewCount> m_Last;
        std::string                     m_LastError; // logged once per message
    };
} // namespace Desert::Graphic::GpuDriven
