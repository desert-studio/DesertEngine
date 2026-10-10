#include <Engine/UI/Ecs/UIAnimationPlayback.hpp>

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
            UIHost( const std::unordered_map<std::string, entt::entity>& byUuid, TimelineUIAnimationSource& frame )
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
            TimelineUIAnimationSource&                           m_Frame;
        };

        void ReportUnresolved( const TL::ApplyReport& report, TimelineUIAnimationSource& frame )
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

    void TimelineUIAnimationSource::Evaluate( const IUITree& scene, const UIAnimationStep& uiStep )
    {
        // The clips live on the ECS (UIAnimComponent), which the tree does not carry: this source is the
        // engine half of the seam and is only ever handed the engine's own tree. Anything else is a host bug,
        // said once and drawn unanimated rather than guessed at.
        const auto* ecs = dynamic_cast<const EcsUITree*>( &scene );
        if ( ecs == nullptr )
        {
            Samples.clear();
            if ( !WarnedForeignTree )
            {
                WarnedForeignTree = true;
                LOG_ERROR( "[UI] the timeline animation source was handed a tree that is not the ECS scene; "
                           "UI clips do not play" );
            }
            return;
        }
        entt::registry& reg       = ecs->Registry();
        const float     dtSeconds = uiStep.DtSeconds;
        const bool      advance   = uiStep.Advance;
        const bool      gameWorld = uiStep.GameWorld;
        Samples.clear();
        auto clips = reg.view<ECS::UIAnimComponent>();
        if ( clips.empty() )
            return;

        // One map per frame: a clip may bind any element of the scene, not only its own.
        std::unordered_map<std::string, entt::entity> byUuid;
        for ( const auto e : reg.view<ECS::UUIDComponent>() )
            byUuid.emplace( reg.get<ECS::UUIDComponent>( e ).UUID.ToString(), e );

        UIHost host( byUuid, *this );
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
                evaluator.Evaluate( TL::TimeStep{ start, start }, Scratch );
                ReportUnresolved( evaluator.Apply( Scratch, host ), *this );
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
            evaluator.Evaluate( step, Scratch );
            ReportUnresolved( evaluator.Apply( Scratch, host ), *this );
        }
    }
} // namespace Desert::UI
