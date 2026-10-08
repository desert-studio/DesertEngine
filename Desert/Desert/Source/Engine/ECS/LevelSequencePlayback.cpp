#include <Engine/ECS/LevelSequencePlayback.hpp>

#include <Engine/ECS/Components.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <algorithm>
#include <charconv>
#include <format>

namespace Desert::ECS
{
    namespace
    {
        namespace T = Animation::Timeline;

        [[nodiscard]] entt::entity EntityOf( const T::ResolvedBinding& target )
        {
            return static_cast<entt::entity>( static_cast<uint32_t>( target.Handle ) );
        }

        [[nodiscard]] const char* KindName( const T::EvaluatedValue& value )
        {
            constexpr const char* kNames[] = { "float", "vector", "rotation", "transform", "bool" };
            return kNames[value.index()];
        }
    } // namespace

    std::optional<LevelSequenceMaterialParameter> ParseLevelSequenceMaterialProperty( std::string_view property )
    {
        if ( !property.starts_with( kLevelSequenceMaterialPropertyPrefix ) )
            return std::nullopt;
        property.remove_prefix( kLevelSequenceMaterialPropertyPrefix.size() );
        const auto dot = property.find( '.' );
        if ( dot == std::string_view::npos || dot == 0 || dot + 1 == property.size() )
            return std::nullopt;
        uint32_t   slot = 0;
        const auto read = std::from_chars( property.data(), property.data() + dot, slot );
        if ( read.ec != std::errc() || read.ptr != property.data() + dot )
            return std::nullopt;
        return LevelSequenceMaterialParameter{ slot, std::string( property.substr( dot + 1 ) ) };
    }

    std::string LevelSequenceMaterialProperty( const LevelSequenceMaterialParameter& parameter )
    {
        return std::format( "{}{}.{}", kLevelSequenceMaterialPropertyPrefix, parameter.Slot, parameter.Name );
    }

    LevelSequenceEntityHost::LevelSequenceEntityHost( entt::registry&               registry,
                                                      const LevelSequenceComponent& component,
                                                      LevelSequenceClipSource       clips,
                                                      LevelSequenceMaterialSlots    materials )
         : m_Registry( registry ), m_Component( component ), m_Clips( std::move( clips ) ),
           m_Materials( std::move( materials ) )
    {
    }

    std::optional<T::ResolvedBinding> LevelSequenceEntityHost::Resolve( const T::Binding& binding )
    {
        if ( binding.Kind != T::BindingKind::Entity )
            return std::nullopt;

        Common::UUID wanted = Common::UUID::Null();
        for ( const LevelSequenceBindingOverride& over : m_Component.BindingOverrides )
            if ( over.Binding == binding.Guid )
                wanted = over.Entity;
        if ( wanted.IsNull() )
        {
            uint64_t          value = 0;
            const auto* const end   = binding.Locator.data() + binding.Locator.size();
            const auto        read  = std::from_chars( binding.Locator.data(), end, value );
            if ( read.ec != std::errc() || read.ptr != end || value == 0 )
                return std::nullopt;
            wanted = Common::UUID( value );
        }

        for ( const auto entity : m_Registry.view<UUIDComponent>() )
            if ( static_cast<uint64_t>( m_Registry.get<UUIDComponent>( entity ).UUID ) ==
                 static_cast<uint64_t>( wanted ) )
                return T::ResolvedBinding{ static_cast<uint64_t>( static_cast<uint32_t>( entity ) ) };
        return std::nullopt;
    }

