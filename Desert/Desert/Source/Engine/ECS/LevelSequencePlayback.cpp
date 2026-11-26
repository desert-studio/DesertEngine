#include <Engine/ECS/LevelSequencePlayback.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/CameraComponent.hpp>
#include <Engine/ECS/LevelSequenceComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/ECS/VisibilityComponent.hpp>
#include <Engine/Scripting/ScriptComponent.hpp>
#include <Engine/ECS/SequencerEffectDrive.hpp>

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
        if ( binding.Kind == T::BindingKind::Bone )
        {
            // A bone is posed through its actor: it resolves to the parent Entity binding's entity, so a
            // bone of a present actor is never "unresolved" (a bone the skeleton lacks is PoseBones' refusal).
            if ( m_Playback == nullptr )
                return std::nullopt;
            const T::Binding* parent = T::FindBinding( m_Playback->Evaluator.GetSequence(), binding.Parent );
            if ( parent == nullptr || parent->Kind != T::BindingKind::Entity )
                return std::nullopt;
            return Resolve( *parent );
        }
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
            SetVisible( m_Registry, entity, *visible );
            return;
        }

        if ( !m_Registry.has<TransformComponent>( entity ) )
        {
            m_Refusals.push_back( std::format( "track '{}': its entity has no Transform", property ) );
            return;
        }
        LocalTransform transform = m_Registry.get<TransformComponent>( entity ).GetLocal();

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
        {
            refuse();
            return;
        }
        SetLocalTransform( m_Registry, entity, transform );
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
            case T::EventActionKind::PlayAudio:
                m_Sounds.push_back( action.Target );
                return;
            case T::EventActionKind::ActivateParticles:
                // UE UNiagaraComponent::Activate(true): the placed system starts bearing, from the start (the VFX
                // module's hook, SequencerEffectDrive.hpp).
                if ( const auto activate = SequencerEffectHooks().Activate;
                     !bound || activate == nullptr || !activate( m_Registry, entity ) )
                    m_Refusals.push_back( std::format(
                         "Event '{}': Activate Particles needs a bound entity with a VFX component", key->Name ) );
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

    void LevelSequenceEntityHost::DriveEffect( const T::ResolvedBinding& target, const T::VFXSample& sample )
    {
        const entt::entity entity = EntityOf( target );
        auto* drive = m_Registry.valid( entity ) ? SequencerEffectDriveOf( m_Registry, entity ) : nullptr;
        if ( drive == nullptr )
        {
            m_Refusals.push_back( "VFX track: the bound entity has no VFX component" );
            return;
        }
        *drive = VFXSequencerDrive{ sample.Active, sample.Active ? sample.AgeSeconds : 0.0 };
    }

    void LevelSequenceEntityHost::Begin( LevelSequencePlayback& playback, const std::string_view writer )
    {
        m_Playback = &playback;
        m_Writer   = writer;
    }

    void LevelSequenceEntityHost::PoseBones( const T::ResolvedBinding& parent, const T::BindingGuid parentGuid,
                                             const Animation::FrameTime at )
    {
        const entt::entity entity = EntityOf( parent );
        const std::string  actor  = m_Registry.valid( entity ) && m_Registry.has<TagComponent>( entity )
                                         ? m_Registry.get<TagComponent>( entity ).Tag
                                         : std::string( "the bound entity" );
        if ( m_Playback == nullptr || m_Writer.empty() )
        {
            m_Refusals.push_back( std::format( "bone tracks of {}: this player poses no bone", actor ) );
            return;
        }
        if ( !m_Registry.valid( entity ) || !m_Registry.has<AnimationComponent>( entity ) ||
             !m_Registry.get<AnimationComponent>( entity ).Animator )
        {
            m_Refusals.push_back( std::format( "bone tracks of {}: it has no Animator (a Skinned Mesh with an "
                                               "Animation component)",
                                               actor ) );
            return;
        }
        Animation::Animator&       animator = *m_Registry.get<AnimationComponent>( entity ).Animator;
        const Animation::Skeleton& skeleton = animator.GetSkeleton();
        const T::Sequence&         sequence = m_Playback->Evaluator.GetSequence();

        auto cache = std::find_if( m_Playback->BoneTables.begin(), m_Playback->BoneTables.end(),
                                   [&]( const LevelSequenceBoneCache& c ) { return c.Parent == parentGuid; } );
        if ( cache == m_Playback->BoneTables.end() )
        {
            m_Playback->BoneTables.push_back( LevelSequenceBoneCache{ parentGuid } );
            cache = std::prev( m_Playback->BoneTables.end() );
            cache->Table.Revision = sequence.Revision + 1; // never valid: bound below
        }
        if ( cache->Table.Revision != sequence.Revision || cache->Skeleton != &skeleton ||
             cache->BoneCount != skeleton.GetBones().size() )
        {
            cache->Skeleton        = &skeleton;
            cache->BoneCount       = skeleton.GetBones().size();
            cache->Table           = T::BindBones( sequence, skeleton, parentGuid );
            cache->MissingReported = false;
        }
        if ( cache->Table.Missing > 0 && !cache->MissingReported )
        {
            cache->MissingReported = true;
            for ( size_t i = 0; i < sequence.Tracks.size(); ++i )
            {
                const T::Binding* bone = T::FindBinding( sequence, sequence.Tracks[i].Binding );
                if ( bone != nullptr && bone->Kind == T::BindingKind::Bone && bone->Parent == parentGuid &&
                     sequence.Tracks[i].Kind == T::TrackKind::Transform &&
                     cache->Table.BoneOfTrack[i] == UINT32_MAX )
                    m_Refusals.push_back(
                         std::format( "{}: the skeleton of {} has no such bone", bone->Locator, actor ) );
            }
        }

        auto pending = std::find_if( m_Pending.begin(), m_Pending.end(),
                                     [entity]( const PendingLayer& p ) { return p.Entity == entity; } );
        if ( pending == m_Pending.end() )
        {
            m_Pending.push_back( PendingLayer{ entity, actor, m_Writer, {} } );
            pending = std::prev( m_Pending.end() );
        }
        // SampleBoneLayer appends, and refuses before appending anything: a refused table leaves the samples
        // gathered so far for this actor as they were.
        if ( const auto sampled = T::SampleBoneLayer( sequence, cache->Table, at, pending->Samples ); !sampled )
            m_Refusals.push_back( std::format( "bone tracks of {}: {}", actor, sampled.GetError() ) );
    }

    void LevelSequenceEntityHost::CommitBoneLayers()
    {
        for ( const PendingLayer& pending : m_Pending )
        {
            if ( !m_Registry.valid( pending.Entity ) )
                continue;
            auto* animation = m_Registry.try_get<AnimationComponent>( pending.Entity );
            if ( animation == nullptr || !animation->Animator )
                continue; // PoseBones refused it by name before gathering anything
            if ( const auto set = animation->Animator->SetSequencerLayer( pending.Writer, pending.Samples ); !set )
            {
                m_Refusals.push_back( std::format( "bone tracks of {}: {}", pending.Actor, set.GetError() ) );
                continue;
            }
            if ( std::find( m_Layered.begin(), m_Layered.end(), pending.Entity ) == m_Layered.end() )
                m_Layered.push_back( pending.Entity );
        }
        m_Pending.clear();
    }

    void ClearLevelSequenceBoneLayers( entt::registry& registry, std::vector<entt::entity>& entities,
                                       const std::string_view writer )
    {
        for ( const entt::entity entity : entities )
            if ( registry.valid( entity ) )
                if ( auto* animation = registry.try_get<AnimationComponent>( entity );
                     animation != nullptr && animation->Animator )
                    animation->Animator->ClearSequencerLayer( writer );
        entities.clear();
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
                std::format_to( std::back_inserter( text ), "{} -> ", SequenceName( source, *it ) );
            std::format_to( std::back_inserter( text ), "{}", SequenceName( source, again ) );
            return text;
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

                child->Writer = playback.Writer;
                host.Begin( *child, child->Writer );
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
        host.Begin( playback, playback.Writer );
        out.Report = playback.Evaluator.Apply( playback.Frame, host );
        std::vector<Common::Content::AssetGuid> path;
        if ( !playback.Asset.IsNull() )
            path.push_back( playback.Asset );
        StepSubsequences( host, playback, true, subsequences, path, out );
        // Every bone binding of the master and its subsequences was gathered: one layer per actor, set once.
        host.CommitBoneLayers();

        // A layer the previous step set and this one did not reach (its section ended, its track was muted or
        // removed) comes off: the actor shows its animation again, not the last keyed bone.
        std::vector<entt::entity> dropped;
        for ( const entt::entity entity : playback.Layered )
            if ( std::find( host.Layered().begin(), host.Layered().end(), entity ) == host.Layered().end() )
                dropped.push_back( entity );
        ClearLevelSequenceBoneLayers( registry, dropped, playback.Writer );
        playback.Layered = host.Layered();
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
