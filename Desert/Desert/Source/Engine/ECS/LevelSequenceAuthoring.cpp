#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Track.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Desert::ECS
{
    namespace T = Animation::Timeline;

    Common::ResultStr<T::BindingGuid> AddEntityBinding( T::Sequence& sequence, const Common::UUID& entity,
                                                        const std::string& label )
    {
        if ( entity.IsNull() )
            return Common::MakeFormattedError<T::BindingGuid>( "add actor '{}': the entity has no UUID", label );
        const std::string    locator = std::to_string( static_cast<uint64_t>( entity ) );
        const T::BindingGuid guid    = T::BindingGuid::ForObject( T::BindingKind::Entity, locator );
        if ( T::FindBinding( sequence, guid ) == nullptr )
        {
            sequence.Bindings.push_back( T::Binding{ guid, T::BindingKind::Entity, locator, label, {} } );
            ++sequence.Revision;
        }
        return Common::MakeSuccess( guid );
    }

    Animation::BoneTransform EntityPose( const TransformComponent& transform )
    {
        Animation::BoneTransform pose;
        pose.Translation = transform.Translation;
        pose.Rotation    = glm::quat( transform.Rotation );
        pose.Scale       = transform.Scale;
        return pose;
    }

    Common::BoolResultStr SetEntityTransformKey( T::Sequence& sequence, const T::BindingGuid& binding,
                                                 const Animation::FrameNumber    tick,
                                                 const Animation::BoneTransform& pose )
    {
        const T::Binding* bound = T::FindBinding( sequence, binding );
        if ( bound == nullptr || bound->Kind != T::BindingKind::Entity )
            return Common::MakeError( "Transform key: the binding is not an actor of this sequence" );

        T::Track* track = nullptr;
        for ( T::Track& candidate : sequence.Tracks )
            if ( candidate.Binding == binding && candidate.Property == kLevelSequenceTransformProperty )
                track = &candidate;
        if ( track == nullptr )
        {
            T::Track created;
            created.Binding  = binding;
            created.Property = kLevelSequenceTransformProperty;
            created.Kind     = T::TrackKind::Transform;
            sequence.Tracks.push_back( std::move( created ) );
            ++sequence.Revision;
            track = &sequence.Tracks.back();
        }
        T::TransformChannel& channel = Animation::ChannelForKey( sequence, *track, tick );
        if ( !Animation::SetTransformKey( channel, tick, pose, sequence.TickRate ) )
            return Common::MakeFormattedError<bool>( "Transform key on '{}': the pose is not finite",
                                                     bound->Label );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AddCameraCut( T::Sequence& sequence, const T::BindingGuid& camera,
                                        const Animation::FrameNumber start, const Animation::FrameNumber end )
    {
        // Edited on a copy and kept only when the whole sequence still validates: "leaves it as it was" is
        // then true of every refusal, including the ones Validate states and this function does not repeat.
        T::Sequence edited = sequence;

        const T::BindingGuid master = T::BindingGuid::ForObject( T::BindingKind::Sequence, {} );
        if ( T::FindBinding( edited, master ) == nullptr )
            edited.Bindings.push_back( T::Binding{ master, T::BindingKind::Sequence, {}, "Camera Cuts", {} } );

        T::Track* track = nullptr;
        for ( T::Track& candidate : edited.Tracks )
            if ( candidate.Binding == master && candidate.Kind == T::TrackKind::CameraCut )
                track = &candidate;
        if ( track == nullptr )
        {
            T::Track created;
            created.Binding  = master;
            created.Property = kLevelSequenceCameraCutProperty;
            created.Kind     = T::TrackKind::CameraCut;
            edited.Tracks.push_back( std::move( created ) );
            track = &edited.Tracks.back();
        }
        T::Section section;
        section.Start   = start;
        section.End     = end;
        section.Content = T::CameraCutSectionContent{ camera };
        track->Sections.push_back( std::move( section ) );

        if ( const auto valid = T::Validate( edited ); !valid )
            return Common::MakeFormattedError<bool>( "Camera Cut: {}", valid.GetError() );
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    LevelSequenceStep LevelSequencePreview::Scrub( entt::registry& registry, const T::Sequence& sequence,
                                                   const Animation::FrameNumber tick )
    {
        LevelSequenceEntityHost host( registry, m_NoOverrides );

        // RECORD BEFORE THE FIRST WRITE, per entity: what the preview gives back is the scene as it was when
        // the sequence first touched it, not as the previous scrub left it.
        for ( const T::Binding& binding : sequence.Bindings )
        {
            const auto resolved = host.Resolve( binding );
            if ( !resolved )
                continue;
            const auto entity = static_cast<entt::entity>( static_cast<uint32_t>( resolved->Handle ) );
            if ( std::any_of( m_Saved.begin(), m_Saved.end(),
                              [&]( const Saved& saved ) { return saved.Entity == entity; } ) )
                continue;
            Saved saved;
            saved.Entity       = entity;
            saved.HadTransform = registry.has<TransformComponent>( entity );
            if ( saved.HadTransform )
                saved.Transform = registry.get<TransformComponent>( entity );
            saved.HadVisibility = registry.has<VisibilityComponent>( entity );
            if ( saved.HadVisibility )
                saved.Visible = registry.get<VisibilityComponent>( entity ).Visible;
            m_Saved.push_back( saved );
        }

        LevelSequencePlayback      playback( sequence );
        const Animation::FrameTime at{ tick, 0.0F };
        return StepLevelSequence( registry, m_NoOverrides, playback, T::TimeStep{ at, at } );
    }

    void LevelSequencePreview::Restore( entt::registry& registry )
    {
        for ( const Saved& saved : m_Saved )
        {
            if ( !registry.valid( saved.Entity ) )
                continue;
            if ( saved.HadTransform && registry.has<TransformComponent>( saved.Entity ) )
                registry.get<TransformComponent>( saved.Entity ) = saved.Transform;
            if ( saved.HadVisibility && registry.has<VisibilityComponent>( saved.Entity ) )
                registry.get<VisibilityComponent>( saved.Entity ).Visible = saved.Visible;
            else if ( !saved.HadVisibility && registry.has<VisibilityComponent>( saved.Entity ) )
                registry.remove<VisibilityComponent>( saved.Entity );
        }
        m_Saved.clear();
    }
} // namespace Desert::ECS