    void LevelSequenceEntityHost::Apply( const T::ResolvedBinding& target, std::string_view property,
                                         const T::EvaluatedValue& value )
    {
        const entt::entity entity = EntityOf( target );
        if ( !m_Registry.valid( entity ) )
        {
            m_Refusals.push_back( std::format( "track '{}': its entity no longer exists", property ) );
            return;
        }

        const auto refuse = [&]
        {
            m_Refusals.push_back( std::format( "track '{}' ({}): no entity property of that name and kind",
                                               property, KindName( value ) ) );
        };

        if ( const auto parameter = ParseLevelSequenceMaterialProperty( property ) )
        {
            if ( !m_Materials )
            {
                m_Refusals.push_back(
                     std::format( "track '{}': this player has no access to material slots", property ) );
                return;
            }
            std::optional<glm::vec4> written;
            if ( const float* scalar = std::get_if<float>( &value ) )
                written = glm::vec4( *scalar, 0.0F, 0.0F, 0.0F );
            else if ( const glm::vec3* color = std::get_if<glm::vec3>( &value ) )
            {
                // The track keys rgb; the alpha the slot already overrides stays (UE: a colour track's
                // unkeyed channel keeps the instance's value), 1 when the slot has no override yet.
                const auto current = m_Materials.Get( m_Registry, entity, *parameter );
                written            = glm::vec4( *color, current ? current->w : 1.0F );
            }
            if ( !written )
            {
                refuse();
                return;
            }
            if ( !m_Materials.Set( m_Registry, entity, *parameter, written ) )
                m_Refusals.push_back( std::format( "track '{}': its entity has no own material in slot {}",
                                                   property, parameter->Slot ) );
            return;
        }

        if ( property == "Visible" )
        {
            const bool* visible = std::get_if<bool>( &value );
            if ( visible == nullptr )
            {
                refuse();
                return;
            }
            auto& component   = m_Registry.has<VisibilityComponent>( entity )
                                     ? m_Registry.get<VisibilityComponent>( entity )
                                     : m_Registry.emplace<VisibilityComponent>( entity );
            component.Visible = *visible;
            return;
        }

        if ( !m_Registry.has<TransformComponent>( entity ) )
        {
            m_Refusals.push_back( std::format( "track '{}': its entity has no Transform", property ) );
            return;
        }
        auto& transform = m_Registry.get<TransformComponent>( entity );

        if ( property == "Transform" )
        {
            const auto* bone = std::get_if<Animation::BoneTransform>( &value );
            if ( bone == nullptr )
            {
                refuse();
                return;
            }
            transform.Translation = bone->Translation;
            transform.Rotation    = glm::eulerAngles( bone->Rotation );
            transform.Scale       = bone->Scale;
        }
        else if ( property == "Translation" || property == "Location" || property == "Scale" )
        {
            const auto* vector = std::get_if<glm::vec3>( &value );
            if ( vector == nullptr )
            {
                refuse();
                return;
            }
            ( property == "Scale" ? transform.Scale : transform.Translation ) = *vector;
        }
        else if ( property == "Rotation" )
        {
            const auto* rotation = std::get_if<glm::quat>( &value );
            if ( rotation == nullptr )
            {
                refuse();
                return;
            }
            transform.Rotation = glm::eulerAngles( *rotation );
        }
        else
            refuse();
    }

    void LevelSequenceEntityHost::Fire( const T::FiredEvent&                     event,
                                        const std::optional<T::ResolvedBinding>& target )
    {
        const T::EventKey* key = event.Event.Key;
        m_Fired.push_back( key != nullptr ? key->Name : std::string() );
        // A ranged key acts once, where it begins: its End edge is a marker, not a second action.
        if ( key == nullptr || !key->Action || event.Event.Edge == T::EventEdge::End )
            return;
        const T::EventAction& action = *key->Action;
        const entt::entity    entity = target ? EntityOf( *target ) : entt::entity( entt::null );
        const bool            bound  = target && m_Registry.valid( entity );
        switch ( action.Kind )
        {
            case T::EventActionKind::PlaySound:
                m_Sounds.push_back( action.Target );
                return;
            case T::EventActionKind::ActivateParticles:
                if ( !bound || !m_Registry.has<ParticleEmitterComponent>( entity ) )
                {
                    m_Refusals.push_back(
                         std::format( "Event '{}': Activate Particles needs a bound entity with a "
                                      "Particle Emitter component",
                                      key->Name ) );
                    return;
                }
                {
                    auto& emitter          = m_Registry.get<ParticleEmitterComponent>( entity );
                    emitter.Data.Enabled   = true;
                    emitter.RequestRestart = true;
                }
                return;
            case T::EventActionKind::CallScript:
                if ( !bound || !m_Registry.has<ScriptComponent>( entity ) )
                {
                    m_Refusals.push_back(
                         std::format( "Event '{}': Call Script '{}' needs a bound entity with a Script component",
                                      key->Name, action.Target ) );
                    return;
                }
                m_Registry.get<ScriptComponent>( entity ).PendingSequenceCalls.push_back(
                     SequenceScriptCall{ action.Target, key->Name } );
                return;
        }
        m_Refusals.push_back( std::format( "Event '{}': action kind {} is not known", key->Name,
                                           static_cast<int>( action.Kind ) ) );
    }

