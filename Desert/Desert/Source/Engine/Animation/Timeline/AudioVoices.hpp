#pragma once

/**
 * AUDIO VOICES: A SEQUENCE'S AUDIO SECTIONS → START / SEEK / GAIN / STOP, FRAME BY FRAME.
 *
 * UE's MovieSceneAudioSystem: the Evaluator says which Audio sections sound at the playhead and where in
 * their sound (Evaluator.hpp, `AudioSample`); a voice, though, is state across frames, so something has to
 * remember what is already playing and say only what changes. That is this class — pure, no audio device:
 * its output is a list of commands an executor (ECS/System/AudioECSSystem.hpp) turns into AudioEngine calls,
 * and a test reads them frame by frame.
 *
 * THE RULES, per frame, against the previous frame's voices:
 *   * a voice in the frame and not playing          → Start at its position and gain;
 *   * a voice playing and absent from the frame     → Stop (the playhead left the section, the player
 *                                                     paused or stopped, the owner was destroyed);
 *   * a voice playing whose position is not where   → Seek (a scrub, a loop wrap, a jump): the sound
 *     the last frame's position plus the frame's      follows the picture, never the other way round;
 *     advance puts it
 *   * a voice playing whose gain changed            → SetGain (fades, volume edits);
 *   * a voice whose sound changed                   → Stop, then Start.
 * A frame that does not move forward sounds nothing: the owner leaves its voices out (UE plays no audio
 * paused or backwards), so the diff stops them, and the next forward frame starts them where the picture is.
 */

#include <Common/Content/AssetEnvelope.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation::Timeline
{
    /// One Audio section sounding this frame. (Owner, Track, Section) names the voice across frames; Owner
    /// is the sequence's owner (an entity id), so two clips' track 0 / section 0 are two voices.
    struct SoundingVoice
    {
        uint64_t    Owner   = 0;
        uint32_t    Track   = 0;
        uint32_t    Section = 0;
        Common::Content::AssetGuid Sound; ///< the `.desound` GUID
        double      Seconds  = 0.0; ///< position in the sound at the frame's playhead
        double      Advanced = 0.0; ///< seconds the playhead moved forward this frame
        float       Gain     = 1.0F;
    };

    enum class AudioCommandKind : uint8_t
    {
        Start,
        Seek,
        SetGain,
        Stop,
    };

    [[nodiscard]] const char* ToString( AudioCommandKind kind );

    struct AudioCommand
    {
        AudioCommandKind Kind    = AudioCommandKind::Start;
        uint64_t         Owner   = 0;
        uint32_t         Track   = 0;
        uint32_t         Section = 0;
        Common::Content::AssetGuid Sound; ///< Start only
        double           Seconds = 0.0;  ///< Start, Seek
        float            Gain    = 1.0F; ///< Start, SetGain
    };

    class AudioVoices
    {
    public:
        /// How far a voice's position may stray from "last + advance" before it is a jump (seconds).
        static constexpr double kSeekTolerance = 1.0e-3;

        /// Diff @p frame (every voice sounding now) against the voices playing; append the commands to @p out.
        void Update( const std::vector<SoundingVoice>& frame, std::vector<AudioCommand>& out );

        /// Stop every playing voice (leaving Play, the executor going away).
        void StopAll( std::vector<AudioCommand>& out );

        [[nodiscard]] size_t Playing() const
        {
            return m_Voices.size();
        }

    private:
        std::vector<SoundingVoice> m_Voices; // what played last frame, as it was then
    };
} // namespace Desert::Animation::Timeline
