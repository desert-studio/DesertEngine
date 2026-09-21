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

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/TrackEditing.hpp>
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
#include <cstdio>
#include <filesystem>
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

        /// The time axis of a lane area, as the ONE mapping the ruler, the dope sheet and the curve view all
        /// read. Three copies of `laneX0 + (t/duration)*laneW` is how a playhead and the key under it end up
        /// two pixels apart at some zoom and nobody can say which of the three is lying.
        [[nodiscard]] Sequencer::CurveViewport TimeAxis( float laneX0, float laneW, float durationSeconds )
        {
            Sequencer::CurveViewport axis;
            axis.X0        = laneX0;
            axis.X1        = laneX0 + laneW;
            axis.TimeStart = 0.0;
            axis.TimeEnd   = durationSeconds > 0.0f ? static_cast<double>( durationSeconds ) : 1.0;
            return axis;
        }

        /// The DISPLAY-RATE frame grid. ONE FUNCTION, TWO CONSUMERS — the ruler and the curve view.
        ///
        /// What it replaces in the ruler was `for (int u = 0; u <= duration; ++u)`: one line per whole
        /// SECOND, labelled with an integer second. So the grid an animator SAW and the grid their dragged
        /// key LANDED ON were different grids, and the display rate — a field A5 added and a number every
        /// `.anim` now states — was visible nowhere in the window that owns it. Report 07 §9.5 asks for this
        /// in as many words ("линейка в КАДРАХ, не в секундах").
        void DrawFrameGrid( ImDrawList* dl, const Sequencer::CurveViewport& axis, float yTop, float yBottom,
                            Animation::FrameNumber durationTicks, Animation::FrameRate tickRate,
                            Animation::FrameRate displayRate, bool labels, ImU32 lineColour )
        {
            const double fps = displayRate.AsDouble();
            if ( !( fps > 0.0 ) )
            {
                return;
            }
            const double  secondsPerFrame = 1.0 / fps;
            const int32_t step =
                 Sequencer::ChooseFrameStep( axis.PixelsPerSecond() * secondsPerFrame, labels ? 44.0f : 9.0f );
            const int32_t last = Animation::DisplayFrameIndex( durationTicks, tickRate, displayRate );

            for ( int32_t frame = 0; frame <= last; frame += step )
            {
                const float x = axis.TimeToX( static_cast<double>( frame ) * secondsPerFrame );
                dl->AddLine( ImVec2( x, yTop ), ImVec2( x, yBottom ), lineColour );
                if ( labels )
                {
                    char buf[16];
                    std::snprintf( buf, sizeof( buf ), "%d", frame );
                    dl->AddText( ImVec2( x + 3.0f, yTop + 3.0f ), IM_COL32( 190, 190, 190, 160 ), buf );
                }
            }
        }
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
    } // namespace

    void SequencerPanel::KeyBonePose( Animation::AnimationClip* clip, const Animation::Animator& animator,
                                      int boneIndex, Animation::FrameTime time )
    {
        const auto& skeleton = animator.GetSkeleton();
        if ( !clip || boneIndex < 0 || boneIndex >= static_cast<int>( skeleton.GetBones().size() ) )
            return;
        const auto& bone = skeleton.GetBones()[boneIndex];

        // Key the EDITABLE animated pose (what the gizmo posed) — NOT the shared bind pose. The gizmo in pose
        // mode writes the Animator's local-pose buffer, so this captures the posed skeleton.
        glm::vec3 t, s, skew;
        glm::vec4 persp;
        glm::quat r;
        glm::decompose( animator.GetBoneLocalPose( static_cast<uint32_t>( boneIndex ) ), s, r, t, skew, persp );

        // Find (or create) the track for this bone.
        Animation::BoneTrack* track = nullptr;
        for ( auto& tr : clip->Tracks )
            if ( tr.BoneName == bone.Name )
            {
                track = &tr;
                break;
            }
        if ( !track )
        {
            Animation::BoneTrack nt;
            nt.BoneName = bone.Name;
            clip->Tracks.push_back( std::move( nt ) );
            track = &clip->Tracks.back();
        }

        // THE KEY LANDS ON A TICK OF THE DISPLAY GRID, and "is there already a key here" is then an `==`.
        // What this replaces was `std::abs( k.Time - time ) < 1e-3f` — an epsilon, because two float paths
        // that mean the same instant do not produce the same float. An epsilon has the two failure modes an
        // epsilon always has: too small and the artist gets a second key a thousandth of a second from the
        // first, too large and a deliberate pair of adjacent keys silently becomes one. Neither exists on
        // the grid.
        const Animation::FrameNumber tick =
             Animation::SnapToDisplayRate( time, clip->TickRate, clip->DisplayRate );

        const auto upsertPos = [&]( auto& keys, auto value, auto make )
        {
            for ( auto& k : keys )
            {
                if ( k.Tick == tick )
                {
                    make( k, value );
                    return;
                }
            }
            keys.push_back( {} );
            keys.back().Tick = tick;
            make( keys.back(), value );
            std::sort( keys.begin(), keys.end() );
        };
        upsertPos( track->PositionKeys, t, []( auto& k, const glm::vec3& v ) { k.Position = v; } );
        upsertPos( track->RotationKeys, r, []( auto& k, const glm::quat& v ) { k.Rotation = v; } );
        upsertPos( track->ScaleKeys, s, []( auto& k, const glm::vec3& v ) { k.Scale = v; } );

        // A KEY WAS ADDED OR MOVED, SO THE CURVE CHANGED ON BOTH SIDES OF IT. Keying is an edit like any
        // other, and the one path that did not recompute would be the one that leaves an animator
        // wondering why the shape drifts as they work.
        Animation::RefreshTangents( *track, clip->TickRate );
    }

    // RequestOpen() AND ITS FILE-STATIC INBOX ARE GONE, and the deletion is the change rather than a
    // tidy-up. `static void RequestOpen()` carried no payload, so the Details button that called it could
    // only ever mean "reveal the one Sequencer window"; opening a SECOND rig beside the first was
    // inexpressible, and so was the window knowing which rig it was about. The button sends a SUBJECT now
    // (Core::SubjectOpenRequests), which is the one wire every document open goes through — see
    // Editor/Core/SubjectOpenRequest.hpp for why there are no longer three private ones.

    std::string SequencerPanel::CreateEmptyClip( const Animation::Skeleton& skeleton )
    {
        if ( !m_AssetManager || !m_Library )
            return {};

        // Unique name so repeated "New Clip" presses don't collide (scan this skeleton's registered clips).
        const auto  existing = m_Library->GetForSkeleton( skeleton );
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
        clip.DurationTicks     = Animation::FrameNumber{ Animation::PROJECT_TICK_RATE.Numerator };
        clip.TickRate          = Animation::PROJECT_TICK_RATE;
        clip.DisplayRate       = Animation::DEFAULT_DISPLAY_RATE;
        clip.SkeletonSignature = skeleton.GetSignature();
        clip.Tracks.reserve( skeleton.GetBones().size() );
        for ( const auto& bone : skeleton.GetBones() )
        {
            Animation::BoneTrack track;
            track.BoneName = bone.Name; // empty channels; the user adds keys in the track editor
            clip.Tracks.push_back( std::move( track ) );
        }

        auto asset = m_AssetManager->CreateAsset<Assets::AnimationAsset>(
             Assets::AssetPriority::Medium, Common::Filepath( "memory://clip/" + name ), false );
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
        std::error_code ec;
        std::filesystem::create_directories( Common::Constants::Path::MESH_PATH_COOKED, ec );
        const std::filesystem::path path =
             Common::Constants::Path::MESH_PATH_COOKED / ( "_" + clip.AnimationName + ".anim" );

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
        const auto                 clips    = m_Library ? m_Library->GetForSkeleton( skeleton )
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
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.40f, 0.28f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.50f, 0.36f, 1.0f ) );
            if ( ImGui::Button( ICON_MDI_PLUS " New Clip" ) )
            {
                const std::string created = CreateEmptyClip( animator->GetSkeleton() );
                if ( !created.empty() )
                {
                    anim.CurrentClip = created;
                    anim.Playing     = false;
                    // Rebind the picker's clip list next frame; play the new (empty) clip so its lanes show.
                    for ( const auto& a : m_Library->GetForSkeleton( animator->GetSkeleton() ) )
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
                 "Write this clip to Cooked/Meshes/_<name>.anim so it survives a restart\n"
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
            const bool canKey    = editClip && authoring.ShowsBones() && selBone >= 0;

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

            // Record toggle (red when armed) — auto-keys while the gizmo moves the selected bone.
            if ( m_Record )
            {
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.70f, 0.15f, 0.15f, 1.0f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.82f, 0.20f, 0.20f, 1.0f ) );
            }
            if ( ImGui::Button( m_Record ? ICON_MDI_RECORD_CIRCLE " REC" : ICON_MDI_RECORD " Record" ) )
                m_Record = !m_Record;
            if ( m_Record )
                ImGui::PopStyleColor( 2 );
            Utils::ImGuiUtilities::Tooltip( "Auto-key: while ON (and in Skeleton Edit), moving the selected bone "
                                            "with the gizmo writes a key at the playhead automatically." );
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
                KeyBonePose( editClip, *animator, selBone, animator->GetCurrentTick() );
                showKeyedPose( editClip );
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor( 2 );
            ImGui::SameLine();
            HelpMarker( "Keyframe by MANIPULATION:\n"
                        "1) In the viewport toolbar, enable Skeleton Edit.\n"
                        "2) Pick a bone and move/rotate it with the gizmo.\n"
                        "3) Turn on Record (auto-key as you move) OR click Key Bone at the playhead.\n"
                        "Repeat at different times to build the motion (drag the diamond keys below to retime)." );

            // Record mode: detect the selected bone's POSE changing (a gizmo drag in pose mode) and auto-key it.
            if ( m_Record && canKey )
            {
                const glm::mat4 cur = animator->GetBoneLocalPose( static_cast<uint32_t>( selBone ) );
                if ( selBone != m_RecordBone )
                {
                    m_RecordBone = selBone; // switched bone -> seed the baseline, don't key yet
                    m_RecordLast = cur;
                }
                else if ( cur != m_RecordLast )
                {
                    anim.Playing = false;
                    KeyBonePose( editClip, *animator, selBone, animator->GetCurrentTick() );
                    showKeyedPose( editClip );
                    // The reload above rewrote the buffer, so re-read the baseline from it rather than
                    // keeping `cur`: otherwise the next frame sees a difference that is the reload, not a
                    // drag, and keys a second time.
                    m_RecordLast = animator->GetBoneLocalPose( static_cast<uint32_t>( selBone ) );
                }
            }
            else
            {
                m_RecordBone = -1; // drop the baseline when not recording
            }
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
                               ruled->TickRate, ruled->DisplayRate, true, IM_COL32( 255, 255, 255, 25 ) );
            }

            // Notify markers (from the current clip).
            if ( const Animation::AnimationClip* clip = animator->GetCurrentClip() )
            {
                for ( const auto& n : clip->Notifies )
                {
                    const float  x = timeToX( static_cast<float>(
                         Animation::FrameTimeToSeconds( Animation::FrameTime{ n.Tick, 0.0F }, clip->TickRate ) ) );
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

        // ---- Additive layers (advanced, collapsed by default) ----
        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        const bool layersOpen =
             Utils::ImGuiUtilities::SectionHeader( ICON_MDI_LAYERS "  Additive Layers  (advanced)", false );
        ImGui::SameLine();
        HelpMarker( "Play a SECOND clip ON TOP of the current one — e.g. a wave or aim while walking.\n\n"
                    "- Weight: how strongly it blends in (0..1).\n"
                    "- Additive: adds the layer's motion as an offset (good for lean/aim); off = it overrides.\n"
                    "- Mask from bone: restrict the layer to that bone + its children (empty = whole body, so a "
                    "'wave' would mask e.g. the right shoulder).\n\n"
                    "This is a preview tool; layers are not saved with the clip." );
        if ( layersOpen && animator )
        {
            ImGui::SetNextItemWidth( 200.0f );
            ImGui::Combo( "Layer clip", &m_LayerClip, clipNames.empty() ? nullptr : clipNames.data(),
                          static_cast<int>( clipNames.size() ) );
            ImGui::SetNextItemWidth( 160.0f );
            ImGui::SliderFloat( "Weight", &m_LayerWeight, 0.0f, 1.0f );
            ImGui::SameLine();
            ImGui::Checkbox( "Additive", &m_LayerAdditive );
            ImGui::SetNextItemWidth( 200.0f );
            ImGui::InputText( "Mask from bone", m_LayerMaskBone, sizeof( m_LayerMaskBone ) );

            const bool canAdd = m_LayerClip >= 0 && m_LayerClip < static_cast<int>( clips.size() );
            ImGui::BeginDisabled( !canAdd );
            if ( ImGui::Button( "Add Layer" ) && canAdd )
            {
                const int idx = animator->AddLayer( clips[m_LayerClip]->GetClip(), m_LayerWeight, m_LayerAdditive,
                                                    /*loop=*/true );
                if ( m_LayerMaskBone[0] != '\0' )
                    animator->SetLayerMaskByNames( idx, { std::string( m_LayerMaskBone ) } );
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text( "Active: %zu", animator->GetLayerCount() );
            ImGui::SameLine();
            if ( ImGui::Button( "Clear Layers" ) )
                animator->ClearLayers();
        }
    }

    void SequencerPanel::DrawClipTracks( Animation::AnimationClip* clip, Animation::Animator* animator,
                                         float contentX0, float gutter, float laneW, float duration )
    {
        if ( !clip || clip->Tracks.empty() )
        {
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            ImGui::TextDisabled( "This clip has no bone tracks." );
            return;
        }

        // The grids come from THE CLIP BEING EDITED, which is this function's argument — not from the
        // animator's current clip, which is the one being PLAYED. They are usually the same clip and were
        // always assumed to be; naming the right one costs nothing and removes the assumption.
        const Animation::FrameRate tickRate    = clip->TickRate;
        const Animation::FrameRate displayRate = clip->DisplayRate;

        const float laneX0    = contentX0 + gutter;
        const float laneH     = 18.0f;
        const char* chName[3] = { "Pos", "Rot", "Scl" };
        const ImU32 chCol[3]  = { IM_COL32( 120, 205, 120, 255 ), IM_COL32( 120, 165, 240, 255 ),
                                  IM_COL32( 235, 185, 110, 255 ) };
        const Sequencer::CurveViewport axis      = TimeAxis( laneX0, laneW, duration );
        const auto                     timeToX   = [&]( float t ) { return axis.TimeToX( t ); };

        const float childH = std::min( 260.0f, 8.0f + clip->Tracks.size() * 3.0f * laneH );
        ImGui::BeginChild( "##seqTracks", ImVec2( gutter + laneW, childH ), false,
                           ImGuiWindowFlags_HorizontalScrollbar );
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float playX = timeToX( animator->GetCurrentTime() );

        bool liveRefresh = false; // a key is being dragged this frame -> refresh the pose live
        bool needSort    = false; // a drag just ended -> re-sort that channel + reselect the moved key
        int  sortTrack   = -1;
        int  sortChannel = -1;

        for ( int ti = 0; ti < static_cast<int>( clip->Tracks.size() ); ++ti )
        {
            auto& tr = clip->Tracks[ti];
            for ( int ch = 0; ch < 3; ++ch )
            {
                const ImVec2 rp    = ImGui::GetCursorScreenPos();
                const float  laneY = rp.y;
                const ImU32  strip = ( ti % 2 ) ? IM_COL32( 40, 40, 46, 255 ) : IM_COL32( 33, 33, 39, 255 );
                dl->AddRectFilled( ImVec2( laneX0, laneY ), ImVec2( laneX0 + laneW, laneY + laneH - 2.0f ),
                                   strip );

                if ( ch == 0 )
                    dl->AddText( ImVec2( contentX0 + 6.0f, laneY + 1.0f ), IM_COL32( 205, 205, 215, 255 ),
                                 tr.BoneName.c_str() );
                dl->AddText( ImVec2( contentX0 + gutter - 58.0f, laneY + 1.0f ), chCol[ch], chName[ch] );

                // Per-lane "+" (add a key at the playhead) so empty channels are keyable from scratch.
                // THE SAME ORIGIN DEFECT §936 NAMES LIVED HERE TOO, eleven lines below the "Add Key @
                // Playhead" button that was fixed for it: this one pushed `glm::vec3( 0.0f )` / an identity
                // quaternion / a scale of 1. A populated channel records its CURVE; an empty one has no
                // curve, so it records the POSE on screen. Neither answer is the origin.
                ImGui::SetCursorScreenPos( ImVec2( contentX0 + gutter - 22.0f, laneY - 1.0f ) );
                ImGui::PushID( ( ti * 3 + ch ) * 4096 + 3999 );
                if ( ImGui::SmallButton( "+" ) )
                {
                    const Animation::FrameNumber t =
                         Animation::SnapToDisplayRate( animator->GetCurrentTick(), tickRate, displayRate );
                    const Animation::TrackChannel channel = ChannelOfLane( ch );
                    const auto                    bone    = animator->GetSkeleton().FindBoneIndex( tr.BoneName );

                    bool empty = tr.ScaleKeys.empty();
                    if ( ch == 0 )
                    {
                        empty = tr.PositionKeys.empty();
                    }
                    else if ( ch == 1 )
                    {
                        empty = tr.RotationKeys.empty();
                    }

                    bool added = false;
                    if ( !empty )
                    {
                        added = Animation::InsertKeyFromCurve( tr, channel, t, tickRate );
                    }
                    else if ( bone.has_value() )
                    {
                        added = Animation::InsertFirstKeyFromPose( tr, channel, t,
                                                                   animator->GetBoneLocalPose( bone.value() ) );
                    }
                    // else: the track names a bone this rig does not have, so there is no pose to record
                    // and no curve to read. Adding nothing is the answer; adding the origin was not.

                    if ( added )
                    {
                        Animation::RefreshTangents( tr, tickRate );
                    }
                    m_SelTrack   = ti;
                    m_SelChannel = ch;
                    if ( bone.has_value() )
                    {
                        SelectBoneFromTrack( bone.value() );
                    }
                    animator->SetTime( animator->GetCurrentTime() );
                }
                ImGui::PopID();

                dl->AddLine( ImVec2( playX, laneY ), ImVec2( playX, laneY + laneH - 2.0f ),
                             IM_COL32( 255, 90, 90, 150 ), 1.0f );

                const size_t nKeys = ( ch == 0 )   ? tr.PositionKeys.size()
                                     : ( ch == 1 ) ? tr.RotationKeys.size()
                                                   : tr.ScaleKeys.size();
                for ( int k = 0; k < static_cast<int>( nKeys ); ++k )
                {
                    Animation::FrameNumber* ktp = &tr.ScaleKeys[k].Tick;
                    if ( ch == 0 )
                    {
                        ktp = &tr.PositionKeys[k].Tick;
                    }
                    else if ( ch == 1 )
                    {
                        ktp = &tr.RotationKeys[k].Tick;
                    }
                    Animation::FrameNumber& kt  = *ktp;
                    const float             kx  = timeToX( TickToSeconds( kt, tickRate ) );
                    const bool  isS = ( m_SelTrack == ti && m_SelChannel == ch && m_SelKey == k );

                    ImGui::SetCursorScreenPos( ImVec2( kx - 6.0f, laneY ) );
                    ImGui::PushID( ( ti * 3 + ch ) * 4096 + k );
                    ImGui::InvisibleButton( "##k", ImVec2( 12.0f, laneH - 2.0f ) );
                    const bool hov = ImGui::IsItemHovered();
                    if ( ImGui::IsItemActivated() )
                    {
                        m_SelTrack   = ti;
                        m_SelChannel = ch;
                        m_SelKey     = k;
                        // Selecting a bone's track also selects that bone on the skeleton (viewport highlight
                        // + gizmo), so the Sequencer and the viewport overlay stay in sync.
                        if ( auto bi = animator->GetSkeleton().FindBoneIndex( tr.BoneName ); bi.has_value() )
                            SelectBoneFromTrack( bi.value() );
                    }
                    if ( ImGui::IsItemActive() && laneW > 0.0f )
                    {
                        // Retime WITHOUT sorting mid-drag: sorting would change this key's index (and thus its
                        // ImGui ID), dropping the drag. We sort once on release instead.
                        // DRAGGED ONTO THE DISPLAY GRID, not to wherever the pixel landed. A retime that
                        // follows the mouse exactly puts a key at 0.4173 s, which no other key and no
                        // playhead position will ever equal again.
                        const float draggedSeconds =
                             std::clamp( ( ImGui::GetMousePos().x - laneX0 ) / laneW, 0.0f, 1.0f ) * duration;
                        kt          = SecondsToSnappedTick( draggedSeconds, tickRate, displayRate );
                        m_DragTime  = TickToSeconds( kt, tickRate );
                        liveRefresh = true;
                    }
                    if ( ImGui::IsItemDeactivated() )
                    {
                        needSort    = true;
                        sortTrack   = ti;
                        sortChannel = ch;
                    }
                    ImGui::PopID();

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

        // Drag released: keep the channel sorted (the sampler needs monotonic time) and re-select the key
        // that just moved (its index changed after the sort).
        if ( needSort && sortTrack >= 0 )
        {
            auto& tr = clip->Tracks[sortTrack];
            if ( sortChannel == 0 )
                std::sort( tr.PositionKeys.begin(), tr.PositionKeys.end() );
            else if ( sortChannel == 1 )
                std::sort( tr.RotationKeys.begin(), tr.RotationKeys.end() );
            else
                std::sort( tr.ScaleKeys.begin(), tr.ScaleKeys.end() );

            const auto nearestKey = [&]( const auto& keys )
            {
                int   best = 0;
                float bd   = 1e9f;
                for ( int i = 0; i < static_cast<int>( keys.size() ); ++i )
                {
                    if ( const float d = std::abs( TickToSeconds( keys[i].Tick, tickRate ) - m_DragTime ); d < bd )
                    {
                        bd   = d;
                        best = i;
                    }
                }
                return best;
            };
            m_SelTrack   = sortTrack;
            m_SelChannel = sortChannel;
            m_SelKey     = sortChannel == 0   ? nearestKey( tr.PositionKeys )
                           : sortChannel == 1 ? nearestKey( tr.RotationKeys )
                                              : nearestKey( tr.ScaleKeys );

            // A TANGENT IS NOT A PROPERTY OF ITS KEY: it is computed from the neighbours, so a retime
            // changes the curve on both sides of the key that moved. Recomputing only the edited key is
            // the version that looks right until the second edit (report 05 §969 item 1).
            Animation::RefreshTangents( tr, tickRate );
            animator->SetTime( animator->GetCurrentTime() ); // refresh the pose to the edited key
        }

        // ---- Selected-key inspector ----
        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        const bool valid = m_SelTrack >= 0 && m_SelTrack < static_cast<int>( clip->Tracks.size() ) &&
                           m_SelChannel >= 0 && m_SelChannel < 3;
        if ( valid )
        {
            auto&      tr    = clip->Tracks[m_SelTrack];
            const auto count = ( m_SelChannel == 0 )   ? tr.PositionKeys.size()
                               : ( m_SelChannel == 1 ) ? tr.RotationKeys.size()
                                                       : tr.ScaleKeys.size();
            const bool keyOk = m_SelKey >= 0 && m_SelKey < static_cast<int>( count );

            ImGui::Text( "%s  /  %s", tr.BoneName.c_str(), chName[m_SelChannel] );
            bool changed = false;

            // THE SHAPE OF THE SEGMENT THIS KEY ENDS. Before A6 a clip had exactly one rule — a straight
            // line, unconditionally — so holding a pose and then easing out of it could only be faked with
            // extra keys. A rotation lane offers two of the three: a cubic through quaternions is not a
            // rotation (see RotationKeyFrame), and offering it here would be a control whose value the
            // loader refuses.
            const auto interpCombo = [&]( Animation::KeyInterp& interp, bool allowCubic )
            {
                const char* names[3] = { "Constant", "Linear", "Cubic" };
                int         current  = static_cast<int>( interp );
                if ( !ImGui::Combo( "Interp", &current, names, allowCubic ? 3 : 2 ) )
                {
                    return false;
                }
                interp = static_cast<Animation::KeyInterp>( current );
                return true;
            };
            const auto tangentCombo = [&]( Animation::TangentMode& mode )
            {
                const char* names[3] = { "Auto", "User", "Break" };
                int         current  = static_cast<int>( mode );
                if ( !ImGui::Combo( "Tangents", &current, names, 3 ) )
                {
                    return false;
                }
                mode = static_cast<Animation::TangentMode>( current );
                return true;
            };
            if ( keyOk )
            {
                if ( m_SelChannel == 0 )
                {
                    auto& key = tr.PositionKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    changed |= ImGui::DragFloat3( "Position", &key.Position.x, 0.01f );
                    changed |= interpCombo( key.Interp, true );
                    changed |= tangentCombo( key.Mode );
                    if ( key.Mode != Animation::TangentMode::Auto )
                    {
                        changed |= ImGui::DragFloat3( "Arrive /s", &key.ArriveTangent.x, 0.1f );
                        changed |= ImGui::DragFloat3( "Leave /s", &key.LeaveTangent.x, 0.1f );
                    }
                }
                else if ( m_SelChannel == 1 )
                {
                    auto& key = tr.RotationKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    changed |= interpCombo( key.Interp, false );
                    glm::vec3 euler = glm::degrees( glm::eulerAngles( key.Rotation ) );
                    if ( ImGui::DragFloat3( "Euler", &euler.x, 0.5f ) )
                    {
                        key.Rotation = glm::quat( glm::radians( euler ) );
                        changed      = true;
                    }
                }
                else
                {
                    auto& key = tr.ScaleKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    changed |= ImGui::DragFloat3( "Scale", &key.Scale.x, 0.01f );
                    changed |= interpCombo( key.Interp, true );
                    changed |= tangentCombo( key.Mode );
                    if ( key.Mode != Animation::TangentMode::Auto )
                    {
                        changed |= ImGui::DragFloat3( "Arrive /s", &key.ArriveTangent.x, 0.1f );
                        changed |= ImGui::DragFloat3( "Leave /s", &key.LeaveTangent.x, 0.1f );
                    }
                }

                if ( ImGui::Button( ICON_MDI_DELETE "  Delete Key" ) )
                {
                    if ( m_SelChannel == 0 )
                        tr.PositionKeys.erase( tr.PositionKeys.begin() + m_SelKey );
                    else if ( m_SelChannel == 1 )
                        tr.RotationKeys.erase( tr.RotationKeys.begin() + m_SelKey );
                    else
                        tr.ScaleKeys.erase( tr.ScaleKeys.begin() + m_SelKey );
                    m_SelKey = -1;
                    changed  = true;
                }
                ImGui::SameLine();
            }

            if ( ImGui::Button( ICON_MDI_PLUS "  Add Key @ Playhead" ) )
            {
                // RECORDS WHAT THE CURVE SAYS HERE, seeded with its slope — report 05 §936. What this
                // replaces inserted `glm::vec3( 0.0f )`: a position key AT THE ORIGIN, which yanked the
                // bone across the scene the moment the button was pressed, and a scale key of 1 that
                // flattened whatever the animator had built.
                const Animation::FrameNumber t =
                     Animation::SnapToDisplayRate( animator->GetCurrentTick(), tickRate, displayRate );
                const Animation::TrackChannel channel = ChannelOfLane( m_SelChannel );
                changed                               = Animation::InsertKeyFromCurve( tr, channel, t, tickRate );
            }

            if ( changed )
                animator->SetTime( animator->GetCurrentTime() );
        }
        else
        {
            ImGui::TextDisabled( "Click a keyframe to edit it. Drag keys to retime." );
        }
    }

    // ---- UI property timeline ---------------------------------------------------------------------
    // The skeletal editor above keys bones; this keys a UI element's Offset / Size / Opacity / Color.
    // It edits UIAnimComponent directly: lanes with draggable key diamonds, a scrubbable ruler, and a
    // transport. The playhead is a runtime-only field, so scrubbing never dirties the scene.
    std::vector<SequencerPanel::DocumentAction> SequencerPanel::Actions()
    {
        if ( m_Timeline != Timeline::Skeletal )
        {
            return {};
        }
        return { DocumentAction{ m_CurveView ? "Show the dope sheet" : "Show the curves", [this]
                                 {
                                     m_CurveView       = !m_CurveView;
                                     m_CurveFitPending = true;
                                 } } };
    }

    void SequencerPanel::DrawCurveView( Animation::AnimationClip* clip, Animation::Animator* animator,
                                        float contentX0, float gutter, float laneW, float duration )
    {
        if ( clip == nullptr || clip->Tracks.empty() )
        {
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            ImGui::TextDisabled( "This clip has no bone tracks." );
            return;
        }

        // WITH NOTHING SELECTED IT SHOWS THE FIRST CHANNEL THAT HAS KEYS, rather than an instruction to go
        // and select something. A curve view whose empty state is "select a key in the other view" makes the
        // animator do the tool's work, and it also makes the panel unreachable from the control channel,
        // which is the only way anything about this window gets verified (`--shot` draws no interface).
        //
        // The fallback is LOCAL and does not write the selection: the dope sheet's own highlight is the
        // animator's, and a view that silently moved it would change what the next key edit applies to.
        int viewTrack   = m_SelTrack;
        int viewChannel = m_SelChannel;
        if ( viewTrack < 0 || viewTrack >= static_cast<int>( clip->Tracks.size() ) || viewChannel < 0 )
        {
            viewTrack   = -1;
            viewChannel = 0;
            for ( int ti = 0; ti < static_cast<int>( clip->Tracks.size() ) && viewTrack < 0; ++ti )
            {
                if ( !clip->Tracks[ti].PositionKeys.empty() )
                {
                    viewTrack   = ti;
                    viewChannel = 0;
                }
                else if ( !clip->Tracks[ti].ScaleKeys.empty() )
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

        Animation::BoneTrack& track = clip->Tracks[viewTrack];

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
        const Animation::FrameRate tickRate    = clip->TickRate;
        const Animation::FrameRate displayRate = clip->DisplayRate;

        // THE SAME SCALARS THE TANGENT RULES USE — see TrackEditing::LiftChannel. A view that built its own
        // would be a second statement of what a channel is.
        std::vector<Animation::ScalarKey> lifted[3];
        for ( int component = 0; component < 3; ++component )
        {
            lifted[component] = Animation::LiftChannel( track, channel, component );
        }
        if ( lifted[0].empty() )
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
                     track.BoneName.c_str() );

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
            const glm::vec3            value = ( channel == Animation::TrackChannel::Position )
                                                    ? track.GetInterpolatedPosition( at, tickRate )
                                                    : track.GetInterpolatedScale( at, tickRate );
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
                const bool   selected = ( k == m_SelKey );
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
                        m_SelKey             = k;
                        m_CurveDragKey       = k;
                        m_CurveDragComponent = component;
                        m_CurveDragHandle    = 0;
                    }
                }
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
            m_CurveDragKey       = -1;
            m_CurveDragComponent = -1;
            m_CurveDragHandle    = 0;
        }

        if ( edited )
        {
            const int  component = m_CurveDragComponent;
            const bool written   = Animation::ApplyChannel( track, channel, component, lifted[component] );
            if ( written )
            {
                // The retime, if the drag was a retime: on the TRACK, where a key's tick lives, and after
                // the values are in — so the two halves of one drag cannot half-apply.
                if ( retimeKey >= 0 )
                {
                    if ( channel == Animation::TrackChannel::Position &&
                         retimeKey < static_cast<int>( track.PositionKeys.size() ) )
                    {
                        track.PositionKeys[retimeKey].Tick = retimeTo;
                    }
                    else if ( channel == Animation::TrackChannel::Scale &&
                              retimeKey < static_cast<int>( track.ScaleKeys.size() ) )
                    {
                        track.ScaleKeys[retimeKey].Tick = retimeTo;
                    }
                }
                // A key may have crossed a neighbour: the channel is re-sorted and the whole channel's auto
                // tangents recomputed, exactly as every other edit path does (§969 item 1).
                if ( channel == Animation::TrackChannel::Position )
                {
                    std::sort( track.PositionKeys.begin(), track.PositionKeys.end() );
                }
                else
                {
                    std::sort( track.ScaleKeys.begin(), track.ScaleKeys.end() );
                }
                // THE SORT INVALIDATES THE INDEX THE MOUSE IS HOLDING. A key dragged past its neighbour
                // changes place in the vector, and a drag that kept the old index would carry on moving
                // whatever landed there instead — the key the animator is dragging would swap under the
                // cursor. The dope sheet re-finds its key after a sort for the same reason; this one can
                // do it by TICK exactly, because A5 put every key on one.
                if ( retimeKey >= 0 )
                {
                    const auto& sorted = ( channel == Animation::TrackChannel::Position )
                                              ? track.PositionKeys.size()
                                              : track.ScaleKeys.size();
                    for ( std::size_t i = 0; i < sorted; ++i )
                    {
                        const Animation::FrameNumber tick = ( channel == Animation::TrackChannel::Position )
                                                                 ? track.PositionKeys[i].Tick
                                                                 : track.ScaleKeys[i].Tick;
                        if ( tick == retimeTo )
                        {
                            m_CurveDragKey = static_cast<int>( i );
                            m_SelKey       = static_cast<int>( i );
                            break;
                        }
                    }
                }

                Animation::RefreshTangents( track, tickRate );
                animator->SetTime( animator->GetCurrentTime() );
            }
        }

        // The playhead, over everything.
        const float playX = vp.TimeToX( animator->GetCurrentTime() );
        dl->AddLine( ImVec2( playX, vp.Y0 ), ImVec2( playX, vp.Y1 ), IM_COL32( 255, 90, 90, 190 ), 1.5f );

        ImGui::SetCursorScreenPos( ImVec2( contentX0, vp.Y1 + 6.0f ) );
        ImGui::TextDisabled( "Drag a key to retime (snapped to the display grid) and revalue it; drag a "
                             "handle on a User/Break key to set its tangent." );
    }

    void SequencerPanel::DrawUITracks( ECS::Entity& entity )
    {
        namespace ImGui = ::ImGui;

        // NO "ADD UI ANIMATION" BUTTON HERE ANY MORE. The clip IS this window's subject, so a window over
        // an element that has none is a window over nothing — it cannot open at all (IsSubjectAlive), and
        // it closes if the component is removed underneath it. Adding the clip is Details ▸ UI Layout ▸
        // "Add UI Animation", which adds the component and opens this window on it in one press: the same
        // create-then-open the terrain and cloud material rows use.
        auto& clip = entity.GetComponent<ECS::UIAnimComponent>().Data;

        // --- transport -------------------------------------------------------------------------------
        if ( ImGui::Button( clip.Playing ? ICON_MDI_PAUSE "  Pause" : ICON_MDI_PLAY "  Play" ) )
            clip.Playing = !clip.Playing;
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_STOP "  Rewind" ) )
            clip.Time = 0.0f;
        ImGui::SameLine();
        ImGui::Checkbox( "Loop", &clip.Loop );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 110.0f );
        ImGui::DragFloat( "Duration", &clip.Duration, 0.05f, 0.05f, 120.0f, "%.2f s" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 140.0f );
        ImGui::SliderFloat( "Time", &clip.Time, 0.0f, std::max( 0.05f, clip.Duration ), "%.2f s" );
        if ( ImGui::IsItemActive() )
            clip.Playing = false; // scrubbing takes over from playback, as a timeline should

        // --- add a lane ------------------------------------------------------------------------------
        const char* const propNames[] = { "Offset", "Size", "Opacity", "Color" };
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        static int newProp = 0;
        ImGui::Combo( "##uiprop", &newProp, propNames, 4 );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_PLUS "  Track" ) )
        {
            ECS::UIAnimTrack tr;
            tr.Property = static_cast<ECS::UITweenProperty>( newProp );
            tr.Keys.push_back( { 0.0f, glm::vec4( 0.0f ), ECS::UIEasing::CubicOut } );
            clip.Tracks.push_back( std::move( tr ) );
        }

        if ( clip.Tracks.empty() )
        {
            ImGui::Separator();
            ImGui::TextDisabled( "No tracks. Pick a property above and press + Track." );
            return;
        }

        // --- lanes -----------------------------------------------------------------------------------
        const float duration = std::max( 0.05f, clip.Duration );
        const float gutter   = 120.0f;
        const float laneH    = 22.0f;

        ImGui::Separator();
        const float contentX0 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
        const float laneW     = std::max( 120.0f, ImGui::GetContentRegionAvail().x - gutter - 12.0f );
        const float laneX0    = contentX0 + gutter;
        const auto  timeToX   = [&]( float t ) { return laneX0 + ( t / duration ) * laneW; };
        const auto  xToTime   = [&]( float x )
        { return std::clamp( ( x - laneX0 ) / laneW * duration, 0.0f, duration ); };

        ImGui::BeginChild( "##uiTracks",
                           ImVec2( gutter + laneW, std::min( 260.0f, 12.0f + clip.Tracks.size() * laneH ) ),
                           false );
        ImDrawList* dl = ImGui::GetWindowDrawList();

        int deleteTrack = -1;
        for ( int ti = 0; ti < static_cast<int>( clip.Tracks.size() ); ++ti )
        {
            ECS::UIAnimTrack& tr    = clip.Tracks[ti];
            const ImVec2      rp    = ImGui::GetCursorScreenPos();
            const float       laneY = rp.y;
            dl->AddRectFilled( ImVec2( laneX0, laneY ), ImVec2( laneX0 + laneW, laneY + laneH - 3.0f ),
                               ( ti % 2 ) ? IM_COL32( 40, 40, 46, 255 ) : IM_COL32( 33, 33, 39, 255 ) );
            dl->AddText( ImVec2( contentX0 + 6.0f, laneY + 3.0f ), IM_COL32( 205, 205, 215, 255 ),
                         propNames[static_cast<int>( tr.Property )] );

            // per-lane: key at the playhead, and drop the lane
            ImGui::SetCursorScreenPos( ImVec2( contentX0 + gutter - 46.0f, laneY ) );
            ImGui::PushID( ti * 8192 + 7 );
            if ( ImGui::SmallButton( "+" ) )
            {
                tr.Keys.push_back( { clip.Time, tr.Keys.empty() ? glm::vec4( 0.0f ) : tr.Keys.back().Value,
                                     ECS::UIEasing::CubicOut } );
                std::sort( tr.Keys.begin(), tr.Keys.end(),
                           []( const ECS::UIAnimKey& a, const ECS::UIAnimKey& b ) { return a.Time < b.Time; } );
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x" ) )
                deleteTrack = ti;
            ImGui::PopID();

            // keys as diamonds; drag horizontally to retime
            for ( int ki = 0; ki < static_cast<int>( tr.Keys.size() ); ++ki )
            {
                const float  kx = timeToX( tr.Keys[ki].Time );
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
                }
                if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left ) )
                {
                    tr.Keys[ki].Time = xToTime( ImGui::GetIO().MousePos.x );
                    clip.Playing     = false;
                    clip.Time        = tr.Keys[ki].Time; // the pose follows the key being moved
                }
                if ( ImGui::IsItemDeactivated() )
                {
                    const float moved = tr.Keys[ki].Time;
                    std::sort( tr.Keys.begin(), tr.Keys.end(),
                               []( const ECS::UIAnimKey& a, const ECS::UIAnimKey& b )
                               { return a.Time < b.Time; } );
                    for ( int i = 0; i < static_cast<int>( tr.Keys.size() ); ++i )
                        if ( tr.Keys[i].Time == moved )
                        {
                            m_UIKey = i; // keep the dragged key selected after the re-sort
                            break;
                        }
                }
                ImGui::PopID();
            }

            ImGui::SetCursorScreenPos( ImVec2( rp.x, laneY + laneH ) );
        }

        // playhead over every lane
        const float playX = timeToX( clip.Time );
        dl->AddLine( ImVec2( playX, ImGui::GetWindowPos().y ),
                     ImVec2( playX, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y ),
                     IM_COL32( 255, 120, 90, 220 ), 2.0f );
        ImGui::EndChild();

        if ( deleteTrack >= 0 )
        {
            clip.Tracks.erase( clip.Tracks.begin() + deleteTrack );
            m_UITrack = m_UIKey = -1;
        }

        // --- selected key ----------------------------------------------------------------------------
        if ( m_UITrack >= 0 && m_UITrack < static_cast<int>( clip.Tracks.size() ) )
        {
            ECS::UIAnimTrack& tr = clip.Tracks[m_UITrack];
            if ( m_UIKey >= 0 && m_UIKey < static_cast<int>( tr.Keys.size() ) )
            {
                ECS::UIAnimKey& k = tr.Keys[m_UIKey];
                ImGui::Separator();
                ImGui::Text( "Key %d of %s", m_UIKey, propNames[static_cast<int>( tr.Property )] );
                ImGui::SetNextItemWidth( 120.0f );
                if ( ImGui::DragFloat( "Time", &k.Time, 0.01f, 0.0f, duration, "%.2f s" ) )
                    clip.Time = k.Time;

                // The value is read per property, exactly as the renderer reads it.
                switch ( tr.Property )
                {
                    case ECS::UITweenProperty::Offset:
                    case ECS::UITweenProperty::Size:
                        ImGui::SetNextItemWidth( 200.0f );
                        ImGui::DragFloat2( "Value (px)", &k.Value.x, 1.0f );
                        break;
                    case ECS::UITweenProperty::Opacity:
                        ImGui::SetNextItemWidth( 200.0f );
                        ImGui::SliderFloat( "Opacity", &k.Value.x, 0.0f, 1.0f );
                        break;
                    case ECS::UITweenProperty::Color:
                        ImGui::SetNextItemWidth( 200.0f );
                        ImGui::ColorEdit3( "Color", &k.Value.x );
                        break;
                }

                const char* const easeNames[] = { "Linear",   "QuadIn",     "QuadOut", "QuadInOut",  "CubicIn",
                                                  "CubicOut", "CubicInOut", "BackOut", "ElasticOut", "BounceOut" };
                int               ease        = static_cast<int>( k.Easing );
                ImGui::SetNextItemWidth( 140.0f );
                if ( ImGui::Combo( "Ease in", &ease, easeNames, 10 ) )
                    k.Easing = static_cast<ECS::UIEasing>( ease );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Delete key" ) )
                {
                    tr.Keys.erase( tr.Keys.begin() + m_UIKey );
                    m_UIKey = -1;
                }
            }
        }
    }

    // IsContextual/IsRelevant ARE GONE WITH THE PANEL. They answered "should this window appear because
    // of what is selected?", which is a question only a singleton tool can be asked — a document is
    // opened, by subject, and selecting something else is not a request to open or close one.

} // namespace Desert::Editor
