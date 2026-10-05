#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Track.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <utility>
#include <variant>

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

    namespace
    {
        constexpr std::array kPoseParts = { Animation::TrackChannel::Position, Animation::TrackChannel::Rotation,
                                            Animation::TrackChannel::Scale };

        T::Track* EntityTransformTrack( T::Sequence& sequence, const T::BindingGuid& binding )
        {
            for ( T::Track& candidate : sequence.Tracks )
                if ( candidate.Binding == binding && candidate.Property == kLevelSequenceTransformProperty )
                    return &candidate;
            return nullptr;
        }

        void AddTicks( const T::FloatChannel& lane, std::vector<Animation::FrameNumber>& out )
        {
            for ( const auto& key : lane.Keys )
                out.push_back( key.Tick );
        }

        /// Whether any lane of @p part has a key on @p tick in any section of @p track.
        bool PartKeyedOn( const T::Track& track, const Animation::TrackChannel part, const Animation::FrameNumber tick )
        {
            for ( const T::Section& section : track.Sections )
            {
                const auto* channel = std::get_if<T::Channel>( &section.Content );
                const auto* pose    = channel != nullptr ? std::get_if<T::TransformChannel>( channel ) : nullptr;
                if ( pose == nullptr )
                    continue;
                std::vector<Animation::FrameNumber> ticks;
                if ( part == Animation::TrackChannel::Position )
                    for ( const T::FloatChannel* lane : { &pose->Translation.X, &pose->Translation.Y, &pose->Translation.Z } )
                        AddTicks( *lane, ticks );
                else if ( part == Animation::TrackChannel::Scale )
                    for ( const T::FloatChannel* lane : { &pose->Scale.X, &pose->Scale.Y, &pose->Scale.Z } )
                        AddTicks( *lane, ticks );
                else
                    for ( const T::FloatChannel* lane :
                          { &pose->Rotation.X, &pose->Rotation.Y, &pose->Rotation.Z, &pose->Rotation.W } )
                        AddTicks( *lane, ticks );
                if ( std::ranges::find( ticks, tick ) != ticks.end() )
                    return true;
            }
            return false;
        }
    } // namespace

    std::vector<Animation::FrameNumber> EntityTransformKeyTicks( const T::Sequence&    sequence,
                                                                 const T::BindingGuid& binding )
    {
        std::vector<Animation::FrameNumber> ticks;
        for ( const T::Track& track : sequence.Tracks )
        {
            if ( track.Binding != binding || track.Property != kLevelSequenceTransformProperty )
                continue;
            for ( const T::Section& section : track.Sections )
            {
                const auto* channel = std::get_if<T::Channel>( &section.Content );
                const auto* pose    = channel != nullptr ? std::get_if<T::TransformChannel>( channel ) : nullptr;
                if ( pose == nullptr )
                    continue;
                for ( const T::FloatChannel* lane :
                      { &pose->Translation.X, &pose->Translation.Y, &pose->Translation.Z, &pose->Rotation.X,
                        &pose->Rotation.Y, &pose->Rotation.Z, &pose->Rotation.W, &pose->Scale.X, &pose->Scale.Y,
                        &pose->Scale.Z } )
                    AddTicks( *lane, ticks );
            }
        }
        std::ranges::sort( ticks, []( const auto a, const auto b ) { return a.Value < b.Value; } );
        ticks.erase( std::unique( ticks.begin(), ticks.end() ), ticks.end() );
        return ticks;
    }

    Common::BoolResultStr MoveEntityTransformKeys( T::Sequence& sequence, const T::BindingGuid& binding,
                                                   const std::vector<Animation::FrameNumber>& from,
                                                   const int32_t                              delta )
    {
        if ( delta == 0 || from.empty() )
            return Common::MakeSuccess( true );
        // Edited on a copy: a refusal half-way through leaves the sequence exactly as it was.
        T::Sequence edited = sequence;
        T::Track*   track  = EntityTransformTrack( edited, binding );
        if ( track == nullptr )
            return Common::MakeError( "Move keys: the binding has no Transform track" );
        const T::Binding* bound = T::FindBinding( edited, binding );
        const std::string label = bound != nullptr ? bound->Label : std::string( "actor" );

        // The leading key moves first, so a selection moved by less than its own spread steps into ticks its
        // own members have already vacated rather than onto them.
        std::vector<Animation::FrameNumber> order = from;
        std::ranges::sort( order, [delta]( const auto a, const auto b )
                           { return delta > 0 ? a.Value > b.Value : a.Value < b.Value; } );
        for ( const Animation::FrameNumber tick : order )
        {
            const Animation::FrameNumber to{ tick.Value + delta };
            bool                         movedAny = false;
            for ( const Animation::TrackChannel part : kPoseParts )
            {
                if ( !PartKeyedOn( *track, part, tick ) )
                    continue;
                if ( const auto moved = Animation::MoveTrackKey( edited, *track, label, part, tick, to ); !moved )
                    return Common::MakeFormattedError<bool>( "Move keys: {}", moved.GetError() );
                movedAny = true;
            }
            if ( !movedAny )
                return Common::MakeFormattedError<bool>( "Move keys on '{}': no key at tick {}", label,
                                                         tick.Value );
        }
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveEntityTransformKeys( T::Sequence& sequence, const T::BindingGuid& binding,
                                                     const std::vector<Animation::FrameNumber>& ticks )
    {
        if ( ticks.empty() )
            return Common::MakeSuccess( true );
        T::Sequence edited = sequence;
        T::Track*   track  = EntityTransformTrack( edited, binding );
        if ( track == nullptr )
            return Common::MakeError( "Delete keys: the binding has no Transform track" );
        const T::Binding* bound = T::FindBinding( edited, binding );
        const std::string label = bound != nullptr ? bound->Label : std::string( "actor" );
        for ( const Animation::FrameNumber tick : ticks )
        {
            bool removedAny = false;
            for ( const Animation::TrackChannel part : kPoseParts )
            {
                if ( !PartKeyedOn( *track, part, tick ) )
                    continue;
                if ( const auto removed = Animation::RemoveTrackKey( edited, *track, label, part, tick ); !removed )
                    return Common::MakeFormattedError<bool>( "Delete keys: {}", removed.GetError() );
                removedAny = true;
            }
            if ( !removedAny )
                return Common::MakeFormattedError<bool>( "Delete keys on '{}': no key at tick {}", label,
                                                         tick.Value );
        }
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<uint32_t> LevelSequenceAutoKey::Observe( entt::registry& registry, T::Sequence& sequence,
                                                               const Animation::FrameNumber tick, const bool held )
    {
        const LevelSequenceComponent noOverrides;
        LevelSequenceEntityHost      host( registry, noOverrides );
        const auto                   transformOf = [&]( const T::Binding& binding ) -> const TransformComponent*
        {
            if ( binding.Kind != T::BindingKind::Entity )
                return nullptr;
            const auto resolved = host.Resolve( binding );
            if ( !resolved )
                return nullptr;
            return registry.try_get<TransformComponent>(
                 static_cast<entt::entity>( static_cast<uint32_t>( resolved->Handle ) ) );
        };

        if ( held && !m_Held )
        {
            // The rising edge: what every bound actor looked like before the gesture touched it.
            m_Before.clear();
            for ( const T::Binding& binding : sequence.Bindings )
                if ( const TransformComponent* transform = transformOf( binding ) )
                    m_Before.emplace_back( binding.Guid, EntityPose( *transform ) );
        }
        const bool released = m_Held && !held;
        m_Held              = held;
        if ( !released )
            return Common::MakeSuccess( 0U );

        uint32_t keyed = 0;
        for ( const auto& [guid, before] : m_Before )
        {
            const T::Binding* binding = T::FindBinding( sequence, guid );
            const TransformComponent* transform = binding != nullptr ? transformOf( *binding ) : nullptr;
            if ( transform == nullptr )
                continue;
            const Animation::BoneTransform now = EntityPose( *transform );
            if ( now.Translation == before.Translation && now.Rotation == before.Rotation &&
                 now.Scale == before.Scale )
                continue;
            if ( const auto written = SetEntityTransformKey( sequence, guid, tick, now ); !written )
                return Common::MakeFormattedError<uint32_t>( "Auto Key: {}", written.GetError() );
            ++keyed;
        }
        m_Before.clear();
        return Common::MakeSuccess( keyed );
    }

    void LevelSequenceAutoKey::Reset()
    {
        m_Held = false;
        m_Before.clear();
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

    Common::BoolResultStr AddAnimationSection( T::Sequence& sequence, const T::BindingGuid& binding,
                                               const Common::Content::AssetGuid& clip,
                                               const Animation::FrameNumber      start,
                                               const Animation::FrameNumber end, const bool loop )
    {
        const T::Binding* bound = T::FindBinding( sequence, binding );
        if ( bound == nullptr || bound->Kind != T::BindingKind::Entity )
            return Common::MakeError( "Animation track: the binding is not an actor (Entity) binding" );

        T::Sequence edited = sequence;
        T::Track*   track  = nullptr;
        for ( T::Track& candidate : edited.Tracks )
            if ( candidate.Binding == binding && candidate.Kind == T::TrackKind::Animation )
                track = &candidate;
        if ( track == nullptr )
        {
            T::Track created;
            created.Binding  = binding;
            created.Property = kLevelSequenceAnimationProperty;
            created.Kind     = T::TrackKind::Animation;
            edited.Tracks.push_back( std::move( created ) );
            track = &edited.Tracks.back();
        }
        // UE stacks an overlapping skeletal section on the next row; the first row whose sections all miss
        // [start, end] takes it.
        int32_t row = 0;
        for ( bool clash = true; clash; )
        {
            clash =
                 std::any_of( track->Sections.begin(), track->Sections.end(), [&]( const T::Section& other )
                              { return other.Row == row && !( other.End < start ) && !( end < other.Start ); } );
            if ( clash )
                ++row;
        }
        T::Section section;
        section.Start   = start;
        section.End     = end;
        section.Row     = row;
        section.Content = T::AnimationSectionContent{ clip, Animation::FrameNumber{ 0 }, 1.0, loop };
        track->Sections.push_back( std::move( section ) );

        if ( const auto valid = T::Validate( edited ); !valid )
            return Common::MakeFormattedError<bool>( "Animation track: {}", valid.GetError() );
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    namespace
    {
        template <typename SequenceT>
        auto* VisibilityTrack( SequenceT& sequence, const T::BindingGuid& binding )
        {
            for ( auto& candidate : sequence.Tracks )
                if ( candidate.Binding == binding && candidate.Kind == T::TrackKind::Bool &&
                     candidate.Property == kLevelSequenceVisibilityProperty )
                    return &candidate;
            return static_cast<decltype( &sequence.Tracks.front() )>( nullptr );
        }

        /// Upsert a Constant 0/1 key, keeping `Keys` sorted with one key per tick (FloatChannel's invariant).
        void UpsertBit( T::FloatChannel& bits, const Animation::FrameNumber tick, const bool value )
        {
            Animation::ScalarKey key;
            key.Tick      = tick;
            key.Value     = value ? 1.0F : 0.0F;
            key.Interp    = Animation::KeyInterp::Constant;
            const auto at = std::ranges::lower_bound( bits.Keys, tick, {}, &Animation::ScalarKey::Tick );
            if ( at != bits.Keys.end() && at->Tick == tick )
                *at = key;
            else
                bits.Keys.insert( at, key );
        }

        /// The section a key at @p tick lands on and a value at @p tick is read from: the highest-row section
        /// whose range holds the tick (the one the fold lets win), else the first section.
        template <typename TrackT>
        auto* VisibilityChannelAt( TrackT& track, const Animation::FrameNumber tick )
        {
            decltype( std::get_if<T::BoolChannel>(
                 std::get_if<T::Channel>( &track.Sections.front().Content ) ) ) target    = nullptr;
            decltype( target )                                                  first     = nullptr;
            int32_t                                                             targetRow = -1;
            for ( auto& section : track.Sections )
            {
                auto* channel = std::get_if<T::Channel>( &section.Content );
                auto* bits    = channel != nullptr ? std::get_if<T::BoolChannel>( channel ) : nullptr;
                if ( bits == nullptr )
                    continue;
                if ( first == nullptr )
                    first = bits;
                if ( !( tick < section.Start ) && !( section.End < tick ) && section.Row > targetRow )
                {
                    target    = bits;
                    targetRow = section.Row;
                }
            }
            return target != nullptr ? target : first;
        }
    } // namespace

    Common::BoolResultStr AddVisibilityTrack( T::Sequence& sequence, const T::BindingGuid& binding,
                                              const bool current )
    {
        const T::Binding* bound = T::FindBinding( sequence, binding );
        if ( bound == nullptr || bound->Kind != T::BindingKind::Entity )
            return Common::MakeError( "Visibility track: the binding is not an actor (Entity) binding" );
        if ( VisibilityTrack( sequence, binding ) != nullptr )
            return Common::MakeFormattedError<bool>( "Visibility track: '{}' already has one", bound->Label );

        T::Sequence edited = sequence;
        T::Track    created;
        created.Binding  = binding;
        created.Property = kLevelSequenceVisibilityProperty;
        created.Kind     = T::TrackKind::Bool;
        T::BoolChannel channel;
        channel.Bits.Default = current ? 1.0F : 0.0F;
        UpsertBit( channel.Bits, sequence.Start, current );
        T::Section section;
        section.Start   = sequence.Start;
        section.End     = sequence.End;
        section.Content = T::Channel{ channel };
        created.Sections.push_back( std::move( section ) );
        edited.Tracks.push_back( std::move( created ) );

        if ( const auto valid = T::Validate( edited ); !valid )
            return Common::MakeFormattedError<bool>( "Visibility track: {}", valid.GetError() );
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    bool HasVisibilityTrack( const T::Sequence& sequence, const T::BindingGuid& binding )
    {
        return VisibilityTrack( sequence, binding ) != nullptr;
    }

    Common::BoolResultStr SetVisibilityKey( T::Sequence& sequence, const T::BindingGuid& binding,
                                            const Animation::FrameNumber tick, const bool visible )
    {
        T::Sequence edited = sequence;
        T::Track*   track  = VisibilityTrack( edited, binding );
        if ( track == nullptr )
            return Common::MakeError(
                 "Visibility key: the binding has no Visibility track (+ Track ▸ Visibility)" );

        T::BoolChannel* target = VisibilityChannelAt( *track, tick );
        if ( target == nullptr )
            return Common::MakeError( "Visibility key: the Visibility track has no section" );
        UpsertBit( target->Bits, tick, visible );

        if ( const auto valid = T::Validate( edited ); !valid )
            return Common::MakeFormattedError<bool>( "Visibility key: {}", valid.GetError() );
        // A key changes what the actor shows at a tick the preview may already be posed at: Revision++ is how
        // every cache of this sequence (the preview, the Player) learns it changed.
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    std::optional<bool> VisibilityAt( const T::Sequence& sequence, const T::BindingGuid& binding,
                                      const Animation::FrameNumber tick )
    {
        const T::Track* track = VisibilityTrack( sequence, binding );
        if ( track == nullptr || track->Sections.empty() )
            return std::nullopt;
        const T::BoolChannel* bits = VisibilityChannelAt( *track, tick );
        if ( bits == nullptr )
            return std::nullopt;
        return T::Evaluate( *bits, Animation::FrameTime{ tick, 0.0F }, sequence.TickRate );
    }

    std::vector<VisibilityKey> VisibilityKeys( const T::Sequence& sequence, const T::BindingGuid& binding )
    {
        std::vector<VisibilityKey> keys;
        const T::Track*            track = VisibilityTrack( sequence, binding );
        if ( track == nullptr )
            return keys;
        for ( const T::Section& section : track->Sections )
        {
            const auto* channel = std::get_if<T::Channel>( &section.Content );
            const auto* bits    = channel != nullptr ? std::get_if<T::BoolChannel>( channel ) : nullptr;
            if ( bits == nullptr )
                continue;
            for ( const Animation::ScalarKey& key : bits->Bits.Keys )
                keys.push_back( VisibilityKey{ key.Tick, key.Value != 0.0F } );
        }
        std::ranges::stable_sort( keys, {}, &VisibilityKey::Tick );
        return keys;
    }

    namespace
    {
        template <typename SequenceT>
        auto* MaterialParameterTrack( SequenceT& sequence, const T::BindingGuid& binding,
                                      const LevelSequenceMaterialParameter& parameter )
        {
            for ( auto& candidate : sequence.Tracks )
                if ( candidate.Binding == binding &&
                     ( candidate.Kind == T::TrackKind::Float || candidate.Kind == T::TrackKind::Vector ) &&
                     ParseLevelSequenceMaterialProperty( candidate.Property ) == parameter )
                    return &candidate;
            return static_cast<decltype( &sequence.Tracks.front() )>( nullptr );
        }

        /// Upsert a Linear key, keeping `Keys` sorted with one key per tick (FloatChannel's invariant).
        void UpsertLinear( T::FloatChannel& channel, const Animation::FrameNumber tick, const float value )
        {
            Animation::ScalarKey key;
            key.Tick      = tick;
            key.Value     = value;
            const auto at = std::ranges::lower_bound( channel.Keys, tick, {}, &Animation::ScalarKey::Tick );
            if ( at != channel.Keys.end() && at->Tick == tick )
                *at = key;
            else
                channel.Keys.insert( at, key );
        }

        /// Key @p value into @p channel (a Float channel takes .x, a Vector channel .xyz); false for any other.
        bool UpsertParameter( T::Channel& channel, const Animation::FrameNumber tick, const glm::vec4& value )
        {
            if ( auto* scalar = std::get_if<T::FloatChannel>( &channel ) )
            {
                UpsertLinear( *scalar, tick, value.x );
                return true;
            }
            if ( auto* vector = std::get_if<T::VectorChannel>( &channel ) )
            {
                UpsertLinear( vector->X, tick, value.x );
                UpsertLinear( vector->Y, tick, value.y );
                UpsertLinear( vector->Z, tick, value.z );
                return true;
            }
            return false;
        }

        /// The channel of @p track a key at @p tick goes to and a read at @p tick comes from: the highest-row
        /// section whose range holds the tick (the one the fold lets win), else the first channel section.
        template <typename TrackT>
        auto* MaterialChannelAt( TrackT& track, const Animation::FrameNumber tick )
        {
            decltype( std::get_if<T::Channel>( &track.Sections.front().Content ) ) target    = nullptr;
            decltype( target )                                                     first     = nullptr;
            int32_t                                                                targetRow = -1;
            for ( auto& section : track.Sections )
            {
                auto* channel = std::get_if<T::Channel>( &section.Content );
                if ( channel == nullptr )
                    continue;
                if ( first == nullptr )
                    first = channel;
                if ( !( tick < section.Start ) && !( section.End < tick ) && section.Row > targetRow )
                {
                    target    = channel;
                    targetRow = section.Row;
                }
            }
            return target != nullptr ? target : first;
        }
    } // namespace

    Common::BoolResultStr AddMaterialParameterTrack( T::Sequence& sequence, const T::BindingGuid& binding,
                                                     const LevelSequenceMaterialParameter& parameter,
                                                     const T::TrackKind kind, const glm::vec4& current )
    {
        const T::Binding* bound = T::FindBinding( sequence, binding );
        if ( bound == nullptr || bound->Kind != T::BindingKind::Entity )
            return Common::MakeError( "Material Parameter track: the binding is not an actor (Entity) binding" );
        if ( kind != T::TrackKind::Float && kind != T::TrackKind::Vector )
            return Common::MakeError( "Material Parameter track: a parameter is a Scalar (Float) or a Vector" );
        if ( parameter.Name.empty() || parameter.Name.find( '.' ) != std::string::npos )
            return Common::MakeError( "Material Parameter track: the parameter name is empty or holds a '.'" );
        if ( MaterialParameterTrack( sequence, binding, parameter ) != nullptr )
            return Common::MakeFormattedError<bool>( "Material Parameter track: '{}' already has one for {}",
                                                     bound->Label, LevelSequenceMaterialProperty( parameter ) );

        T::Sequence edited = sequence;
        T::Track    created;
        created.Binding    = binding;
        created.Property   = LevelSequenceMaterialProperty( parameter );
        created.Kind       = kind;
        T::Channel channel = T::MakeChannel( static_cast<T::ChannelKind>( kind ) );
        if ( auto* scalar = std::get_if<T::FloatChannel>( &channel ) )
            scalar->Default = current.x;
        else if ( auto* vector = std::get_if<T::VectorChannel>( &channel ) )
        {
            vector->X.Default = current.x;
            vector->Y.Default = current.y;
            vector->Z.Default = current.z;
        }
        UpsertParameter( channel, sequence.Start, current );
        T::Section section;
        section.Start   = sequence.Start;
        section.End     = sequence.End;
        section.Content = std::move( channel );
        created.Sections.push_back( std::move( section ) );
        edited.Tracks.push_back( std::move( created ) );

        if ( const auto valid = T::Validate( edited ); !valid )
            return Common::MakeFormattedError<bool>( "Material Parameter track: {}", valid.GetError() );
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    bool HasMaterialParameterTrack( const T::Sequence& sequence, const T::BindingGuid& binding,
                                    const LevelSequenceMaterialParameter& parameter )
    {
        return MaterialParameterTrack( sequence, binding, parameter ) != nullptr;
    }

    Common::BoolResultStr SetMaterialParameterKey( T::Sequence& sequence, const T::BindingGuid& binding,
                                                   const LevelSequenceMaterialParameter& parameter,
                                                   const Animation::FrameNumber tick, const glm::vec4& value )
    {
        T::Sequence edited = sequence;
        T::Track*   track  = MaterialParameterTrack( edited, binding, parameter );
        if ( track == nullptr )
            return Common::MakeFormattedError<bool>(
                 "Material Parameter key: the binding has no track for {} (+ Track ▸ Material Parameter)",
                 LevelSequenceMaterialProperty( parameter ) );

        T::Channel* target = track->Sections.empty() ? nullptr : MaterialChannelAt( *track, tick );
        if ( target == nullptr || !UpsertParameter( *target, tick, value ) )
            return Common::MakeError( "Material Parameter key: the track has no Float / Vector section" );

        if ( const auto valid = T::Validate( edited ); !valid )
            return Common::MakeFormattedError<bool>( "Material Parameter key: {}", valid.GetError() );
        edited.Revision = sequence.Revision + 1;
        sequence        = std::move( edited );
        return Common::MakeSuccess( true );
    }

    std::optional<glm::vec4> MaterialParameterAt( const T::Sequence& sequence, const T::BindingGuid& binding,
                                                  const LevelSequenceMaterialParameter& parameter,
                                                  const Animation::FrameNumber          tick )
    {
        const T::Track* track = MaterialParameterTrack( sequence, binding, parameter );
        if ( track == nullptr || track->Sections.empty() )
            return std::nullopt;
        const T::Channel* channel = MaterialChannelAt( *track, tick );
        if ( channel == nullptr )
            return std::nullopt;
        const Animation::FrameTime at{ tick, 0.0F };
        if ( const auto* scalar = std::get_if<T::FloatChannel>( channel ) )
            return glm::vec4( T::Evaluate( *scalar, at, sequence.TickRate ), 0.0F, 0.0F, 0.0F );
        if ( const auto* vector = std::get_if<T::VectorChannel>( channel ) )
            return glm::vec4( T::Evaluate( *vector, at, sequence.TickRate ), 0.0F );
        return std::nullopt;
    }

    std::vector<Animation::FrameNumber>
    MaterialParameterKeyTicks( const T::Sequence& sequence, const T::BindingGuid& binding,
                               const LevelSequenceMaterialParameter& parameter )
    {
        std::vector<Animation::FrameNumber> ticks;
        const T::Track*                     track = MaterialParameterTrack( sequence, binding, parameter );
        if ( track == nullptr )
            return ticks;
        const auto collect = [&ticks]( const T::FloatChannel& channel )
        {
            for ( const Animation::ScalarKey& key : channel.Keys )
                ticks.push_back( key.Tick );
        };
        for ( const T::Section& section : track->Sections )
        {
            const auto* channel = std::get_if<T::Channel>( &section.Content );
            if ( channel == nullptr )
                continue;
            if ( const auto* scalar = std::get_if<T::FloatChannel>( channel ) )
                collect( *scalar );
            else if ( const auto* vector = std::get_if<T::VectorChannel>( channel ) )
            {
                collect( vector->X );
                collect( vector->Y );
                collect( vector->Z );
            }
        }
        std::ranges::sort( ticks, {}, &Animation::FrameNumber::Value );
        const auto repeated = std::ranges::unique( ticks, {}, &Animation::FrameNumber::Value );
        ticks.erase( repeated.begin(), repeated.end() );
        return ticks;
    }

    std::vector<std::pair<LevelSequenceMaterialParameter, T::TrackKind>>
    MaterialParameterTracks( const T::Sequence& sequence, const T::BindingGuid& binding )
    {
        std::vector<std::pair<LevelSequenceMaterialParameter, T::TrackKind>> tracks;
        for ( const T::Track& track : sequence.Tracks )
        {
            if ( track.Binding != binding ||
                 ( track.Kind != T::TrackKind::Float && track.Kind != T::TrackKind::Vector ) )
                continue;
            if ( auto parameter = ParseLevelSequenceMaterialProperty( track.Property ) )
                tracks.emplace_back( std::move( *parameter ), track.Kind );
        }
        return tracks;
    }

    T::BindingGuid LevelSequenceMasterBinding()
    {
        return T::BindingGuid::ForObject( T::BindingKind::Sequence, {} );
    }

    namespace
    {
        template <typename SequenceT>
        auto* EventTrack( SequenceT& sequence, const T::BindingGuid& binding )
        {
            for ( auto& candidate : sequence.Tracks )
                if ( candidate.Binding == binding && candidate.Kind == T::TrackKind::Event &&
                     candidate.Property == kLevelSequenceEventProperty )
                    return &candidate;
            return static_cast<decltype( &sequence.Tracks.front() )>( nullptr );
        }

        T::EventChannel* EventChannelOf( T::Section& section )
        {
            auto* channel = std::get_if<T::Channel>( &section.Content );
            return channel != nullptr ? std::get_if<T::EventChannel>( channel ) : nullptr;
        }

        /// The (channel, key index) of the event at flat @p index in `EventKeys` order; nullopt past the end.
        std::optional<std::pair<T::EventChannel*, size_t>> EventAt( T::Track& track, size_t index )
        {
            for ( T::Section& section : track.Sections )
            {
                T::EventChannel* events = EventChannelOf( section );
                if ( events == nullptr )
                    continue;
                if ( index < events->Keys.size() )
                    return std::pair{ events, index };
                index -= events->Keys.size();
            }
            return std::nullopt;
        }

        /// The flat `EventKeys` index of key @p local of @p events within @p track.
        size_t FlatIndexOf( T::Track& track, const T::EventChannel* events, const size_t local )
        {
            size_t before = 0;
            for ( T::Section& section : track.Sections )
            {
                T::EventChannel* candidate = EventChannelOf( section );
                if ( candidate == nullptr )
                    continue;
                if ( candidate == events )
                    return before + local;
                before += candidate->Keys.size();
            }
            return before + local;
        }

        /// Inserts @p key after every key on or before its tick (several events on one tick fire in list order,
        /// so a new one fires last); returns its index in the channel.
        size_t InsertEvent( T::EventChannel& events, T::EventKey key )
        {
            const auto at = std::ranges::upper_bound( events.Keys, key.Tick, {}, &T::EventKey::Tick );
            return static_cast<size_t>( events.Keys.insert( at, std::move( key ) ) - events.Keys.begin() );
        }

        Common::BoolResultStr Commit( T::Sequence& sequence, T::Sequence& edited, const char* what )
        {
            if ( const auto valid = T::Validate( edited ); !valid )
                return Common::MakeFormattedError<bool>( "{}: {}", what, valid.GetError() );
            edited.Revision = sequence.Revision + 1;
            sequence        = std::move( edited );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::BoolResultStr AddEventTrack( T::Sequence& sequence, const T::BindingGuid& binding )
    {
        T::Sequence    edited = sequence;
        const bool     master = binding == LevelSequenceMasterBinding();
        const T::Binding* bound = T::FindBinding( edited, binding );
        if ( master && bound == nullptr )
            edited.Bindings.push_back( T::Binding{ binding, T::BindingKind::Sequence, {}, "Sequence", {} } );
        else if ( !master && ( bound == nullptr || bound->Kind != T::BindingKind::Entity ) )
            return Common::MakeError( "Event track: the binding is neither an actor nor the sequence" );
        if ( EventTrack( edited, binding ) != nullptr )
            return Common::MakeError( "Event track: this binding already has one" );

        T::Track created;
        created.Binding  = binding;
        created.Property = kLevelSequenceEventProperty;
        created.Kind     = T::TrackKind::Event;
        T::Section section;
        section.Start   = sequence.Start;
        section.End     = sequence.End;
        section.Content = T::Channel{ T::EventChannel{} };
        created.Sections.push_back( std::move( section ) );
        edited.Tracks.push_back( std::move( created ) );
        return Commit( sequence, edited, "Event track" );
    }

    bool HasEventTrack( const T::Sequence& sequence, const T::BindingGuid& binding )
    {
        return EventTrack( sequence, binding ) != nullptr;
    }

    std::vector<LevelEventKey> EventKeys( const T::Sequence& sequence, const T::BindingGuid& binding )
    {
        std::vector<LevelEventKey> keys;
        const T::Track*            track = EventTrack( sequence, binding );
        if ( track == nullptr )
            return keys;
        for ( const T::Section& section : track->Sections )
        {
            const auto* channel = std::get_if<T::Channel>( &section.Content );
            const auto* events  = channel != nullptr ? std::get_if<T::EventChannel>( channel ) : nullptr;
            if ( events == nullptr )
                continue;
            for ( const T::EventKey& key : events->Keys )
                keys.push_back( LevelEventKey{ key.Tick, key.Name } );
        }
        return keys;
    }

    Common::ResultStr<size_t> AddEventKey( T::Sequence& sequence, const T::BindingGuid& binding,
                                           const Animation::FrameNumber tick, std::string name )
    {
        if ( name.empty() )
            return Common::MakeError<size_t>( "Event key: an event needs a name" );
        T::Sequence edited = sequence;
        T::Track*   track  = EventTrack( edited, binding );
        if ( track == nullptr )
            return Common::MakeError<size_t>( "Event key: the binding has no Event track (+ Track ▸ Event)" );

        // The highest-row section whose range holds the tick, else the first event section.
        T::EventChannel* target    = nullptr;
        T::EventChannel* first     = nullptr;
        int32_t          targetRow = -1;
        for ( T::Section& section : track->Sections )
        {
            T::EventChannel* events = EventChannelOf( section );
            if ( events == nullptr )
                continue;
            if ( first == nullptr )
                first = events;
            if ( section.Covers( tick ) && section.Row > targetRow )
            {
                target    = events;
                targetRow = section.Row;
            }
        }
        if ( target == nullptr )
            target = first;
        if ( target == nullptr )
            return Common::MakeError<size_t>( "Event key: the Event track has no section" );

        T::EventKey key;
        key.Tick          = tick;
        key.Name          = std::move( name );
        const size_t flat = FlatIndexOf( *track, target, InsertEvent( *target, std::move( key ) ) );
        if ( const auto committed = Commit( sequence, edited, "Event key" ); !committed )
            return Common::MakeError<size_t>( committed.GetError() );
        return Common::MakeSuccess( flat );
    }

    Common::BoolResultStr RenameEventKey( T::Sequence& sequence, const T::BindingGuid& binding, const size_t index,
                                          std::string name )
    {
        if ( name.empty() )
            return Common::MakeError( "Rename event: an event needs a name" );
        T::Sequence edited = sequence;
        T::Track*   track  = EventTrack( edited, binding );
        const auto  at     = track != nullptr ? EventAt( *track, index ) : std::nullopt;
        if ( !at )
            return Common::MakeFormattedError<bool>( "Rename event: no event {} on this binding", index );
        at->first->Keys[at->second].Name = std::move( name );
        return Commit( sequence, edited, "Rename event" );
    }

    Common::ResultStr<size_t> MoveEventKey( T::Sequence& sequence, const T::BindingGuid& binding, const size_t index,
                                            const int32_t delta )
    {
        T::Sequence edited = sequence;
        T::Track*   track  = EventTrack( edited, binding );
        const auto  at     = track != nullptr ? EventAt( *track, index ) : std::nullopt;
        if ( !at )
            return Common::MakeFormattedError<size_t>( "Move event: no event {} on this binding", index );
        T::EventKey key = std::move( at->first->Keys[at->second] );
        at->first->Keys.erase( at->first->Keys.begin() + static_cast<std::ptrdiff_t>( at->second ) );
        key.Tick.Value += delta;
        if ( key.Tick < edited.Start || edited.End < key.Tick )
            return Common::MakeFormattedError<size_t>( "Move event '{}': tick {} is outside the playback range",
                                                       key.Name, key.Tick.Value );
        const size_t flat = FlatIndexOf( *track, at->first, InsertEvent( *at->first, std::move( key ) ) );
        if ( const auto committed = Commit( sequence, edited, "Move event" ); !committed )
            return Common::MakeError<size_t>( committed.GetError() );
        return Common::MakeSuccess( flat );
    }

    Common::BoolResultStr RemoveEventKey( T::Sequence& sequence, const T::BindingGuid& binding, const size_t index )
    {
        T::Sequence edited = sequence;
        T::Track*   track  = EventTrack( edited, binding );
        const auto  at     = track != nullptr ? EventAt( *track, index ) : std::nullopt;
        if ( !at )
            return Common::MakeFormattedError<bool>( "Delete event: no event {} on this binding", index );
        at->first->Keys.erase( at->first->Keys.begin() + static_cast<std::ptrdiff_t>( at->second ) );
        return Commit( sequence, edited, "Delete event" );
    }

    LevelSequenceStep LevelSequencePreview::Scrub( entt::registry& registry, const T::Sequence& sequence,
                                                   const Animation::FrameNumber      tick,
                                                   const LevelSequenceClipSource&    clips,
                                                   const LevelSequenceMaterialSlots& materials )
    {
        LevelSequenceEntityHost host( registry, m_NoOverrides );
        m_Materials = materials;

        // Every slot override a Material Parameter track is about to write, before its first write.
        if ( materials )
            for ( const T::Track& track : sequence.Tracks )
            {
                const auto        parameter = ParseLevelSequenceMaterialProperty( track.Property );
                const T::Binding* bound     = parameter ? T::FindBinding( sequence, track.Binding ) : nullptr;
                const auto        resolved  = bound != nullptr ? host.Resolve( *bound ) : std::nullopt;
                if ( !resolved )
                    continue;
                const auto entity = static_cast<entt::entity>( static_cast<uint32_t>( resolved->Handle ) );
                if ( std::any_of( m_SavedMaterials.begin(), m_SavedMaterials.end(),
                                  [&]( const SavedMaterialParameter& saved )
                                  { return saved.Entity == entity && saved.Parameter == *parameter; } ) )
                    continue;
                m_SavedMaterials.push_back(
                     SavedMaterialParameter{ entity, *parameter, materials.Get( registry, entity, *parameter ) } );
            }

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
            if ( const auto* animation = registry.try_get<AnimationComponent>( entity );
                 animation != nullptr && animation->Animator )
            {
                saved.HadAnimator = true;
                saved.Clip        = animation->Animator->GetCurrentClip();
                saved.Tick        = animation->Animator->GetCurrentTick();
                saved.Loop        = animation->Loop;
                saved.Playing     = animation->Playing;
            }
            m_Saved.push_back( saved );
        }

        LevelSequencePlayback      playback( sequence );
        const Animation::FrameTime at{ tick, 0.0F };
        return StepLevelSequence( registry, m_NoOverrides, playback, T::TimeStep{ at, at }, clips, materials );
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
            if ( auto* animation = registry.try_get<AnimationComponent>( saved.Entity );
                 saved.HadAnimator && animation != nullptr && animation->Animator )
            {
                // The clip the entity played before the preview, at the tick it stood on; an entity that had
                // none keeps the sequence's (the Animator cannot be emptied back — Stop keeps the clip too).
                if ( saved.Clip != nullptr && animation->Animator->GetCurrentClip() != saved.Clip )
                {
                    animation->Animator->Play( *saved.Clip, saved.Loop );
                    animation->CurrentClip = saved.Clip->AnimationName;
                }
                if ( saved.Clip != nullptr )
                    animation->Animator->SetTick( saved.Tick );
                animation->Playing = saved.Playing;
            }
        }
        m_Saved.clear();
        for ( const SavedMaterialParameter& saved : m_SavedMaterials )
            if ( registry.valid( saved.Entity ) && m_Materials )
                (void)m_Materials.Set( registry, saved.Entity, saved.Parameter, saved.Override );
        m_SavedMaterials.clear();
    }
} // namespace Desert::ECS
