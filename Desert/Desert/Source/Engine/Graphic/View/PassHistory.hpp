#pragma once

#include <Engine/Graphic/View/ViewFrame.hpp>

#include <cstdint>

// TAA1 step 3 — WHEN A PASS'S OWN HISTORY IS THE PREVIOUS FRAME OF ITS VIEW.
//
// SSR, the RSM GI resolve and the volumetric clouds each keep an accumulation target they reproject into the
// current frame through ViewFrame::PrevViewProjection. That matrix describes the view's previous frame — so the
// pass's history may be read only when the pass wrote it IN that frame. Before TAA1 each of them kept the matrix
// of the frame it last ran in instead (m_PrevViewProj); that kept matrix and history consistent with each other
// but let both drift from the view: one previous-frame source per view replaces them, and this stamp is what a
// pass keeps instead — WHICH view frame wrote its history, not a copy of that frame's matrices.
//
// Readable in @p frame iff: the history was written (a resize, a scene swap or a reset since drops it), the view
// itself did not reset (ViewFrame::HistoryReset: first frame, camera cut, resize, method change, pass fault), and
// the frame that wrote it is exactly the view's previous frame (a pass skipped for one frame — SSR toggled, GI
// off, clouds culled — restarts its accumulation instead of reprojecting a history two frames old through a
// matrix one frame old).
namespace Desert::Graphic
{
    class PassHistoryStamp
    {
    public:
        [[nodiscard]] bool ReadableIn( const ViewFrame& frame ) const
        {
            return m_Written && frame.HistoryValid() && m_ViewFrameIndex + 1 == frame.FrameIndex;
        }

        // The pass recorded its history write in the view frame @p viewFrameIndex (ViewFrame::FrameIndex, captured
        // when the pass was added: the write is recorded while the graph executes, after the frame was built).
        void Stamp( const uint64_t viewFrameIndex )
        {
            m_Written        = true;
            m_ViewFrameIndex = viewFrameIndex;
        }

        // The history images were recreated, the scene replaced, or the history otherwise lost its meaning.
        void Invalidate()
        {
            m_Written = false;
        }

    private:
        bool     m_Written        = false;
        uint64_t m_ViewFrameIndex = 0;
    };
} // namespace Desert::Graphic
