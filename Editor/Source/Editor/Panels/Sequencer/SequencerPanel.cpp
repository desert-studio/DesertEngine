#include "SequencerPanel.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/ToastManager.hpp>
// SelectionManager and PanelContext are gone from this file with the selection it used to follow. What is
// left of Selection here is SkeletonEditMode, which is not a selection at all: it is the viewport MODE the
// bone gizmo runs in, and keying by manipulation reads it.
#include <Editor/Core/Selection/SkeletonEditMode.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>
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
        [[nodiscard]] Animation::FrameNumber SecondsToSnappedTick( float seconds,
                                                                   Animation::FrameRate tickRate,
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
            tick                       = Animation::FrameNumber{ static_cast<int32_t>(
                 std::llround( static_cast<double>( frame ) * ticksPerFrame ) ) };
            return true;
        }
    } // namespace

    namespace ImGui = ::ImGui;

    SequencerPanel::SequencerPanel( const SubjectId& subject, const std::string& displayName,
                                    const Timeline timeline, const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    Animation::AnimationLibrary* library, Assets::AssetManager* assetManager )
         : ISubjectDocument( displayName, subject ), m_Scene( scene ), m_Library( library ),
           m_AssetManager( assetManager ), m_Timeline( timeline )
    {
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
        clip.DurationTicks = Animation::FrameNumber{ Animation::PROJECT_TICK_RATE.Numerator };
        clip.TickRate      = Animation::PROJECT_TICK_RATE;
        clip.DisplayRate   = Animation::DEFAULT_DISPLAY_RATE;
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

    void SequencerPanel::DrawSkeletalTimeline( ECS::Entity& entity )
    {
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

        // THE CLIP'S TWO GRIDS, read once for the whole panel. Hoisted to this scope rather than to the
        // timeline block below because four separate scopes — the ruler, the lanes, the key drag and the
        // inspector — all convert between pixels and time, and a grid visible to only the first of them
        // is how four converters come to disagree.
        const Animation::AnimationClip* gridClip = animator ? animator->GetCurrentClip() : nullptr;
        const Animation::FrameRate tickRate = gridClip ? gridClip->TickRate : Animation::PROJECT_TICK_RATE;
        const Animation::FrameRate displayRate =
             gridClip ? gridClip->DisplayRate : Animation::DEFAULT_DISPLAY_RATE;
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
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::SliderFloat( "Zoom", &m_PxPerSec, 20.0f, 240.0f, "%.0f px/u" );
        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::TextColored( ImVec4( 0.80f, 0.86f, 0.98f, 1.0f ), "%.2f / %.2f s", playTime, duration );

        // ---- Keyframe toolbar: author BY MANIPULATION (pose a bone in Skeleton Edit, then key/record) ----
        if ( animator )
        {
            auto*      editClip = const_cast<Animation::AnimationClip*>( animator->GetCurrentClip() );
            const int  selBone  = Core::SkeletonEditMode::GetSelectedBone();
            const bool canKey   = editClip && Core::SkeletonEditMode::IsActive() && selBone >= 0;

            // Author-by-posing: while a clip is open in Skeleton Edit, the bone gizmo edits the Animator's
            // editable pose buffer (not the rig's bind pose), and keying captures that buffer.
            Core::SkeletonEditMode::SetPoseMode( Core::SkeletonEditMode::IsActive() && editClip != nullptr );

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

            const auto timeToX = [&]( float t ) { return laneX0 + ( t / duration ) * laneW; };


            dl->AddRectFilled( ImVec2( laneX0, origin.y ), ImVec2( laneX0 + laneW, origin.y + rulerH ),
                               IM_COL32( 24, 24, 28, 255 ) );
            dl->AddText( ImVec2( origin.x + 6.0f, origin.y + 8.0f ), IM_COL32( 170, 170, 180, 255 ), "TIMELINE" );

            // Whole-unit tick lines + labels.
            for ( int u = 0; u <= static_cast<int>( duration ); ++u )
            {
                const float x = timeToX( static_cast<float>( u ) );
                dl->AddLine( ImVec2( x, origin.y ), ImVec2( x, origin.y + rulerH ),
                             IM_COL32( 255, 255, 255, 25 ) );
                char buf[16];
                std::snprintf( buf, sizeof( buf ), "%d", u );
                dl->AddText( ImVec2( x + 3.0f, origin.y + 3.0f ), IM_COL32( 190, 190, 190, 160 ), buf );
            }

            // Notify markers (from the current clip).
            if ( const Animation::AnimationClip* clip = animator->GetCurrentClip() )
            {
                for ( const auto& n : clip->Notifies )
                {
                    const float x = timeToX( static_cast<float>( Animation::FrameTimeToSeconds(
                         Animation::FrameTime{ n.Tick, 0.0F }, clip->TickRate ) ) );
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

            // ---- Keyframe tracks ----
            DrawClipTracks( const_cast<Animation::AnimationClip*>( animator->GetCurrentClip() ), animator,
                            origin.x, gutter, laneW, duration );
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
        const auto  timeToX   = [&]( float t ) { return laneX0 + ( t / duration ) * laneW; };

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
                ImGui::SetCursorScreenPos( ImVec2( contentX0 + gutter - 22.0f, laneY - 1.0f ) );
                ImGui::PushID( ( ti * 3 + ch ) * 4096 + 3999 );
                if ( ImGui::SmallButton( "+" ) )
                {
                    const Animation::FrameNumber t = Animation::SnapToDisplayRate(
                         animator->GetCurrentTick(), tickRate, displayRate );
                    if ( ch == 0 )
                        tr.PositionKeys.push_back( { t, glm::vec3( 0.0f ) } );
                    else if ( ch == 1 )
                        tr.RotationKeys.push_back( { t, glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) } );
                    else
                        tr.ScaleKeys.push_back( { t, glm::vec3( 1.0f ) } );
                    if ( ch == 0 )
                        std::sort( tr.PositionKeys.begin(), tr.PositionKeys.end() );
                    else if ( ch == 1 )
                        std::sort( tr.RotationKeys.begin(), tr.RotationKeys.end() );
                    else
                        std::sort( tr.ScaleKeys.begin(), tr.ScaleKeys.end() );
                    m_SelTrack   = ti;
                    m_SelChannel = ch;
                    if ( auto bi = animator->GetSkeleton().FindBoneIndex( tr.BoneName ); bi.has_value() )
                        Core::SkeletonEditMode::SetSelectedBone( static_cast<int>( bi.value() ) );
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
                    Animation::FrameNumber& kt = ( ch == 0 )   ? tr.PositionKeys[k].Tick
                                                 : ( ch == 1 ) ? tr.RotationKeys[k].Tick
                                                               : tr.ScaleKeys[k].Tick;
                    const float kx = timeToX( TickToSeconds( kt, tickRate ) );
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
                        // + gizmo), so Sequencer <-> Skeleton Edit stay in sync.
                        if ( auto bi = animator->GetSkeleton().FindBoneIndex( tr.BoneName ); bi.has_value() )
                            Core::SkeletonEditMode::SetSelectedBone( static_cast<int>( bi.value() ) );
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
                        kt         = SecondsToSnappedTick( draggedSeconds, tickRate, displayRate );
                        m_DragTime = TickToSeconds( kt, tickRate );
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
            if ( keyOk )
            {
                if ( m_SelChannel == 0 )
                {
                    auto& key = tr.PositionKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
                    changed |= ImGui::DragFloat3( "Position", &key.Position.x, 0.01f );
                }
                else if ( m_SelChannel == 1 )
                {
                    auto& key = tr.RotationKeys[m_SelKey];
                    changed |= FrameField( key.Tick, animator->GetDurationTicks(), tickRate, displayRate );
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
                const Animation::FrameNumber t =
                     Animation::SnapToDisplayRate( animator->GetCurrentTick(), tickRate, displayRate );
                if ( m_SelChannel == 0 )
                {
                    tr.PositionKeys.push_back( { t, glm::vec3( 0.0f ) } );
                    std::sort( tr.PositionKeys.begin(), tr.PositionKeys.end() );
                }
                else if ( m_SelChannel == 1 )
                {
                    tr.RotationKeys.push_back( { t, glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) } );
                    std::sort( tr.RotationKeys.begin(), tr.RotationKeys.end() );
                }
                else
                {
                    tr.ScaleKeys.push_back( { t, glm::vec3( 1.0f ) } );
                    std::sort( tr.ScaleKeys.begin(), tr.ScaleKeys.end() );
                }
                changed = true;
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
