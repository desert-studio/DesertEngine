#include "SequencerPanel.hpp"

#include "CurveView.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/ToastManager.hpp>
// SelectionManager and PanelContext are gone from this file with the selection it used to follow. What is
// left of Selection here is the AUTHORING CONTEXT: the mode the bone gizmo runs in and the bone it runs on,
// which keying by manipulation reads and — while this window is the one the user is working in — writes.
//
// THIS WINDOW IS WHY THAT TYPE EXISTS. The line below used to be
// `SkeletonEditMode::SetPoseMode( IsActive() && editClip != nullptr )`, written unconditionally every frame
// by EVERY open Sequencer into one process-wide bit; two characters open side by side (the thing making
// this a document bought) therefore decided each other's pose mode by draw order.
#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/GizmoState.hpp>

#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Editor/Panels/Sequencer/TimelineRuler.hpp>

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <ranges>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        // THE TIMELINE'S TWO GRIDS, AS THREE FUNCTIONS. Free rather than lambdas captured once, because the
        // ruler, the lanes, the drag and the inspector live in four different scopes of one very long
        // function and a capture that reaches only the first of them is how three of the four ended up
        // converting time their own way.
        [[nodiscard]] float TickToSeconds( Animation::FrameNumber tick, Animation::FrameRate tickRate )
        {
            return static_cast<float>(
                 Animation::FrameTimeToSeconds( Animation::FrameTime{ tick, 0.0F }, tickRate ) );
        }

        /// Seconds from a pixel, landing on the DISPLAY grid — the reason that grid is a field and not a
        /// number in a file. A retime that follows the mouse exactly puts a key at 0.4173 s, which no
        /// other key and no playhead position will ever equal again.
        [[nodiscard]] Animation::FrameNumber SecondsToSnappedTick( float seconds, Animation::FrameRate tickRate,
                                                                   Animation::FrameRate displayRate )
        {
            return Animation::SnapToDisplayRate(
                 Animation::SecondsToFrameTime( static_cast<double>( seconds ), tickRate ), tickRate,
                 displayRate );
        }

        /// A key's time, edited as the FRAME NUMBER an animator would state it in. The float seconds this
        /// replaces had a value between every pair of frames that the playhead could never reach, so a key
        /// authored there could not be keyed over a second time.
        [[nodiscard]] bool FrameField( Animation::FrameNumber& tick, Animation::FrameNumber duration,
                                       Animation::FrameRate tickRate, Animation::FrameRate displayRate )
        {
            int frame = Animation::DisplayFrameIndex( tick, tickRate, displayRate );
            if ( !::ImGui::DragInt( "Frame", &frame, 1.0f, 0,
                                    Animation::DisplayFrameIndex( duration, tickRate, displayRate ) ) )
            {
                return false;
            }
            const double ticksPerFrame = tickRate.AsDouble() / displayRate.AsDouble();
            tick                       = Animation::FrameNumber{
                 static_cast<int32_t>( std::llround( static_cast<double>( frame ) * ticksPerFrame ) ) };
            return true;
        }

        using Sequencer::DrawFrameGrid;
        using Sequencer::TimeAxis;
    } // namespace

    namespace ImGui = ::ImGui;

    SequencerPanel::SequencerPanel( const SubjectId& subject, const std::string& displayName,
                                    const Timeline timeline, const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    Animation::AnimationLibrary* library, Assets::AssetManager* assetManager )
         : ISubjectDocument( displayName, subject ), m_Scene( scene ), m_Library( library ),
           m_AssetManager( assetManager ), m_Timeline( timeline ),
           m_AuthoringOwner( Core::AuthoringOwner::ForDocument( subject ) )
    {
        // THE DOCUMENT'S CONTEXT IS ABOUT ITS OWN SUBJECT, fixed for its whole life exactly as the subject
        // is. This is the field that keeps two characters apart: the host adopts an incoming context only
        // when the entity matches, so a second Sequencer never inherits the first one's selected bone.
        m_Authoring.Entity = subject.Owner;
    }

    SequencerPanel::~SequencerPanel()
    {
        // Closing gives bone authoring back. Refused — and correctly ignored — when this window was not the
        // one holding it; see the note in ViewportPanel's destructor.
        (void)Core::ActiveAuthoringContext().Release( m_AuthoringOwner );
    }

    void SequencerPanel::TakeAuthoringContextIfFocused()
    {
        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) )
            Core::ActiveAuthoringContext().Focus( m_AuthoringOwner, m_Authoring );
    }

    std::optional<std::reference_wrapper<const ECS::Entity>> SequencerPanel::ResolveEntity() const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return std::nullopt;
        return scene->FindEntityByID( Subject().Owner );
    }

    bool SequencerPanel::IsSubjectAlive() const
    {
        const auto entOpt = ResolveEntity();
        if ( !entOpt )
            return false;

        const ECS::Entity& entity = entOpt->get();
        if ( m_Timeline == Timeline::UI )
            return entity.HasComponent<ECS::UIAnimComponent>();

        // BOTH, and the class note says why: a rig with no AnimationComponent has no clip to pick and no
        // animator to pose, so there is nothing this window could key. Asking for only the component the
        // subject is named after would leave a window open over half a subject, drawing an empty state —
        // which is the shape making it a document removes.
        return entity.HasComponent<ECS::SkinnedMeshComponent>() && entity.HasComponent<ECS::AnimationComponent>();
    }

    namespace
    {
        namespace TL = Animation::Timeline;

        // A hoverable "(?)" that shows a wrapped explanation — used to demystify advanced controls.
        void HelpMarker( const char* text )
        {
            ImGui::TextDisabled( ICON_MDI_HELP_CIRCLE_OUTLINE );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos( 340.0f );
                ImGui::TextUnformatted( text );
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }

        /// A lane index (0 = position, 1 = rotation, 2 = scale) as the channel it edits. Written once because
        /// it was written twice: the per-lane "+" and "Add Key @ Playhead" each spelled out the same mapping,
        /// and the second copy is how the two buttons came to disagree about what a key at the playhead is.
        Animation::TrackChannel ChannelOfLane( int lane )
        {
            if ( lane == 0 )
            {
                return Animation::TrackChannel::Position;
            }
            if ( lane == 1 )
            {
                return Animation::TrackChannel::Rotation;
            }
            return Animation::TrackChannel::Scale;
        }

        // ── THE CLIP'S TIMELINE, READ AND EDITED THROUGH THE ENGINE ──────────────────────────────────────
        //
        // A bone's keys are the `TransformChannel` of the sections of its Transform track (TrackEditing.hpp);
        // these helpers only FIND things in that model. Every structural edit (insert, remove, retime a key;
        // add, move, resize, re-row a section) is an engine function — TrackEditing's or Timeline/Track's.

        /// The bone a track animates, or null when it is not a bone's Transform track.
        const std::string* BoneOfTrack( const TL::Sequence& sequence, const TL::Track& track )
        {
            if ( track.Kind != TL::TrackKind::Transform || !track.Property.empty() )
            {
                return nullptr;
            }
            const TL::Binding* binding = TL::FindBinding( sequence, track.Binding );
            return binding != nullptr && binding->Kind == TL::BindingKind::Bone ? &binding->Locator : nullptr;
        }

        /// What a track row is called: its binding's locator (a bone, an element) and property, or its kind.
        std::string TrackLabel( const TL::Sequence& sequence, const TL::Track& track )
        {
            const TL::Binding* binding = TL::FindBinding( sequence, track.Binding );
            std::string        label   = binding != nullptr && !binding->Locator.empty()
                                              ? binding->Locator
                                              : std::string( TL::ToString( track.Kind ) );
            if ( !track.Property.empty() )
            {
                label += "." + track.Property;
            }
            return label;
        }

        TL::TransformChannel* TransformOf( TL::Section& section )
        {
            auto* channel = std::get_if<TL::Channel>( &section.Content );
            return channel != nullptr ? std::get_if<TL::TransformChannel>( channel ) : nullptr;
        }

        const TL::TransformChannel* TransformOf( const TL::Section& section )
        {
            const auto* channel = std::get_if<TL::Channel>( &section.Content );
            return channel != nullptr ? std::get_if<TL::TransformChannel>( channel ) : nullptr;
        }

        /// The part's components: x, y, z (and w for a rotation; null otherwise).
        std::array<TL::FloatChannel*, 4> ComponentsOf( TL::TransformChannel&   channel,
                                                       Animation::TrackChannel part )
        {
            switch ( part )
            {
                case Animation::TrackChannel::Position:
                    return { &channel.Translation.X, &channel.Translation.Y, &channel.Translation.Z, nullptr };
                case Animation::TrackChannel::Rotation:
                    return { &channel.Rotation.X, &channel.Rotation.Y, &channel.Rotation.Z, &channel.Rotation.W };
                case Animation::TrackChannel::Scale:
                    return { &channel.Scale.X, &channel.Scale.Y, &channel.Scale.Z, nullptr };
            }
            return { nullptr, nullptr, nullptr, nullptr };
        }

        bool ComponentHasKeyAt( const TL::FloatChannel& component, Animation::FrameNumber tick )
        {
            return std::ranges::any_of( component.Keys,
                                        [&]( const Animation::ScalarKey& k ) { return k.Tick == tick; } );
        }

        /// The part's key ticks over every section of the track, sorted and unique — one diamond per tick.
        std::vector<Animation::FrameNumber> PartTicks( const TL::Track& track, Animation::TrackChannel part )
        {
            std::vector<Animation::FrameNumber> ticks;
            for ( const TL::Section& section : track.Sections )
            {
                const TL::TransformChannel* channel = TransformOf( section );
                if ( channel == nullptr )
                {
                    continue;
                }
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — ComponentsOf only takes addresses
                for ( const TL::FloatChannel* component :
                      ComponentsOf( const_cast<TL::TransformChannel&>( *channel ), part ) )
                {
                    if ( component == nullptr )
                        continue;
                    for ( const Animation::ScalarKey& key : component->Keys )
                        ticks.push_back( key.Tick );
                }
            }
            std::ranges::sort( ticks, []( Animation::FrameNumber a, Animation::FrameNumber b ) { return a < b; } );
            ticks.erase( std::unique( ticks.begin(), ticks.end() ), ticks.end() );
            return ticks;
        }

        /// The section channel holding the part's key at @p tick (the one MoveBoneKey/RemoveBoneKey edit).
        TL::TransformChannel* ChannelHoldingPartKey( TL::Track& track, Animation::TrackChannel part,
                                                     Animation::FrameNumber tick )
        {
            for ( TL::Section& section : track.Sections )
            {
                TL::TransformChannel* channel = TransformOf( section );
                if ( channel == nullptr )
                    continue;
                for ( const TL::FloatChannel* component : ComponentsOf( *channel, part ) )
                {
                    if ( component != nullptr && ComponentHasKeyAt( *component, tick ) )
                        return channel;
                }
            }
            return nullptr;
        }

        /// One part's key at a tick, as the inspector edits it: each component and its key there (null when
        /// that component has none), and the key whose shape the part shows.
        struct PartKeys
        {
            std::array<TL::FloatChannel*, 4>     Components{};
            std::array<Animation::ScalarKey*, 4> Keys{};
            Animation::ScalarKey*                Shape = nullptr;
        };

        PartKeys PartKeysAt( TL::TransformChannel& channel, Animation::TrackChannel part,
                             Animation::FrameNumber tick )
        {
            PartKeys keys;
            keys.Components = ComponentsOf( channel, part );
            for ( size_t i = 0; i < keys.Components.size(); ++i )
            {
                if ( keys.Components[i] == nullptr )
                    continue;
                for ( Animation::ScalarKey& key : keys.Components[i]->Keys )
                {
                    if ( key.Tick == tick )
                    {
                        keys.Keys[i] = &key;
                        keys.Shape   = keys.Shape != nullptr ? keys.Shape : &key;
                    }
                }
            }
            return keys;
        }

        /// Write @p value at @p tick into one component: the key there, or a new one shaped like @p shape
        /// (a part's key is a key in every component; a component that lacked one gets it here).
        void SetComponentValue( TL::FloatChannel& component, Animation::FrameNumber tick, float value,
                                Animation::ScalarKey shape )
        {
            for ( Animation::ScalarKey& key : component.Keys )
            {
                if ( key.Tick == tick )
                {
                    key.Value = value;
                    return;
                }
            }
            shape.Tick    = tick;
            shape.Value   = value;
            const auto at = std::ranges::find_if( component.Keys,
                                                  [&]( const Animation::ScalarKey& k ) { return tick < k.Tick; } );
            component.Keys.insert( at, shape );
        }

        /// A value or shape edit made in place: the neighbours' Auto tangents follow (§969 item 1) and the
        /// revision moves, as every TrackEditing edit does (the Animator's binding cache is keyed by it).
        void FinishChannelEdit( TL::Sequence& sequence, TL::TransformChannel& channel )
        {
            Animation::RefreshTangents( channel, sequence.TickRate );
            ++sequence.Revision;
        }

        /// A key on one part of a bone at @p tick: what the curve says there (`InsertBoneKey`), or — when the
        /// part has no curve yet — the bone's pose on screen, in the section a key there lands in.
        Common::BoolResultStr KeyPartAtTick( Animation::AnimationClip& clip, const Animation::Animator& animator,
                                             const std::string& bone, Animation::TrackChannel part,
                                             Animation::FrameNumber tick )
        {
            TL::Track* track = Animation::FindBoneTrack( clip.Sequence, bone );
            if ( track == nullptr )
            {
                return Common::MakeFormattedError<bool>( "'{}' has no track in '{}'", bone, clip.AnimationName );
            }
            if ( !PartTicks( *track, part ).empty() )
            {
                return Animation::InsertBoneKey( clip.Sequence, bone, part, tick );
            }
            const auto boneIndex = animator.GetSkeleton().FindBoneIndex( bone );
            if ( !boneIndex.has_value() )
            {
                // No pose to record and no curve to read: adding nothing is the answer, the origin is not.
                return Common::MakeFormattedError<bool>( "'{}' is not a bone of this rig", bone );
            }
            if ( tick < clip.Sequence.Start || clip.Sequence.End < tick )
            {
                return Common::MakeFormattedError<bool>( "tick {} is outside the clip [{}, {}]", tick.Value,
                                                         clip.Sequence.Start.Value, clip.Sequence.End.Value );
            }
            TL::TransformChannel& channel = Animation::ChannelForKey( clip.Sequence, *track, tick );
            if ( !Animation::InsertFirstKeyFromPose( channel, part, tick,
                                                     animator.GetBoneLocalPose( boneIndex.value() ) ) )
            {
                return Common::MakeFormattedError<bool>( "'{}': the pose on screen does not decompose", bone );
            }
            FinishChannelEdit( clip.Sequence, channel );
            return Common::MakeSuccess( true );
        }

        /// Every scalar a channel is made of, in a fixed order — how a new section is seeded.
        std::vector<TL::FloatChannel*> FloatsOf( TL::Channel& channel )
        {
            if ( auto* f = std::get_if<TL::FloatChannel>( &channel ) )
                return { f };
            if ( auto* v = std::get_if<TL::VectorChannel>( &channel ) )
                return { &v->X, &v->Y, &v->Z };
            if ( auto* r = std::get_if<TL::RotationChannel>( &channel ) )
                return { &r->X, &r->Y, &r->Z, &r->W };
            if ( auto* t = std::get_if<TL::TransformChannel>( &channel ) )
                return { &t->Translation.X, &t->Translation.Y, &t->Translation.Z, &t->Rotation.X, &t->Rotation.Y,
                         &t->Rotation.Z,    &t->Rotation.W,    &t->Scale.X,       &t->Scale.Y,    &t->Scale.Z };
            if ( auto* b = std::get_if<TL::BoolChannel>( &channel ) )
                return { &b->Bits };
            return {};
        }

        /// The section whose value is SEEN at @p tick: highest row, then last in the list (the fold's order).
        const TL::Section* TopmostAt( const TL::Track& track, Animation::FrameNumber tick )
        {
            const TL::Section* top = nullptr;
            for ( const TL::Section& section : track.Sections )
            {
                if ( section.Covers( tick ) && ( top == nullptr || !( section.Row < top->Row ) ) )
                    top = &section;
            }
            return top;
        }

        constexpr std::array<Animation::TrackChannel, 3> kParts = { Animation::TrackChannel::Position,
                                                                    Animation::TrackChannel::Rotation,
                                                                    Animation::TrackChannel::Scale };

        /// Retime a control's key — a POSE, so every part keyed on @p from — all or nothing: a part that
        /// already has a key on @p to refuses the whole move, since a merge would silently delete a key.
        bool MoveControlKey( TL::Sequence& sequence, const std::string& bone, Animation::FrameNumber from,
                             Animation::FrameNumber to )
        {
            const TL::Track* track = Animation::FindBoneTrack( sequence, bone );
            if ( track == nullptr )
            {
                return false;
            }
            std::vector<Animation::TrackChannel> moving;
            for ( const Animation::TrackChannel part : kParts )
            {
                const auto ticks = PartTicks( *track, part );
                if ( std::ranges::find( ticks, from ) == ticks.end() )
                {
                    continue;
                }
                if ( std::ranges::find( ticks, to ) != ticks.end() )
                {
                    return false;
                }
                moving.push_back( part );
            }
            bool moved = !moving.empty();
            for ( const Animation::TrackChannel part : moving )
            {
                moved = Animation::MoveBoneKey( sequence, bone, part, from, to ).IsSuccess() && moved;
            }
            return moved;
        }
    } // namespace

    Animation::ControlKeyTarget SequencerPanel::KeyTargetFor( Animation::AnimationClip*  clip,
                                                              const Animation::Animator& animator ) const
    {
        // THE HAND-ROLLED UPSERT THAT USED TO BE HERE IS GONE, and its deletion is the change rather than
        // a tidy-up. `TrackEditing.hpp` says of `SetTransformKey`: "AN UPSERT, and the only one in the
        // tree" — and there were two. They did not agree: this one gave a NEW key the `KeyInterp` default
        // (Linear) while the engine's gives it Cubic/Auto, so a bone keyed from Record mode and the same
        // bone keyed through the control path produced differently shaped curves from identical gestures.
        //
        // Everything that was here — the track lookup, the tick snap, the three-channel write and the
        // tangent refresh — is `Animation::ControlKeyer`, where a suite can reach it.
        Animation::ControlKeyTarget target;
        target.Skeleton     = &animator.GetSkeleton();
        target.Clip         = clip;
        target.AuthoredPose = &animator.GetAuthoringPose();
        target.Tick         = Animation::SnapToDisplayRate(
             animator.GetCurrentTick(), clip != nullptr ? clip->Sequence.TickRate : Animation::PROJECT_TICK_RATE,
             clip != nullptr ? clip->Sequence.DisplayRate : Animation::DEFAULT_DISPLAY_RATE );
        return target;
    }

    // RequestOpen() AND ITS FILE-STATIC INBOX ARE GONE, and the deletion is the change rather than a
    // tidy-up. `static void RequestOpen()` carried no payload, so the Details button that called it could
    // only ever mean "reveal the one Sequencer window"; opening a SECOND rig beside the first was
    // inexpressible, and so was the window knowing which rig it was about. The button sends a SUBJECT now
    // (Core::SubjectOpenRequests), which is the one wire every document open goes through — see
    // Editor/Core/SubjectOpenRequest.hpp for why there are no longer three private ones.

    std::string SequencerPanel::CreateEmptyClip( const Animation::Skeleton&             skeleton,
                                                 const Animation::MeshSkeletonIdentity& mesh )
    {
        // A clip that references no skeleton plays on nothing (ClipPlaysOnMesh); the button is disabled then.
        if ( m_AssetManager == nullptr || m_Library == nullptr || mesh.Skeleton.Guid.IsNull() )
            return {};

        // Unique name so repeated "New Clip" presses don't collide (scan the clips this mesh already plays).
        const auto  existing = m_Library->GetForMesh( mesh );
        std::string name     = "NewClip";
        for ( int n = 1;; ++n )
        {
            bool taken = false;
            for ( const auto& a : existing )
                if ( a && a->GetClip().AnimationName == name )
                {
                    taken = true;
                    break;
                }
            if ( !taken )
                break;
            name = "NewClip_" + std::to_string( n );
        }

        Animation::AnimationClip clip;
        clip.AnimationName = name;
        // One second on the project grid, shown on the default display rate. `Duration = 1.0f` with
        // `TicksPerSecond = 25.0f` used to say "25 ticks" and mean "one second", which is the confusion
        // the tick grid exists to end.
        clip.Sequence.TickRate    = Animation::PROJECT_TICK_RATE;
        clip.Sequence.DisplayRate = Animation::DEFAULT_DISPLAY_RATE;
        clip.Sequence.Start       = Animation::FrameNumber{ 0 };
        clip.Sequence.End         = Animation::FrameNumber{ Animation::PROJECT_TICK_RATE.Numerator };
        clip.Skeleton = mesh.Skeleton.Guid; // the mesh's skeleton asset: the clip plays where it was authored
        // One Transform track per bone and no section yet: the first key of a bone makes its section
        // (TrackEditing::ChannelForKey), so an empty clip states nothing it would have to take back.
        for ( const auto& bone : skeleton.GetBones() )
        {
            static_cast<void>( Animation::AddBoneTrack( clip.Sequence, bone.Name ) );
        }

        auto asset = m_AssetManager->CreateAsset<Assets::AnimationAsset>(
             Common::Filepath( "memory://clip/" + name ), false );
        if ( !asset )
            return {};
        asset->SetInMemoryClip( clip );
        m_Library->Register( asset );
        return name;
    }

    // WHAT IS LEFT HERE IS THE PANEL'S PART: where the file goes. The clip -> `.anim` conversion and the
    // write moved to Assets::Serialization::SaveClipToFile, beside the loader that reads them back
    // (Д35) — this panel cannot be compiled into a test binary, and the write half of the format was
    // the half no suite could reach because of it.
    Common::ResultStr<std::string> SequencerPanel::SaveClipToDisk( const Animation::AnimationClip& clip )
    {
        // An authored clip is content (AF8b): it goes to the project's Animations/ folder, which the
        // registry gathers with the rest of the assets root.
        std::error_code ec;
        std::filesystem::create_directories( Common::Constants::Path::ANIMATION_PATH, ec );
        if ( ec )
            return Common::MakeFormattedError<std::string>(
                 "cannot create '{}': {}", Common::Constants::Path::ANIMATION_PATH.string(), ec.message() );
        const std::filesystem::path path =
             Common::Constants::Path::ANIMATION_PATH / ( clip.AnimationName + ".anim" );

        // IT RETURNS A RESULT AND NOT A PATH-OR-EMPTY-STRING. The old signature was `std::string`, an
        // empty one meaning failure — and its only caller, the Save button, discarded it, so a refusal
        // had nowhere to go. The reason has to reach the person who pressed the button, not the log
        // they do not have open.
        if ( const auto written = Assets::Serialization::SaveClipToFile( path, clip ); !written )
        {
            LOG_ERROR( "[Sequencer] {}", written.GetError() );
            return Common::MakeError<std::string>( written.GetError() );
        }

        LOG_INFO( "[Sequencer] Saved clip '{}' -> {}", clip.AnimationName, path.string() );
        return Common::MakeSuccess( path.string() );
    }

    // FOUR EMPTY STATES USED TO STAND HERE — no scene, nothing selected, selection not in the scene,
    // selection is neither a rig nor a UI element — and every one of them existed because the window was
    // about "whatever is selected" and therefore had to have an opinion about every way that can be
    // nothing. A document is about its subject: the only thing left to say is that the subject has gone,
    // and even that is one frame at most, because the editor's own liveness sweep (IsSubjectAlive) closes
    // the window with a named reason on the same frame it notices.
    void SequencerPanel::OnUIRender()
    {
        const auto entOpt = ResolveEntity();
        if ( !entOpt || !IsSubjectAlive() )
        {
            ImGui::TextDisabled( "What this timeline was editing no longer exists; closing." );
            return;
        }

        // Entity is a HANDLE and the resolution hands back a const one; copying gives a writable handle
        // onto the same entity, which is what both timelines edit through.
        ECS::Entity entity = entOpt->get();

        if ( m_Timeline == Timeline::UI )
        {
            DrawUITracks( entity );
            return;
        }

        DrawSkeletalTimeline( entity );
    }

    void SequencerPanel::SelectBoneFromTrack( uint32_t bone )
    {
        // CLICKING A TRACK IS THE USER WORKING IN THIS WINDOW, so take the context before writing rather
        // than waiting for the focus flag: on the click itself ImGui's focus can still be describing the
        // previous frame, and a selection dropped for one frame reads as "clicking that lane does nothing".
        // Focus() adopts the live context when it is about the same entity, so the mode and everything else
        // the viewport had is carried over untouched.
        Core::ActiveAuthoringContext().Focus( m_AuthoringOwner, m_Authoring );

        const auto picked = Core::ActiveAuthoringContext().SetSelectedBone( m_AuthoringOwner, m_Authoring, bone );
        if ( !picked.IsSuccess() )
            LOG_WARN( "[Sequencer] bone selection refused: {}", picked.GetError() );
    }

    void SequencerPanel::DrawSkeletalTimeline( ECS::Entity& entity )
    {
        TakeAuthoringContextIfFocused();

        // A TRANSACTION THAT LOST ITS CLOSER, SWEPT AT THE TOP OF THE FRAME. Every EXPLICIT driver in this
        // file is a widget being held, so one still open while ImGui reports no active item has lost the
        // widget that was going to close it — a tangent field stops being drawn the moment the combo above
        // it switches the key back to `Auto`, and its `IsItemDeactivated` never arrives. Left open, the
        // transaction would swallow every later edit into one enormous undo step, which is worse than the
        // no-undo state this change replaced. The EDGE-driven one is deliberately not swept: ImGuizmo
        // submits no ImGui item, so a bone drag looks identical to a lost widget from here.
        if ( m_ClipEdit.OpenExplicitly() && !ImGui::IsAnyItemActive() )
        {
            if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] {}", ended.GetError() );
            }
        }

        auto&       anim = entity.GetComponent<ECS::AnimationComponent>();
        const auto& smc  = entity.GetComponent<ECS::SkinnedMeshComponent>();
        // Editor-built runtime rig (Convert to Skinned) has no MeshHandle — prefer it (mirrors the render /
        // pick / animation paths) so a just-converted character animates in the Sequencer too.
        Desert::Mesh* mesh = smc.RuntimeMesh ? static_cast<Desert::Mesh*>( smc.RuntimeMesh.get() )
                                             : Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle );
        if ( !mesh || !mesh->IsSkinned() )
        {
            ImGui::TextDisabled( "Skinned mesh not resolved yet." );
            return;
        }
        const Animation::Skeleton& skeleton = static_cast<SkinnedMesh*>( mesh )->GetSkeleton();
        const auto                 clips    = m_Library != nullptr
                                                   ? m_Library->GetForMesh( m_Library->IdentifyMeshHandle( smc.MeshHandle ) )
                                                   : std::vector<Assets::Asset<Assets::AnimationAsset>>{};

        // Names for the clip combos + the index of the currently-selected clip.
        std::vector<const char*> clipNames;
        clipNames.reserve( clips.size() );
        int currentClipIdx = -1;
        for ( size_t i = 0; i < clips.size(); ++i )
        {
            clipNames.push_back( clips[i]->GetClip().AnimationName.c_str() );
            if ( clips[i]->GetClip().AnimationName == anim.CurrentClip )
                currentClipIdx = static_cast<int>( i );
        }

        Animation::Animator* animator = anim.Animator.get();

        // ---- Clip picker ----
        ImGui::SetNextItemWidth( 240.0f );
        if ( ImGui::Combo( "Clip", &currentClipIdx, clipNames.empty() ? nullptr : clipNames.data(),
                           static_cast<int>( clipNames.size() ) ) )
        {
            if ( currentClipIdx >= 0 && currentClipIdx < static_cast<int>( clips.size() ) )
            {
                anim.CurrentClip = clips[currentClipIdx]->GetClip().AnimationName;
                // PAUSE + Play the picked clip so it previews immediately and STAYS: an attached AnimGraph
                // drives the clip only while Playing, so pausing stops it from overriding the manual pick the
                // very next frame (the old bug where the viewport didn't change). Press Play to resume the graph.
                anim.Playing = false;
                if ( animator )
                {
                    animator->Play( clips[currentClipIdx]->GetClip(), anim.Loop );
                    animator->SetTime( 0.0f ); // Play() only sets the clip; SetTime recomputes the pose NOW
                                               // (paused => Update never runs, so the viewport would keep the
                                               // old clip's pose without this)
                }
            }
        }

        // ---- New empty clip (authored from scratch on this skeleton) ----
        ImGui::SameLine();
        if ( animator && m_AssetManager )
        {
            // The new clip references the mesh's skeleton asset; an editor-built rig (Convert to Skinned) has
            // none yet, so there is nothing a clip could name and the button says so instead of acting.
            const Animation::MeshSkeletonIdentity meshSkeleton =
                 m_Library != nullptr ? m_Library->IdentifyMeshHandle( smc.MeshHandle )
                                      : Animation::MeshSkeletonIdentity{};
            const bool canCreate = !meshSkeleton.Skeleton.Guid.IsNull();
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.40f, 0.28f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.50f, 0.36f, 1.0f ) );
            ImGui::BeginDisabled( !canCreate );
            const bool pressed = ImGui::Button( ICON_MDI_PLUS " New Clip" );
            ImGui::EndDisabled();
            if ( !canCreate )
                Utils::ImGuiUtilities::Tooltip(
                     "This mesh references no skeleton asset (an editor-built rig, or the "
                     "mesh is not loaded): a new clip would have no skeleton to name." );
            if ( pressed )
            {
                const std::string created = CreateEmptyClip( animator->GetSkeleton(), meshSkeleton );
                if ( !created.empty() )
                {
                    anim.CurrentClip = created;
                    anim.Playing     = false;
                    // Rebind the picker's clip list next frame; play the new (empty) clip so its lanes show.
                    for ( const auto& a : m_Library->GetForMesh( meshSkeleton ) )
                        if ( a && a->GetClip().AnimationName == created )
                        {
                            animator->Play( a->GetClip(), false );
                            animator->SetTime( 0.0f ); // recompute the pose now (see the clip picker note)
                            break;
                        }
                }
            }
            ImGui::PopStyleColor( 2 );
        }

        // ---- Save the authored clip to disk so it persists across sessions ----
        if ( animator && animator->GetCurrentClip() )
        {
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            {
                // The refusal is put in front of the person who pressed the button. This call used to
                // discard its result entirely, so a save that did not happen looked exactly like one
                // that did — which is the Д31-D shape this row was the worst instance of.
                const auto saved = SaveClipToDisk( *animator->GetCurrentClip() );
                if ( saved )
                    ToastManager::Push( "Saved clip to " + saved.GetValue(), ToastLevel::Success );
                else
                    ToastManager::Push( "Clip NOT saved: " + saved.GetError(), ToastLevel::Error, 8.0f );
            }
            Utils::ImGuiUtilities::Tooltip(
                 "Write this clip to Assets/Animations/<name>.anim so it survives a restart\n"
                 "(rediscovered by the asset preloader next session)." );
        }

        const float duration = animator ? animator->GetDuration() : 0.0f;

        const float playTime = animator ? animator->GetCurrentTime() : 0.0f;

        // ---- Transport toolbar (icon buttons with tooltips) ----
        ImGui::SameLine( 0.0f, 20.0f );
        if ( ImGui::Button( ICON_MDI_SKIP_PREVIOUS ) && animator )
        {
            anim.Playing = false;
            animator->SetTime( 0.0f );
        }
        Utils::ImGuiUtilities::Tooltip( "Go to start" );
        ImGui::SameLine();
        if ( ImGui::Button( anim.Playing ? ICON_MDI_PAUSE : ICON_MDI_PLAY ) )
            anim.Playing = !anim.Playing;
        Utils::ImGuiUtilities::Tooltip( anim.Playing ? "Pause" : "Play (resumes the AnimGraph, if any)" );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_STOP ) )
        {
            anim.Playing = false;
            if ( animator )
                animator->SetTime( 0.0f );
        }
        Utils::ImGuiUtilities::Tooltip( "Stop + rewind" );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_SKIP_NEXT ) && animator )
        {
            anim.Playing = false;
            animator->SetTime( duration );
        }
        Utils::ImGuiUtilities::Tooltip( "Go to end" );

        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::Checkbox( "Loop", &anim.Loop );
        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::SetNextItemWidth( 110.0f );
        ImGui::SliderFloat( "Speed", &anim.PlaybackSpeed, 0.0f, 3.0f, "%.2fx" );
        ImGui::SameLine( 0.0f, 16.0f );
        // THE "Zoom" SLIDER THAT USED TO BE HERE MOVED NOTHING, and it is gone rather than kept for later.
        // `m_PxPerSec` was written by it and read by no one (report 07 §3.1 counts it among three such
        // knobs): both lane areas map the whole clip onto their width, so there is no pixels-per-second
        // anywhere to scale. A real zoom is a scroll model — a visible time WINDOW, horizontal panning, and
        // a ruler that follows it — which is a change to the dope sheet, not a line in the curve view. What
        // is here instead is the control that now has a referent.
        if ( ImGui::Button( m_CurveView ? "Dope Sheet" : "Curves" ) )
        {
            m_CurveView       = !m_CurveView;
            m_CurveFitPending = true;
        }
        Utils::ImGuiUtilities::Tooltip( m_CurveView ? "Show the keys as a dope sheet"
                                                    : "Show the selected channel as curves (T4.3)" );
        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::TextColored( ImVec4( 0.80f, 0.86f, 0.98f, 1.0f ), "%.2f / %.2f s", playTime, duration );

        // ---- Keyframe toolbar: author BY MANIPULATION (pose a bone in Skeleton Edit, then key/record) ----
        if ( animator )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — see the note at the other cast below
            auto*      editClip = const_cast<Animation::AnimationClip*>( animator->GetCurrentClip() );
            auto&      authoring = Core::ActiveAuthoringContext();
            const int  selBone   = authoring.SelectedBoneIndex();
            const bool canKey    = editClip != nullptr && authoring.ShowsBones() && selBone >= 0;

            // Author-by-posing: while a clip is open and bones are being authored, the bone gizmo edits the
            // Animator's editable pose buffer (not the rig's bind pose), and keying captures that buffer.
            //
            // ONLY THIS WINDOW, AND ONLY WHILE IT HOLDS THE CONTEXT. The write is refused otherwise, which
            // is the entire point: the unconditional version of this line, multiplied by every open
            // Sequencer, is what made two characters impossible to author side by side. A refusal here is
            // the ORDINARY case (the user is working in another window) and is therefore not logged — it
            // says "not your turn", not "something went wrong".
            if ( authoring.ShowsBones() )
            {
                (void)authoring.SetMode( m_AuthoringOwner, m_Authoring,
                                         editClip != nullptr ? Core::AuthoringMode::Pose
                                                             : Core::AuthoringMode::Skeleton );
            }

            // Record toggle (red when armed). IT IS THE KEYER'S `AutoChangeMode`, READ AND WRITTEN
            // DIRECTLY, not a second bool that has to be kept in step with one: a REC light that can
            // disagree with whether anything is being recorded is the shape this tree keeps paying for.
            const bool armed = m_Keyer.Modes().AutoChange != Animation::AutoChangeMode::None;
            if ( armed )
            {
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.70f, 0.15f, 0.15f, 1.0f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.82f, 0.20f, 0.20f, 1.0f ) );
            }
            if ( ImGui::Button( armed ? ICON_MDI_RECORD_CIRCLE " Auto Key" : ICON_MDI_RECORD " Auto Key" ) )
            {
                SetAutoKey( !armed );
            }
            if ( armed )
                ImGui::PopStyleColor( 2 );
            Utils::ImGuiUtilities::Tooltip(
                 "Auto Key (UE ships this OFF): while ON, posing the selected bone or "
                 "control writes ONE key when you let go of the gizmo — not one per mouse "
                 "move, and not one per tick the playhead crossed." );
            ImGui::SameLine();

            // THE TWO KEYING-MODE ENUMS, ON THE SURFACE THAT USES THEM. A mode nothing can reach is a knob
            // that moves nothing; these are the only two the tree can honour, and `ControlKeyer.hpp`
            // carries the count that refuses UE's third.
            ImGui::SetNextItemWidth( 110.0f );
            {
                Animation::KeyingModes modes   = m_Keyer.Modes();
                int                    change  = static_cast<int>( modes.AutoChange );
                const char*            changes = "Off\0Key existing\0Track only\0Create + key\0";
                if ( ImGui::Combo( "##autochange", &change, changes ) )
                {
                    modes.AutoChange = static_cast<Animation::AutoChangeMode>( change );
                    m_Keyer.SetModes( modes );
                }
                Utils::ImGuiUtilities::Tooltip( "What an automatic change does: nothing / key a bone that is "
                                                "already animated / give it a track but no key / both." );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 110.0f );
                int         group  = static_cast<int>( modes.KeyGroup );
                const char* groups = "Changed only\0Moved bones\0Whole rig\0";
                if ( ImGui::Combo( "##keygroup", &group, groups ) )
                {
                    modes.KeyGroup = static_cast<Animation::KeyGroupMode>( group );
                    m_Keyer.SetModes( modes );
                }
                Utils::ImGuiUtilities::Tooltip( "Which bones an automatic key covers. 'Changed only' skips a "
                                                "bone the curve already agrees with at this tick." );
            }
            ImGui::SameLine();

            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.44f, 0.31f, 0.10f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.58f, 0.41f, 0.14f, 1.0f ) );
            ImGui::BeginDisabled( !canKey );
            // AFTER A KEY, SHOW THE CLIP'S POSE AND NOT JUST THIS BONE'S. `SetTime` alone rebuilds the
            // render from the clip's tracks while the authoring buffer the gizmo writes keeps its old
            // contents, so from the second bone onward the artist posed against a skeleton that no longer
            // showed their earlier work. Reloading the buffer from the clip at the playhead makes the two
            // agree: every bone already keyed is in the clip, so every bone already keyed stays on screen.
            const auto showKeyedPose = [&]( Animation::AnimationClip* clip )
            {
                animator->SetTime( playTime );
                animator->SampleClipIntoLocalPose( *clip, animator->GetCurrentTick() );
                animator->ApplyLocalPose();
            };

            if ( ImGui::Button( ICON_MDI_KEY_PLUS " Key Bone @ Playhead" ) && canKey )
            {
                anim.Playing = false;
                // ONE UNDO STEP FOR THE WHOLE PRESS, and the scope has to cover `showKeyedPose` below as
                // well as the key: that call reloads the WHOLE authoring buffer from the clip, so the pose
                // the animator is left looking at is part of what this press did.
                const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( editClip ), animator );
                const auto keyed =
                     m_Keyer.WriteBone( KeyTargetFor( editClip, *animator ), static_cast<uint32_t>( selBone ) );
                if ( !keyed.IsSuccess() )
                {
                    // A BUTTON THAT REFUSED SILENTLY IS THE DEFECT CLASS §1.4 NAMES. Unlike the authoring
                    // context's "not your turn", every refusal reachable from here is a real answer the
                    // animator has to see: a tick outside the clip, a bone the pose buffer does not hold.
                    LOG_ERROR( "[Sequencer] key refused: {}", keyed.GetError() );
                }
                showKeyedPose( editClip );
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor( 2 );
            ImGui::SameLine();
            HelpMarker( "Keyframe by MANIPULATION:\n"
                        "1) In the viewport toolbar, enable Skeleton Edit.\n"
                        "2) Pick a bone and move/rotate it with the gizmo.\n"
                        "3) Turn on Record (auto-key ON RELEASE) OR click Key Bone at the playhead.\n"
                        "Repeat at different times to build the motion (drag the diamond keys below to retime)." );

            // RECORD MODE IS ONE CALL NOW, and the rule it used to spell here lives in the keyer because
            // this file is compiled by no test suite. What is left is the two facts only this window knows:
            // whether the manipulator is being held, and whether the selected bone's pose moved since the
            // last frame.
            if ( canKey )
            {
                const glm::mat4 cur = animator->GetBoneLocalPose( static_cast<uint32_t>( selBone ) );
                bool            moved = false;
                if ( selBone != m_RecordBone )
                {
                    m_RecordBone = selBone; // switched bone -> seed the baseline, do not call it a move
                    m_RecordLast = cur;
                }
                else
                {
                    moved = ( cur != m_RecordLast );
                }

                const bool held     = Core::GizmoState::PoseInteraction();
                const auto observed = m_Keyer.Observe(
                     KeyTargetFor( editClip, *animator ),
                     Animation::KeySubject{ Animation::KeySubjectKind::Bone, static_cast<uint32_t>( selBone ) },
                     held, moved );
                if ( !observed.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] auto-key refused: {}", observed.GetError() );
                }
                else if ( observed.GetValue() > 0 )
                {
                    // Keys landed, so the clip moved under the buffer the gizmo is posing. Reload it and
                    // re-seed the baseline from the reload rather than from `cur`, or the next frame reads
                    // the reload as a drag and keys a second time.
                    anim.Playing = false;
                    showKeyedPose( editClip );
                    m_RecordLast = animator->GetBoneLocalPose( static_cast<uint32_t>( selBone ) );
                }
                else if ( moved )
                {
                    m_RecordLast = cur;
                }
            }
            else
            {
                m_RecordBone = -1; // drop the baseline when there is nothing to key
            }

            // THE UNDO TRANSACTION CLOSES LAST, and that ordering is the whole of its correctness: the
            // keyer writes its deferred keys on this same falling edge, and `showKeyedPose` above reloads
            // the pose out of the clip afterwards. A transaction closed before either of them would record
            // an "after" that is missing its own keys, and Ctrl+Z would put the pose back while leaving
            // them in the clip -- the two states disagreeing, which is what keying exists to prevent.
            //
            // Called with the same bit `ControlKeyer::Observe` was given, not a second one: two bools for
            // one fact is how a REC light comes to disagree with what is being recorded (see m_Keyer).
            const auto recorded =
                 m_ClipEdit.Observe( OwnerOf( editClip ), animator, Core::GizmoState::PoseInteraction() );
            if ( !recorded.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] pose edit not undoable: {}", recorded.GetError() );
            }
            // AFTER the bone transaction's edge: a control gesture opens its own, and the two must not nest.
            UpdateControlRig( editClip, animator );
        }

        // ---- Timeline (gutter-aligned ruler; the track lanes below share the same time mapping) ----
        if ( animator && duration > 0.0f )
        {
            const float  gutter = 150.0f; // label column, shared by ruler + track lanes
            const float  rulerH = 30.0f;
            const ImVec2 avail  = ImGui::GetContentRegionAvail();
            const float  totalW = std::max( avail.x, gutter + 80.0f );
            const float  laneW  = totalW - gutter;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float  laneX0 = origin.x + gutter;
            ImDrawList*  dl     = ImGui::GetWindowDrawList();

            const Sequencer::CurveViewport axis    = TimeAxis( laneX0, laneW, duration );
            const auto                     timeToX = [&]( float t ) { return axis.TimeToX( t ); };

            dl->AddRectFilled( ImVec2( laneX0, origin.y ), ImVec2( laneX0 + laneW, origin.y + rulerH ),
                               IM_COL32( 24, 24, 28, 255 ) );
            dl->AddText( ImVec2( origin.x + 6.0f, origin.y + 8.0f ), IM_COL32( 170, 170, 180, 255 ), "TIMELINE" );

            // THE RULER IS IN FRAMES NOW, and they are the display rate's frames — the same grid a dragged
            // key snaps to. See DrawFrameGrid for what the seconds it replaces were hiding.
            if ( const Animation::AnimationClip* ruled = animator->GetCurrentClip() )
            {
                DrawFrameGrid( dl, axis, origin.y, origin.y + rulerH, animator->GetDurationTicks(),
                               ruled->Sequence.TickRate, ruled->Sequence.DisplayRate, true,
                               IM_COL32( 255, 255, 255, 25 ) );
            }

            // Notify markers (from the current clip).
            if ( const Animation::AnimationClip* clip = animator->GetCurrentClip() )
            {
                for ( const auto& n : ClipNotifies( *clip ) )
                {
                    const float  x = timeToX( static_cast<float>( Animation::FrameTimeToSeconds(
                         Animation::FrameTime{ n.Tick, 0.0F }, clip->Sequence.TickRate ) ) );
                    const ImVec2 d0( x, origin.y + rulerH - 11.0f );
                    dl->AddTriangleFilled( ImVec2( d0.x - 5.0f, d0.y ), ImVec2( d0.x + 5.0f, d0.y ),
                                           ImVec2( d0.x, d0.y + 9.0f ), IM_COL32( 240, 200, 90, 255 ) );
                }
            }

            // Playhead over the ruler.
            const float px = timeToX( playTime );
            dl->AddLine( ImVec2( px, origin.y ), ImVec2( px, origin.y + rulerH ), IM_COL32( 255, 90, 90, 255 ),
                         2.0f );

            // Scrub over the ruler's lane area only (so it doesn't fight the key hit-boxes below).
            ImGui::SetCursorScreenPos( ImVec2( laneX0, origin.y ) );
            if ( laneW > 0.0f )
            {
                ImGui::InvisibleButton( "##seqScrub", ImVec2( laneW, rulerH ) );
                if ( ImGui::IsItemActive() )
                {
                    const float mx = ImGui::GetMousePos().x - laneX0;
                    anim.Playing   = false;
                    animator->SetTime( std::clamp( mx / laneW, 0.0f, 1.0f ) * duration );
                }
            }
            ImGui::SetCursorScreenPos( ImVec2( origin.x, origin.y + rulerH + 3.0f ) );

            // ---- Keyframe tracks (dope sheet) or the same keys as curves ----
            //
            // ONE CAST, NOT ONE PER BRANCH — and the cast itself is a seam this task did not open and
            // deliberately did not paper over. `Animator` holds the clip as `const AnimationClip*`
            // (`Play` takes a const reference), because PLAYING a clip does not change it; the Sequencer
            // EDITS the clip the library owns. Adding an "editing" accessor to Animator would have been a
            // second lie about ownership — the animator has no mutable clip to hand out. The real fix is
            // for the panel to take its clip from the library asset it opened rather than from the thing
            // that is playing it, which is a change to how the clip picker works and is not T4.3.
            //
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            auto* editable = const_cast<Animation::AnimationClip*>( animator->GetCurrentClip() );

            // THE SECTION LANE SITS BETWEEN THE RULER AND THE KEYS, and in both view modes. A section is
            // a statement about a RANGE of this clip, so it belongs against the same time axis whether
            // the keys below are drawn as diamonds or as curves — putting it inside one of the two
            // branches would make "which section am I in" a question only one of the views could answer.
            DrawControlRigTracks( entity, editable, animator, origin.x, gutter, laneW, duration );
            DrawSectionLane( editable, animator, origin.x, gutter, laneW, duration );

            if ( m_CurveView )
            {
                DrawCurveView( editable, animator, origin.x, gutter, laneW, duration );
            }
            else
            {
                DrawClipTracks( editable, animator, origin.x, gutter, laneW, duration );
            }
        }
        else
        {
            ImGui::TextDisabled( "No clip playing — pick a clip above (or the mesh has no animations)." );
        }

        // ---- Sections (the range/blend/weight authoring surface) ----
        if ( animator != nullptr )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — the cast the dope sheet documents
            DrawSectionInspector( const_cast<Animation::AnimationClip*>( animator->GetCurrentClip() ), animator );
        }
    }

    void SequencerPanel::DrawClipTracks( Animation::AnimationClip* clip, Animation::Animator* animator,
                                         float contentX0, float gutter, float laneW, float duration )
    {
        // THE ROWS ARE THE SEQUENCE'S BONE TRACKS. A bone's keys are the Transform channel of the sections
        // of its track (TrackEditing.hpp), so a lane shows the union of the part's key ticks over every
        // section and every edit is a TrackEditing sequence edit — the lane never writes a key list itself.
        size_t boneTracks = 0;
        if ( clip != nullptr )
        {
            for ( const TL::Track& track : clip->Sequence.Tracks )
            {
                boneTracks += BoneOfTrack( clip->Sequence, track ) != nullptr ? 1U : 0U;
            }
        }
        if ( boneTracks == 0 )
        {
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            ImGui::TextDisabled( "This clip has no bone tracks." );
            return;
        }

        // The grids come from THE CLIP BEING EDITED, which is this function's argument — not from the
        // animator's current clip, which is the one being PLAYED.
        TL::Sequence&                      sequence    = clip->Sequence;
        const Animation::FrameRate         tickRate    = sequence.TickRate;
        const Animation::FrameRate         displayRate = sequence.DisplayRate;
        const Animation::ControlHierarchy* rigControls =
             animator->GetRig() != nullptr ? &animator->GetRig()->GetHierarchy() : nullptr;

        const float laneX0    = contentX0 + gutter;
        const float laneH     = 18.0f;
        const char* chName[3] = { "Pos", "Rot", "Scl" };
        const ImU32 chCol[3]  = { IM_COL32( 120, 205, 120, 255 ), IM_COL32( 120, 165, 240, 255 ),
                                  IM_COL32( 235, 185, 110, 255 ) };
        const Sequencer::CurveViewport axis      = TimeAxis( laneX0, laneW, duration );
        const auto                     timeToX   = [&]( float t ) { return axis.TimeToX( t ); };

        const float childH = std::min( 260.0f, 8.0f + static_cast<float>( boneTracks ) * 3.0f * laneH );
        ImGui::BeginChild( "##seqTracks", ImVec2( gutter + laneW, childH ), false,
                           ImGuiWindowFlags_HorizontalScrollbar );
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float playX       = timeToX( animator->GetCurrentTime() );
        bool        liveRefresh = false; // a key moved this frame -> refresh the pose live

        for ( int ti = 0; ti < static_cast<int>( sequence.Tracks.size() ); ++ti )
        {
            const std::string* bonePtr = BoneOfTrack( sequence, sequence.Tracks[ti] );
            if ( bonePtr == nullptr )
            {
                continue;
            }
            // A copy: an edit below may grow `Tracks`/`Bindings` and move the string it points at.
            const std::string bone = *bonePtr;
            // A CONTROL'S TRACK IS DRAWN IN THE RIG BLOCK above, as one row; three bone lanes of it here
            // would be a second place to edit the same keys.
            if ( rigControls != nullptr && rigControls->Find( bone ) != Animation::ControlHierarchy::INVALID )
            {
                continue;
            }
            for ( int ch = 0; ch < 3; ++ch )
            {
                const Animation::TrackChannel part  = ChannelOfLane( ch );
                const ImVec2                  rp    = ImGui::GetCursorScreenPos();
                const float                   laneY = rp.y;
                const ImU32 strip = ( ti % 2 ) ? IM_COL32( 40, 40, 46, 255 ) : IM_COL32( 33, 33, 39, 255 );
                dl->AddRectFilled( ImVec2( laneX0, laneY ), ImVec2( laneX0 + laneW, laneY + laneH - 2.0f ),
                                   strip );

                if ( ch == 0 )
                    dl->AddText( ImVec2( contentX0 + 6.0f, laneY + 1.0f ), IM_COL32( 205, 205, 215, 255 ),
                                 bone.c_str() );
                dl->AddText( ImVec2( contentX0 + gutter - 58.0f, laneY + 1.0f ), chCol[ch], chName[ch] );

                ImGui::PushID( ti * 3 + ch );

                // Per-lane "+" (add a key at the playhead) so empty parts are keyable from scratch: a
                // populated part records its CURVE, an empty one the POSE on screen (§936) — never the origin.
                ImGui::SetCursorScreenPos( ImVec2( contentX0 + gutter - 22.0f, laneY - 1.0f ) );
                if ( ImGui::SmallButton( "+" ) )
                {
                    const Animation::FrameNumber t =
                         Animation::SnapToDisplayRate( animator->GetCurrentTick(), tickRate, displayRate );
                    {
                        // Opened BEFORE the insert: the insert also refreshes the part's tangents, and the
                        // "before" has to predate both.
                        const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), animator );
                        if ( const auto added = KeyPartAtTick( *clip, *animator, bone, part, t );
                             !added.IsSuccess() )
                        {
                            LOG_WARN( "[Sequencer] add key refused: {}", added.GetError() );
                        }
                    }
                    SelectKey( ti, ch, t, sequence );
                    if ( const auto bi = animator->GetSkeleton().FindBoneIndex( bone ); bi.has_value() )
                    {
                        SelectBoneFromTrack( bi.value() );
                    }
                    animator->SetTime( animator->GetCurrentTime() );
                }

                dl->AddLine( ImVec2( playX, laneY ), ImVec2( playX, laneY + laneH - 2.0f ),
                             IM_COL32( 255, 90, 90, 150 ), 1.0f );

                const std::vector<Animation::FrameNumber> ticks    = PartTicks( sequence.Tracks[ti], part );
                const bool                                thisLane = m_SelTrack == ti && m_SelChannel == ch;

                // ONE BUTTON PER LANE, hit-tested by hand (the control-rig rows do the same): a button per
                // diamond would change its ID with the tick it is dragged to and lose the drag on the first
                // frame the key moved.
                ImGui::SetCursorScreenPos( ImVec2( laneX0, laneY ) );
                ImGui::InvisibleButton( "##keys", ImVec2( std::max( laneW, 1.0f ), laneH - 2.0f ) );
                const float mouseX = ImGui::GetMousePos().x;
                int         hover  = -1;
                for ( int k = 0; k < static_cast<int>( ticks.size() ); ++k )
                {
                    if ( std::abs( timeToX( TickToSeconds( ticks[k], tickRate ) ) - mouseX ) <= 6.0f )
                    {
                        hover = k;
                        break;
                    }
                }
                if ( ImGui::IsItemActivated() )
                {
                    m_SelTrack   = ti;
                    m_SelChannel = ch;
                    m_SelKey     = hover;
                    SelectSection( -1 ); // the section list is the selected track's
                    if ( hover >= 0 )
                    {
                        m_SelKeyTick = ticks[hover];
                        // THE RETIME'S OWN RISING EDGE, which this widget knows and nothing else does.
                        if ( const auto began = m_ClipEdit.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
                        {
                            LOG_ERROR( "[Sequencer] retime not undoable: {}", began.GetError() );
                        }
                    }
                    // Selecting a bone's track also selects that bone on the skeleton (viewport highlight
                    // + gizmo), so the Sequencer and the viewport overlay stay in sync.
                    if ( const auto bi = animator->GetSkeleton().FindBoneIndex( bone ); bi.has_value() )
                    {
                        SelectBoneFromTrack( bi.value() );
                    }
                }
                if ( ImGui::IsItemActive() && thisLane && m_SelKey >= 0 && laneW > 0.0f &&
                     ImGui::IsMouseDragging( ImGuiMouseButton_Left ) )
                {
                    // DRAGGED ONTO THE DISPLAY GRID, not to wherever the pixel landed. The move is the
                    // engine's (MoveBoneKey: keeps the key's shape, refuses a tick that is taken, refreshes
                    // the tangents on both sides) and runs live, so the pose follows the drag.
                    const float draggedSeconds = std::clamp( ( mouseX - laneX0 ) / laneW, 0.0f, 1.0f ) * duration;
                    const Animation::FrameNumber to =
                         SecondsToSnappedTick( draggedSeconds, tickRate, displayRate );
                    if ( !( to == m_SelKeyTick ) &&
                         Animation::MoveBoneKey( sequence, bone, part, m_SelKeyTick, to ).IsSuccess() )
                    {
                        SelectKey( ti, ch, to, sequence );
                        liveRefresh = true;
                    }
                }
                if ( ImGui::IsItemDeactivated() && thisLane && m_ClipEdit.OpenExplicitly() )
                {
                    if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
                    {
                        LOG_ERROR( "[Sequencer] retime not undoable: {}", ended.GetError() );
                    }
                }
                ImGui::PopID();

                for ( int k = 0; k < static_cast<int>( ticks.size() ); ++k )
                {
                    const float  kx  = timeToX( TickToSeconds( ticks[k], tickRate ) );
                    const bool   isS = thisLane && m_SelKey >= 0 && ticks[k] == m_SelKeyTick;
                    const bool   hov = ImGui::IsItemHovered() && k == hover;
                    const ImVec2 c( kx, laneY + ( laneH - 2.0f ) * 0.5f );
                    const float  r    = isS ? 6.0f : ( hov ? 5.5f : 4.0f );
                    ImVec2 diamond[4] = { ImVec2( c.x, c.y - r ), ImVec2( c.x + r, c.y ), ImVec2( c.x, c.y + r ),
                                          ImVec2( c.x - r, c.y ) };
                    dl->AddConvexPolyFilled( diamond, 4, isS ? IM_COL32( 255, 170, 60, 255 ) : chCol[ch] );
                    dl->AddPolyline( diamond, 4, IM_COL32( 18, 18, 22, 220 ), ImDrawFlags_Closed, 1.0f );
                }

                ImGui::SetCursorScreenPos( ImVec2( contentX0, laneY + laneH ) );
            }
        }
        ImGui::EndChild();

        if ( liveRefresh )
            animator->SetTime( animator->GetCurrentTime() ); // show the retimed pose live while dragging

        // ---- Selected-key inspector ----
        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        const bool valid = m_SelTrack >= 0 && m_SelTrack < static_cast<int>( sequence.Tracks.size() ) &&
                           m_SelChannel >= 0 && m_SelChannel < 3;
        const std::string* bonePtr = valid ? BoneOfTrack( sequence, sequence.Tracks[m_SelTrack] ) : nullptr;
        if ( bonePtr == nullptr )
        {
            ImGui::TextDisabled( "Click a keyframe to edit it. Drag keys to retime." );
            return;
        }
        const std::string             bone  = *bonePtr;
        const Animation::TrackChannel part  = ChannelOfLane( m_SelChannel );
        const auto                    ticks = PartTicks( sequence.Tracks[m_SelTrack], part );
        const bool keyOk = m_SelKey >= 0 && std::ranges::find( ticks, m_SelKeyTick ) != ticks.end();

        ImGui::Text( "%s  /  %s", bone.c_str(), chName[m_SelChannel] );
        bool changed = false;

        // A NUMERIC FIELD'S DRAG IS AN INTERACTION LIKE THE GIZMO'S: both of its edges are ImGui's, so a drag
        // across forty frames is ONE undo step. `IsItemDeactivated` rather than `...AfterEdit`: a click that
        // changed nothing must still close the transaction (it pushes nothing: the diff is empty).
        const auto bracketField = [&]()
        {
            if ( ImGui::IsItemActivated() )
            {
                if ( const auto began = m_ClipEdit.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] key edit not undoable: {}", began.GetError() );
                }
            }
            if ( ImGui::IsItemDeactivated() && m_ClipEdit.OpenExplicitly() )
            {
                if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] key edit not undoable: {}", ended.GetError() );
                }
            }
        };

        if ( keyOk )
        {
            Animation::FrameNumber tick = m_SelKeyTick;

            // ---- The key's frame: a retime is the engine's MoveBoneKey, never a write to `Tick` ----
            Animation::FrameNumber edited = tick;
            if ( FrameField( edited, animator->GetDurationTicks(), tickRate, displayRate ) && !( edited == tick ) )
            {
                if ( const auto moved = Animation::MoveBoneKey( sequence, bone, part, tick, edited );
                     moved.IsSuccess() )
                {
                    SelectKey( m_SelTrack, m_SelChannel, edited, sequence );
                    tick    = edited;
                    changed = true;
                }
                else
                {
                    LOG_TRACE( "[Sequencer] retime refused: {}", moved.GetError() );
                }
            }
            bracketField();

            TL::TransformChannel* channel = ChannelHoldingPartKey( sequence.Tracks[m_SelTrack], part, tick );
            if ( channel != nullptr )
            {
                const PartKeys keys = PartKeysAt( *channel, part, tick );
                // THE SHAPE OF THE SEGMENT THIS KEY ENDS, one per part: every component key at this tick
                // takes it. A rotation offers two of the three — a cubic through quaternions is not a
                // rotation (Channel.hpp).
                const auto interpCombo = [&]( bool allowCubic )
                {
                    const char* names[3] = { "Constant", "Linear", "Cubic" };
                    int         current  = static_cast<int>( keys.Shape->Interp );
                    if ( !ImGui::Combo( "Interp", &current, names, allowCubic ? 3 : 2 ) )
                    {
                        return false;
                    }
                    // BETWEEN THE WIDGET AND THE WRITE: the key still holds its old value on this line.
                    const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), animator );
                    for ( Animation::ScalarKey* key : keys.Keys )
                    {
                        if ( key != nullptr )
                            key->Interp = static_cast<Animation::KeyInterp>( current );
                    }
                    FinishChannelEdit( sequence, *channel );
                    return true;
                };
                const auto tangentCombo = [&]()
                {
                    const char* names[3] = { "Auto", "User", "Break" };
                    int         current  = static_cast<int>( keys.Shape->Mode );
                    if ( !ImGui::Combo( "Tangents", &current, names, 3 ) )
                    {
                        return false;
                    }
                    const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), animator );
                    for ( Animation::ScalarKey* key : keys.Keys )
                    {
                        if ( key != nullptr )
                            key->Mode = static_cast<Animation::TangentMode>( current );
                    }
                    FinishChannelEdit( sequence, *channel );
                    return true;
                };
                const auto vectorField = [&]( const char* label, float speed, auto&& get, auto&& set )
                {
                    glm::vec3 value( 0.0f );
                    for ( int i = 0; i < 3; ++i )
                    {
                        value[i] = get( i );
                    }
                    const bool edited3 = ImGui::DragFloat3( label, &value.x, speed );
                    // The bracket comes FIRST: on the frame ImGui reports activation nothing is written yet.
                    bracketField();
                    if ( edited3 )
                    {
                        for ( int i = 0; i < 3; ++i )
                        {
                            set( i, value[i] );
                        }
                        FinishChannelEdit( sequence, *channel );
                        changed = true;
                    }
                };
                const auto valueOf = [&]( int i )
                { return keys.Keys[i] != nullptr ? keys.Keys[i]->Value : keys.Components[i]->Default; };
                const auto writeValue = [&]( int i, float v )
                { SetComponentValue( *keys.Components[i], tick, v, *keys.Shape ); };

                if ( part == Animation::TrackChannel::Rotation )
                {
                    changed |= interpCombo( false );
                    const glm::quat q( valueOf( 3 ), valueOf( 0 ), valueOf( 1 ), valueOf( 2 ) );
                    glm::vec3       euler       = glm::degrees( glm::eulerAngles( glm::normalize( q ) ) );
                    const bool      eulerEdited = ImGui::DragFloat3( "Euler", &euler.x, 0.5f );
                    bracketField();
                    if ( eulerEdited )
                    {
                        const glm::quat r = glm::quat( glm::radians( euler ) );
                        writeValue( 0, r.x );
                        writeValue( 1, r.y );
                        writeValue( 2, r.z );
                        writeValue( 3, r.w );
                        FinishChannelEdit( sequence, *channel );
                        changed = true;
                    }
                }
                else
                {
                    vectorField( part == Animation::TrackChannel::Position ? "Position" : "Scale", 0.01f, valueOf,
                                 writeValue );
                    changed |= interpCombo( true );
                    changed |= tangentCombo();
                    if ( keys.Shape->Mode != Animation::TangentMode::Auto )
                    {
                        vectorField(
                             "Arrive", 0.1f,
                             [&]( int i ) { return keys.Keys[i] != nullptr ? keys.Keys[i]->ArriveTangent : 0.0f; },
                             [&]( int i, float v )
                             {
                                 if ( keys.Keys[i] != nullptr )
                                     keys.Keys[i]->ArriveTangent = v;
                             } );
                        vectorField(
                             "Leave", 0.1f,
                             [&]( int i ) { return keys.Keys[i] != nullptr ? keys.Keys[i]->LeaveTangent : 0.0f; },
                             [&]( int i, float v )
                             {
                                 if ( keys.Keys[i] != nullptr )
                                     keys.Keys[i]->LeaveTangent = v;
                             } );
                    }
                }
            }

            if ( ImGui::Button( ICON_MDI_DELETE "  Delete Key" ) )
            {
                const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), animator );
                if ( const auto removed = Animation::RemoveBoneKey( sequence, bone, part, tick );
                     !removed.IsSuccess() )
                {
                    ToastManager::Push( "delete key: " + removed.GetError(), ToastLevel::Error, 6.0f );
                }
                m_SelKey = -1;
                changed  = true;
            }
            ImGui::SameLine();
        }

        if ( ImGui::Button( ICON_MDI_PLUS "  Add Key @ Playhead" ) )
        {
            // RECORDS WHAT THE CURVE SAYS HERE, seeded with its slope — report 05 §936; an empty part records
            // the pose on screen. What this replaced inserted a key AT THE ORIGIN.
            const Animation::FrameNumber t =
                 Animation::SnapToDisplayRate( animator->GetCurrentTick(), tickRate, displayRate );
            {
                const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), animator );
                if ( const auto added = KeyPartAtTick( *clip, *animator, bone, part, t ); !added.IsSuccess() )
                {
                    ToastManager::Push( "add key: " + added.GetError(), ToastLevel::Error, 6.0f );
                }
            }
            SelectKey( m_SelTrack, m_SelChannel, t, sequence );
            changed = true;
        }

        if ( changed )
            animator->SetTime( animator->GetCurrentTime() );
    }

    void SequencerPanel::SelectKey( int track, int lane, Animation::FrameNumber tick,
                                    const TL::Sequence& sequence )
    {
        m_SelTrack   = track;
        m_SelChannel = lane;
        m_SelKeyTick = tick;
        m_SelKey     = -1;
        if ( track < 0 || track >= static_cast<int>( sequence.Tracks.size() ) || lane < 0 || lane > 2 )
        {
            return;
        }
        // THE INDEX FOLLOWS THE TICK: the curve view addresses the key by its place in the part's sorted
        // ticks, and a retime or an insert changes that place.
        const auto ticks = PartTicks( sequence.Tracks[track], ChannelOfLane( lane ) );
        if ( const auto it = std::ranges::find( ticks, tick ); it != ticks.end() )
        {
            m_SelKey = static_cast<int>( it - ticks.begin() );
        }
    }

    // ---- UI property timeline ---------------------------------------------------------------------
    // The skeletal editor above keys bones; this keys a UI element's Offset / Size / Opacity / Color.
    // It edits UIAnimComponent directly: lanes with draggable key diamonds, a scrubbable ruler, and a
    // transport. The playhead is a runtime-only field, so scrubbing never dirties the scene.
    // ── SECTIONS: THE LANE, THE INSPECTOR AND THE COMMANDS ───────────────────────────────────────────────
    //
    // A section belongs to a TRACK (Timeline/Track.hpp, UE's UMovieSceneTrack::Sections), not to the clip:
    // the lane and the list show the sections of the track selected in the dope sheet, and every edit below
    // is a Timeline/Track.hpp function inside one SequenceEditTransaction — so the lane, the inspector and
    // the palette cannot disagree about what a section edit is.

    void SequencerPanel::SelectSection( int index )
    {
        m_SelSection = index;
        // THE RENAME BUFFER IS INVALIDATED HERE AND NOWHERE ELSE. It is refilled lazily by the inspector,
        // which is the only thing that knows the section's current name.
        m_SectionNameFor = -1;
    }

    std::optional<SequencerPanel::SectionTarget> SequencerPanel::ResolveSectionTarget() const
    {
        if ( m_Timeline != Timeline::Skeletal )
        {
            return std::nullopt;
        }
        const auto resolved = ResolveEntity();
        if ( !resolved )
        {
            return std::nullopt;
        }
        const ECS::Entity& entity = resolved->get();
        if ( !entity.HasComponent<ECS::AnimationComponent>() )
        {
            return std::nullopt;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — the same cast, and the same seam, the
        // dope sheet documents above: `Animator` holds the clip it PLAYS as const, and this window edits
        // the clip the library owns.
        auto& anim = const_cast<ECS::AnimationComponent&>( entity.GetComponent<ECS::AnimationComponent>() );
        Animation::Animator* animator = anim.Animator.get();
        if ( animator == nullptr )
        {
            return std::nullopt;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        auto* clip = const_cast<Animation::AnimationClip*>( animator->GetCurrentClip() );
        if ( clip == nullptr )
        {
            return std::nullopt;
        }
        TL::Track* track = m_SelTrack >= 0 && m_SelTrack < static_cast<int>( clip->Sequence.Tracks.size() )
                                ? &clip->Sequence.Tracks[static_cast<size_t>( m_SelTrack )]
                                : nullptr;
        return SectionTarget{ animator, clip, track };
    }

    void
    SequencerPanel::RunSectionEdit( const char*                                                           what,
                                    const std::function<Common::BoolResultStr( SectionTarget&, size_t )>& edit )
    {
        auto target = ResolveSectionTarget();
        if ( !target )
        {
            ToastManager::Push( std::string( what ) + ": no clip is open on this timeline", ToastLevel::Error,
                                6.0f );
            return;
        }
        if ( target->Track == nullptr )
        {
            ToastManager::Push( std::string( what ) + ": select a track first — sections belong to a track",
                                ToastLevel::Error, 6.0f );
            return;
        }
        if ( m_SelSection < 0 || m_SelSection >= static_cast<int>( target->Track->Sections.size() ) )
        {
            // A COMMAND WITH NO SELECTION IS A REFUSAL IN WORDS, not a no-op. The palette runs without a
            // mouse, so "nothing happened" is the one answer that cannot be told apart from a defect.
            ToastManager::Push( std::format( "{}: select a section first (the track has {})", what,
                                             target->Track->Sections.size() ),
                                ToastLevel::Error, 6.0f );
            return;
        }
        const auto index = static_cast<size_t>( m_SelSection );
        // ONE INTERACTION, ONE UNDO STEP. The guard opens before the edit and closes after it, so a
        // command that changes nothing pushes nothing (SequenceEditTransaction::End compares by value).
        Common::BoolResultStr done = Common::MakeError<bool>( "the edit did not run" );
        {
            const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( target->Clip ), target->Animator );
            done = edit( *target, index );
            if ( done.IsSuccess() )
            {
                ++target->Clip->Sequence.Revision;
            }
        }
        if ( !done.IsSuccess() )
        {
            LOG_ERROR( "[Sequencer] {}: {}", what, done.GetError() );
            ToastManager::Push( std::string( what ) + ": " + done.GetError(), ToastLevel::Error, 8.0f );
        }
    }

    void SequencerPanel::DrawSectionLane( Animation::AnimationClip* clip, Animation::Animator* animator,
                                          float contentX0, float gutter, float laneW, float duration )
    {
        if ( clip == nullptr || animator == nullptr || laneW <= 0.0f )
        {
            return;
        }
        TL::Sequence& sequence = clip->Sequence;
        TL::Track*    track    = m_SelTrack >= 0 && m_SelTrack < static_cast<int>( sequence.Tracks.size() )
                                      ? &sequence.Tracks[static_cast<size_t>( m_SelTrack )]
                                      : nullptr;
        if ( track == nullptr || m_SelSection >= static_cast<int>( track->Sections.size() ) )
        {
            SelectSection( -1 ); // the picker changed the clip, or the track, under the selection
        }

        constexpr float kLaneH = 28.0f;
        const ImVec2    origin = ImGui::GetCursorScreenPos();
        const float     laneX0 = contentX0 + gutter;
        ImDrawList*     dl     = ImGui::GetWindowDrawList();

        const Sequencer::CurveViewport axis = TimeAxis( laneX0, laneW, duration );
        const float                    y0   = origin.y;
        const float                    y1   = origin.y + kLaneH;
        const Animation::FrameRate     rate = sequence.TickRate;

        dl->AddRectFilled( ImVec2( laneX0, y0 ), ImVec2( laneX0 + laneW, y1 ), IM_COL32( 20, 20, 24, 255 ) );
        const std::string title = track != nullptr ? "SECTIONS  " + TrackLabel( sequence, *track ) : "SECTIONS";
        dl->PushClipRect( ImVec2( contentX0, y0 ), ImVec2( laneX0 - 4.0f, y1 ), true );
        dl->AddText( ImVec2( contentX0 + 6.0f, y0 + 7.0f ), IM_COL32( 170, 170, 180, 255 ), title.c_str() );
        dl->PopClipRect();
        if ( track == nullptr )
        {
            dl->AddText( ImVec2( laneX0 + 8.0f, y0 + 7.0f ), IM_COL32( 120, 120, 130, 255 ),
                         "Select a track below: sections belong to a track." );
            ImGui::SetCursorScreenPos( ImVec2( contentX0, y1 + 3.0f ) );
            return;
        }

        // LOWER ROWS FIRST, so the section whose value is SEEN (highest row, then last) is drawn on top.
        std::vector<size_t> order( track->Sections.size() );
        for ( size_t i = 0; i < order.size(); ++i )
            order[i] = i;
        std::ranges::stable_sort( order, [&]( size_t a, size_t b )
                                  { return track->Sections[a].Row < track->Sections[b].Row; } );

        for ( const size_t i : order )
        {
            const TL::Section& section = track->Sections[i];
            const float        xa      = axis.TimeToX( TickToSeconds( section.Start, rate ) );
            // THE END IS INCLUSIVE (Section.hpp), so the bar is drawn to the end of that tick.
            const float xb       = axis.TimeToX( TickToSeconds( section.End, rate ) );
            const bool  additive = section.Blend == Animation::SectionBlendType::Additive;
            const bool  selected = static_cast<int>( i ) == m_SelSection;

            const ImU32 fill = additive
                                    ? ( selected ? IM_COL32( 190, 135, 55, 235 ) : IM_COL32( 130, 92, 38, 200 ) )
                                    : ( selected ? IM_COL32( 78, 124, 180, 235 ) : IM_COL32( 52, 82, 122, 200 ) );
            dl->AddRectFilled( ImVec2( xa, y0 + 3.0f ), ImVec2( std::max( xb, xa + 2.0f ), y1 - 3.0f ), fill,
                               3.0f );
            dl->AddRect( ImVec2( xa, y0 + 3.0f ), ImVec2( std::max( xb, xa + 2.0f ), y1 - 3.0f ),
                         selected ? IM_COL32( 255, 255, 255, 220 ) : IM_COL32( 0, 0, 0, 140 ), 3.0f, 0,
                         selected ? 2.0f : 1.0f );

            // THE FADE, INSIDE THE BAR IT BELONGS TO. Drawn only when the weight HAS keys: an empty weight
            // is full weight (Section.hpp), and a flat line along the top of every section would say nothing.
            if ( !section.Weight.empty() && xb > xa + 2.0f )
            {
                const float top    = y0 + 4.0f;
                const float bottom = y1 - 4.0f;
                const int   steps  = std::min( 160, static_cast<int>( xb - xa ) );
                const float s0     = TickToSeconds( section.Start, rate );
                const float s1     = TickToSeconds( section.End, rate );
                ImVec2      previous( 0.0f, 0.0f );
                for ( int s = 0; s <= steps; ++s )
                {
                    const float t = static_cast<float>( s ) / static_cast<float>( std::max( 1, steps ) );
                    const auto  at =
                         Animation::SecondsToFrameTime( static_cast<double>( s0 + t * ( s1 - s0 ) ), rate );
                    const float  w = TL::WeightAt( section, at, rate );
                    const ImVec2 pt( xa + t * ( xb - xa ),
                                     bottom - std::clamp( w, 0.0f, 1.0f ) * ( bottom - top ) );
                    if ( s > 0 )
                    {
                        dl->AddLine( previous, pt, IM_COL32( 255, 240, 190, 235 ), 1.6f );
                    }
                    previous = pt;
                }
            }

            const std::string label = std::format( "{}  {}  {}  row {}", i, section.Name,
                                                   Animation::SectionBlendName( section.Blend ), section.Row );
            dl->PushClipRect( ImVec2( xa + 2.0f, y0 ), ImVec2( std::max( xb, xa + 2.0f ), y1 ), true );
            dl->AddText( ImVec2( xa + 5.0f, y0 + 7.0f ), IM_COL32( 240, 240, 245, 255 ), label.c_str() );
            dl->PopClipRect();
        }

        // ---- Pick, move and resize ----
        ImGui::SetCursorScreenPos( ImVec2( laneX0, y0 ) );
        ImGui::InvisibleButton( "##sectionLane", ImVec2( laneW, kLaneH ) );
        const auto tickUnderMouse = [&]()
        {
            const float seconds = std::clamp( ( ImGui::GetMousePos().x - laneX0 ) / laneW, 0.0f, 1.0f ) * duration;
            return SecondsToSnappedTick( seconds, rate, sequence.DisplayRate );
        };

        if ( ImGui::IsItemActivated() )
        {
            constexpr float kGrab = 5.0f; // pixels of edge that resize rather than move
            const float     mx    = ImGui::GetMousePos().x;
            m_SectionDrag         = -1;
            // TOPMOST FIRST — the reverse of the draw order — so a click lands on the section the animator
            // is looking at, not one hidden under it.
            for ( auto it = order.rbegin(); it != order.rend(); ++it )
            {
                const TL::Section& section = track->Sections[*it];
                const float        xa      = axis.TimeToX( TickToSeconds( section.Start, rate ) );
                const float        xb      = axis.TimeToX( TickToSeconds( section.End, rate ) );
                if ( mx < xa - kGrab || mx > xb + kGrab )
                {
                    continue;
                }
                m_SectionDrag     = static_cast<int>( *it );
                m_SectionDragEdge = ( mx <= xa + kGrab ) ? 1 : ( mx >= xb - kGrab ? 2 : 0 );
                break;
            }
            SelectSection( m_SectionDrag );
            m_SectionDragTick = tickUnderMouse();
            if ( m_SectionDrag >= 0 )
            {
                if ( const auto began = m_ClipEdit.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] section edit not undoable: {}", began.GetError() );
                }
            }
        }

        if ( ImGui::IsItemActive() && m_SectionDrag >= 0 &&
             m_SectionDrag < static_cast<int>( track->Sections.size() ) )
        {
            const auto now   = tickUnderMouse();
            const auto index = static_cast<size_t>( m_SectionDrag );
            if ( !( now == m_SectionDragTick ) )
            {
                Common::BoolResultStr moved = Common::MakeSuccess( true );
                if ( m_SectionDragEdge == 0 )
                {
                    moved = TL::MoveSection( *track, index, now.Value - m_SectionDragTick.Value );
                }
                else if ( m_SectionDragEdge == 1 )
                {
                    moved = TL::SetSectionRange( *track, index, now, track->Sections[index].End );
                }
                else
                {
                    moved = TL::SetSectionRange( *track, index, track->Sections[index].Start, now );
                }
                // THE ANCHOR ADVANCES ONLY ON AN ACCEPTED MOVE, so a section pushed against a limit starts
                // moving again the instant the mouse comes back past where it stopped.
                if ( moved.IsSuccess() )
                {
                    m_SectionDragTick = now;
                    ++sequence.Revision;
                }
            }
        }

        if ( ImGui::IsItemDeactivated() )
        {
            m_SectionDrag = -1;
            if ( m_ClipEdit.OpenExplicitly() )
            {
                if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] section edit not undoable: {}", ended.GetError() );
                }
            }
        }

        ImGui::SetCursorScreenPos( ImVec2( contentX0, y1 + 3.0f ) );
    }

    void SequencerPanel::DrawSectionInspector( Animation::AnimationClip* clip, Animation::Animator* animator )
    {
        if ( clip == nullptr || animator == nullptr )
        {
            return;
        }
        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        const bool open = Utils::ImGuiUtilities::SectionHeader( ICON_MDI_VIEW_SEQUENTIAL "  Sections", true );
        ImGui::SameLine();
        // Report 05 §972: UE's section weight blends POSES on an Absolute section and VALUES on an Additive
        // one. We blend values in both cases, and this is the place an animator reads before dragging.
        HelpMarker( "A section is a RANGE of one track, how its values reach the pose, and how much of it "
                    "arrives.\n\n"
                    "WEIGHT IS A PROPORTION OF THE VALUE, NOT A BLEND OF TWO POSES.\n"
                    "At 50 % an Absolute section drives the track halfway from the rest pose to the number "
                    "on the curve; an Additive one applies half the authored offset.\n\n"
                    "Weight empty = full weight, which is not the same as a single key of 0.\n"
                    "Two sections of one track at one tick: the one on the HIGHER row wins; on one row, the "
                    "later one." );
        if ( !open )
        {
            return;
        }

        TL::Sequence& sequence = clip->Sequence;
        if ( m_SelTrack < 0 || m_SelTrack >= static_cast<int>( sequence.Tracks.size() ) )
        {
            ImGui::TextDisabled( "Select a track in the dope sheet: sections belong to a track." );
            return;
        }
        TL::Track&                 track = sequence.Tracks[static_cast<size_t>( m_SelTrack )];
        const Animation::FrameRate rate  = sequence.TickRate;
        ImGui::TextDisabled( "Track: %s", TrackLabel( sequence, track ).c_str() );

        // ---- The list, and the operations on it ----
        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.40f, 0.28f, 1.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.50f, 0.36f, 1.0f ) );
        if ( ImGui::Button( ICON_MDI_PLUS " Add Section" ) )
        {
            AddSectionAtPlayhead();
        }
        ImGui::PopStyleColor( 2 );
        Utils::ImGuiUtilities::Tooltip( "A new section of this track from the playhead to the end of the clip, "
                                        "on the first free row: Absolute, full weight, holding the value the "
                                        "track shows at the playhead." );

        const bool hasSelection = m_SelSection >= 0 && m_SelSection < static_cast<int>( track.Sections.size() );

        ImGui::SameLine();
        ImGui::BeginDisabled( !hasSelection );
        if ( ImGui::Button( ICON_MDI_DELETE " Delete" ) )
        {
            RunSectionEdit( "delete section", []( SectionTarget& target, size_t index )
                            { return TL::RemoveSection( *target.Track, index ); } );
            SelectSection( -1 );
        }
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_ARROW_DOWN " Lower row" ) )
        {
            ReorderSelectedSection( -1 );
        }
        Utils::ImGuiUtilities::Tooltip( "One row DOWN: where it overlaps a section on a higher row, it loses." );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_ARROW_UP " Raise row" ) )
        {
            ReorderSelectedSection( 1 );
        }
        Utils::ImGuiUtilities::Tooltip( "One row UP, where it wins an overlap." );
        ImGui::EndDisabled();

        if ( track.Sections.empty() )
        {
            ImGui::TextDisabled( "This track has no section: it has no keys yet." );
            return;
        }

        for ( size_t i = 0; i < track.Sections.size(); ++i )
        {
            const TL::Section& section = track.Sections[i];
            const std::string  row =
                 std::format( "{}   {}   [{}..{}]   {}   row {}##sectionRow{}", i, section.Name,
                              Animation::DisplayFrameIndex( section.Start, rate, sequence.DisplayRate ),
                              Animation::DisplayFrameIndex( section.End, rate, sequence.DisplayRate ),
                              Animation::SectionBlendName( section.Blend ), section.Row, i );
            if ( ImGui::Selectable( row.c_str(), static_cast<int>( i ) == m_SelSection ) )
            {
                SelectSection( static_cast<int>( i ) );
            }
        }

        if ( !hasSelection )
        {
            ImGui::TextDisabled( "Pick a section above to edit it." );
            return;
        }

        const auto   index   = static_cast<size_t>( m_SelSection );
        TL::Section& section = track.Sections[index];

        // A DRAG ACROSS FORTY FRAMES IS ONE UNDO STEP — the same two ImGui edges the key fields use.
        const auto bracketField = [&]()
        {
            if ( ImGui::IsItemActivated() )
            {
                if ( const auto began = m_ClipEdit.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] section edit not undoable: {}", began.GetError() );
                }
            }
            if ( ImGui::IsItemDeactivated() && m_ClipEdit.OpenExplicitly() )
            {
                if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] section edit not undoable: {}", ended.GetError() );
                }
            }
        };
        // Every accepted edit moves the revision, as every TrackEditing edit does.
        const auto accepted = [&]( const Common::BoolResultStr& result, const char* what )
        {
            if ( result.IsSuccess() )
            {
                ++sequence.Revision;
                return;
            }
            // LOGGED, NOT TOASTED: a drag crosses a limit on its way to a legal value and would otherwise
            // raise one toast per frame; the field simply does not move.
            LOG_TRACE( "[Sequencer] {} refused: {}", what, result.GetError() );
        };

        ImGui::Separator();

        // ---- Name ----
        if ( m_SectionNameFor != m_SelSection )
        {
            std::snprintf( m_SectionName, sizeof( m_SectionName ), "%s", section.Name.c_str() );
            m_SectionNameFor = m_SelSection;
        }
        ImGui::SetNextItemWidth( 240.0f );
        if ( ImGui::InputText( "Name", m_SectionName, sizeof( m_SectionName ) ) )
        {
            section.Name = m_SectionName;
        }
        bracketField();

        // ---- Range, in the frames the ruler shows ----
        int          first     = Animation::DisplayFrameIndex( section.Start, rate, sequence.DisplayRate );
        int          last      = Animation::DisplayFrameIndex( section.End, rate, sequence.DisplayRate );
        const int    lastFrame = Animation::DisplayFrameIndex( clip->DurationTicks(), rate, sequence.DisplayRate );
        const double ticksPerFrame = rate.AsDouble() / sequence.DisplayRate.AsDouble();
        const auto   frameToTick   = [&]( int frame )
        {
            return Animation::FrameNumber{
                 static_cast<int32_t>( std::llround( static_cast<double>( frame ) * ticksPerFrame ) ) };
        };

        ImGui::SetNextItemWidth( 120.0f );
        if ( ImGui::DragInt( "Start frame", &first, 1.0f, 0, lastFrame ) )
        {
            accepted( TL::SetSectionRange( track, index, frameToTick( first ), section.End ), "section start" );
        }
        bracketField();
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        if ( ImGui::DragInt( "End frame", &last, 1.0f, 0, lastFrame ) )
        {
            accepted( TL::SetSectionRange( track, index, section.Start, frameToTick( last ) ), "section end" );
        }
        bracketField();
        ImGui::SameLine();
        ImGui::TextDisabled( "inclusive, of %d", lastFrame );

        // ---- Blend type ----
        int         blend      = static_cast<int>( section.Blend );
        const char* blendNames = "Absolute\0Additive\0";
        ImGui::SetNextItemWidth( 160.0f );
        if ( ImGui::Combo( "Blend", &blend, blendNames ) )
        {
            const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), animator );
            section.Blend = static_cast<Animation::SectionBlendType>( blend );
            ++sequence.Revision;
        }
        ImGui::SameLine();
        HelpMarker( "Absolute — the track's value IS the pose; weight walks each number from the rest pose "
                    "towards it.\n"
                    "Additive — the track's value is an OFFSET on top of the rest pose; weight scales the "
                    "offset, so an identity value changes nothing at any weight.\n\n"
                    "Both blend VALUES. Neither interpolates between two evaluated poses." );

        // ---- The weight channel ----
        ImGui::Separator();
        const Animation::FrameTime playhead = animator->GetCurrentTick();
        ImGui::Text( "Weight here: %.3f", TL::WeightAt( section, playhead, rate ) );
        ImGui::SameLine();
        if ( section.Weight.empty() )
        {
            ImGui::TextDisabled( "(no fade authored — full weight, which is not silence)" );
        }
        else
        {
            ImGui::TextDisabled( "(%zu key(s))", section.Weight.size() );
        }

        ImGui::SetNextItemWidth( 160.0f );
        ImGui::SliderFloat( "##sectionWeightValue", &m_SectionWeight, 0.0f, 1.0f, "%.2f" );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_KEY_PLUS " Key weight @ playhead" ) )
        {
            RunSectionEdit( "key weight", [this, tick = playhead.Frame]( SectionTarget& target, size_t i )
                            { return TL::SetSectionWeightKey( *target.Track, i, tick, m_SectionWeight ); } );
        }
        ImGui::SameLine();
        ImGui::BeginDisabled( section.Weight.empty() );
        if ( ImGui::Button( "Clear fade" ) )
        {
            RunSectionEdit( "clear fade", []( SectionTarget& target, size_t i )
                            { return TL::ClearSectionWeight( *target.Track, i ); } );
        }
        ImGui::EndDisabled();
        Utils::ImGuiUtilities::Tooltip( "Back to no fade, which is FULL weight — not silence." );

        for ( size_t k = 0; k < track.Sections[index].Weight.size(); ++k )
        {
            const Animation::ScalarKey key = track.Sections[index].Weight[k];
            ImGui::PushID( static_cast<int>( k ) );
            float value = key.Value;
            ImGui::Text( "f%d", Animation::DisplayFrameIndex( key.Tick, rate, sequence.DisplayRate ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::DragFloat( "##weightKey", &value, 0.005f, 0.0f, 1.0f, "%.3f" ) )
            {
                accepted( TL::SetSectionWeightKey( track, index, key.Tick, value ), "weight key" );
            }
            bracketField();
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
            {
                RunSectionEdit( "remove weight key", [k]( SectionTarget& target, size_t i )
                                { return TL::RemoveSectionWeightKey( *target.Track, i, k ); } );
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    void SequencerPanel::AddSectionAtPlayhead()
    {
        auto target = ResolveSectionTarget();
        if ( !target || target->Track == nullptr )
        {
            ToastManager::Push( "add section: select a track first — sections belong to a track",
                                ToastLevel::Error, 6.0f );
            return;
        }
        Animation::AnimationClip* clip  = target->Clip;
        TL::Track&                track = *target->Track;
        if ( static_cast<uint8_t>( track.Kind ) > static_cast<uint8_t>( TL::TrackKind::Event ) )
        {
            ToastManager::Push( std::format( "add section: a {} track's sections are not authored on a clip",
                                             TL::ToString( track.Kind ) ),
                                ToastLevel::Error, 6.0f );
            return;
        }
        // FROM THE PLAYHEAD TO THE END. A new section that has to be dragged open before it says anything
        // is a button that half-works.
        const Animation::FrameNumber start = target->Animator->GetCurrentTick().Frame;
        const Animation::FrameNumber end   = clip->Sequence.End;
        if ( end < start || start < clip->Sequence.Start )
        {
            ToastManager::Push( "add section: the playhead is outside the clip", ToastLevel::Error, 6.0f );
            return;
        }

        // IT HOLDS WHAT THE TRACK SHOWS AT THE PLAYHEAD (UE seeds a new section's channel defaults from the
        // current value), so adding a section does not move the pose on the frame it is added.
        TL::Channel content = TL::MakeChannel( static_cast<TL::ChannelKind>( track.Kind ) );
        if ( const TL::Section* under = TopmostAt( track, start ) )
        {
            if ( const auto* seen = std::get_if<TL::Channel>( &under->Content ) )
            {
                TL::Channel source = *seen;
                const auto  from   = FloatsOf( source );
                const auto  to     = FloatsOf( content );
                for ( size_t i = 0; i < to.size() && i < from.size(); ++i )
                {
                    to[i]->Default =
                         TL::Evaluate( *from[i], Animation::FrameTime{ start, 0.0F }, clip->Sequence.TickRate );
                }
            }
        }

        {
            const ScopedSequenceEdit undoStep( m_ClipEdit, OwnerOf( clip ), target->Animator );
            TL::Section&             section = TL::AddSection( track, start, end );
            section.Name                     = std::format( "Section {}", track.Sections.size() );
            section.Blend                    = Animation::SectionBlendType::Absolute;
            section.Content                  = std::move( content );
            ++clip->Sequence.Revision;
        }
        // THE NEW SECTION IS SELECTED — selecting it is how the animator finds out where it went.
        SelectSection( static_cast<int>( track.Sections.size() ) - 1 );
    }

    void SequencerPanel::ReorderSelectedSection( int delta )
    {
        auto target = ResolveSectionTarget();
        if ( !target || target->Track == nullptr || m_SelSection < 0 ||
             m_SelSection >= static_cast<int>( target->Track->Sections.size() ) )
        {
            ToastManager::Push( "section row: select a section first", ToastLevel::Error, 6.0f );
            return;
        }
        // PRIORITY IS THE ROW (UE's RowIndex), not the place in the list: the edit moves the section one
        // row and the selection stays on it, because the list order does not change.
        RunSectionEdit( "section row", [delta]( SectionTarget& t, size_t index )
                        { return TL::SetSectionRow( *t.Track, index, t.Track->Sections[index].Row + delta ); } );
    }

    // ── THE UI TIMELINE'S EDITS, REACHABLE WITHOUT A MOUSE ───────────────────────────────────────────
    //
    // The same argument the section actions below carry, applied to the other timeline: every edit in
    // DrawUITracks is a widget, synthetic input is closed on this machine at both doors, and an edit that
    // only a hand can make is an edit no screenshot can show and no unattended run can undo. These call
    // the SAME bodies the widgets call, so a command and a button cannot come to mean different things.
    //
    // WHY NOT EVERY EDIT. A value drag and a retime take a NUMBER, and PaletteCommand::Run takes no
    // arguments -- a tableful of "set the key to 0.1, 0.2, 0.3" entries would be a dictionary nobody
    // could read. What is offered is the two gestures that need no argument: pick a lane, and key it at
    // the playhead. Between them they reach the transaction, which is what had to become observable.
    std::vector<SequencerPanel::DocumentAction> SequencerPanel::UIActions()
    {
        std::vector<DocumentAction> actions;
        actions.push_back( DocumentAction{ "Select the next lane", [this]
                                           {
                                               const auto clip = ResolveUIClip();
                                               if ( clip == nullptr || clip->Sequence.Tracks.empty() )
                                               {
                                                   ToastManager::Push( "this clip has no lane", ToastLevel::Error,
                                                                       6.0f );
                                                   return;
                                               }
                                               const int count =
                                                    static_cast<int>( clip->Sequence.Tracks.size() );
                                               m_UITrack       = ( m_UITrack + 1 ) % count;
                                               m_UIKey         = -1;
                                           } } );
        actions.push_back( DocumentAction{ "Add a key at the playhead on the selected lane", [this]
                                           {
                                               if ( ECS::UIAnimData* clip = ResolveUIClip() )
                                               {
                                                   AddUIKeyAtPlayhead( *clip, m_UITrack );
                                               }
                                           } } );
        return actions;
    }

    ECS::UIAnimData* SequencerPanel::ResolveUIClip()
    {
        // RESOLVED PER CALL AND NEVER STORED. The component lives in an entt pool that relocates when it
        // grows, so an address kept between frames is the hazard UIClipEdit.hpp's register row is about.
        const auto entOpt = ResolveEntity();
        if ( !entOpt )
        {
            return nullptr;
        }
        ECS::Entity entity = entOpt->get();
        if ( !entity.HasComponent<ECS::UIAnimComponent>() )
        {
            return nullptr;
        }
        return &entity.GetComponent<ECS::UIAnimComponent>().Data;
    }

    std::vector<SequencerPanel::DocumentAction> SequencerPanel::Actions()
    {
        if ( m_Timeline == Timeline::UI )
        {
            return UIActions();
        }
        if ( m_Timeline != Timeline::Skeletal )
        {
            return {};
        }
        std::vector<DocumentAction> actions;
        actions.push_back( DocumentAction{ m_CurveView ? "Show the dope sheet" : "Show the curves", [this]
                                           {
                                               m_CurveView       = !m_CurveView;
                                               m_CurveFitPending = true;
                                           } } );

        // ── EVERY SECTION EDIT, REACHABLE WITHOUT A MOUSE ────────────────────────────────────────────
        //
        // Not a convenience. Synthetic input is closed at both doors on this platform, so a control that
        // exists only as a widget is a control no unattended run can exercise and no screenshot can
        // prove — which is the argument a previous attempt at this task used to refuse it, and the
        // argument IPanel::DocumentAction exists to retire. The lane and the inspector call the SAME
        // functions these do (`AddSectionAtPlayhead`, `ReorderSelectedSection`, `RunSectionEdit`), so a
        // command and a button cannot drift into meaning different things.
        //
        // THEY ARE OFFERED EVEN WITH NOTHING SELECTED, and refuse in words when run. A palette that
        // hid them would make "the command is missing" and "the command did nothing" the same
        // observation from outside — the empty successful answer, one layer up.
        // THE CONTROL RIG'S KEYING, reachable without a mouse (ANV2b) — the same functions S, the Auto Key
        // toggle and the ruler call. Set Time is a grid, as the Animation Editor's is.
        actions.push_back( DocumentAction{ "Key selected controls", [this] { KeySelectedControls(); } } );
        actions.push_back( DocumentAction{ "Auto Key On", [this] { SetAutoKey( true ); } } );
        actions.push_back( DocumentAction{ "Auto Key Off", [this] { SetAutoKey( false ); } } );
        static constexpr std::array kTimePercents = { 0, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90, 100 };
        for ( const int percent : kTimePercents )
        {
            actions.push_back( DocumentAction{ std::format( "Set Time {}%", percent ),
                                               [this, percent] { SetTimePercent( percent ); } } );
        }

        actions.push_back( DocumentAction{ "Add a section at the playhead", [this] { AddSectionAtPlayhead(); } } );
        // SAVING IS THE OTHER HALF OF AUTHORING, and it was a button too. Without it a section authored
        // through the palette exists only in memory, so "the editor can author a section" could be shown
        // and "the section it authored survives a save and a load" could not — and the second is the one
        // that matters to the animator. The same call the Save button makes, with the same refusal.
        actions.push_back( DocumentAction{
             "Save this clip to disk", [this]
             {
                 const auto target = ResolveSectionTarget();
                 if ( !target )
                 {
                     ToastManager::Push( "save clip: no clip is open", ToastLevel::Error, 6.0f );
                     return;
                 }
                 const auto saved = SaveClipToDisk( *target->Clip );
                 if ( saved )
                 {
                     ToastManager::Push( "Saved clip to " + saved.GetValue(), ToastLevel::Success );
                 }
                 else
                 {
                     ToastManager::Push( "Clip NOT saved: " + saved.GetError(), ToastLevel::Error, 8.0f );
                 }
             } } );
        // SECTIONS BELONG TO A TRACK (UE: UMovieSceneSection lives in its UMovieSceneTrack), so the palette
        // picks the track first; every section command below refuses in words until one is picked.
        actions.push_back( DocumentAction{ "Select the next track", [this]
                                           {
                                               const auto target = ResolveSectionTarget();
                                               if ( !target || target->Clip->Sequence.Tracks.empty() )
                                               {
                                                   ToastManager::Push( "this clip has no track", ToastLevel::Error,
                                                                       6.0f );
                                                   return;
                                               }
                                               const int count =
                                                    static_cast<int>( target->Clip->Sequence.Tracks.size() );
                                               m_SelTrack = ( m_SelTrack + 1 ) % count;
                                               m_SelKey   = -1;
                                               SelectSection( -1 );
                                           } } );
        actions.push_back( DocumentAction{ "Select the next section", [this]
                                           {
                                               const auto target = ResolveSectionTarget();
                                               if ( !target || target->Track == nullptr ||
                                                    target->Track->Sections.empty() )
                                               {
                                                   ToastManager::Push( "the selected track states no section",
                                                                       ToastLevel::Error, 6.0f );
                                                   return;
                                               }
                                               const int count = static_cast<int>( target->Track->Sections.size() );
                                               SelectSection( ( m_SelSection + 1 ) % count );
                                           } } );
        actions.push_back( DocumentAction{
             "Delete the selected section", [this]
             {
                 RunSectionEdit( "delete section", []( SectionTarget& target, size_t index )
                                 { return TL::RemoveSection( *target.Track, index ); } );
                 SelectSection( -1 );
             } } );
        actions.push_back( DocumentAction{ "Set the selected section's start to the playhead", [this]
                                           {
                                               RunSectionEdit( "section start",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return TL::SetSectionRange(
                                                                        *target.Track, index,
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        target.Track->Sections[index].End );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Set the selected section's end to the playhead", [this]
                                           {
                                               RunSectionEdit( "section end",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return TL::SetSectionRange(
                                                                        *target.Track, index,
                                                                        target.Track->Sections[index].Start,
                                                                        target.Animator->GetCurrentTick().Frame );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Set the selected section to Additive", [this]
                                           {
                                               RunSectionEdit( "section blend",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   target.Track->Sections[index].Blend =
                                                                        Animation::SectionBlendType::Additive;
                                                                   return Common::MakeSuccess( true );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Set the selected section to Absolute", [this]
                                           {
                                               RunSectionEdit( "section blend",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   target.Track->Sections[index].Blend =
                                                                        Animation::SectionBlendType::Absolute;
                                                                   return Common::MakeSuccess( true );
                                                               } );
                                           } } );
        // TWO FIXED VALUES AND NOT ONE PARAMETERISED ENTRY, because a palette entry carries no argument.
        // Nought and one are also the two an animator actually authors — a fade-out and a fade-in — and
        // anything between them is the slider in the inspector.
        actions.push_back( DocumentAction{ "Fade the selected section to 0 at the playhead", [this]
                                           {
                                               RunSectionEdit( "section weight",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return TL::SetSectionWeightKey(
                                                                        *target.Track, index,
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        0.0f );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Fade the selected section to 1 at the playhead", [this]
                                           {
                                               RunSectionEdit( "section weight",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return TL::SetSectionWeightKey(
                                                                        *target.Track, index,
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        1.0f );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Clear the selected section's fade", [this]
                                           {
                                               RunSectionEdit( "clear fade",
                                                               []( SectionTarget& target, size_t index )
                                                               { return TL::ClearSectionWeight( *target.Track, index ); } );
                                           } } );
        actions.push_back(
             DocumentAction{ "Raise the selected section's priority", [this] { ReorderSelectedSection( 1 ); } } );
        actions.push_back(
             DocumentAction{ "Lower the selected section's priority", [this] { ReorderSelectedSection( -1 ); } } );
        return actions;
    }

    void SequencerPanel::DrawCurveView( Animation::AnimationClip* clip, Animation::Animator* animator,
                                        float contentX0, float gutter, float laneW, float duration )
    {
        if ( clip == nullptr || clip->Sequence.Tracks.empty() )
        {
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            ImGui::TextDisabled( "This clip has no bone tracks." );
            return;
        }

        // THE CURVE VIEW'S DRAG IS ITS OWN INTERACTION, and `m_CurveDragKey` already IS its boundary: it
        // goes from -1 to a key when the mouse grabs one and back to -1 when it lets go. Read here,
        // before any of the grab tests below can move it, so the rising edge is visible further down.
        const int curveDragBefore = m_CurveDragKey;

        // WITH NOTHING SELECTED IT SHOWS THE FIRST CHANNEL THAT HAS KEYS, rather than an instruction to go
        // and select something. A curve view whose empty state is "select a key in the other view" makes the
        // animator do the tool's work, and it also makes the panel unreachable from the control channel,
        // which is the only way anything about this window gets verified (`--shot` draws no interface).
        //
        // The fallback is LOCAL and does not write the selection: the dope sheet's own highlight is the
        // animator's, and a view that silently moved it would change what the next key edit applies to.
        TL::Sequence& sequence    = clip->Sequence;
        const auto    isBoneTrack = [&]( int index )
        {
            return index >= 0 && index < static_cast<int>( sequence.Tracks.size() ) &&
                   BoneOfTrack( sequence, sequence.Tracks[static_cast<size_t>( index )] ) != nullptr;
        };
        int viewTrack   = m_SelTrack;
        int viewChannel = m_SelChannel;
        if ( !isBoneTrack( viewTrack ) || viewChannel < 0 )
        {
            viewTrack   = -1;
            viewChannel = 0;
            for ( int ti = 0; ti < static_cast<int>( sequence.Tracks.size() ) && viewTrack < 0; ++ti )
            {
                if ( !isBoneTrack( ti ) )
                {
                    continue;
                }
                const TL::Track& candidate = sequence.Tracks[static_cast<size_t>( ti )];
                if ( !PartTicks( candidate, Animation::TrackChannel::Position ).empty() )
                {
                    viewTrack   = ti;
                    viewChannel = 0;
                }
                else if ( !PartTicks( candidate, Animation::TrackChannel::Scale ).empty() )
                {
                    viewTrack   = ti;
                    viewChannel = 2;
                }
            }
            if ( viewTrack < 0 )
            {
                ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
                ImGui::TextDisabled( "No position or scale keys in this clip yet." );
                return;
            }
        }

        TL::Track&         track = sequence.Tracks[static_cast<size_t>( viewTrack )];
        const std::string& bone  = *BoneOfTrack( sequence, track );

        // ROTATION IS REFUSED BY NAME rather than drawn as four meaningless lines. The same argument A6 used
        // to refuse a cubic rotation key: the four components of a quaternion are not four curves, and the
        // channels that would be readable (Euler) do not exist in the format. Inventing them inside a view
        // would be a format decision taken where nobody would look for it.
        if ( viewChannel == 1 )
        {
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            ImGui::TextColored( ImVec4( 0.95f, 0.75f, 0.40f, 1.0f ),
                                "Rotation has no curve view: a rotation key is a quaternion, and its four "
                                "components are not four curves an animator can read." );
            ImGui::TextDisabled( "Euler channels would be — and they would be a change to the .anim format, "
                                 "not a change to this panel." );
            return;
        }

        const Animation::TrackChannel channel =
             ( viewChannel == 0 ) ? Animation::TrackChannel::Position : Animation::TrackChannel::Scale;
        const Animation::FrameRate tickRate    = sequence.TickRate;
        const Animation::FrameRate displayRate = sequence.DisplayRate;

        // ONE SECTION'S CHANNEL IS ONE SET OF CURVES (UE's curve editor shows a section's channels): the
        // selected section when it is on this track, else the section holding the selected key, else the
        // first section with keys on this part. A view that merged sections would draw keys no edit can reach.
        const bool            keyHere = viewTrack == m_SelTrack && m_SelKey >= 0;
        TL::TransformChannel* shown   = nullptr;
        if ( viewTrack == m_SelTrack && m_SelSection >= 0 &&
             m_SelSection < static_cast<int>( track.Sections.size() ) )
        {
            shown = TransformOf( track.Sections[static_cast<size_t>( m_SelSection )] );
        }
        if ( shown == nullptr && keyHere )
        {
            shown = ChannelHoldingPartKey( track, channel, m_SelKeyTick );
        }
        for ( size_t si = 0; si < track.Sections.size() && shown == nullptr; ++si )
        {
            TL::TransformChannel* candidate = TransformOf( track.Sections[si] );
            if ( candidate != nullptr && !Animation::LiftChannel( *candidate, channel, 0 ).empty() )
            {
                shown = candidate;
            }
        }

        // THE SAME SCALARS THE TANGENT RULES USE — see TrackEditing::LiftChannel. A view that built its own
        // would be a second statement of what a channel is.
        std::vector<Animation::ScalarKey> lifted[3];
        for ( int component = 0; component < 3 && shown != nullptr; ++component )
        {
            lifted[component] = Animation::LiftChannel( *shown, channel, component );
        }
        if ( shown == nullptr || lifted[0].empty() )
        {
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            ImGui::TextDisabled( "This channel has no keys yet — add one from the dope sheet's + button." );
            return;
        }

        // The value window is refitted when the SELECTION changes, not every frame: a box that rescales
        // itself while a key is dragged moves the key out from under the mouse.
        if ( m_CurveFitPending || m_CurveFitTrack != viewTrack || m_CurveFitChannel != viewChannel )
        {
            glm::vec2 range( 0.0f, 0.0f );
            bool      first = true;
            for ( const auto& scalars : lifted )
            {
                const glm::vec2 one = Sequencer::FitValueRange( scalars, 0.15f );
                range = first ? one : glm::vec2( std::min( range.x, one.x ), std::max( range.y, one.y ) );
                first = false;
            }
            m_CurveRange      = range;
            m_CurveFitPending = false;
            m_CurveFitTrack   = viewTrack;
            m_CurveFitChannel = viewChannel;
        }

        const float  plotH  = 220.0f;
        const float  laneX0 = contentX0 + gutter;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        Sequencer::CurveViewport vp = TimeAxis( laneX0, laneW, duration );
        vp.Y0                       = origin.y;
        vp.Y1                       = origin.y + plotH;
        vp.ValueMin                 = m_CurveRange.x;
        vp.ValueMax                 = m_CurveRange.y;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( ImVec2( laneX0, vp.Y0 ), ImVec2( vp.X1, vp.Y1 ), IM_COL32( 22, 22, 26, 255 ) );

        // The SAME grid the ruler above draws, from the SAME function.
        DrawFrameGrid( dl, vp, vp.Y0, vp.Y1, animator->GetDurationTicks(), tickRate, displayRate, false,
                       IM_COL32( 255, 255, 255, 18 ) );

        // Zero line and the two range labels, so a curve is readable as numbers and not only as a shape.
        if ( m_CurveRange.x < 0.0f && m_CurveRange.y > 0.0f )
        {
            const float zeroY = vp.ValueToY( 0.0f );
            dl->AddLine( ImVec2( laneX0, zeroY ), ImVec2( vp.X1, zeroY ), IM_COL32( 255, 255, 255, 45 ) );
        }
        char label[32];
        std::snprintf( label, sizeof( label ), "%.2f", m_CurveRange.y );
        dl->AddText( ImVec2( contentX0 + 6.0f, vp.Y0 + 2.0f ), IM_COL32( 170, 170, 180, 200 ), label );
        std::snprintf( label, sizeof( label ), "%.2f", m_CurveRange.x );
        dl->AddText( ImVec2( contentX0 + 6.0f, vp.Y1 - 16.0f ), IM_COL32( 170, 170, 180, 200 ), label );
        dl->AddText( ImVec2( contentX0 + 6.0f, vp.Y0 + plotH * 0.5f - 8.0f ), IM_COL32( 200, 200, 210, 220 ),
                     bone.c_str() );

        // ---- the three component curves, sampled through the evaluator playback calls -------------------
        const ImU32   compCol[3] = { IM_COL32( 235, 110, 110, 255 ), IM_COL32( 130, 225, 130, 255 ),
                                     IM_COL32( 120, 170, 245, 255 ) };
        constexpr int kSamples   = 160;

        // SAMPLED THROUGH THE CLIP'S OWN SAMPLER — the function playback calls, not a second one written
        // beside it. A curve view with its own evaluator draws a picture of what the tool believes instead
        // of a picture of what the animation does, and the two would part company at the first fix to
        // either. It costs a vec3 per sample where a scalar would do, and that is the right trade.
        ImVec2 previous[3] = {};
        for ( int sample = 0; sample <= kSamples; ++sample )
        {
            const double seconds = static_cast<double>( duration ) * static_cast<double>( sample ) / kSamples;
            const Animation::FrameTime at    = Animation::SecondsToFrameTime( seconds, tickRate );
            const Animation::BoneTransform pose  = TL::Evaluate( *shown, at, tickRate );
            const glm::vec3                value = ( channel == Animation::TrackChannel::Position )
                                                        ? pose.Translation
                                                        : pose.Scale;
            const float                x     = vp.TimeToX( seconds );

            for ( int component = 0; component < 3; ++component )
            {
                const ImVec2 point( x, vp.ValueToY( value[component] ) );
                if ( sample > 0 && lifted[component].size() >= 2 )
                {
                    dl->AddLine( previous[component], point, compCol[component], 1.6f );
                }
                previous[component] = point;
            }
        }

        // ---- keys, and the handles of the selected one --------------------------------------------------
        const auto keyAt = [&]( const Animation::ScalarKey& key )
        {
            return ImVec2(
                 vp.TimeToX( Animation::FrameTimeToSeconds( Animation::FrameTime{ key.Tick, 0.0f }, tickRate ) ),
                 vp.ValueToY( key.Value ) );
        };

        constexpr float kHandlePixels = 46.0f;
        constexpr float kGrab         = 6.0f;
        const ImVec2    mouse         = ImGui::GetIO().MousePos;

        ImGui::SetCursorScreenPos( ImVec2( laneX0, vp.Y0 ) );
        ImGui::InvisibleButton( "##curvePlot", ImVec2( std::max( 1.0f, laneW ), plotH ) );
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();

        bool                   edited = false;
        Animation::FrameNumber retimeTo{ 0 };
        int                    retimeKey = -1;

        for ( int component = 0; component < 3; ++component )
        {
            const auto& keys = lifted[component];
            for ( int k = 0; k < static_cast<int>( keys.size() ); ++k )
            {
                const ImVec2 p        = keyAt( keys[k] );
                const bool   selected = keyHere && keys[k].Tick == m_SelKeyTick;
                dl->AddCircleFilled( p, selected ? 4.5f : 3.0f,
                                     selected ? IM_COL32( 255, 235, 160, 255 ) : compCol[component] );

                if ( !selected || keys[k].Mode == Animation::TangentMode::Auto )
                {
                    continue;
                }

                // HANDLES ONLY ON A KEY WHOSE TANGENTS ARE THE ANIMATOR'S. An `Auto` key's handles are
                // recomputed from its neighbours on the next edit, so a handle drawn on one is something
                // that can be grabbed, moved, and then silently overwritten.
                for ( int side = 0; side < 2; ++side )
                {
                    const bool      leaving = ( side == 1 );
                    const float     slope   = leaving ? keys[k].LeaveTangent : keys[k].ArriveTangent;
                    const glm::vec2 off     = vp.HandleOffset( slope, kHandlePixels, leaving );
                    const ImVec2    h( p.x + off.x, p.y + off.y );
                    dl->AddLine( p, h, IM_COL32( 255, 235, 160, 160 ), 1.2f );
                    dl->AddRectFilled( ImVec2( h.x - 3.0f, h.y - 3.0f ), ImVec2( h.x + 3.0f, h.y + 3.0f ),
                                       IM_COL32( 255, 235, 160, 255 ) );

                    if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) &&
                         std::abs( mouse.x - h.x ) < kGrab && std::abs( mouse.y - h.y ) < kGrab )
                    {
                        m_CurveDragKey       = k;
                        m_CurveDragComponent = component;
                        m_CurveDragHandle    = leaving ? 2 : 1;
                    }
                }
            }
        }

        // Grabbing a KEY (after the handles, so a handle sitting over a key still wins its own click).
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && m_CurveDragHandle == 0 &&
             m_CurveDragKey < 0 )
        {
            for ( int component = 0; component < 3; ++component )
            {
                for ( int k = 0; k < static_cast<int>( lifted[component].size() ); ++k )
                {
                    const ImVec2 p = keyAt( lifted[component][k] );
                    if ( std::abs( mouse.x - p.x ) < kGrab && std::abs( mouse.y - p.y ) < kGrab )
                    {
                        SelectKey( viewTrack, viewChannel, lifted[component][k].Tick, sequence );
                        m_CurveDragKey       = k;
                        m_CurveDragComponent = component;
                        m_CurveDragHandle    = 0;
                    }
                }
            }
        }

        // The grab tests above are the only writers of `m_CurveDragKey` on this side of the frame, and the
        // edit below is the first thing that touches the track — so the transaction opens between them.
        if ( curveDragBefore < 0 && m_CurveDragKey >= 0 )
        {
            if ( const auto began = m_ClipEdit.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] curve edit not undoable: {}", began.GetError() );
            }
        }

        if ( m_CurveDragKey >= 0 && active )
        {
            const int component = m_CurveDragComponent;
            auto&     keys      = lifted[component];
            if ( m_CurveDragKey < static_cast<int>( keys.size() ) )
            {
                Animation::ScalarKey& key = keys[m_CurveDragKey];
                if ( m_CurveDragHandle == 0 )
                {
                    // THE VALUE FOLLOWS THE MOUSE; THE TIME LANDS ON THE DISPLAY GRID. Both halves matter:
                    // a value is continuous and a time is not, and a key dropped between two frames is one
                    // the playhead can never stand on again (A5).
                    //
                    // THE TICK IS NOT WRITTEN INTO THE LIFTED COPY, and finding out why is the reason
                    // `ApplyChannel` checks ticks at all: a lift-edit-apply that moved a key in TIME would
                    // be writing values against a different key than the one they came from. Retiming is a
                    // separate operation on the track, applied below — my own first version changed the
                    // tick here and `ApplyChannel` refused the whole write, so a dragged key silently did
                    // not move. The guard caught it; without it the values would have landed on the
                    // neighbours.
                    key.Value = vp.YToValue( mouse.y );
                    retimeTo  = SecondsToSnappedTick( static_cast<float>( vp.XToTime( mouse.x ) ), tickRate,
                                                      displayRate );
                    retimeKey = m_CurveDragKey;
                }
                else
                {
                    const bool      leaving = ( m_CurveDragHandle == 2 );
                    const ImVec2    p       = keyAt( key );
                    const glm::vec2 offset( mouse.x - p.x, mouse.y - p.y );
                    if ( const auto slope = vp.SlopeFromHandle( offset, leaving ); slope.has_value() )
                    {
                        // Dragging a handle makes the tangents the ANIMATOR'S — an auto pass that then
                        // overwrote them would undo the drag on the next unrelated edit anywhere in the
                        // channel. `Break` stays `Break`: it already means "the two sides are independent".
                        if ( key.Mode != Animation::TangentMode::Break )
                        {
                            key.Mode = Animation::TangentMode::User;
                        }
                        if ( leaving || key.Mode != Animation::TangentMode::Break )
                        {
                            key.LeaveTangent = slope.value();
                        }
                        if ( !leaving || key.Mode != Animation::TangentMode::Break )
                        {
                            key.ArriveTangent = slope.value();
                        }
                    }
                }
                edited = true;
            }
        }

        if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
        {
            if ( m_CurveDragKey >= 0 && m_ClipEdit.OpenExplicitly() )
            {
                // The last frame that edited anything was the previous one (`active` is false now), so the
                // track already holds the finished curve and this closes over all of it.
                if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] curve edit not undoable: {}", ended.GetError() );
                }
            }
            m_CurveDragKey       = -1;
            m_CurveDragComponent = -1;
            m_CurveDragHandle    = 0;
        }

        if ( edited )
        {
            const int  component = m_CurveDragComponent;
            const bool written   = Animation::ApplyChannel( *shown, channel, component, lifted[component] );
            if ( written )
            {
                FinishChannelEdit( sequence, *shown );
                // THE RETIME MOVES THE PART'S KEY IN EVERY COMPONENT (a position key is one key of three
                // floats), through the one function the dope sheet's drag calls.
                if ( retimeKey >= 0 && lifted[component][static_cast<size_t>( retimeKey )].Tick != retimeTo )
                {
                    const Animation::FrameNumber from = lifted[component][static_cast<size_t>( retimeKey )].Tick;
                    if ( Animation::MoveBoneKey( sequence, bone, channel, from, retimeTo ).IsSuccess() )
                    {
                        const auto after = Animation::LiftChannel( *shown, channel, component );
                        for ( size_t i = 0; i < after.size(); ++i )
                        {
                            if ( after[i].Tick == retimeTo )
                            {
                                m_CurveDragKey = static_cast<int>( i );
                                break;
                            }
                        }
                        SelectKey( viewTrack, viewChannel, retimeTo, sequence );
                    }
                }
                animator->SetTime( animator->GetCurrentTime() );
            }
        }

        // The playhead, over everything.
        Sequencer::DrawPlayhead( dl, vp, static_cast<double>( animator->GetCurrentTime() ), vp.Y0, vp.Y1, vp.Y0 );

        ImGui::SetCursorScreenPos( ImVec2( contentX0, vp.Y1 + 6.0f ) );
        ImGui::TextDisabled( "Drag a key to retime (snapped to the display grid) and revalue it; drag a "
                             "handle on a User/Break key to set its tangent." );
    }

    namespace
    {
        // ── THE UI HOST'S KEYS, over Timeline (UE: UWidgetAnimation is a UMovieScene) ──────────────────
        // A UI track is Float (Opacity) or Vector (Offset, Size, Color); a key of the track is a tick keyed
        // in its components. These helpers are the only place the lanes and the inspector touch keys.
        constexpr std::array<const char*, 4> kUIProperties = { "Offset", "Size", "Opacity", "Color" };

        std::vector<Animation::FrameNumber> UIKeyTicks( TL::Track& track )
        {
            std::vector<Animation::FrameNumber> ticks;
            for ( TL::Section& section : track.Sections )
            {
                if ( auto* channel = std::get_if<TL::Channel>( &section.Content ) )
                {
                    for ( const TL::FloatChannel* component : FloatsOf( *channel ) )
                        for ( const Animation::ScalarKey& key : component->Keys )
                            ticks.push_back( key.Tick );
                }
            }
            std::ranges::sort( ticks, []( Animation::FrameNumber x, Animation::FrameNumber y ) { return x < y; } );
            ticks.erase( std::unique( ticks.begin(), ticks.end() ), ticks.end() );
            return ticks;
        }

        /// The section a key at @p tick belongs to: the one holding a key there, else the topmost covering it.
        TL::Section* UISectionAt( TL::Track& track, Animation::FrameNumber tick )
        {
            for ( TL::Section& section : track.Sections )
            {
                if ( auto* channel = std::get_if<TL::Channel>( &section.Content ) )
                    for ( const TL::FloatChannel* component : FloatsOf( *channel ) )
                        for ( const Animation::ScalarKey& key : component->Keys )
                            if ( key.Tick == tick )
                                return &section;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — TopmostAt only reads
            return const_cast<TL::Section*>( TopmostAt( track, tick ) );
        }

        /// Moves the key on @p from to @p to in every component; refused when any component already has @p to.
        bool MoveUIKey( TL::Track& track, Animation::FrameNumber from, Animation::FrameNumber to )
        {
            TL::Section* section = UISectionAt( track, from );
            auto* channel = section != nullptr ? std::get_if<TL::Channel>( &section->Content ) : nullptr;
            if ( channel == nullptr || from == to )
                return false;
            const auto components = FloatsOf( *channel );
            for ( const TL::FloatChannel* component : components )
                for ( const Animation::ScalarKey& key : component->Keys )
                    if ( key.Tick == to )
                        return false;
            for ( TL::FloatChannel* component : components )
            {
                for ( Animation::ScalarKey& key : component->Keys )
                    if ( key.Tick == from )
                        key.Tick = to;
                std::ranges::sort( component->Keys, []( const Animation::ScalarKey& x, const Animation::ScalarKey& y )
                                   { return x.Tick < y.Tick; } );
            }
            return true;
        }
    } // namespace

    void SequencerPanel::DrawUITracks( ECS::Entity& entity )
    {
        namespace ImGui = ::ImGui;

        // NO "ADD UI ANIMATION" BUTTON HERE: the clip IS this window's subject (Details ▸ UI Layout ▸
        // "Add UI Animation" adds the component and opens this window on it).
        auto&         clip     = entity.GetComponent<ECS::UIAnimComponent>().Data;
        TL::Sequence& sequence = clip.Sequence;

        // A TRANSACTION MUST NEVER SPAN TWO SUBJECTS: the component's address moves when entt grows the pool.
        if ( m_UIClipEdit.Open() && m_UIClipEdit.Subject() != &clip )
        {
            m_UIClipEdit.Cancel();
        }
        if ( !clip.Playback.has_value() )
        {
            clip.Playback.emplace( sequence.TickRate, sequence.Start, sequence.End );
            clip.Playback->SetLoopMode( clip.Loop );
        }
        TL::Player&                  player   = *clip.Playback;
        const Animation::FrameRate   tickRate = sequence.TickRate;
        const Animation::FrameNumber playTick = player.Current().Frame;

        // --- transport -------------------------------------------------------------------------------
        const bool playing = player.State() == TL::PlayState::Playing;
        if ( ImGui::Button( playing ? ICON_MDI_PAUSE "  Pause" : ICON_MDI_PLAY "  Play" ) )
        {
            playing ? player.Pause() : player.Play();
        }
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_STOP "  Rewind" ) )
        {
            (void)player.JumpTo( Animation::FrameTime{ sequence.Start, 0.0f } );
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 110.0f );
        if ( ImGui::BeginCombo( "Loop", TL::ToString( clip.Loop ) ) )
        {
            for ( const TL::LoopMode mode : TL::kLoopModes )
            {
                if ( ImGui::Selectable( TL::ToString( mode ), mode == clip.Loop ) && mode != clip.Loop )
                {
                    RecordUIClipToggle( clip, mode );
                }
            }
            ImGui::EndCombo();
        }
        // THE PLAYBACK RANGE, in display frames (the sequence's End). Editing it drops the player so the next
        // frame re-creates it on the new range (UIAnimData's contract).
        const auto toFrame = [&]( Animation::FrameNumber tick )
        { return static_cast<int>( std::lround( Animation::FrameTimeToSeconds( Animation::FrameTime{ tick, 0.0f }, tickRate ) *
                                                sequence.DisplayRate.AsDouble() ) ); };
        const auto toTick = [&]( int frame )
        { return SecondsToSnappedTick( static_cast<float>( frame / sequence.DisplayRate.AsDouble() ), tickRate,
                                       sequence.DisplayRate ); };
        int endFrame = toFrame( sequence.End );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 110.0f );
        if ( ImGui::DragInt( "End frame", &endFrame, 1.0f, toFrame( sequence.Start ) + 1, 100000 ) )
        {
            sequence.End = toTick( endFrame );
            clip.Playback.reset();
        }
        BracketUIClipEditFromItem( clip );
        const float duration = std::max(
             0.05f, static_cast<float>( Animation::FrameTimeToSeconds( Animation::FrameTime{ sequence.End, 0.0f }, tickRate ) ) );
        float now = static_cast<float>( Animation::FrameTimeToSeconds( Animation::FrameTime{ playTick, 0.0f }, tickRate ) );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 140.0f );
        if ( ImGui::SliderFloat( "Time", &now, 0.0f, duration, "%.2f s" ) && clip.Playback.has_value() )
        {
            clip.Playback->Pause(); // scrubbing takes over from playback, as a timeline should
            (void)clip.Playback->JumpTo( Animation::SecondsToFrameTime( now, tickRate ) );
        }

        // --- add a track: a Widget binding naming this element + (property, kind) ------------------------
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        static int newProp = 0;
        ImGui::Combo( "##uiprop", &newProp, kUIProperties.data(), static_cast<int>( kUIProperties.size() ) );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_PLUS "  Track" ) )
        {
            const std::string locator  = entity.GetComponent<ECS::UUIDComponent>().UUID.ToString();
            const char*       property = kUIProperties[static_cast<size_t>( newProp )];
            TL::BindingGuid   guid;
            for ( const TL::Binding& binding : sequence.Bindings )
                if ( binding.Kind == TL::BindingKind::Widget && binding.Locator == locator )
                    guid = binding.Guid;
            if ( !guid.IsNull() && TL::FindTrack( sequence, guid, property ) != nullptr )
            {
                ToastManager::Push( std::format( "add track: this element already animates {}", property ),
                                    ToastLevel::Error, 6.0f );
            }
            else
            {
                const ScopedSequenceEdit step( m_UIClipEdit, OwnerOf( &clip ) );
                if ( guid.IsNull() )
                {
                    TL::Binding binding;
                    binding.Guid    = TL::BindingGuid::Generate();
                    binding.Kind    = TL::BindingKind::Widget;
                    binding.Locator = locator;
                    binding.Label   = entity.HasComponent<ECS::TagComponent>() ? entity.GetComponent<ECS::TagComponent>().Tag
                                                                                 : locator;
                    guid            = binding.Guid;
                    sequence.Bindings.push_back( std::move( binding ) );
                }
                TL::Track track;
                track.Binding  = guid;
                track.Property = property;
                track.Kind     = newProp == 2 ? TL::TrackKind::Float : TL::TrackKind::Vector;
                sequence.Tracks.push_back( std::move( track ) );
                (void)TL::AddSection( sequence.Tracks.back(), sequence.Start, sequence.End );
                ++sequence.Revision;
            }
        }

        if ( sequence.Tracks.empty() )
        {
            ImGui::Separator();
            ImGui::TextDisabled( "No tracks. Pick a property above and press + Track." );
            return;
        }

        // --- lanes -----------------------------------------------------------------------------------
        const float gutter = 160.0f;
        const float laneH  = 22.0f;

        ImGui::Separator();
        const float contentX0 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
        const float laneW     = std::max( 120.0f, ImGui::GetContentRegionAvail().x - gutter - 12.0f );
        const float laneX0    = contentX0 + gutter;
        const auto  tickToX   = [&]( Animation::FrameNumber tick )
        {
            const auto seconds = Animation::FrameTimeToSeconds( Animation::FrameTime{ tick, 0.0f }, tickRate );
            return laneX0 + ( static_cast<float>( seconds ) / duration ) * laneW;
        };
        const auto xToTick = [&]( float x )
        {
            const float seconds = std::clamp( ( x - laneX0 ) / laneW * duration, 0.0f, duration );
            return SecondsToSnappedTick( seconds, tickRate, sequence.DisplayRate );
        };

        ImGui::BeginChild( "##uiTracks",
                           ImVec2( gutter + laneW, std::min( 260.0f, 12.0f + sequence.Tracks.size() * laneH ) ),
                           false );
        ImDrawList* dl = ImGui::GetWindowDrawList();

        int deleteTrack = -1;
        for ( int ti = 0; ti < static_cast<int>( sequence.Tracks.size() ); ++ti )
        {
            TL::Track&        track = sequence.Tracks[static_cast<size_t>( ti )];
            const ImVec2      rp    = ImGui::GetCursorScreenPos();
            const float       laneY = rp.y;
            const TL::Binding* binding = TL::FindBinding( sequence, track.Binding );
            const std::string label =
                 std::format( "{} {}", binding != nullptr ? binding->Label : std::string( "?" ), track.Property );
            dl->AddRectFilled( ImVec2( laneX0, laneY ), ImVec2( laneX0 + laneW, laneY + laneH - 3.0f ),
                               ( ti % 2 ) ? IM_COL32( 40, 40, 46, 255 ) : IM_COL32( 33, 33, 39, 255 ) );
            dl->AddText( ImVec2( contentX0 + 6.0f, laneY + 3.0f ), IM_COL32( 205, 205, 215, 255 ), label.c_str() );

            ImGui::SetCursorScreenPos( ImVec2( contentX0 + gutter - 46.0f, laneY ) );
            ImGui::PushID( ti * 8192 + 7 );
            if ( ImGui::SmallButton( "+" ) )
            {
                AddUIKeyAtPlayhead( clip, ti );
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x" ) )
                deleteTrack = ti;
            ImGui::PopID();

            // keys as diamonds; drag horizontally to retime (snapped to the display grid)
            const auto ticks = UIKeyTicks( track );
            for ( int ki = 0; ki < static_cast<int>( ticks.size() ); ++ki )
            {
                const float  kx = tickToX( ticks[static_cast<size_t>( ki )] );
                const ImVec2 c( kx, laneY + ( laneH - 3.0f ) * 0.5f );
                const bool   sel = ( ti == m_UITrack && ki == m_UIKey );
                dl->AddNgonFilled( c, sel ? 7.0f : 5.5f,
                                   sel ? IM_COL32( 255, 205, 90, 255 ) : IM_COL32( 190, 200, 220, 255 ), 4 );

                ImGui::SetCursorScreenPos( ImVec2( kx - 7.0f, laneY ) );
                ImGui::PushID( ti * 8192 + ki );
                ImGui::InvisibleButton( "##k", ImVec2( 14.0f, laneH - 3.0f ) );
                if ( ImGui::IsItemActivated() )
                {
                    m_UITrack = ti;
                    m_UIKey   = ki;
                    if ( const auto began = m_UIClipEdit.Begin( OwnerOf( &clip ) ); !began.IsSuccess() )
                    {
                        LOG_ERROR( "[UIClipUndo] this key drag will not be undoable: {}", began.GetError() );
                    }
                }
                if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left ) )
                {
                    const Animation::FrameNumber to = xToTick( ImGui::GetIO().MousePos.x );
                    if ( MoveUIKey( track, ticks[static_cast<size_t>( ki )], to ) )
                    {
                        ++sequence.Revision;
                        const auto moved = UIKeyTicks( track );
                        m_UIKey = static_cast<int>( std::ranges::find( moved, to ) - moved.begin() );
                        player.Pause();
                        (void)player.JumpTo( Animation::FrameTime{ to, 0.0f } ); // the pose follows the key
                    }
                }
                if ( ImGui::IsItemDeactivated() )
                {
                    EndUIClipEdit();
                }
                ImGui::PopID();
            }

            ImGui::SetCursorScreenPos( ImVec2( rp.x, laneY + laneH ) );
        }

        // playhead over every lane
        const float playX = tickToX( playTick );
        dl->AddLine( ImVec2( playX, ImGui::GetWindowPos().y ),
                     ImVec2( playX, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y ),
                     IM_COL32( 255, 120, 90, 220 ), 2.0f );
        ImGui::EndChild();

        if ( deleteTrack >= 0 )
        {
            const ScopedSequenceEdit step( m_UIClipEdit, OwnerOf( &clip ) );
            sequence.Tracks.erase( sequence.Tracks.begin() + deleteTrack );
            ++sequence.Revision;
            m_UITrack = m_UIKey = -1;
        }

        // --- selected key ----------------------------------------------------------------------------
        if ( m_UITrack >= 0 && m_UITrack < static_cast<int>( sequence.Tracks.size() ) )
        {
            TL::Track& track = sequence.Tracks[static_cast<size_t>( m_UITrack )];
            const auto ticks = UIKeyTicks( track );
            TL::Section* section =
                 m_UIKey >= 0 && m_UIKey < static_cast<int>( ticks.size() )
                      ? UISectionAt( track, ticks[static_cast<size_t>( m_UIKey )] )
                      : nullptr;
            auto* channel = section != nullptr ? std::get_if<TL::Channel>( &section->Content ) : nullptr;
            if ( channel != nullptr )
            {
                const Animation::FrameNumber tick       = ticks[static_cast<size_t>( m_UIKey )];
                const auto                   components = FloatsOf( *channel );
                std::array<float, 4>         value{};
                Animation::ScalarKey         shape;
                for ( size_t i = 0; i < components.size() && i < value.size(); ++i )
                {
                    value[i] = TL::Evaluate( *components[i], Animation::FrameTime{ tick, 0.0f }, tickRate );
                    for ( const Animation::ScalarKey& key : components[i]->Keys )
                        if ( key.Tick == tick )
                            shape = key;
                }
                ImGui::Separator();
                ImGui::Text( "Key %d of %s", m_UIKey, track.Property.c_str() );

                // The value is read per property, exactly as the renderer reads it.
                bool changed = false;
                ImGui::SetNextItemWidth( 200.0f );
                if ( track.Property == "Opacity" )
                    changed = ImGui::SliderFloat( "Opacity", value.data(), 0.0f, 1.0f );
                else if ( track.Property == "Color" )
                    changed = ImGui::ColorEdit3( "Color", value.data() );
                else
                    changed = ImGui::DragFloat2( "Value (px)", value.data(), 1.0f );
                if ( changed )
                {
                    for ( size_t i = 0; i < components.size() && i < value.size(); ++i )
                        SetComponentValue( *components[i], tick, value[i], shape );
                    ++sequence.Revision;
                }
                BracketUIClipEditFromItem( clip );

                // EASING SHAPES THE SEGMENT ENDING AT THIS KEY (Timeline::ApplyEasingPreset, UIEasing's one
                // table via PresetOf) — the same preset the UI lift and UITween use.
                const char* const easeNames[] = { "Linear",   "QuadIn",     "QuadOut", "QuadInOut",  "CubicIn",
                                                  "CubicOut", "CubicInOut", "BackOut", "ElasticOut", "BounceOut" };
                static int        ease        = 5;
                ImGui::SetNextItemWidth( 140.0f );
                ImGui::Combo( "##ease", &ease, easeNames, 10 );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Ease into this key" ) )
                {
                    const ScopedSequenceEdit step( m_UIClipEdit, OwnerOf( &clip ) );
                    for ( TL::FloatChannel* component : components )
                    {
                        const auto end = std::ranges::find_if( component->Keys, [&]( const Animation::ScalarKey& k )
                                                               { return k.Tick == tick; } );
                        const auto index = static_cast<size_t>( end - component->Keys.begin() );
                        if ( end == component->Keys.end() || index == 0 )
                            continue;
                        const auto eased = TL::ApplyEasingPreset( component->Keys, index,
                                                                  TL::PresetOf( static_cast<ECS::UIEasing>( ease ) ),
                                                                  tickRate, sequence.DisplayRate );
                        if ( !eased.IsSuccess() )
                            ToastManager::Push( "ease: " + eased.GetError(), ToastLevel::Error, 6.0f );
                    }
                    ++sequence.Revision;
                }
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Delete key" ) )
                {
                    const ScopedSequenceEdit step( m_UIClipEdit, OwnerOf( &clip ) );
                    for ( TL::FloatChannel* component : components )
                        std::erase_if( component->Keys, [&]( const Animation::ScalarKey& k ) { return k.Tick == tick; } );
                    ++sequence.Revision;
                    m_UIKey = -1;
                }
            }
        }

        // THE SWEEP: every opener above is a held widget, so a transaction open while no item is active has
        // lost its closer; leaving it open would swallow every later edit into one undo step.
        if ( m_UIClipEdit.Open() && !ImGui::IsAnyItemActive() )
        {
            EndUIClipEdit();
        }
    }

    void SequencerPanel::AddUIKeyAtPlayhead( ECS::UIAnimData& clip, int lane )
    {
        // ONE BODY FOR THE BUTTON AND THE COMMAND: the command is the only one an unattended run can reach.
        TL::Sequence& sequence = clip.Sequence;
        if ( lane < 0 || lane >= static_cast<int>( sequence.Tracks.size() ) )
        {
            ToastManager::Push( "add UI key: no lane is selected", ToastLevel::Error, 6.0f );
            return;
        }
        TL::Track&                   track = sequence.Tracks[static_cast<size_t>( lane )];
        const Animation::FrameNumber tick =
             clip.Playback.has_value() ? clip.Playback->Current().Frame : sequence.Start;
        const ScopedSequenceEdit step( m_UIClipEdit, OwnerOf( &clip ) );
        TL::Section* section = UISectionAt( track, tick );
        if ( section == nullptr )
        {
            section = &TL::AddSection( track, sequence.Start, sequence.End ); // the first free row, >= 0
        }
        auto* channel = std::get_if<TL::Channel>( &section->Content );
        if ( channel == nullptr )
        {
            return;
        }
        // THE KEY HOLDS WHAT THE TRACK SHOWS THERE, so keying does not move the element on this frame.
        for ( TL::FloatChannel* component : FloatsOf( *channel ) )
        {
            const float here = TL::Evaluate( *component, Animation::FrameTime{ tick, 0.0f }, sequence.TickRate );
            SetComponentValue( *component, tick, here, Animation::ScalarKey{} );
        }
        ++sequence.Revision;
        m_UITrack       = lane;
        const auto keys = UIKeyTicks( track );
        m_UIKey         = static_cast<int>( std::ranges::find( keys, tick ) - keys.begin() );
    }

    void SequencerPanel::BracketUIClipEditFromItem( ECS::UIAnimData& clip )
    {
        // ONE PLACE FOR THE TWO EDGES OF A HELD WIDGET. IsItemDeactivated, NOT IsItemDeactivatedAfterEdit: a
        // widget released without a change fires only the former; End() then answers 0 and pushes nothing.
        if ( ::ImGui::IsItemActivated() )
        {
            if ( const auto began = m_UIClipEdit.Begin( OwnerOf( &clip ) ); !began.IsSuccess() )
            {
                LOG_ERROR( "[UIClipUndo] this edit will not be undoable: {}", began.GetError() );
            }
        }
        if ( ::ImGui::IsItemDeactivated() )
        {
            EndUIClipEdit();
        }
    }

    void SequencerPanel::RecordUIClipToggle( ECS::UIAnimData& clip, TL::LoopMode loop )
    {
        // The loop mode lives beside the Sequence on UIAnimData; the step brackets the one write.
        const ScopedSequenceEdit step( m_UIClipEdit, OwnerOf( &clip ) );
        clip.Loop = loop;
        if ( clip.Playback.has_value() )
        {
            clip.Playback->SetLoopMode( loop );
        }
    }

    void SequencerPanel::EndUIClipEdit()
    {
        if ( const auto ended = m_UIClipEdit.End(); !ended.IsSuccess() )
        {
            LOG_ERROR( "[UIClipUndo] {}", ended.GetError() );
        }
    }

    // IsContextual/IsRelevant ARE GONE WITH THE PANEL. They answered "should this window appear because
    // of what is selected?", which is a question only a singleton tool can be asked — a document is
    // opened, by subject, and selecting something else is not a request to open or close one.

    // ── THE CONTROL RIG'S TRACKS (ANV2b) ─────────────────────────────────────────────────────────────────
    //
    // Every decision that can be wrong here is made by the functions in PoseEditTransaction.hpp that
    // ClipEditUndo measures (one key per S, one per auto-keyed gesture, a row move/delete across three
    // channels); what is left in this file is widgets and the order they are called in.

    Animation::ControlKeyTarget SequencerPanel::ControlTargetFor( Animation::AnimationClip* clip,
                                                                  Animation::Animator&      animator ) const
    {
        Animation::ControlKeyTarget target = KeyTargetFor( clip, animator );
        Animation::ControlRigStage* rig    = animator.GetRig();
        target.Hierarchy                   = rig != nullptr ? &rig->GetHierarchy() : nullptr;
        return target;
    }

    std::optional<uint32_t> SequencerPanel::SelectedControlHere() const
    {
        const auto& live = Core::ActiveAuthoringContext();
        if ( live.Entity() != Subject().Owner )
        {
            return std::nullopt;
        }
        return live.SelectedControl();
    }

    void SequencerPanel::SelectControlFromTrack( uint32_t control )
    {
        // The click IS the user choosing this window — take the context first, as SelectBoneFromTrack does.
        auto& live = Core::ActiveAuthoringContext();
        live.Focus( m_AuthoringOwner, m_Authoring );
        if ( const auto mode = live.SetMode( m_AuthoringOwner, m_Authoring, Core::AuthoringMode::Control );
             !mode.IsSuccess() )
        {
            LOG_WARN( "[Sequencer] control mode refused: {}", mode.GetError() );
            return;
        }
        if ( const auto picked = live.SetSelectedControl( m_AuthoringOwner, m_Authoring, control );
             !picked.IsSuccess() )
        {
            LOG_WARN( "[Sequencer] control selection refused: {}", picked.GetError() );
        }
    }

    void SequencerPanel::KeySelectedControls()
    {
        const auto target = ResolveSectionTarget();
        if ( !target )
        {
            LOG_WARN( "[Sequencer] Key selected controls: no clip is playing on this window's entity" );
            return;
        }
        const auto control = SelectedControlHere();
        if ( !control )
        {
            LOG_WARN( "[Sequencer] Key selected controls: no control of this entity is selected" );
            return;
        }
        const Animation::ControlKeyTarget keyTarget = ControlTargetFor( target->Clip, *target->Animator );
        const std::array<uint32_t, 1>     controls  = { *control };
        const auto                        keyed =
             KeyControlsRecorded( m_ClipEdit, target->Animator, m_ControlKeyer, keyTarget, controls );
        if ( !keyed.IsSuccess() )
        {
            LOG_WARN( "[Sequencer] Key selected controls refused: {}", keyed.GetError() );
            return;
        }
        LOG_INFO( "[Sequencer] keyed {} control channel set(s) at tick {}", keyed.GetValue(),
                  keyTarget.Tick.Value );
    }

    void SequencerPanel::SetAutoKey( bool on )
    {
        // THE BONE KEYER'S MODES ARE THE ONE SOURCE; the control keyer copies them every frame.
        Animation::KeyingModes modes = m_Keyer.Modes();
        modes.AutoChange             = on ? Animation::AutoChangeMode::All : Animation::AutoChangeMode::None;
        m_Keyer.SetModes( modes );
    }

    void SequencerPanel::SetTimePercent( int percent )
    {
        const auto resolved = ResolveEntity();
        if ( !resolved || !resolved->get().HasComponent<ECS::AnimationComponent>() )
        {
            LOG_WARN( "[Sequencer] Set Time: this window's entity has no animation" );
            return;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — the seam ResolveSectionTarget documents
        auto& anim =
             const_cast<ECS::AnimationComponent&>( resolved->get().GetComponent<ECS::AnimationComponent>() );
        if ( !anim.Animator )
        {
            LOG_WARN( "[Sequencer] Set Time: the animation has no animator" );
            return;
        }
        anim.Playing = false;
        anim.Animator->SetTime( anim.Animator->GetDuration() * static_cast<float>( percent ) / 100.0f );
    }

    void SequencerPanel::UpdateControlRig( Animation::AnimationClip* clip, Animation::Animator* animator )
    {
        if ( clip == nullptr || animator == nullptr )
        {
            return;
        }
        const Animation::ControlKeyTarget target = ControlTargetFor( clip, *animator );
        if ( target.Hierarchy == nullptr )
        {
            return;
        }
        m_ControlKeyer.SetModes( m_Keyer.Modes() );

        const bool held = Core::GizmoState::ControlInteraction();
        if ( const auto selected = SelectedControlHere() )
        {
            const auto stepped =
                 m_ControlAutoKey.Step( m_ClipEdit, animator, m_ControlKeyer, target, *selected, held );
            if ( !stepped.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] control auto-key: {}", stepped.GetError() );
            }
        }

        // THE CLIP ONTO THE CONTROLS, only when the playhead moved and nothing is held: re-applying every
        // frame would pull a control the user is posing back to its keyed value under the mouse.
        if ( !held && target.Tick.Value != m_ControlTickShown )
        {
            const auto applied = Animation::ApplyClipToControls( target, m_ControlKeyer );
            if ( !applied.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] clip -> controls at tick {}: {}", target.Tick.Value, applied.GetError() );
            }
            m_ControlTickShown = target.Tick.Value;
            // The bones follow the controls through the rig on the animator's next evaluation.
            animator->SetTime( animator->GetCurrentTime() );
        }

        const ImGuiIO& io        = ImGui::GetIO();
        const bool     focused   = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
        const bool     rigOnThis = Core::ActiveAuthoringContext().ShowsControls() &&
                               Core::ActiveAuthoringContext().Entity() == Subject().Owner;
        const bool typing = io.WantTextInput || io.KeyCtrl || ImGui::IsMouseDown( ImGuiMouseButton_Right );
        if ( !typing && ( focused || rigOnThis ) && ImGui::IsKeyPressed( ImGuiKey_S, false ) )
        {
            KeySelectedControls();
        }
        if ( !typing && focused && m_ControlKeyRow < target.Hierarchy->Size() &&
             ImGui::IsKeyPressed( ImGuiKey_Delete, false ) )
        {
            const std::string& name = target.Hierarchy->Get( m_ControlKeyRow ).Name;
            // A CONTROL KEY IS A POSE: the key on this tick goes from every part that has one.
            if ( const TL::Track* track = Animation::FindBoneTrack( clip->Sequence, name ) )
            {
                const Animation::FrameNumber tick{ m_ControlKeyTick };
                const ScopedSequenceEdit     edit( m_ClipEdit, OwnerOf( clip ), animator );
                uint32_t                     removed = 0;
                for ( const Animation::TrackChannel part : kParts )
                {
                    const auto ticks = PartTicks( *track, part );
                    if ( std::ranges::find( ticks, tick ) != ticks.end() )
                    {
                        removed +=
                             Animation::RemoveBoneKey( clip->Sequence, name, part, tick ).IsSuccess() ? 1U : 0U;
                    }
                }
                LOG_INFO( "[Sequencer] deleted {} key(s) of '{}' at tick {}", removed, name, m_ControlKeyTick );
            }
            m_ControlKeyRow    = Animation::ControlHierarchy::INVALID;
            m_ControlTickShown = INT32_MIN;
        }
    }

    void SequencerPanel::DrawControlRigTracks( const ECS::Entity& entity, Animation::AnimationClip* clip,
                                               Animation::Animator* animator, float contentX0, float gutter,
                                               float laneW, float duration )
    {
        if ( clip == nullptr || animator == nullptr || animator->GetRig() == nullptr )
        {
            return;
        }
        const Animation::ControlHierarchy& hierarchy = animator->GetRig()->GetHierarchy();
        const auto                         count     = static_cast<uint32_t>( hierarchy.Size() );
        if ( count == 0 )
        {
            return;
        }

        // THE TREE, in the order ControlRigPanel draws it: a control sits under the first CONTROL among its
        // parent spaces. Built per draw from the hierarchy, so it cannot go stale.
        std::vector<std::vector<uint32_t>> children( count );
        std::vector<uint32_t>              roots;
        for ( uint32_t c = 0; c < count; ++c )
        {
            uint32_t parent = Animation::ControlHierarchy::INVALID;
            for ( const Animation::ControlSpace& space : hierarchy.Get( c ).Parents )
            {
                if ( space.Kind == Animation::ControlSpaceKind::Control )
                {
                    parent = space.Index;
                    break;
                }
            }
            ( parent < count && parent != c ? children[parent] : roots ).push_back( c );
        }
        std::vector<std::pair<uint32_t, int>> rows;
        std::vector<std::pair<uint32_t, int>> stack;
        for ( const uint32_t root : std::views::reverse( roots ) )
        {
            stack.emplace_back( root, 0 );
        }
        while ( !stack.empty() && rows.size() < count )
        {
            const auto [c, depth] = stack.back();
            stack.pop_back();
            rows.emplace_back( c, depth );
            for ( const uint32_t child : std::views::reverse( children[c] ) )
            {
                stack.emplace_back( child, depth + 1 );
            }
        }

        const Animation::FrameRate     tickRate    = clip->Sequence.TickRate;
        const Animation::FrameRate     displayRate = clip->Sequence.DisplayRate;
        const float                    rowH        = 18.0f;
        const float                    laneX0      = contentX0 + gutter;
        const Sequencer::CurveViewport axis        = TimeAxis( laneX0, laneW, duration );
        const float childH = std::min( 240.0f, 8.0f + static_cast<float>( rows.size() + 2 ) * rowH );
        ImGui::BeginChild( "##rigTracks", ImVec2( gutter + laneW, childH ), false );
        ImDrawList*  dl       = ImGui::GetWindowDrawList();
        const auto   selected = SelectedControlHere();
        const ImVec2 top      = ImGui::GetCursorScreenPos();
        const auto   header   = [&]( const char* text, float indent, ImU32 color )
        {
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled( rp, ImVec2( rp.x + gutter + laneW, rp.y + rowH - 1.0f ),
                               IM_COL32( 34, 34, 40, 255 ) );
            dl->AddText( ImVec2( rp.x + 4.0f + indent, rp.y + 2.0f ), color, text );
            ImGui::Dummy( ImVec2( gutter + laneW, rowH ) );
        };
        header( entity.HasComponent<ECS::TagComponent>() ? entity.GetComponent<ECS::TagComponent>().Tag.c_str()
                                                         : "(entity)",
                0.0f, IM_COL32( 225, 225, 230, 255 ) );
        header( "Control Rig", 12.0f, IM_COL32( 170, 200, 255, 255 ) );

        for ( const auto& [c, depth] : rows )
        {
            const Animation::ControlElement& element = hierarchy.Get( c );
            const ImU32                      color   = ImGui::ColorConvertFloat4ToU32(
                 ImVec4( element.Color.r, element.Color.g, element.Color.b, 1.0f ) );
            const ImVec2 rp    = ImGui::GetCursorScreenPos();
            const bool   isSel = selected.has_value() && *selected == c;
            dl->AddRectFilled( rp, ImVec2( rp.x + gutter + laneW, rp.y + rowH - 1.0f ),
                               isSel ? IM_COL32( 58, 68, 98, 255 ) : IM_COL32( 24, 24, 29, 255 ) );
            const float textX = rp.x + 34.0f + static_cast<float>( depth ) * 12.0f;
            dl->AddRectFilled( ImVec2( textX - 10.0f, rp.y + 5.0f ), ImVec2( textX - 4.0f, rp.y + 11.0f ), color );
            dl->AddText( ImVec2( textX, rp.y + 2.0f ), color, element.Name.c_str() );

            ImGui::PushID( static_cast<int>( c ) );
            ImGui::SetCursorScreenPos( rp );
            ImGui::InvisibleButton( "##label", ImVec2( gutter, rowH - 1.0f ) );
            if ( ImGui::IsItemClicked() )
            {
                SelectControlFromTrack( c );
            }

            const TL::Track* track = Animation::FindBoneTrack( clip->Sequence, element.Name );
            // ONE DIAMOND PER TICK: the union of the three parts, because a control key is a pose.
            std::vector<int32_t> ticks;
            if ( track != nullptr )
            {
                for ( const Animation::TrackChannel part : kParts )
                {
                    for ( const Animation::FrameNumber tick : PartTicks( *track, part ) )
                        ticks.push_back( tick.Value );
                }
                std::sort( ticks.begin(), ticks.end() );
                ticks.erase( std::unique( ticks.begin(), ticks.end() ), ticks.end() );
            }

            // ONE BUTTON PER ROW, hit-tested by hand: a button per diamond would change its ID with the
            // tick it is dragged to and lose the drag on the first frame the key moved.
            ImGui::SetCursorScreenPos( ImVec2( laneX0, rp.y ) );
            ImGui::InvisibleButton( "##keys", ImVec2( std::max( laneW, 1.0f ), rowH - 1.0f ) );
            if ( ImGui::IsItemActivated() )
            {
                SelectControlFromTrack( c );
                m_ControlKeyRow    = Animation::ControlHierarchy::INVALID;
                const float mouseX = ImGui::GetMousePos().x;
                for ( const int32_t tick : ticks )
                {
                    const float x = axis.TimeToX( TickToSeconds( Animation::FrameNumber{ tick }, tickRate ) );
                    if ( std::abs( x - mouseX ) <= 6.0f )
                    {
                        m_ControlKeyRow  = c;
                        m_ControlKeyTick = tick;
                        if ( const auto began = m_ClipEdit.Begin( OwnerOf( clip ), animator ); !began.IsSuccess() )
                        {
                            LOG_ERROR( "[Sequencer] control key move not undoable: {}", began.GetError() );
                        }
                        break;
                    }
                }
            }
            if ( ImGui::IsItemActive() && track != nullptr && m_ControlKeyRow == c &&
                 ImGui::IsMouseDragging( ImGuiMouseButton_Left ) )
            {
                const Animation::FrameNumber to = SecondsToSnappedTick(
                     static_cast<float>( axis.XToTime( ImGui::GetMousePos().x ) ), tickRate, displayRate );
                if ( to.Value >= 0 && to.Value != m_ControlKeyTick )
                {
                    if ( MoveControlKey( clip->Sequence, element.Name, Animation::FrameNumber{ m_ControlKeyTick },
                                         to ) )
                    {
                        m_ControlKeyTick   = to.Value;
                        m_ControlTickShown = INT32_MIN;
                    }
                }
            }
            if ( ImGui::IsItemDeactivated() && m_ControlKeyRow == c && m_ClipEdit.OpenExplicitly() )
            {
                if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] control key move not recorded: {}", ended.GetError() );
                }
            }

            for ( const int32_t tick : ticks )
            {
                const float  x = axis.TimeToX( TickToSeconds( Animation::FrameNumber{ tick }, tickRate ) );
                const ImVec2 center( x, rp.y + rowH * 0.5f - 0.5f );
                const bool   keySel = m_ControlKeyRow == c && m_ControlKeyTick == tick;
                const float  r      = 5.0f;
                dl->AddQuadFilled( ImVec2( center.x, center.y - r ), ImVec2( center.x + r, center.y ),
                                   ImVec2( center.x, center.y + r ), ImVec2( center.x - r, center.y ),
                                   keySel ? IM_COL32( 255, 255, 255, 255 ) : color );
                dl->AddQuad( ImVec2( center.x, center.y - r ), ImVec2( center.x + r, center.y ),
                             ImVec2( center.x, center.y + r ), ImVec2( center.x - r, center.y ),
                             IM_COL32( 0, 0, 0, 200 ) );
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos( ImVec2( rp.x, rp.y + rowH ) );
        }

        const float playX  = axis.TimeToX( animator->GetCurrentTime() );
        const float bottom = ImGui::GetCursorScreenPos().y;
        dl->AddLine( ImVec2( playX, top.y ), ImVec2( playX, bottom ), IM_COL32( 255, 90, 90, 255 ), 2.0f );
        ImGui::Dummy( ImVec2( 0.0f, 0.0f ) );
        ImGui::EndChild();
    }

} // namespace Desert::Editor
