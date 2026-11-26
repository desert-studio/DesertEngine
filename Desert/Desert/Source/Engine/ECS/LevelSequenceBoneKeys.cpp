#include <Engine/ECS/LevelSequenceBoneKeys.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>
#include <Engine/ECS/LevelSequenceComponent.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>
#include <Engine/ECS/TagComponent.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace Desert::ECS
{
    namespace
    {
        namespace T = Animation::Timeline;

        [[nodiscard]] std::string ActorName( const entt::registry& registry, const entt::entity entity )
        {
            return registry.valid( entity ) && registry.has<TagComponent>( entity )
                        ? registry.get<TagComponent>( entity ).Tag
                        : std::string( "the actor" );
        }

        struct AnimatorBone
        {
            const Animation::Animator* Animator = nullptr;
            uint32_t                   Bone     = 0;
        };

        [[nodiscard]] Common::ResultStr<AnimatorBone>
        FindAnimatorBone( const entt::registry& registry, const entt::entity entity, const std::string_view bone )
        {
            const auto* animation =
                 registry.valid( entity ) ? registry.try_get<AnimationComponent>( entity ) : nullptr;
            if ( animation == nullptr || !animation->Animator )
                return Common::MakeFormattedError<AnimatorBone>(
                     "{}: {} has no Animator (a Skinned Mesh with an Animation component)", bone,
                     ActorName( registry, entity ) );
            const auto index = animation->Animator->GetSkeleton().FindBoneIndex( std::string( bone ) );
            if ( !index )
                return Common::MakeFormattedError<AnimatorBone>( "{}: the skeleton of {} has no such bone", bone,
                                                                 ActorName( registry, entity ) );
            return Common::MakeSuccess( AnimatorBone{ animation->Animator.get(), *index } );
        }

        [[nodiscard]] Common::ResultStr<const T::Binding*>
        BoneBinding( const T::Sequence& sequence, const T::BindingGuid& bone, const char* what )
        {
            const T::Binding* bound = T::FindBinding( sequence, bone );
            if ( bound == nullptr || bound->Kind != T::BindingKind::Bone )
                return Common::MakeFormattedError<const T::Binding*>(
                     "{}: the binding is not a bone of this sequence", what );
            return Common::MakeSuccess( bound );
        }

        [[nodiscard]] T::Track* PoseTrack( T::Sequence& sequence, const T::BindingGuid& bone )
        {
            for ( T::Track& track : sequence.Tracks )
                if ( IsLevelPoseTrack( track, bone ) )
                    return &track;
            return nullptr;
        }

        [[nodiscard]] const T::Track* PoseTrack( const T::Sequence& sequence, const T::BindingGuid& bone )
        {
            for ( const T::Track& track : sequence.Tracks )
                if ( IsLevelPoseTrack( track, bone ) )
                    return &track;
            return nullptr;
        }

        /// The section covering @p tick that wins the fold (highest row, last in list order), or null.
        template <typename TrackT>
        [[nodiscard]] auto* WinningSection( TrackT& track, const Animation::FrameNumber tick )
        {
            decltype( &track.Sections.front() ) best = nullptr;
            for ( auto& section : track.Sections )
                if ( !( tick < section.Start ) && !( section.End < tick ) &&
                     ( best == nullptr || section.Row >= best->Row ) )
                    best = &section;
            return best;
        }

        [[nodiscard]] T::TransformChannel* TransformOf( T::Section& section )
        {
            auto* channel = std::get_if<T::Channel>( &section.Content );
            return channel != nullptr ? std::get_if<T::TransformChannel>( channel ) : nullptr;
        }

        /// The channel the key at @p tick lands in and the blend of its section.
        [[nodiscard]] Common::ResultStr<std::pair<T::TransformChannel*, T::SectionBlendType>>
        KeyTarget( T::Sequence& sequence, const T::BindingGuid& bone, const Animation::FrameNumber tick,
                   const char* what )
        {
            using Target     = std::pair<T::TransformChannel*, T::SectionBlendType>;
            const auto bound = BoneBinding( sequence, bone, what );
            if ( !bound )
                return Common::MakeFormattedError<Target>( "{}", bound.GetError() );
            T::Track* track = PoseTrack( sequence, bone );
            if ( track == nullptr )
                return Common::MakeFormattedError<Target>( "{} '{}': the bone has no Transform track", what,
                                                           bound.GetValue()->Label );
            if ( T::Section* section = WinningSection( *track, tick ) )
            {
                T::TransformChannel* channel = TransformOf( *section );
                if ( channel == nullptr )
                    return Common::MakeFormattedError<Target>( "{} '{}': the section holds no transform", what,
                                                               bound.GetValue()->Label );
                return Common::MakeSuccess( Target{ channel, section->Blend } );
            }
            // No section covers the tick: a new Absolute one over the gap (TrackEditing's rule for every pose).
            return Common::MakeSuccess(
                 Target{ &Animation::ChannelForKey( sequence, *track, tick ), T::SectionBlendType::Absolute } );
        }

        void SetDefaults( T::TransformChannel& channel, const Animation::BoneTransform& pose )
        {
            channel.Translation.X.Default = pose.Translation.x;
            channel.Translation.Y.Default = pose.Translation.y;
            channel.Translation.Z.Default = pose.Translation.z;
            channel.Rotation.X.Default    = pose.Rotation.x;
            channel.Rotation.Y.Default    = pose.Rotation.y;
            channel.Rotation.Z.Default    = pose.Rotation.z;
            channel.Rotation.W.Default    = pose.Rotation.w;
            channel.Scale.X.Default       = pose.Scale.x;
            channel.Scale.Y.Default       = pose.Scale.y;
            channel.Scale.Z.Default       = pose.Scale.z;
        }

        /// The value an Additive section holds for @p local over @p preLayer: the inverse of the Additive fold
        /// at weight 1 (Evaluator.cpp: pre + t, pre * r, pre * s).
        [[nodiscard]] Animation::BoneTransform ToOffset( const Animation::BoneTransform& local,
                                                         const Animation::BoneTransform& preLayer )
        {
            Animation::BoneTransform offset;
            offset.Translation = local.Translation - preLayer.Translation;
            offset.Rotation    = glm::normalize( glm::inverse( preLayer.Rotation ) * local.Rotation );
            offset.Scale       = local.Scale / preLayer.Scale;
            return offset;
        }

        [[nodiscard]] Animation::BoneTransform FromOffset( const Animation::BoneTransform& offset,
                                                           const Animation::BoneTransform& preLayer )
        {
            Animation::BoneTransform local;
            local.Translation = preLayer.Translation + offset.Translation;
            local.Rotation    = glm::normalize( preLayer.Rotation * offset.Rotation );
            local.Scale       = preLayer.Scale * offset.Scale;
            return local;
        }

        /// Re-expresses every key and the defaults of @p channel from @p from to @p to over @p preLayer, so the
        /// pose the section shows at @p preLayer is kept (UE: FK Control Rig "Convert to Additive / Absolute").
        [[nodiscard]] bool ConvertChannel( T::TransformChannel& channel, const T::SectionBlendType from,
                                           const T::SectionBlendType to, const Animation::BoneTransform& preLayer,
                                           const Animation::FrameRate& tickRate )
        {
            if ( from == to )
                return true;
            const auto convert = [&]( const Animation::BoneTransform& value )
            { return to == T::SectionBlendType::Additive ? ToOffset( value, preLayer ) : FromOffset( value, preLayer ); };
            std::vector<Animation::FrameNumber> ticks;
            for ( const T::FloatChannel* lane :
                  { &channel.Translation.X, &channel.Translation.Y, &channel.Translation.Z, &channel.Rotation.X,
                    &channel.Rotation.Y, &channel.Rotation.Z, &channel.Rotation.W, &channel.Scale.X, &channel.Scale.Y,
                    &channel.Scale.Z } )
                for ( const auto& key : lane->Keys )
                    ticks.push_back( key.Tick );
            std::ranges::sort( ticks, []( const auto a, const auto b ) { return a.Value < b.Value; } );
            ticks.erase( std::unique( ticks.begin(), ticks.end() ), ticks.end() );
            // Read every value before writing any: a key written first would change what the next one reads.
            std::vector<Animation::BoneTransform> values;
            values.reserve( ticks.size() );
            for ( const auto tick : ticks )
                values.push_back( convert( T::Evaluate( channel, Animation::FrameTime{ tick, 0.0F }, tickRate ) ) );
            Animation::BoneTransform rest;
            rest.Translation = { channel.Translation.X.Default, channel.Translation.Y.Default,
                                 channel.Translation.Z.Default };
            rest.Rotation    = glm::quat( channel.Rotation.W.Default, channel.Rotation.X.Default,
                                          channel.Rotation.Y.Default, channel.Rotation.Z.Default );
            rest.Scale = { channel.Scale.X.Default, channel.Scale.Y.Default, channel.Scale.Z.Default };
            SetDefaults( channel, convert( rest ) );
            for ( size_t i = 0; i < ticks.size(); ++i )
                if ( !Animation::SetTransformKey( channel, ticks[i], values[i], tickRate ) )
                    return false;
            return true;
        }
    } // namespace

    Common::ResultStr<entt::entity> BoneActorEntity( const T::Sequence& sequence, entt::registry& registry,
                                                     const T::BindingGuid actor )
    {
        const T::Binding* parent = T::FindBinding( sequence, actor );
        if ( parent == nullptr || parent->Kind != T::BindingKind::Entity )
            return Common::MakeError<entt::entity>( "the actor is not an Entity binding of this sequence" );
        const LevelSequenceComponent noOverrides;
        LevelSequenceEntityHost      host( registry, noOverrides );
        const auto                   resolved = host.Resolve( *parent );
        if ( !resolved )
            return Common::MakeFormattedError<entt::entity>( "actor '{}' names no entity of this scene",
                                                             parent->Label );
        return Common::MakeSuccess( static_cast<entt::entity>( static_cast<uint32_t>( resolved->Handle ) ) );
    }

    std::vector<std::string> ActorBoneNames( const entt::registry& registry, const entt::entity actor )
    {
        const auto* animation = registry.valid( actor ) ? registry.try_get<AnimationComponent>( actor ) : nullptr;
        std::vector<std::string> names;
        if ( animation == nullptr || !animation->Animator )
            return names;
        for ( const auto& bone : animation->Animator->GetSkeleton().GetBones() )
            names.push_back( bone.Name );
        return names;
    }

    Common::ResultStr<T::BindingGuid> AddBoneBinding( T::Sequence& sequence, entt::registry& registry,
                                                      const T::BindingGuid actor, const std::string_view bone )
    {
        const auto resolved = BoneActorEntity( sequence, registry, actor );
        if ( !resolved )
            return Common::MakeFormattedError<T::BindingGuid>( "Add Bone Track '{}': {}", bone, resolved.GetError() );
        const entt::entity entity = resolved.GetValue();
        const auto         found  = FindAnimatorBone( registry, entity, bone );
        if ( !found )
            return Common::MakeFormattedError<T::BindingGuid>( "Add Bone Track {}", found.GetError() );

        const T::BindingGuid guid = T::BindingGuid::ForBone( actor, bone );
        if ( T::FindBinding( sequence, guid ) != nullptr )
            return Common::MakeSuccess( guid );

        const AnimatorBone&            target = found.GetValue();
        const Animation::LocalPose&    under  = target.Animator->GetPreLayerPose();
        const Animation::BoneTransform rest =
             target.Bone < under.Size() ? under[target.Bone] : Animation::BoneTransform{};

        sequence.Bindings.push_back(
             T::Binding{ guid, T::BindingKind::Bone, std::string( bone ), std::string( bone ), actor } );
        T::Track track;
        track.Binding = guid;
        track.Property.clear(); // a bone's local transform (Track.hpp)
        track.Kind          = T::TrackKind::Transform;
        T::Section& section = T::AddSection( track, sequence.Start, sequence.End );
        if ( T::TransformChannel* channel = TransformOf( section ) )
            SetDefaults( *channel, rest );
        sequence.Tracks.push_back( std::move( track ) );
        ++sequence.Revision;
        return Common::MakeSuccess( guid );
    }

    Common::BoolResultStr RemoveBoneBinding( T::Sequence& sequence, const T::BindingGuid bone )
    {
        if ( const auto bound = BoneBinding( sequence, bone, "Remove Bone Track" ); !bound )
            return Common::MakeFormattedError<bool>( "{}", bound.GetError() );
        std::erase_if( sequence.Tracks, [&]( const T::Track& track ) { return track.Binding == bone; } );
        std::erase_if( sequence.Bindings, [&]( const T::Binding& binding ) { return binding.Guid == bone; } );
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }

    T::BindingGuid FindBoneBinding( const T::Sequence& sequence, const T::BindingGuid actor,
                                    const std::string_view bone )
    {
        for ( const T::Binding& binding : sequence.Bindings )
            if ( binding.Kind == T::BindingKind::Bone && binding.Parent == actor && binding.Locator == bone )
                return binding.Guid;
        return {};
    }

    Common::ResultStr<Animation::BoneTransform> BonePose( const entt::registry& registry, const entt::entity actor,
                                                          const std::string_view bone )
    {
        const auto found = FindAnimatorBone( registry, actor, bone );
        if ( !found )
            return Common::MakeFormattedError<Animation::BoneTransform>( "{}", found.GetError() );
        return Common::MakeSuccess( found.GetValue().Animator->GetPublishedLocal( found.GetValue().Bone ) );
    }

    Common::ResultStr<Animation::BoneTransform>
    BonePreLayerPose( const entt::registry& registry, const entt::entity actor, const std::string_view bone )
    {
        const auto found = FindAnimatorBone( registry, actor, bone );
        if ( !found )
            return Common::MakeFormattedError<Animation::BoneTransform>( "{}", found.GetError() );
        const Animation::LocalPose& under = found.GetValue().Animator->GetPreLayerPose();
        if ( found.GetValue().Bone >= under.Size() )
            return Common::MakeFormattedError<Animation::BoneTransform>( "{}: the Animator has not posed yet",
                                                                         bone );
        return Common::MakeSuccess( under[found.GetValue().Bone] );
    }

    Common::BoolResultStr SetBoneKey( T::Sequence& sequence, const T::BindingGuid bone,
                                      const Animation::FrameNumber tick, const Animation::BoneTransform& local,
                                      const Animation::BoneTransform& preLayer )
    {
        const auto target = KeyTarget( sequence, bone, tick, "Key Bone" );
        if ( !target )
            return Common::MakeFormattedError<bool>( "{}", target.GetError() );
        const Animation::BoneTransform value =
             target.GetValue().second == T::SectionBlendType::Additive ? ToOffset( local, preLayer ) : local;
        if ( !Animation::SetTransformKey( *target.GetValue().first, tick, value, sequence.TickRate ) )
            return Common::MakeError( "Key Bone: the pose is not finite" );
        ++sequence.Revision; // a key changes what the bone shows at a tick the preview may be posed at
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetBoneChannelKey( T::Sequence& sequence, const T::BindingGuid bone,
                                             const Animation::FrameNumber    tick,
                                             const Animation::BoneTransform& value )
    {
        const auto target = KeyTarget( sequence, bone, tick, "Bone key" );
        if ( !target )
            return Common::MakeFormattedError<bool>( "{}", target.GetError() );
        if ( !Animation::SetTransformKey( *target.GetValue().first, tick, value, sequence.TickRate ) )
            return Common::MakeError( "Bone key: the value is not finite" );
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<Animation::BoneTransform>
    BoneChannelValue( const T::Sequence& sequence, const T::BindingGuid bone, const Animation::FrameNumber tick )
    {
        const T::Track*   track   = PoseTrack( sequence, bone );
        const T::Section* section = track != nullptr ? WinningSection( *track, tick ) : nullptr;
        const auto*       channel = section != nullptr ? std::get_if<T::Channel>( &section->Content ) : nullptr;
        const auto*       pose    = channel != nullptr ? std::get_if<T::TransformChannel>( channel ) : nullptr;
        if ( pose == nullptr )
            return Common::MakeFormattedError<Animation::BoneTransform>( "bone: no section covers tick {}",
                                                                         tick.Value );
        return Common::MakeSuccess( T::Evaluate( *pose, Animation::FrameTime{ tick, 0.0F }, sequence.TickRate ) );
    }

    std::vector<Animation::FrameNumber> BoneKeyTicks( const T::Sequence& sequence, const T::BindingGuid bone )
    {
        return EntityTransformKeyTicks( sequence, bone ); // IsLevelPoseTrack: one tick rule for actor and bone
    }

    Common::BoolResultStr SetBoneSectionBlend( T::Sequence& sequence, const T::BindingGuid bone,
                                               const T::SectionBlendType blend, const float weight,
                                               const Animation::BoneTransform& preLayer )
    {
        const auto bound = BoneBinding( sequence, bone, "Bone blend" );
        if ( !bound )
            return Common::MakeFormattedError<bool>( "{}", bound.GetError() );
        if ( !std::isfinite( weight ) )
            return Common::MakeFormattedError<bool>( "Bone blend '{}': the weight is not finite",
                                                     bound.GetValue()->Label );
        T::Track* track = PoseTrack( sequence, bone );
        if ( track == nullptr )
            return Common::MakeFormattedError<bool>( "Bone blend '{}': the bone has no Transform track",
                                                     bound.GetValue()->Label );
        for ( size_t i = 0; i < track->Sections.size(); ++i )
        {
            if ( T::TransformChannel* channel = TransformOf( track->Sections[i] );
                 channel != nullptr &&
                 !ConvertChannel( *channel, track->Sections[i].Blend, blend, preLayer, sequence.TickRate ) )
                return Common::MakeFormattedError<bool>( "Bone blend '{}': the converted pose is not finite",
                                                         bound.GetValue()->Label );
            track->Sections[i].Blend = blend;
            if ( auto cleared = T::ClearSectionWeight( *track, i ); !cleared )
                return cleared;
            if ( weight != 1.0F )
                if ( auto keyed = T::SetSectionWeightKey( *track, i, track->Sections[i].Start, weight ); !keyed )
                    return keyed;
        }
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<BoneSectionBlend> BoneSectionBlendOf( const T::Sequence&   sequence,
                                                            const T::BindingGuid bone )
    {
        const T::Track* track = PoseTrack( sequence, bone );
        if ( track == nullptr || track->Sections.empty() )
            return Common::MakeError<BoneSectionBlend>( "bone: no section" );
        const T::Section& section = track->Sections.front();
        return Common::MakeSuccess(
             BoneSectionBlend{ section.Blend, T::WeightAt( section, Animation::FrameTime{ section.Start, 0.0F },
                                                           sequence.TickRate ) } );
    }
} // namespace Desert::ECS