    void LevelSequenceEntityHost::SetCamera( const std::optional<T::ResolvedBinding>& camera )
    {
        if ( !camera )
        {
            m_CameraCut.reset();
            return;
        }
        const entt::entity entity = EntityOf( *camera );
        if ( !m_Registry.valid( entity ) || !m_Registry.has<CameraComponent>( entity ) )
        {
            m_Refusals.emplace_back( "Camera Cut: the bound entity has no Camera component" );
            m_CameraCut.reset();
            return;
        }
        m_CameraCut = entity;
    }

    void LevelSequenceEntityHost::PlayAnimation( const T::ResolvedBinding& target,
                                                 const T::AnimationSample& sample )
    {
        const std::string  clipText = Common::Content::AssetGuidToText( sample.Clip );
        const entt::entity entity   = EntityOf( target );
        if ( !m_Registry.valid( entity ) || !m_Registry.has<AnimationComponent>( entity ) ||
             !m_Registry.get<AnimationComponent>( entity ).Animator )
        {
            m_Refusals.push_back( std::format( "Animation section of clip {}: the bound entity has no Animator "
                                               "(a Skinned Mesh with an Animation component)",
                                               clipText ) );
            return;
        }
        if ( sample.Blend != T::SectionBlendType::Absolute || sample.Weight < 1.0F )
        {
            // UE blends overlapping skeletal sections by weight; this host poses ONE clip at full weight, so a
            // blended section is named rather than played as if it were absolute.
            m_Refusals.push_back( std::format( "Animation section of clip {}: only an absolute section at full "
                                               "weight poses the skeleton (weight {})",
                                               clipText, sample.Weight ) );
            return;
        }
        const Animation::AnimationClip* clip = m_Clips ? m_Clips( sample.Clip ) : nullptr;
        if ( clip == nullptr )
        {
            m_Refusals.push_back(
                 std::format( "Animation section of clip {}: no such clip is loaded", clipText ) );
            return;
        }

        // THE SKELETAL TIMELINE'S PATH (SequencerPanel's clip picker + scrub): pause the component so the ECS
        // Animation system does not advance the playhead the sequence set, make the clip current, move to the
        // tick. SetTick recomputes the pose now and wraps / clamps by the section's Loop.
        AnimationComponent&  animation = m_Registry.get<AnimationComponent>( entity );
        Animation::Animator& animator  = *animation.Animator;
        animation.Playing              = false;
        if ( animator.GetCurrentClip() != clip )
        {
            animator.Play( *clip, sample.Loop );
            animation.CurrentClip = clip->AnimationName;
        }
        else
        {
            animator.SetLoop( sample.Loop );
        }
        animator.SetTick( sample.ClipTime );
    }

    namespace
    {
        [[nodiscard]] std::string SequenceName( const LevelSequenceSubsequenceSource& source,
                                                const Common::Content::AssetGuid&     guid )
        {
            return source.Name ? source.Name( guid ) : Common::Content::AssetGuidToText( guid );
        }

