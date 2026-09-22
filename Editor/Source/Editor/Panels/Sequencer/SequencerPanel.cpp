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
             animator.GetCurrentTick(), clip != nullptr ? clip->TickRate : Animation::PROJECT_TICK_RATE,
             clip != nullptr ? clip->DisplayRate : Animation::DEFAULT_DISPLAY_RATE );
        return target;
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
            if ( ImGui::Button( armed ? ICON_MDI_RECORD_CIRCLE " REC" : ICON_MDI_RECORD " Record" ) )
            {
                Animation::KeyingModes modes = m_Keyer.Modes();
                modes.AutoChange = armed ? Animation::AutoChangeMode::None : Animation::AutoChangeMode::All;
                m_Keyer.SetModes( modes );
            }
            if ( armed )
                ImGui::PopStyleColor( 2 );
            Utils::ImGuiUtilities::Tooltip( "Auto-key (UE ships this OFF): while ON, posing the selected bone "
                                            "writes ONE key when you let go of the gizmo — not one per mouse "
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
                const ScopedPoseEdit undoStep( m_ClipEdit, animator, editClip );
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
            const auto recorded = m_ClipEdit.Observe( animator, editClip, Core::GizmoState::PoseInteraction() );
            if ( !recorded.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] pose edit not undoable: {}", recorded.GetError() );
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

            // THE SECTION LANE SITS BETWEEN THE RULER AND THE KEYS, and in both view modes. A section is
            // a statement about a RANGE of this clip, so it belongs against the same time axis whether
            // the keys below are drawn as diamonds or as curves — putting it inside one of the two
            // branches would make "which section am I in" a question only one of the views could answer.
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
                    // Opened BEFORE the insert below, which is the only place it can be opened: the button
                    // both adds the key and refreshes the whole channel's tangents, and the "before" has to
                    // predate both.
                    ScopedPoseEdit               undoStep( m_ClipEdit, animator, clip );
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
                        // THE RETIME'S OWN RISING EDGE, which this widget knows and nothing else does. It
                        // is opened here rather than through `Observe` because ImGui reports activation
                        // BEFORE the first drag delta is applied — so unlike the gizmo, whose bit is
                        // published from another file a frame late, this one needs no baseline.
                        if ( const auto began = m_ClipEdit.Begin( animator, clip ); !began.IsSuccess() )
                        {
                            LOG_ERROR( "[Sequencer] retime not undoable: {}", began.GetError() );
                        }
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
        // CLOSED AFTER THE SORT AND THE TANGENT REFRESH, not at the mouse-up: both of those are part of
        // what the drag did to the track, and a transaction closed before them would leave an undo that
        // puts the keys back in the dragged order.
        if ( needSort )
        {
            if ( const auto ended = m_ClipEdit.End(); !ended.IsSuccess() )
            {
                LOG_ERROR( "[Sequencer] retime not undoable: {}", ended.GetError() );
            }
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

            // A NUMERIC FIELD'S DRAG IS AN INTERACTION LIKE THE GIZMO'S, and ImGui publishes both of its
            // edges for the item just submitted — so unlike the gizmo this one needs no baseline and no
            // bit passed between files. Called after each field, it makes a drag across forty frames ONE
            // undo step. `IsItemDeactivated` rather than `...AfterEdit`: a click that changed nothing must
            // still close the transaction (it pushes nothing, because the diff is empty), or the next edit
            // would be swallowed into it.
            const auto bracketField = [&]()
            {
                if ( ImGui::IsItemActivated() )
                {
                    if ( const auto began = m_ClipEdit.Begin( animator, clip ); !began.IsSuccess() )
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
                // BETWEEN THE WIDGET AND THE WRITE. `Combo` edits the local `current`, so the key still
                // holds its old value on this line and the transaction's "before" is the true one.
                const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
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
                const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip ); // see interpCombo above
                mode = static_cast<Animation::TangentMode>( current );
                return true;
            };
            if ( keyOk )
            {
                if ( m_SelChannel == 0 )
                {
                    auto& key = tr.PositionKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    bracketField();
                    changed |= ImGui::DragFloat3( "Position", &key.Position.x, 0.01f );
                    bracketField();
                    changed |= interpCombo( key.Interp, true );
                    changed |= tangentCombo( key.Mode );
                    if ( key.Mode != Animation::TangentMode::Auto )
                    {
                        changed |= ImGui::DragFloat3( "Arrive /s", &key.ArriveTangent.x, 0.1f );
                        bracketField();
                        changed |= ImGui::DragFloat3( "Leave /s", &key.LeaveTangent.x, 0.1f );
                        bracketField();
                    }
                }
                else if ( m_SelChannel == 1 )
                {
                    auto& key = tr.RotationKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    bracketField();
                    changed |= interpCombo( key.Interp, false );
                    glm::vec3 euler = glm::degrees( glm::eulerAngles( key.Rotation ) );
                    const bool eulerEdited = ImGui::DragFloat3( "Euler", &euler.x, 0.5f );
                    // The bracket comes FIRST: on the frame ImGui reports activation the field has not
                    // edited anything yet, so the transaction opens over a key that still holds its
                    // original rotation.
                    bracketField();
                    if ( eulerEdited )
                    {
                        key.Rotation = glm::quat( glm::radians( euler ) );
                        changed      = true;
                    }
                }
                else
                {
                    auto& key = tr.ScaleKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    bracketField();
                    changed |= ImGui::DragFloat3( "Scale", &key.Scale.x, 0.01f );
                    bracketField();
                    changed |= interpCombo( key.Interp, true );
                    changed |= tangentCombo( key.Mode );
                    if ( key.Mode != Animation::TangentMode::Auto )
                    {
                        changed |= ImGui::DragFloat3( "Arrive /s", &key.ArriveTangent.x, 0.1f );
                        bracketField();
                        changed |= ImGui::DragFloat3( "Leave /s", &key.LeaveTangent.x, 0.1f );
                        bracketField();
                    }
                }

                if ( ImGui::Button( ICON_MDI_DELETE "  Delete Key" ) )
                {
                    const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
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
                ScopedPoseEdit                undoStep( m_ClipEdit, animator, clip );
                changed = Animation::InsertKeyFromCurve( tr, channel, t, tickRate );
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
    // ── SECTIONS: THE LANE, THE INSPECTOR AND THE THIRTEEN COMMANDS ──────────────────────────────────
    //
    // Before this, `kAnimationVersion` 3 could describe a section and nothing in the editor could author
    // one: the migrator wrote "Whole clip" into every file and that was the only section any animator
    // would ever see. What a section IS lives in Engine/Animation/ClipSection.hpp — including the two
    // rules about its two "empty means" fields — and every edit below goes through the functions there,
    // so the lane, the inspector and the palette cannot disagree about what a section edit is.

    void SequencerPanel::SelectSection( int index )
    {
        m_SelSection = index;
        // THE RENAME BUFFER IS INVALIDATED HERE AND NOWHERE ELSE. It is refilled lazily by the inspector,
        // which is the only thing that knows the section's current name; a copy made here would go stale
        // the moment a palette command renamed the same section.
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
        return SectionTarget{ animator, clip };
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
        if ( m_SelSection < 0 || m_SelSection >= static_cast<int>( target->Clip->Sections.size() ) )
        {
            // A COMMAND WITH NO SELECTION IS A REFUSAL IN WORDS, not a no-op. The palette runs without a
            // mouse, so "nothing happened" is the one answer that cannot be told apart from a defect.
            ToastManager::Push( std::string( what ) + ": select a section first (the clip has " +
                                     std::to_string( target->Clip->Sections.size() ) + ")",
                                ToastLevel::Error, 6.0f );
            return;
        }
        const auto index = static_cast<size_t>( m_SelSection );
        // ONE INTERACTION, ONE UNDO STEP. The guard opens before the edit and closes after it, so a
        // command that changes nothing pushes nothing (PoseEditTransaction::End says why).
        Common::BoolResultStr done = Common::MakeError<bool>( "the edit did not run" );
        {
            const ScopedPoseEdit undoStep( m_ClipEdit, target->Animator, target->Clip );
            done = edit( *target, index );
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
        if ( m_SelSection >= static_cast<int>( clip->Sections.size() ) )
        {
            SelectSection( -1 ); // the picker changed the clip under the selection
        }

        constexpr float kLaneH = 28.0f;
        const ImVec2    origin = ImGui::GetCursorScreenPos();
        const float     laneX0 = contentX0 + gutter;
        ImDrawList*     dl     = ImGui::GetWindowDrawList();

        const Sequencer::CurveViewport axis = TimeAxis( laneX0, laneW, duration );
        const float                    y0   = origin.y;
        const float                    y1   = origin.y + kLaneH;

        dl->AddRectFilled( ImVec2( laneX0, y0 ), ImVec2( laneX0 + laneW, y1 ), IM_COL32( 20, 20, 24, 255 ) );
        dl->AddText( ImVec2( contentX0 + 6.0f, y0 + 7.0f ), IM_COL32( 170, 170, 180, 255 ), "SECTIONS" );

        for ( size_t i = 0; i < clip->Sections.size(); ++i )
        {
            const Animation::ClipSection& section = clip->Sections[i];
            const float                   xa      = axis.TimeToX( TickToSeconds( section.Start, clip->TickRate ) );
            // THE END IS INCLUSIVE (ClipSection.hpp), so the bar is drawn to the end of that tick and not
            // to its left edge — a section ending on the last frame that stopped one frame short of the
            // ruler would read as an off-by-one in the FORMAT rather than in this line.
            const float xb       = axis.TimeToX( TickToSeconds( section.End, clip->TickRate ) );
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

            // THE FADE, INSIDE THE BAR IT BELONGS TO. Drawn only when the channel HAS keys: an empty
            // channel is full weight (ClipSection.hpp), and a flat line along the top of every migrated
            // clip in the repository would be ink that says nothing.
            if ( !section.Weight.empty() && xb > xa + 2.0f )
            {
                const float top    = y0 + 4.0f;
                const float bottom = y1 - 4.0f;
                const int   steps  = std::min( 160, static_cast<int>( xb - xa ) );
                ImVec2      previous( 0.0f, 0.0f );
                for ( int s = 0; s <= steps; ++s )
                {
                    const float t       = static_cast<float>( s ) / static_cast<float>( std::max( 1, steps ) );
                    const float seconds = TickToSeconds( section.Start, clip->TickRate ) +
                                          t * ( TickToSeconds( section.End, clip->TickRate ) -
                                                TickToSeconds( section.Start, clip->TickRate ) );
                    const auto at =
                         Animation::SecondsToFrameTime( static_cast<double>( seconds ), clip->TickRate );
                    const float  w = section.WeightAt( at, clip->TickRate );
                    const ImVec2 pt( xa + t * ( xb - xa ),
                                     bottom - std::clamp( w, 0.0f, 1.0f ) * ( bottom - top ) );
                    if ( s > 0 )
                    {
                        dl->AddLine( previous, pt, IM_COL32( 255, 240, 190, 235 ), 1.6f );
                    }
                    previous = pt;
                }
            }

            char label[96];
            std::snprintf( label, sizeof( label ), "%zu  %s  %s", i, section.Name.c_str(),
                           Animation::SectionBlendName( section.Blend ) );
            dl->PushClipRect( ImVec2( xa + 2.0f, y0 ), ImVec2( std::max( xb, xa + 2.0f ), y1 ), true );
            dl->AddText( ImVec2( xa + 5.0f, y0 + 7.0f ), IM_COL32( 240, 240, 245, 255 ), label );
            dl->PopClipRect();
        }

        // ---- Pick, move and resize ----
        ImGui::SetCursorScreenPos( ImVec2( laneX0, y0 ) );
        ImGui::InvisibleButton( "##sectionLane", ImVec2( laneW, kLaneH ) );
        const auto tickUnderMouse = [&]()
        {
            const float seconds = std::clamp( ( ImGui::GetMousePos().x - laneX0 ) / laneW, 0.0f, 1.0f ) * duration;
            return SecondsToSnappedTick( seconds, clip->TickRate, clip->DisplayRate );
        };

        if ( ImGui::IsItemActivated() )
        {
            constexpr float kGrab = 5.0f; // pixels of edge that resize rather than move
            const float     mx    = ImGui::GetMousePos().x;
            m_SectionDrag         = -1;
            // BACK TO FRONT, because the front of the list is drawn first and the LAST section is the one
            // that wins an overlap (AnimationClip::SectionFor). Picking front-to-back would hand the
            // animator the section whose value they are not seeing.
            for ( size_t j = clip->Sections.size(); j-- > 0; )
            {
                const float xa = axis.TimeToX( TickToSeconds( clip->Sections[j].Start, clip->TickRate ) );
                const float xb = axis.TimeToX( TickToSeconds( clip->Sections[j].End, clip->TickRate ) );
                if ( mx < xa - kGrab || mx > xb + kGrab )
                {
                    continue;
                }
                m_SectionDrag     = static_cast<int>( j );
                m_SectionDragEdge = ( mx <= xa + kGrab ) ? 1 : ( mx >= xb - kGrab ? 2 : 0 );
                break;
            }
            SelectSection( m_SectionDrag );
            m_SectionDragTick = tickUnderMouse();
            if ( m_SectionDrag >= 0 )
            {
                if ( const auto began = m_ClipEdit.Begin( animator, clip ); !began.IsSuccess() )
                {
                    LOG_ERROR( "[Sequencer] section edit not undoable: {}", began.GetError() );
                }
            }
        }

        if ( ImGui::IsItemActive() && m_SectionDrag >= 0 &&
             m_SectionDrag < static_cast<int>( clip->Sections.size() ) )
        {
            const auto   now   = tickUnderMouse();
            const auto   index = static_cast<size_t>( m_SectionDrag );
            if ( !( now == m_SectionDragTick ) )
            {
                Common::BoolResultStr moved = Common::MakeSuccess( true );
                if ( m_SectionDragEdge == 0 )
                {
                    moved = Animation::MoveSection( clip->Sections, index, now.Value - m_SectionDragTick.Value,
                                                    clip->DurationTicks );
                }
                else if ( m_SectionDragEdge == 1 )
                {
                    moved = Animation::SetSectionRange( clip->Sections, index, now, clip->Sections[index].End,
                                                        clip->DurationTicks );
                }
                else
                {
                    moved = Animation::SetSectionRange( clip->Sections, index, clip->Sections[index].Start, now,
                                                        clip->DurationTicks );
                }
                // THE ANCHOR ADVANCES ONLY ON AN ACCEPTED MOVE. A section pushed against tick 0 stops
                // there and starts moving again the instant the mouse comes back past where it stopped —
                // the version that advanced unconditionally teleported it as soon as the mouse reversed.
                if ( moved.IsSuccess() )
                {
                    m_SectionDragTick = now;
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
        // THE SENTENCE Docs/Animation/09_sections_and_weight.md COMMITS US TO, ON THE SURFACE THAT SETS
        // THE NUMBER. Report 05 §972 warns that UE's section weight blends POSES on an Absolute section
        // and VALUES on an Additive one — one slider, two visibly different results. We blend values in
        // both cases, and the place that has to say so is the place an animator reads before dragging.
        HelpMarker( "A section is a RANGE of this clip, the tracks it speaks for, how its values reach the "
                    "pose, and how much of it arrives.\n\n"
                    "WEIGHT IS A PROPORTION OF THE VALUE, NOT A BLEND OF TWO POSES.\n"
                    "At 50 % an Absolute section drives each track halfway from the rest pose to the number "
                    "on the curve; an Additive one applies half the authored offset. Either way it is the "
                    "NUMBER the curve shows that is halved — never the place the bones ended up, which is "
                    "what a pose blend would give and what the curve could not predict.\n\n"
                    "Tracks empty = every track in the clip. Weight empty = full weight, which is not the "
                    "same as a single key of 0.\n"
                    "Two sections over one track at one tick: the LOWER one in the list wins." );
        if ( !open )
        {
            return;
        }

        // ---- The list, and the four operations on it ----
        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.40f, 0.28f, 1.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.50f, 0.36f, 1.0f ) );
        if ( ImGui::Button( ICON_MDI_PLUS " Add Section" ) )
        {
            AddSectionAtPlayhead();
        }
        ImGui::PopStyleColor( 2 );
        Utils::ImGuiUtilities::Tooltip( "A new section from the playhead to the end of the clip: every "
                                        "track, Absolute, full weight — which is what a section that has "
                                        "not been narrowed yet MEANS." );

        const bool hasSelection = m_SelSection >= 0 && m_SelSection < static_cast<int>( clip->Sections.size() );

        ImGui::SameLine();
        ImGui::BeginDisabled( !hasSelection );
        if ( ImGui::Button( ICON_MDI_DELETE " Delete" ) )
        {
            RunSectionEdit( "delete section", []( SectionTarget& target, size_t index )
                            { return Animation::RemoveSection( target.Clip->Sections, index ); } );
            SelectSection( -1 );
        }
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_ARROW_UP " Lower priority" ) )
        {
            ReorderSelectedSection( -1 );
        }
        Utils::ImGuiUtilities::Tooltip( "Move it one place EARLIER in the list. The later of two sections "
                                        "over the same track wins, so earlier means it loses." );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_ARROW_DOWN " Raise priority" ) )
        {
            ReorderSelectedSection( 1 );
        }
        Utils::ImGuiUtilities::Tooltip( "Move it one place LATER in the list, where it wins an overlap." );
        ImGui::EndDisabled();

        if ( clip->Sections.empty() )
        {
            ImGui::TextDisabled( "This clip states no section. Every track plays exactly as authored." );
            return;
        }

        // The picker. A list rather than a combo: the ORDER is the priority rule, so a control that hides
        // all but one row hides the only thing the order does.
        for ( size_t i = 0; i < clip->Sections.size(); ++i )
        {
            const Animation::ClipSection& section = clip->Sections[i];
            char                          row[160];
            std::snprintf( row, sizeof( row ), "%zu   %s   [%d..%d]   %s   %s##sectionRow%zu", i,
                           section.Name.c_str(),
                           Animation::DisplayFrameIndex( section.Start, clip->TickRate, clip->DisplayRate ),
                           Animation::DisplayFrameIndex( section.End, clip->TickRate, clip->DisplayRate ),
                           Animation::SectionBlendName( section.Blend ),
                           section.Tracks.empty() ? "all tracks" : "some tracks", i );
            if ( ImGui::Selectable( row, static_cast<int>( i ) == m_SelSection ) )
            {
                SelectSection( static_cast<int>( i ) );
            }
        }

        if ( !hasSelection )
        {
            ImGui::TextDisabled( "Pick a section above to edit it." );
            return;
        }

        const auto              index   = static_cast<size_t>( m_SelSection );
        Animation::ClipSection& section = clip->Sections[index];

        // A DRAG ACROSS FORTY FRAMES IS ONE UNDO STEP, and this is the bracket the dope-sheet fields use
        // above — the same two ImGui edges, so a section field and a key field cannot disagree about what
        // one interaction is.
        const auto bracketField = [&]()
        {
            if ( ImGui::IsItemActivated() )
            {
                if ( const auto began = m_ClipEdit.Begin( animator, clip ); !began.IsSuccess() )
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
        int       first = Animation::DisplayFrameIndex( section.Start, clip->TickRate, clip->DisplayRate );
        int       last  = Animation::DisplayFrameIndex( section.End, clip->TickRate, clip->DisplayRate );
        const int lastFrame =
             Animation::DisplayFrameIndex( clip->DurationTicks, clip->TickRate, clip->DisplayRate );
        const double ticksPerFrame = clip->TickRate.AsDouble() / clip->DisplayRate.AsDouble();
        const auto   frameToTick   = [&]( int frame )
        {
            return Animation::FrameNumber{
                 static_cast<int32_t>( std::llround( static_cast<double>( frame ) * ticksPerFrame ) ) };
        };

        ImGui::SetNextItemWidth( 120.0f );
        if ( ImGui::DragInt( "Start frame", &first, 1.0f, 0, lastFrame ) )
        {
            // REFUSALS ARE LOGGED AND NOT TOASTED HERE. A drag crosses the clip's end on its way to a
            // legal value and would otherwise raise one toast per frame; the field simply does not move,
            // which is the answer a drag can read.
            if ( const auto set = Animation::SetSectionRange( clip->Sections, index, frameToTick( first ),
                                                              section.End, clip->DurationTicks );
                 !set.IsSuccess() )
            {
                LOG_TRACE( "[Sequencer] section start refused: {}", set.GetError() );
            }
        }
        bracketField();
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        if ( ImGui::DragInt( "End frame", &last, 1.0f, 0, lastFrame ) )
        {
            if ( const auto set = Animation::SetSectionRange( clip->Sections, index, section.Start,
                                                              frameToTick( last ), clip->DurationTicks );
                 !set.IsSuccess() )
            {
                LOG_TRACE( "[Sequencer] section end refused: {}", set.GetError() );
            }
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
            const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
            section.Blend = static_cast<Animation::SectionBlendType>( blend );
        }
        ImGui::SameLine();
        HelpMarker( "Absolute — the track's value IS the pose; weight walks each number from the rest pose "
                    "towards it.\n"
                    "Additive — the track's value is an OFFSET on top of the rest pose; weight scales the "
                    "offset, so an identity value changes nothing at any weight.\n\n"
                    "Both blend VALUES. Neither interpolates between two evaluated poses." );

        // ---- Which tracks it speaks for ----
        bool everyTrack = section.Tracks.empty();
        if ( ImGui::Checkbox( "Speaks for every track in the clip", &everyTrack ) )
        {
            const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
            if ( everyTrack )
            {
                Animation::SetSectionSpeaksForEveryTrack( section );
            }
            else if ( !clip->Tracks.empty() )
            {
                // UNTICKING IT HAS TO NAME SOMETHING, and the honest something is what it already speaks
                // for minus one — the first track. Leaving the list empty would tick the box again next
                // frame, which reads as the control being broken.
                std::vector<std::string> named;
                named.reserve( clip->Tracks.size() );
                for ( const auto& track : clip->Tracks )
                {
                    named.push_back( track.BoneName );
                }
                section.Tracks = std::move( named );
                if ( section.Tracks.size() > 1 )
                {
                    section.Tracks.pop_back();
                }
            }
        }
        ImGui::SameLine();
        HelpMarker( "An empty list IS 'every track', and that is not a shorthand: a list naming every "
                    "track goes stale the first time the keyer adds one. Ticking the last box here puts "
                    "the list back to empty for that reason." );

        if ( !section.Tracks.empty() )
        {
            ImGui::Indent();
            if ( ImGui::BeginChild( "##sectionTracks", ImVec2( 0.0f, 120.0f ), true ) )
            {
                std::vector<std::string> allTracks;
                allTracks.reserve( clip->Tracks.size() );
                for ( const auto& track : clip->Tracks )
                {
                    allTracks.push_back( track.BoneName );
                }
                for ( const auto& name : allTracks )
                {
                    bool on = section.Speaks( name );
                    if ( ImGui::Checkbox( name.c_str(), &on ) )
                    {
                        const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
                        if ( const auto set = Animation::SetSectionSpeaksFor( section, name, on, allTracks );
                             !set.IsSuccess() )
                        {
                            ToastManager::Push( set.GetError(), ToastLevel::Error, 6.0f );
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::Unindent();
        }

        // ---- The weight channel ----
        ImGui::Separator();
        const Animation::FrameTime playhead = animator->GetCurrentTick();
        ImGui::Text( "Weight here: %.3f", section.WeightAt( playhead, clip->TickRate ) );
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
            const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
            if ( const auto keyed = Animation::SetSectionWeightKey( section, playhead.Frame, m_SectionWeight );
                 !keyed.IsSuccess() )
            {
                ToastManager::Push( keyed.GetError(), ToastLevel::Error, 6.0f );
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled( section.Weight.empty() );
        if ( ImGui::Button( "Clear fade" ) )
        {
            const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
            Animation::ClearSectionWeight( section );
        }
        ImGui::EndDisabled();
        Utils::ImGuiUtilities::Tooltip( "Back to no fade, which is FULL weight — not silence." );

        for ( size_t k = 0; k < section.Weight.size(); ++k )
        {
            ImGui::PushID( static_cast<int>( k ) );
            ImGui::SetNextItemWidth( 90.0f );
            float value = section.Weight[k].Value;
            ImGui::Text( "f%d", Animation::DisplayFrameIndex( section.Weight[k].Tick, clip->TickRate,
                                                              clip->DisplayRate ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::DragFloat( "##weightKey", &value, 0.005f, 0.0f, 1.0f, "%.3f" ) )
            {
                if ( const auto set = Animation::SetSectionWeightKey( section, section.Weight[k].Tick, value );
                     !set.IsSuccess() )
                {
                    LOG_TRACE( "[Sequencer] weight key refused: {}", set.GetError() );
                }
            }
            bracketField();
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
            {
                const ScopedPoseEdit undoStep( m_ClipEdit, animator, clip );
                if ( const auto removed = Animation::RemoveSectionWeightKey( section, k ); !removed.IsSuccess() )
                {
                    ToastManager::Push( removed.GetError(), ToastLevel::Error, 6.0f );
                }
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    void SequencerPanel::AddSectionAtPlayhead()
    {
        auto target = ResolveSectionTarget();
        if ( !target )
        {
            ToastManager::Push( "add section: no clip is open on this timeline", ToastLevel::Error, 6.0f );
            return;
        }
        Animation::AnimationClip* clip = target->Clip;
        // FROM THE PLAYHEAD TO THE END, and named by its own position in the list. A range of zero length
        // would be legal (the end is inclusive, so it is a one-tick section) and useless: the first thing
        // the animator would do is drag it open, and a new section that has to be repaired before it says
        // anything is a button that half-works.
        const Animation::FrameNumber start = target->Animator->GetCurrentTick().Frame;
        char                         name[48];
        std::snprintf( name, sizeof( name ), "Section %zu", clip->Sections.size() + 1 );

        const ScopedPoseEdit undoStep( m_ClipEdit, target->Animator, clip );
        const auto     added = Animation::AddSection( clip->Sections, name, start, clip->DurationTicks,
                                                      Animation::SectionBlendType::Absolute, clip->DurationTicks );
        if ( !added.IsSuccess() )
        {
            LOG_ERROR( "[Sequencer] add section: {}", added.GetError() );
            ToastManager::Push( std::string( "add section: " ) + added.GetError(), ToastLevel::Error, 8.0f );
            return;
        }
        // THE NEW SECTION IS SELECTED, and it is the LAST one — which is also the one that wins an
        // overlap. Selecting it is how the animator finds out.
        SelectSection( static_cast<int>( clip->Sections.size() ) - 1 );
    }

    void SequencerPanel::ReorderSelectedSection( int delta )
    {
        auto target = ResolveSectionTarget();
        if ( !target || m_SelSection < 0 || m_SelSection >= static_cast<int>( target->Clip->Sections.size() ) )
        {
            ToastManager::Push( "reorder section: select a section first", ToastLevel::Error, 6.0f );
            return;
        }
        const auto            index = static_cast<size_t>( m_SelSection );
        Common::BoolResultStr moved = Common::MakeError<bool>( "the edit did not run" );
        {
            const ScopedPoseEdit undoStep( m_ClipEdit, target->Animator, target->Clip );
            moved = Animation::ReorderSection( target->Clip->Sections, index, delta );
        }
        if ( !moved.IsSuccess() )
        {
            ToastManager::Push( std::string( "reorder section: " ) + moved.GetError(), ToastLevel::Error, 6.0f );
            return;
        }
        // THE SELECTION FOLLOWS THE SECTION, not the index. Leaving it behind would point the inspector at
        // whichever section was swapped INTO the slot, and the next edit would land on the wrong one.
        SelectSection( m_SelSection + delta );
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
                                               if ( clip == nullptr || clip->Tracks.empty() )
                                               {
                                                   ToastManager::Push( "this clip has no lane", ToastLevel::Error,
                                                                       6.0f );
                                                   return;
                                               }
                                               const int count = static_cast<int>( clip->Tracks.size() );
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
        actions.push_back( DocumentAction{ "Select the next section", [this]
                                           {
                                               const auto target = ResolveSectionTarget();
                                               if ( !target || target->Clip->Sections.empty() )
                                               {
                                                   ToastManager::Push( "this clip states no section",
                                                                       ToastLevel::Error, 6.0f );
                                                   return;
                                               }
                                               const int count = static_cast<int>( target->Clip->Sections.size() );
                                               SelectSection( ( m_SelSection + 1 ) % count );
                                           } } );
        actions.push_back( DocumentAction{
             "Delete the selected section", [this]
             {
                 RunSectionEdit( "delete section", []( SectionTarget& target, size_t index )
                                 { return Animation::RemoveSection( target.Clip->Sections, index ); } );
                 SelectSection( -1 );
             } } );
        actions.push_back( DocumentAction{ "Set the selected section's start to the playhead", [this]
                                           {
                                               RunSectionEdit( "section start",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return Animation::SetSectionRange(
                                                                        target.Clip->Sections, index,
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        target.Clip->Sections[index].End,
                                                                        target.Clip->DurationTicks );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Set the selected section's end to the playhead", [this]
                                           {
                                               RunSectionEdit( "section end",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return Animation::SetSectionRange(
                                                                        target.Clip->Sections, index,
                                                                        target.Clip->Sections[index].Start,
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        target.Clip->DurationTicks );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Set the selected section to Additive", [this]
                                           {
                                               RunSectionEdit( "section blend",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   target.Clip->Sections[index].Blend =
                                                                        Animation::SectionBlendType::Additive;
                                                                   return Common::MakeSuccess( true );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Set the selected section to Absolute", [this]
                                           {
                                               RunSectionEdit( "section blend",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   target.Clip->Sections[index].Blend =
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
                                                                   return Animation::SetSectionWeightKey(
                                                                        target.Clip->Sections[index],
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        0.0f );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Fade the selected section to 1 at the playhead", [this]
                                           {
                                               RunSectionEdit( "section weight",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   return Animation::SetSectionWeightKey(
                                                                        target.Clip->Sections[index],
                                                                        target.Animator->GetCurrentTick().Frame,
                                                                        1.0f );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{ "Clear the selected section's fade", [this]
                                           {
                                               RunSectionEdit( "clear fade",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   Animation::ClearSectionWeight(
                                                                        target.Clip->Sections[index] );
                                                                   return Common::MakeSuccess( true );
                                                               } );
                                           } } );
        actions.push_back( DocumentAction{
             "Limit the selected section to the selected bone's track", [this]
             {
                 RunSectionEdit(
                      "section tracks",
                      []( SectionTarget& target, size_t index ) -> Common::BoolResultStr
                      {
                          const int bone = Core::ActiveAuthoringContext().SelectedBoneIndex();
                          if ( bone < 0 ||
                               bone >= static_cast<int>( target.Animator->GetSkeleton().GetBones().size() ) )
                          {
                              return Common::MakeError<bool>(
                                   "no bone is selected — pick one in the lanes below" );
                          }
                          const std::string& name =
                               target.Animator->GetSkeleton().GetBones()[static_cast<size_t>( bone )].Name;
                          // The whole list is replaced rather than narrowed: "limit to
                          // THIS one" is one statement, and doing it as a sequence of
                          // removals would leave a different list behind on every rig.
                          for ( const auto& track : target.Clip->Tracks )
                          {
                              if ( track.BoneName == name )
                              {
                                  target.Clip->Sections[index].Tracks = { name };
                                  return Common::MakeSuccess( true );
                              }
                          }
                          return Common::MakeFormattedError<bool>( "the clip has no track for bone '{}'", name );
                      } );
             } } );
        actions.push_back( DocumentAction{ "Let the selected section speak for every track", [this]
                                           {
                                               RunSectionEdit( "section tracks",
                                                               []( SectionTarget& target, size_t index )
                                                               {
                                                                   Animation::SetSectionSpeaksForEveryTrack(
                                                                        target.Clip->Sections[index] );
                                                                   return Common::MakeSuccess( true );
                                                               } );
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
        if ( clip == nullptr || clip->Tracks.empty() )
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

        // The grab tests above are the only writers of `m_CurveDragKey` on this side of the frame, and the
        // edit below is the first thing that touches the track — so the transaction opens between them.
        if ( curveDragBefore < 0 && m_CurveDragKey >= 0 )
        {
            if ( const auto began = m_ClipEdit.Begin( animator, clip ); !began.IsSuccess() )
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

        // A TRANSACTION MUST NEVER SPAN TWO SUBJECTS. This window is a document over one element, but the
        // component's ADDRESS moves when entt grows the pool, and a transaction opened last frame against
        // the old address would commit a "before" that belongs to memory the registry has reused. The
        // subject is therefore compared every frame, and a mismatch abandons the entry rather than
        // guessing which clip it was about.
        if ( m_UIClipEdit.Open() && m_UIClipEdit.Subject() != &clip )
        {
            m_UIClipEdit.Cancel();
        }

        // --- transport -------------------------------------------------------------------------------
        if ( ImGui::Button( clip.Playing ? ICON_MDI_PAUSE "  Pause" : ICON_MDI_PLAY "  Play" ) )
            clip.Playing = !clip.Playing;
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_STOP "  Rewind" ) )
            clip.Time = 0.0f;
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Loop", &clip.Loop ) )
        {
            // The checkbox has ALREADY written the field, so the scope cannot bracket the write the way
            // the other instantaneous edits do. The "before" is therefore reconstructed from the value in
            // hand -- one bool, and the only field this edit can have touched.
            RecordUIClipToggle( clip, !clip.Loop );
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 110.0f );
        ImGui::DragFloat( "Duration", &clip.Duration, 0.05f, 0.05f, 120.0f, "%.2f s" );
        BracketUIClipEditFromItem( clip );
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
            const ScopedUIClipEdit step( m_UIClipEdit, &clip );
            ECS::UIAnimTrack       tr;
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
                AddUIKeyAtPlayhead( clip, ti );
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
                    if ( const auto began = m_UIClipEdit.Begin( &clip ); !began.IsSuccess() )
                    {
                        LOG_ERROR( "[UIClipUndo] this key drag will not be undoable: {}", began.GetError() );
                    }
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
                    // CLOSED AFTER THE SORT, not before it: the sort is part of what the drag did, and an
                    // entry recorded before it would hold an ordering the lane never had.
                    EndUIClipEdit();
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
            const ScopedUIClipEdit step( m_UIClipEdit, &clip );
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
                BracketUIClipEditFromItem( clip );

                // The value is read per property, exactly as the renderer reads it.
                switch ( tr.Property )
                {
                    case ECS::UITweenProperty::Offset:
                    case ECS::UITweenProperty::Size:
                        ImGui::SetNextItemWidth( 200.0f );
                        ImGui::DragFloat2( "Value (px)", &k.Value.x, 1.0f );
                        BracketUIClipEditFromItem( clip );
                        break;
                    case ECS::UITweenProperty::Opacity:
                        ImGui::SetNextItemWidth( 200.0f );
                        ImGui::SliderFloat( "Opacity", &k.Value.x, 0.0f, 1.0f );
                        BracketUIClipEditFromItem( clip );
                        break;
                    case ECS::UITweenProperty::Color:
                        ImGui::SetNextItemWidth( 200.0f );
                        ImGui::ColorEdit3( "Color", &k.Value.x );
                        BracketUIClipEditFromItem( clip );
                        break;
                }

                const char* const easeNames[] = { "Linear",   "QuadIn",     "QuadOut", "QuadInOut",  "CubicIn",
                                                  "CubicOut", "CubicInOut", "BackOut", "ElasticOut", "BounceOut" };
                int               ease        = static_cast<int>( k.Easing );
                ImGui::SetNextItemWidth( 140.0f );
                if ( ImGui::Combo( "Ease in", &ease, easeNames, 10 ) )
                {
                    const ScopedUIClipEdit step( m_UIClipEdit, &clip );
                    k.Easing = static_cast<ECS::UIEasing>( ease );
                }
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Delete key" ) )
                {
                    const ScopedUIClipEdit step( m_UIClipEdit, &clip );
                    tr.Keys.erase( tr.Keys.begin() + m_UIKey );
                    m_UIKey = -1;
                }
            }
        }

        // THE SWEEP, and it is the same one PoseEditTransaction::OpenExplicitly exists for. Every opener
        // above is a WIDGET being held, so a transaction still open while ImGui reports no active item has
        // lost its closer -- the lane it belonged to was deleted, or the property combo changed the branch
        // that was drawing the field. Leaving it open would swallow every later edit into one enormous
        // undo step, which is worse than having none.
        if ( m_UIClipEdit.Open() && !ImGui::IsAnyItemActive() )
        {
            EndUIClipEdit();
        }
    }

    void SequencerPanel::AddUIKeyAtPlayhead( ECS::UIAnimData& clip, int lane )
    {
        // ONE BODY FOR THE BUTTON AND THE COMMAND, for the reason RunSectionEdit above exists: two copies
        // of "add a key" are two answers that drift, and the command is the ONLY one of the two that an
        // unattended run can reach -- so a drift would be invisible in exactly the run that checks it.
        if ( lane < 0 || lane >= static_cast<int>( clip.Tracks.size() ) )
        {
            ToastManager::Push( "add UI key: no lane is selected", ToastLevel::Error, 6.0f );
            return;
        }
        const ScopedUIClipEdit step( m_UIClipEdit, &clip );
        ECS::UIAnimTrack&      track = clip.Tracks[lane];
        track.Keys.push_back( { clip.Time, track.Keys.empty() ? glm::vec4( 0.0f ) : track.Keys.back().Value,
                                ECS::UIEasing::CubicOut } );
        std::sort( track.Keys.begin(), track.Keys.end(),
                   []( const ECS::UIAnimKey& a, const ECS::UIAnimKey& b ) { return a.Time < b.Time; } );
    }

    void SequencerPanel::BracketUIClipEditFromItem( ECS::UIAnimData& clip )
    {
        // ONE PLACE FOR THE TWO EDGES OF A HELD WIDGET. Written as a member rather than as a lambda at
        // each call site because there are six of them and six copies is six chances to bracket only one
        // end -- which produces an undo step that begins in one interaction and ends in another.
        //
        // IsItemDeactivated, NOT IsItemDeactivatedAfterEdit: a widget grabbed and released without a
        // change fires only the former, and a transaction closed by the latter alone would stay open into
        // the next interaction. The "nothing changed" case costs nothing -- End() answers 0 and pushes no
        // entry, which is the contract that makes closing on every release safe.
        if ( ::ImGui::IsItemActivated() )
        {
            if ( const auto began = m_UIClipEdit.Begin( &clip ); !began.IsSuccess() )
            {
                LOG_ERROR( "[UIClipUndo] this edit will not be undoable: {}", began.GetError() );
            }
        }
        if ( ::ImGui::IsItemDeactivated() )
        {
            EndUIClipEdit();
        }
    }

    void SequencerPanel::RecordUIClipToggle( ECS::UIAnimData& clip, bool loopBefore )
    {
        const bool loopAfter = clip.Loop;
        clip.Loop            = loopBefore;
        {
            const ScopedUIClipEdit step( m_UIClipEdit, &clip );
            clip.Loop = loopAfter;
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

} // namespace Desert::Editor
