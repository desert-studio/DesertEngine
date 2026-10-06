#include "UIAnimationPlayback.hpp"

#include <Engine/ECS/Components.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
#include <type_traits>
#include <variant>

namespace Desert::UI
{
    namespace
    {
        namespace TL = Animation::Timeline;

        // The UI host: a Widget binding → the element whose entity UUID it names; a track's value → that
        // element's sample. Validate admits only Widget bindings with Vector/Float tracks on this host, so it
        // never meets an event, a camera cut or an animation section.
        class UIHost final : public TL::ITimelineHost
        {
        public:
            UIHost( const std::unordered_map<std::string, entt::entity>& byUuid, UIClipFrame& frame )
                 : m_ByUuid( byUuid ), m_Frame( frame )
            {
            }

            std::optional<TL::ResolvedBinding> Resolve( const TL::Binding& binding ) override
            {
                if ( binding.Kind != TL::BindingKind::Widget )
                    return std::nullopt;
                const auto it = m_ByUuid.find( binding.Locator );
                if ( it == m_ByUuid.end() )
                    return std::nullopt;
                return TL::ResolvedBinding{
                     static_cast<uint64_t>( static_cast<std::underlying_type_t<entt::entity>>( it->second ) ) };
            }

            void Apply( const TL::ResolvedBinding& target, const std::string_view property,
                        const TL::EvaluatedValue& value ) override
            {
                const auto element = static_cast<entt::entity>(
                     static_cast<std::underlying_type_t<entt::entity>>( target.Handle ) );
                UIClipSample& sample = m_Frame.Samples[element];
                const auto*   vec    = std::get_if<glm::vec3>( &value );
                const auto*   scalar = std::get_if<float>( &value );
                if ( property == "Offset" && vec != nullptr )
                    sample.Offset += glm::vec2( *vec );
                else if ( property == "Size" && vec != nullptr )
                    sample.Size += glm::vec2( *vec );
                else if ( property == "Opacity" && scalar != nullptr )
                    sample.Tint.a *= *scalar;
                else if ( property == "Color" && vec != nullptr )
                    sample.Tint *= glm::vec4( *vec, 1.0F );
                else if ( property == "Reveal" && scalar != nullptr )
                    sample.Reveal = std::clamp( *scalar, 0.0F, 1.0F );
                else if ( property == "HazeAmplitude" && scalar != nullptr )
                    sample.HazeAmplitude = std::max( *scalar, 0.0F );
                else if ( m_Frame.Warned.insert( std::format( "property:{}", property ) ).second )
                    LOG_WARN( "[UI] a UI animation track drives '{}' ({} value), which no UI element has; the "
                              "track is skipped",
                              property, value.index() );
            }

            void Fire( const TL::FiredEvent& ) override
            {
            }
            void SetCamera( const std::optional<TL::ResolvedBinding>& ) override
            {
            }
            void PlayAnimation( const TL::ResolvedBinding&, const TL::AnimationSample& ) override
            {
            }

        private:
            const std::unordered_map<std::string, entt::entity>& m_ByUuid;
            UIClipFrame&                                         m_Frame;
        };

        /// The clip's Audio sections → the voices it sounds this frame (UIAnimData::Sounding). Only a step
        /// that MOVES FORWARD sounds — paused, stopped, scrubbed or played backwards it is silence (UE), and
        /// the executor's diff stops the voices; the next forward step starts them where the picture is.
        void CollectSounding( const TL::TimeStep& step, const TL::Sequence& sequence,
                              const TL::EvaluatedFrame& evaluated, std::vector<TL::SoundingVoice>& out )
        {
            out.clear();
            const double from    = step.From.AsTicks();
            const double to      = step.To.AsTicks();
            const bool   forward = step.Direction == TL::PlayDirection::Forward && !step.Reversed &&
                                 ( to > from || step.Wrapped );
            if ( !forward )
            {
                return;
            }
            const double rate     = sequence.TickRate.AsDouble();
            const double advanced = step.Wrapped ? 0.0 : ( to - from ) / rate;
            for ( const TL::AudioSample& sound : evaluated.Sounds )
            {
                out.push_back( TL::SoundingVoice{ 0, sound.TrackIndex, sound.SectionIndex, sound.Sound,
                                                  sound.SoundSeconds, advanced, sound.Gain } );
            }
        }

        void ReportUnresolved( const TL::ApplyReport& report, UIClipFrame& frame )
        {
            for ( const std::string& label : report.Unresolved )
            {
                if ( frame.Warned.insert( std::format( "binding:{}", label ) ).second )
                    LOG_WARN( "[UI] a UI animation binds element '{}', which is not in this scene; its tracks are "
                              "skipped",
                              label );
            }
        }
    } // namespace

    void PlayUIAnimations( entt::registry& reg, const float dtSeconds, const bool advance, const bool gameWorld,
                           UIClipFrame& frame )
    {
        frame.Samples.clear();
        auto clips = reg.view<ECS::UIAnimComponent>();
        if ( clips.empty() )
            return;

        // One map per frame: a clip may bind any element of the scene, not only its own.
        std::unordered_map<std::string, entt::entity> byUuid;
        for ( const auto e : reg.view<ECS::UUIDComponent>() )
            byUuid.emplace( reg.get<ECS::UUIDComponent>( e ).UUID.ToString(), e );

        UIHost host( byUuid, frame );
        for ( const auto e : clips )
        {
            ECS::UIAnimData& clip = clips.get<ECS::UIAnimComponent>( e ).Data;

            // ONLY THE DRIVING VIEW CREATES THE PLAYER, because creating it is where AutoPlay is honoured: a
            // preview that got there first would hand the viewport a player it had already decided about.
            // Until then every view evaluates the clip's first frame.
            if ( !clip.Playback.has_value() && !advance )
            {
                const Animation::FrameTime start{ clip.Sequence.Start, 0.0f };
                TL::Evaluator              evaluator( clip.Sequence );
                evaluator.Evaluate( TL::TimeStep{ start, start }, frame.Scratch );
                ReportUnresolved( evaluator.Apply( frame.Scratch, host ), frame );
                continue;
            }
            if ( !clip.Playback.has_value() )
            {
                clip.Playback.emplace( clip.Sequence.TickRate, clip.Sequence.Start, clip.Sequence.End );
                clip.Playback->SetLoopMode( clip.Loop );
                // AutoPlay is a GAME behaviour (UE: a widget animation plays when the game constructs the
                // widget, never in the designer). In an authored world the player waits at Start and the
                // viewport shows the frame under the playhead — the Sequencer's Play/scrub moves it. Entering
                // Play drops the player (Core::BeginPlay), so the game's run starts here at t = 0.
                if ( clip.AutoPlay && gameWorld )
                    clip.Playback->Play();
            }

            const TL::TimeStep step = advance ? clip.Playback->Advance( dtSeconds )
                                              : TL::TimeStep{ clip.Playback->Current(), clip.Playback->Current() };

            TL::Evaluator evaluator( clip.Sequence );
            evaluator.Evaluate( step, frame.Scratch );
            ReportUnresolved( evaluator.Apply( frame.Scratch, host ), frame );
            // Only the driving view moves the playhead, so only it says what sounds (a preview evaluating
            // the same playhead must not silence it).
            if ( advance )
                CollectSounding( step, clip.Sequence, frame.Scratch, clip.Sounding );
        }
    }
} // namespace Desert::UI