        /// "subsequence cycle: A -> B -> A" — the path from @p again's first visit back to it, by name.
        [[nodiscard]] std::string CycleText( const std::vector<Common::Content::AssetGuid>& path,
                                             const Common::Content::AssetGuid&              again,
                                             const LevelSequenceSubsequenceSource&          source )
        {
            std::string text = "subsequence cycle: ";
            const auto  from = std::find( path.begin(), path.end(), again );
            for ( auto it = from; it != path.end(); ++it )
                text += SequenceName( source, *it ) + " -> ";
            return text + SequenceName( source, again );
        }

        /// Play every Subsequence sample of @p playback's evaluated frame (StepLevelSequence's contract).
        /// @p valuesAllowed is false under a parent leg that only fires events. @p path holds the sequences
        /// being played, root first.
        void StepSubsequences( LevelSequenceEntityHost& host, LevelSequencePlayback& playback,
                               const bool valuesAllowed, const LevelSequenceSubsequenceSource& source,
                               std::vector<Common::Content::AssetGuid>& path, LevelSequenceStep& out )
        {
            const T::Sequence& sequence = playback.Evaluator.GetSequence();
            for ( const T::SubsequenceSample& sample : playback.Frame.Subsequences )
            {
                const T::Section& section = sequence.Tracks[sample.TrackIndex].Sections[sample.SectionIndex];
                const auto&       content = std::get<T::SubsequenceSectionContent>( section.Content );
                if ( std::find( path.begin(), path.end(), content.Sequence ) != path.end() )
                {
                    host.Refuse( CycleText( path, content.Sequence, source ) );
                    continue;
                }
                const T::Sequence* sub = source.Find ? source.Find( content.Sequence ) : nullptr;
                if ( sub == nullptr )
                {
                    host.Refuse( std::format( "Subsequence {}: no such sequence is loaded",
                                              SequenceName( source, content.Sequence ) ) );
                    continue;
                }
                auto& child = playback.Children[{ sample.TrackIndex, sample.SectionIndex }];
                if ( !child || &child->Evaluator.GetSequence() != sub )
                {
                    child        = std::make_unique<LevelSequencePlayback>( *sub );
                    child->Asset = content.Sequence;
                }
                T::TimeStep childStep;
                childStep.From =
                     T::MapSubsequenceTime( section, content, sequence.TickRate, sub->TickRate, sample.From );
                childStep.To =
                     T::MapSubsequenceTime( section, content, sequence.TickRate, sub->TickRate, sample.To );
                childStep.Direction = sample.Direction;
                child->Evaluator.Evaluate( childStep, child->Frame );

                const bool values = valuesAllowed && sample.ValuesAt;
                const auto report = child->Evaluator.Apply(
                     child->Frame, host, values ? T::ApplyScope::Nested : T::ApplyScope::EventsOnly );
                out.Report.Unresolved.insert( out.Report.Unresolved.end(), report.Unresolved.begin(),
                                              report.Unresolved.end() );
                path.push_back( content.Sequence );
                StepSubsequences( host, *child, values, source, path, out );
                path.pop_back();
            }
        }
    } // namespace

    Common::BoolResultStr CheckSubsequenceCycles( const Common::Content::AssetGuid&     root,
                                                  const LevelSequenceSubsequenceSource& source )
    {
        std::vector<Common::Content::AssetGuid>                  path{ root };
        std::string                                              error;
        std::function<bool( const Common::Content::AssetGuid& )> visit =
             [&]( const Common::Content::AssetGuid& guid ) -> bool
        {
            const T::Sequence* sequence = source.Find ? source.Find( guid ) : nullptr;
            if ( sequence == nullptr )
                return true;
            for ( const T::Track& track : sequence->Tracks )
            {
                if ( track.Kind != T::TrackKind::Subsequence )
                    continue;
                for ( const T::Section& section : track.Sections )
                {
                    const auto* content = std::get_if<T::SubsequenceSectionContent>( &section.Content );
                    if ( content == nullptr )
                        continue;
                    if ( std::find( path.begin(), path.end(), content->Sequence ) != path.end() )
                    {
                        error = CycleText( path, content->Sequence, source );
                        return false;
                    }
                    path.push_back( content->Sequence );
                    if ( !visit( content->Sequence ) )
                        return false;
                    path.pop_back();
                }
            }
            return true;
        };
        if ( !visit( root ) )
            return Common::MakeFormattedError<bool>( "{}", error );
        return Common::MakeSuccess( true );
    }

