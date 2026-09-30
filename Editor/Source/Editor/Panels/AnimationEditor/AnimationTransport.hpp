#pragma once

#include <Editor/Core/UICommandInfo.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Editor
{
    /**
     * @brief The Animation Editor's transport — play/pause, frame step, start/end, loop, speed — as plain
     * arithmetic on one clock.
     *
     * RENDERER-FREE ON PURPOSE. The document needs a device; the rules of the transport (where a step lands,
     * what the end of a non-looping clip does, how many frames a clip has) do not, and a suite pins them here
     * without one. The document owns an instance and pushes `Time` into Animator::SetTime every frame, so the
     * pose is a function of this clock alone: a frame rendered twice at the same Time is the same pose,
     * whatever the editor's frame rate was on the way there (UE's FAnimationEditorPreviewScene drives its
     * preview instance the same way — the scrub bar owns the time, the instance is told it).
     *
     * FRAMES ARE THE CLIP'S DISPLAY GRID (AnimationClip::DisplayRate), the grid an artist edits on, not the
     * tick grid keys are counted at.
     */
    /// UE's Persona transport (FAnimViewportPlaybackCommands): the toolbar buttons and the document's palette
    /// actions ("<document>: Play/Pause") are these, run through AnimationTransport::Execute — one home.
    enum class TransportCommand : std::uint8_t
    {
        ToStart,
        PreviousFrame,
        PlayPause,
        NextFrame,
        ToEnd,
    };

    inline constexpr std::array<UICommandInfo, 5> kTransportCommandInfos{ {
         { "Animation Editor", "To Start", "" },
         { "Animation Editor", "Previous Frame", "" },
         { "Animation Editor", "Play/Pause", "" },
         { "Animation Editor", "Next Frame", "" },
         { "Animation Editor", "To End", "" },
    } };

    inline constexpr std::array<TransportCommand, 5> kTransportCommandOrder{
         TransportCommand::ToStart, TransportCommand::PreviousFrame, TransportCommand::PlayPause,
         TransportCommand::NextFrame, TransportCommand::ToEnd };

    [[nodiscard]] constexpr const UICommandInfo& CommandInfo( TransportCommand command )
    {
        return kTransportCommandInfos[static_cast<std::size_t>( command )];
    }

    struct AnimationTransport
    {
        // UE's Persona speed menu, the same five steps.
        static constexpr std::array<float, 5> kSpeeds{ 0.25f, 0.5f, 1.0f, 1.5f, 2.0f };

        double               DurationSeconds = 0.0;
        Animation::FrameRate DisplayRate     = Animation::DEFAULT_DISPLAY_RATE;
        double               Time            = 0.0;
        bool                 Playing         = false;
        bool                 Loop            = true;
        float                Speed           = 1.0f;

        [[nodiscard]] double FrameSeconds() const
        {
            return 1.0 / DisplayRate.AsDouble();
        }

        // The display frame `Time` is on. The epsilon keeps a time that is a frame boundary written as a sum
        // of frame lengths (0.1 + 0.1 + 0.1 at 10 fps) on that frame instead of the one before it.
        [[nodiscard]] int32_t FrameIndex() const
        {
            return static_cast<int32_t>( std::floor( Time * DisplayRate.AsDouble() + 1e-6 ) );
        }

        // Frames the clip spans, counting the last pose: a one-second clip at 30 fps shows 0..30.
        [[nodiscard]] int32_t LastFrame() const
        {
            return static_cast<int32_t>( std::floor( DurationSeconds * DisplayRate.AsDouble() + 1e-6 ) );
        }

        void SetTime( const double seconds )
        {
            Time = std::clamp( seconds, 0.0, std::max( DurationSeconds, 0.0 ) );
        }

        void ToStart()
        {
            Playing = false;
            Time    = 0.0;
        }

        void ToEnd()
        {
            Playing = false;
            Time    = std::max( DurationSeconds, 0.0 );
        }

        // A step pauses, like UE's: stepping a playing clip would move one frame and then run away from it.
        // It lands ON the grid — the step from a time between frames goes to the next boundary, not a frame
        // length past the time — and a looping clip wraps past either end.
        void StepFrames( const int32_t frames )
        {
            Playing            = false;
            const int32_t last = LastFrame();
            int32_t       to   = FrameIndex() + frames;
            if ( Loop && last > 0 )
                to = ( ( to % ( last + 1 ) ) + ( last + 1 ) ) % ( last + 1 );
            to = std::clamp( to, 0, std::max( last, 0 ) );
            SetTime( static_cast<double>( to ) * FrameSeconds() );
        }

        // Advance by REAL seconds scaled by Speed. A looping clip wraps; one that does not stops on its last
        // pose and pauses, so Play from there restarts it (UE's behaviour).
        void Advance( const double realSeconds )
        {
            if ( !Playing || DurationSeconds <= 0.0 )
                return;
            const double next = Time + realSeconds * static_cast<double>( Speed );
            if ( next < DurationSeconds )
            {
                Time = next;
                return;
            }
            if ( Loop )
            {
                Time = std::fmod( next, DurationSeconds );
                return;
            }
            Time    = DurationSeconds;
            Playing = false;
        }

        void TogglePlay()
        {
            if ( !Playing && !Loop && Time >= DurationSeconds )
                Time = 0.0;
            Playing = !Playing;
        }

        void Execute( const TransportCommand command )
        {
            switch ( command )
            {
                case TransportCommand::ToStart:
                    ToStart();
                    return;
                case TransportCommand::PreviousFrame:
                    StepFrames( -1 );
                    return;
                case TransportCommand::PlayPause:
                    TogglePlay();
                    return;
                case TransportCommand::NextFrame:
                    StepFrames( 1 );
                    return;
                case TransportCommand::ToEnd:
                    ToEnd();
                    return;
            }
        }
    };
} // namespace Desert::Editor
