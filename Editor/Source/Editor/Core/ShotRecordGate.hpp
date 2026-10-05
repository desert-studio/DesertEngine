#pragma once

#include <cstdint>

namespace Desert::Editor
{
    // WHICH FRAMES OF A HEADLESS CAPTURE ARE RECORDED — and, under `--play`, which frames the world ticks on.
    //
    // The rule UE's Movie Render Queue and HighResShot keep: frame N of the sequence is tick N of game time.
    // A capture therefore has ONE decision per frame, taken once, before anything ticks, and read by both
    // halves: the world advances a step on exactly the frames that are written, and on no other. Before this
    // gate the two halves had different conditions — Play started as soon as the boot stages were done, the
    // writer waited for the content to settle — so a script's first seconds of gameplay ran under the splash
    // and frame_00001 was taken a dozen ticks into the motion it was meant to record. The same split let the
    // first frames through at 64x64 (the viewport before its panel was laid out), as a magenta frame (the
    // first frame at a new size), and under the loading screen.
    //
    // A frame is recorded when nothing the picture depends on is still in flight:
    //   - no scene load is pending and the staged boot is over,
    //   - the splash is off the screen (the editor window is what the user would see),
    //   - the content the scene asked for has settled,
    //   - the viewport has kept the same, non-empty size for kStableFrames such frames, this one included
    //     (any frame on which something above is still pending restarts the count).
    // Once the first frame is recorded the size is FIXED for the rest of the capture: a frame of another
    // size is not recorded (and does not tick), so every file of a sequence has the same dimensions.
    //
    // Pure state with no device and no clock, so the rule is asserted in a test rather than observed in PNGs
    // (Desert/Tests/Editor/ShotPath).
    struct ShotFrameConditions
    {
        bool     SceneLoadPending = false;
        bool     StartupLoading   = false;
        bool     SplashOnScreen   = false;
        bool     ContentSettling  = false;
        uint32_t ViewportWidth    = 0;
        uint32_t ViewportHeight   = 0;
    };

    class ShotRecordGate
    {
    public:
        // The first frame at a new size is not a finished picture (the targets were recreated under it; the
        // live capture showed it as solid magenta), and a window being shown settles its size over more than
        // one frame. Three frames at one size is the evidence that the layout has stopped moving.
        static constexpr int kStableFrames = 3;

        // Called ONCE per frame, after the deferred resizes are applied and before anything ticks.
        // Returns whether this frame is recorded (and, under `--play`, whether the world steps).
        bool Admit( const ShotFrameConditions& frame )
        {
            // Stability is counted only over frames on which everything else already holds: the size the
            // viewport had under the splash says nothing about the size of the window once it is shown.
            const bool settled = !frame.SceneLoadPending && !frame.StartupLoading && !frame.SplashOnScreen &&
                                 !frame.ContentSettling && frame.ViewportWidth > 0 && frame.ViewportHeight > 0;
            const bool sameSize = frame.ViewportWidth == m_LastWidth && frame.ViewportHeight == m_LastHeight;
            if ( !settled )
                m_StableFrames = 0;
            else
                m_StableFrames = sameSize ? m_StableFrames + 1 : 1;
            m_LastWidth  = frame.ViewportWidth;
            m_LastHeight = frame.ViewportHeight;

            bool ready = settled && m_StableFrames >= kStableFrames;
            if ( ready && m_Recording )
                ready = frame.ViewportWidth == m_RecordWidth && frame.ViewportHeight == m_RecordHeight;
            if ( ready && !m_Recording )
            {
                m_Recording    = true;
                m_RecordWidth  = frame.ViewportWidth;
                m_RecordHeight = frame.ViewportHeight;
            }
            m_Live = ready;
            return ready;
        }

        // Whether the frame last admitted is recorded.
        [[nodiscard]] bool Live() const
        {
            return m_Live;
        }
        // Whether the capture has begun: the first recorded frame has been admitted.
        [[nodiscard]] bool Recording() const
        {
            return m_Recording;
        }
        [[nodiscard]] uint32_t RecordWidth() const
        {
            return m_RecordWidth;
        }
        [[nodiscard]] uint32_t RecordHeight() const
        {
            return m_RecordHeight;
        }

    private:
        uint32_t m_LastWidth    = 0;
        uint32_t m_LastHeight   = 0;
        int      m_StableFrames = 0;
        bool     m_Recording    = false;
        bool     m_Live         = false;
        uint32_t m_RecordWidth  = 0;
        uint32_t m_RecordHeight = 0;
    };
} // namespace Desert::Editor