    LevelSequencePlayback::LevelSequencePlayback( const T::Sequence& sequence )
         : Player( sequence.TickRate, sequence.Start, sequence.End ), Evaluator( sequence )
    {
    }

    LevelSequenceStep StepLevelSequence( entt::registry& registry, const LevelSequenceComponent& component,
                                         LevelSequencePlayback& playback, const T::TimeStep& step,
                                         const LevelSequenceClipSource&        clips,
                                         const LevelSequenceMaterialSlots&     materials,
                                         const LevelSequenceSubsequenceSource& subsequences )
    {
        playback.Evaluator.Evaluate( step, playback.Frame );
        LevelSequenceEntityHost host( registry, component, clips, materials );
        LevelSequenceStep       out;
        out.Report = playback.Evaluator.Apply( playback.Frame, host );
        std::vector<Common::Content::AssetGuid> path;
        if ( !playback.Asset.IsNull() )
            path.push_back( playback.Asset );
        StepSubsequences( host, playback, true, subsequences, path, out );
        out.CameraCut   = host.CameraCut();
        out.Refusals    = host.Refusals();
        out.FiredEvents = host.FiredEvents();
        out.Sounds      = host.Sounds();
        return out;
    }

    void ApplyLevelSequencePlaySettings( T::Player& player, const LevelSequenceComponent& component )
    {
        player.SetLoopMode( component.Loop );
        player.SetPlayRate( component.PlayRate );
    }

    LevelSequenceStep AdvanceLevelSequence( entt::registry& registry, const LevelSequenceComponent& component,
                                            LevelSequencePlayback& playback, const double seconds,
                                            const LevelSequenceClipSource&        clips,
                                            const LevelSequenceMaterialSlots&     materials,
                                            const LevelSequenceSubsequenceSource& subsequences )
    {
        ApplyLevelSequencePlaySettings( playback.Player, component );
        const T::TimeStep step = playback.Player.Advance( seconds );
        return StepLevelSequence( registry, component, playback, step, clips, materials, subsequences );
    }
    std::vector<std::string> TakeNewLevelSequenceErrors( LevelSequenceActorState& state,
                                                         const LevelSequenceStep& step )
    {
        std::vector<std::string> fresh;
        const auto               take = [&]( std::string message )
        {
            if ( state.Reported.insert( message ).second )
                fresh.push_back( std::move( message ) );
        };
        for ( const auto& label : step.Report.Unresolved )
            take( std::format( "binding '{}' names no entity of this scene; its tracks are not applied", label ) );
        for ( const auto& refusal : step.Refusals )
            take( refusal );
        return fresh;
    }

    std::optional<entt::entity> LevelSequenceViewTarget( LevelSequenceActorState& state,
                                                         const LevelSequenceStep& step, entt::entity current )
    {
        if ( step.CameraCut )
        {
            if ( !state.CutInForce )
            {
                state.TargetBeforeCut = current;
                state.CutInForce      = true;
            }
            return step.CameraCut;
        }
        if ( !state.CutInForce )
            return std::nullopt;
        state.CutInForce = false;
        return state.TargetBeforeCut;
    }
    std::vector<const T::Binding*> OverridableBindings( const T::Sequence&            sequence,
                                                        const LevelSequenceComponent& component )
    {
        std::vector<const T::Binding*> bindings;
        for ( const T::Binding& binding : sequence.Bindings )
        {
            if ( binding.Kind != T::BindingKind::Entity )
                continue;
            const bool overridden = std::any_of(
                 component.BindingOverrides.begin(), component.BindingOverrides.end(),
                 [&]( const LevelSequenceBindingOverride& over ) { return over.Binding == binding.Guid; } );
            if ( !overridden )
                bindings.push_back( &binding );
        }
        return bindings;
    }
} // namespace Desert::ECS
