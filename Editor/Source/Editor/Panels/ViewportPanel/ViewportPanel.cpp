#include <array>
#include "ViewportPanel.hpp"
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/SceneOpenRequest.hpp>
#include <Editor/Core/EditorPreferences.hpp>

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Engine/Assets/Prefab/PrefabPlacement.hpp>
#include <Editor/Core/Selection/UIPreview.hpp>
#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Import/MeshMaterial.hpp>
#include <Editor/Import/AsyncMeshLoader.hpp>
#include <filesystem>
#include <Engine/Graphic/Render2D/Transform2D.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <functional>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/SelectionContext.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UILayout.hpp>
#include <Editor/Panels/ViewportPanel/ActiveViewportRule.hpp>
#include <Editor/Panels/UI/UIElementCatalog.hpp>
#include <Editor/Panels/UI/UIElementFactory.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <ImGuizmo.h>
// ImGuiContext::WindowsFocusOrder — the editor's one record of where the user has been. See
// ActiveViewportRule.hpp for why the order is read rather than a focus flag stored.
#include <imgui_internal.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <limits>
#include <random>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <string_view>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // In-scene UI authoring uses the SAME catalog and the SAME factory as the UI Editor panel's toolbar
        // (Editor/Panels/UI/UIElementCatalog.hpp) — this file used to carry its own copy of AddUIChild and its
        // own hand-written menu, and the two lists had already drifted: the viewport offered UI Image and the
        // panel did not.

        // Unity/UE-style anchor presets. Each axis is Min/Center/Max (a fixed-size box pinned to that edge,
        // keeping the element's current size) or Stretch (anchors 0..1, zero offsets -> fills the parent on
        // that axis). Stretch+Stretch = "fill parent" (the full-quad the UI needs). Offsets are in the same
        // screen-px space UICanvasLayout/RenderCanvas2D resolve layout in.
        enum class AnchorAxis
        {
            Min,
            Center,
            Max,
            Stretch
        };

        void AnchorAxisValues( AnchorAxis m, float size, float& aMin, float& aMax, float& offMin, float& offMax )
        {
            switch ( m )
            {
                case AnchorAxis::Stretch:
                    aMin   = 0.0f;
                    aMax   = 1.0f;
                    offMin = 0.0f;
                    offMax = 0.0f;
                    break;
                case AnchorAxis::Min:
                    aMin   = 0.0f;
                    aMax   = 0.0f;
                    offMin = 0.0f;
                    offMax = size;
                    break;
                case AnchorAxis::Center:
                    aMin   = 0.5f;
                    aMax   = 0.5f;
                    offMin = -size * 0.5f;
                    offMax = size * 0.5f;
                    break;
                case AnchorAxis::Max:
                    aMin   = 1.0f;
                    aMax   = 1.0f;
                    offMin = -size;
                    offMax = 0.0f;
                    break;
            }
        }

        void ApplyAnchorPreset( entt::registry& reg, entt::entity e, const ::Desert::UI::Rect& viewRect,
                                AnchorAxis hx, AnchorAxis vy )
        {
            if ( !reg.has<ECS::UILayoutComponent>( e ) )
                return;
            auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;

            // Element size in DESIGN space (the space UILayout offsets are stored in). GetElementRect returns
            // the on-screen rect, so divide by the canvas scale. Fall back to the element's authored size when
            // it isn't resolvable (e.g. not parented under a canvas yet) so Fill/presets never silently no-op.
            //
            // The canvas is THIS ELEMENT's own — derived from the tree it is in, not from whichever canvas
            // the scene lists first. An element in the second canvas used to get the first one's scale.
            const entt::entity canvas = ::Desert::UI::CanvasOf( reg, e );
            const auto         scaleR = ::Desert::UI::CanvasScale( reg, canvas, viewRect );
            const float        scale  = scaleR ? scaleR.GetValue() : 1.0f;
            const float        inv    = scale > 0.0001f ? 1.0f / scale : 1.0f;
            float              sizeX  = std::max( 1.0f, L.OffsetMax.x - L.OffsetMin.x );
            float              sizeY  = std::max( 1.0f, L.OffsetMax.y - L.OffsetMin.y );
            if ( ::Desert::UI::Rect er; ::Desert::UI::GetElementRect( reg, canvas, e, viewRect, er ) )
            {
                sizeX = er.W * inv;
                sizeY = er.H * inv;
            }

            float axMin, axMax, oMinX, oMaxX, ayMin, ayMax, oMinY, oMaxY;
            AnchorAxisValues( hx, sizeX, axMin, axMax, oMinX, oMaxX );
            AnchorAxisValues( vy, sizeY, ayMin, ayMax, oMinY, oMaxY );
            L.AnchorMin = glm::vec2( axMin, ayMin );
            L.AnchorMax = glm::vec2( axMax, ayMax );
            L.OffsetMin = glm::vec2( oMinX, oMinY );
            L.OffsetMax = glm::vec2( oMaxX, oMaxY );
        }

        ImGuiMouseCursor CursorForHandle( UIHandle h )
        {
            switch ( h )
            {
                case UIHandle::L:
                case UIHandle::R:
                    return ImGuiMouseCursor_ResizeEW;
                case UIHandle::T:
                case UIHandle::B:
                    return ImGuiMouseCursor_ResizeNS;
                case UIHandle::TL:
                case UIHandle::BR:
                    return ImGuiMouseCursor_ResizeNWSE;
                case UIHandle::TR:
                case UIHandle::BL:
                    return ImGuiMouseCursor_ResizeNESW;
                case UIHandle::Body:
                    return ImGuiMouseCursor_ResizeAll;
                case UIHandle::AnchorMin:
                case UIHandle::AnchorMax:
                    return ImGuiMouseCursor_Hand;
                default:
                    return ImGuiMouseCursor_Arrow;
            }
        }
    } // namespace

    std::vector<ViewportPanel*> ViewportPanel::s_Live;

    size_t ViewportPanel::ViewIndex() const
    {
        if ( !m_Scene )
            return 0;
        if ( m_ViewRenderer == nullptr )
            return 0; // the primary viewport is view 0 for as long as the scene exists
        const auto index = m_Scene->IndexOfView( m_ViewRenderer );
        return index ? *index : m_Scene->GetViewCount(); // past the end == "this view is gone"
    }

    std::shared_ptr<::Desert::Core::Camera> ViewportPanel::ViewCamera() const
    {
        return m_Scene ? m_Scene->GetViewCamera( ViewIndex() ) : nullptr;
    }

    ViewportPanel::ViewportPanel( const std::shared_ptr<Desert::Core::Scene>& scene,
                                  const Assets::AssetManager* assetManager, std::string title,
                                  uint64_t sceneViewId, Graphic::SceneRenderer* viewRenderer )
         : IPanel( std::move( title ) ), m_Scene( scene ), m_SceneViewId( sceneViewId ),
           m_ViewRenderer( viewRenderer ), m_AuthoringOwner( Core::AuthoringOwner::ForSceneView( sceneViewId ) ),
           m_AssetManager( assetManager )
    {
        m_UIHelper = std::make_unique<Editor::UI::UIHelper>();
        m_UIHelper->Init();

        m_LightGizmoRenderer = std::make_unique<LightGizmoRenderer>( scene, m_UIHelper.get() );
        m_AsyncLoader        = std::make_unique<AsyncMeshLoader>(); // starts the background cook worker

        s_Live.push_back( this );
    }

    // Out-of-line so unique_ptr<AsyncMeshLoader> destroys with the complete type (joins the worker thread) —
    // and so the registry entry goes with the panel rather than outliving it as a dangling suppression.
    ViewportPanel::~ViewportPanel()
    {
        s_Live.erase( std::remove( s_Live.begin(), s_Live.end(), this ), s_Live.end() );

        // A CLOSED VIEW GIVES THE AUTHORING CONTEXT BACK. Ignoring the result is correct and the only
        // correct thing here: Release refuses when this view is not the holder, which is the ordinary case
        // (the user closed a viewport they were not authoring in) and not a fault. The refusal exists so
        // that the closing view cannot take bone authoring away from the surface the user IS working in.
        (void)Core::ActiveAuthoringContext().Release( m_AuthoringOwner );
    }

    Graphic::DebugViewState ViewportPanel::EffectiveDebugView( const Graphic::DebugViewState& user,
                                                               const Desert::Core::Scene&     scene )
    {
        // The modes of every viewport pointed at THIS scene, folded together. Two viewports on one scene
        // share a SceneRenderer and therefore share one answer; OR is the only defensible fold, because a
        // mode says "hide this from me" and the viewport that asked for it must get what it asked for.
        Core::ViewportModes modes;
        for ( const ViewportPanel* panel : s_Live )
            if ( panel->m_Scene.get() == &scene )
                modes.UI2D = modes.UI2D || panel->m_Modes.UI2D;

        return Core::ApplyViewportModes( user, modes );
    }

    void ViewportPanel::ToggleUIMode( const Desert::Core::Scene& scene )
    {
        for ( ViewportPanel* panel : s_Live )
            if ( panel->m_Scene.get() == &scene )
                panel->m_Modes.UI2D = !panel->m_Modes.UI2D;
    }

    void ViewportPanel::UpdateAsyncLoads()
    {
        if ( !m_AsyncLoader || !m_AssetManager )
            return;

        auto& mgr = const_cast<Assets::AssetManager&>( *m_AssetManager );
        for ( const auto& done : m_AsyncLoader->PollCompleted() )
        {
            // The cook finished on the worker -> the main-thread register is now fast (already cooked). Spawn
            // the matching component: a rigged source becomes a SkinnedMesh (+ Animation) so a character can be
            // animated; everything else stays a StaticMesh + gets the pack's sidecar material.
            const auto resolved = MeshDnD::ResolveOrImportMesh( mgr, done.SourcePath );
            if ( resolved.Handle.IsNull() )
                continue;
            if ( auto ref = m_Scene->FindEntityByID( Common::UUID( done.UserData ) ); ref )
            {
                ECS::Entity e = ref->get(); // Entity is a lightweight value handle -> copy to operate mutably
                if ( resolved.Skinned )
                {
                    // Swap the pending StaticMeshComponent for a skinned one + an Animator, so the rig renders
                    // and its clips can be picked in Details immediately.
                    if ( e.HasComponent<ECS::StaticMeshComponent>() )
                        e.RemoveComponent<ECS::StaticMeshComponent>();
                    if ( !e.HasComponent<ECS::SkinnedMeshComponent>() )
                        e.AddComponent<ECS::SkinnedMeshComponent>();
                    e.GetComponent<ECS::SkinnedMeshComponent>().MeshHandle = resolved.Handle;
                    if ( !e.HasComponent<ECS::AnimationComponent>() )
                        e.AddComponent<ECS::AnimationComponent>();
                }
                else
                {
                    if ( e.HasComponent<ECS::StaticMeshComponent>() )
                        e.GetComponent<ECS::StaticMeshComponent>().MeshHandle = resolved.Handle;
                    ApplySidecarMaterial( e, done.SourcePath );
                }
            }
        }

        // Progress overlay while background cooks are in flight (top-left of the viewport). Padded card:
        // roomy inner margins, a spinner-style title row, and a labelled bar so it reads as a polished toast.
        if ( m_AsyncLoader->IsBusy() )
        {
            ImGui::SetNextWindowBgAlpha( 0.90f );
            ImGui::SetNextWindowPos(
                 ImVec2( m_ViewportData.ViewportPos.x + 16.0f, m_ViewportData.ViewportPos.y + 16.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 18.0f, 14.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 8.0f );
            ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 1.0f );
            ImGui::Begin( "##AsyncLoadOverlay", nullptr,
                          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing );

            const int   done  = m_AsyncLoader->Done2();
            const int   total = m_AsyncLoader->Total();
            const float frac  = m_AsyncLoader->Progress();

            ImGui::TextColored( ImVec4( 0.55f, 0.78f, 1.0f, 1.0f ), ICON_MDI_PROGRESS_DOWNLOAD );
            ImGui::SameLine( 0.0f, 8.0f );
            ImGui::TextUnformatted( "Loading meshes" );

            ImGui::Dummy( ImVec2( 0.0f, 6.0f ) );

            char overlayText[32];
            std::snprintf( overlayText, sizeof( overlayText ), "%d / %d", done, total );
            ImGui::ProgressBar( frac, ImVec2( 240.0f, 14.0f ), overlayText );

            ImGui::End();
            ImGui::PopStyleVar( 3 );
        }
    }

    void ViewportPanel::ClaimAuthoringContext()
    {
        const auto&        selected = Core::SelectionManager::GetSelected();
        const Common::UUID entity   = selected.has_value() ? *selected : Common::UUID::Null();

        // A DIFFERENT CHARACTER IS A DIFFERENT CONTEXT. The bone index is an index into ONE skeleton, and
        // the global this replaced carried it across a change of selection — pointing at bone 47 of a rig
        // that may not have 47 bones. The mode survives, because "I am posing" is about the user, not the
        // entity; the selection does not.
        if ( !( m_Authoring.Entity == entity ) )
        {
            m_Authoring.Entity = entity;
            m_Authoring.SelectedBone.reset();
        }

        Core::ActiveAuthoringContext().Focus( m_AuthoringOwner, m_Authoring );
    }

    void ViewportPanel::TakeAuthoringContextIfFocused()
    {
        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) ||
             Core::ActiveAuthoringContext().Holder().IsNone() )
        {
            ClaimAuthoringContext();
        }
    }

    float ViewportPanel::ToolbarGearX()
    {
        return ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight() - 6.0f;
    }

    float ViewportPanel::ToolbarViewModeX()
    {
        return ToolbarGearX() - 160.0f;
    }

    float ViewportPanel::ToolbarRightClusterX()
    {
        return ToolbarViewModeX() - ImGui::GetFrameHeight() - 8.0f;
    }

    Common::BoolResultStr ViewportPanel::ModeAvailability( Core::AuthoringMode mode ) const
    {
        // WHAT EACH MODE NEEDS, AND THE SENTENCE THAT SAYS WHAT IS MISSING. Derived from the entity rather
        // than from a remembered flag, because the answer changes under the user: a component is added in
        // Details, a rig is pointed at a `.derig`, a different character is picked.
        if ( mode != Core::AuthoringMode::Object && Core::ViewportMode::Get() != Core::EditorMode::Select )
        {
            return Common::MakeError<bool>( "rig authoring needs Select mode; Foliage and Modeling "
                                            "give the left button to their own tools." );
        }
        if ( mode == Core::AuthoringMode::Object )
            return Common::MakeSuccess<bool>( true );

        const auto& sel = Core::SelectionManager::GetSelected();
        if ( !sel.has_value() )
            return Common::MakeError<bool>( "nothing is selected; a rig mode is about one character." );

        const auto ref = m_Scene->FindEntityByID( *sel );
        if ( !ref )
            return Common::MakeError<bool>( "the selected entity is not in this viewport's scene." );

        const ECS::Entity& entity = ref->get();
        if ( !entity.HasComponent<ECS::SkinnedMeshComponent>() )
            return Common::MakeError<bool>( "the selected entity has no Skinned Mesh, so it has no rig." );

        switch ( mode )
        {
            case Core::AuthoringMode::Skeleton:
                return Common::MakeSuccess<bool>( true );

            case Core::AuthoringMode::Pose:
                // THE GIZMO WRITES `Animator::SetBoneLocalPose` IN THIS MODE, and there is no animator
                // without the component. Offering it anyway would be a segment that lights up and moves
                // nothing — see GizmoController::RenderBone, which refuses for the same reason.
                if ( !entity.HasComponent<ECS::AnimationComponent>() )
                {
                    return Common::MakeError<bool>( "posing writes the ANIMATOR's pose buffer and this "
                                                    "entity has no Animation component." );
                }
                return Common::MakeSuccess<bool>( true );

            case Core::AuthoringMode::Control:
                if ( !entity.HasComponent<ECS::ControlRigComponent>() )
                {
                    return Common::MakeError<bool>( "this entity has no Control Rig component; add one in "
                                                    "Details and point it at a .derig." );
                }
                if ( !entity.HasComponent<ECS::AnimationComponent>() )
                {
                    return Common::MakeError<bool>( "a control rig is the last stage of the pose pipeline "
                                                    "and the pipeline lives in the Animation component." );
                }
                return Common::MakeSuccess<bool>( true );

            case Core::AuthoringMode::Object:
                break;
        }
        return Common::MakeSuccess<bool>( true );
    }

    void ViewportPanel::EnterAuthoringMode( Core::AuthoringMode mode )
    {
        // Pressing a segment IS the user working in this viewport, so take the context first — the ImGui
        // focus flag can still be describing the previous frame on the click itself.
        ClaimAuthoringContext();
        if ( const auto changed = Core::ActiveAuthoringContext().SetMode( m_AuthoringOwner, m_Authoring, mode );
             !changed )
        {
            LOG_WARN( "[Viewport] {} mode refused: {}", Core::AuthoringModeName( mode ), changed.GetError() );
        }
    }

    void ViewportPanel::DrawAuthoringModeSwitch()
    {
        // 07 §14.2. ONE control with four segments, not a toggle plus whatever else grew beside it: the
        // strip this replaced could say "Skeleton on" and had no way at all to say "Pose", because pose
        // authoring was a second bit only the Sequencer could write. The four states are one value now,
        // so the strip shows the value.
        //
        // ALWAYS DRAWN, and the unavailable segments are DISABLED rather than absent. A segment that
        // vanishes tells the user nothing; a greyed one with its own reason under the pointer tells them
        // what to add to the entity, which is the whole difference between a missing feature and a
        // missing component.
        auto& authoring = Core::ActiveAuthoringContext();

        static constexpr const char* kLabels[] = {
             ICON_MDI_CUBE_OUTLINE "  Object",
             ICON_MDI_BONE "  Skeleton",
             ICON_MDI_HUMAN_HANDSUP "  Pose",
             ICON_MDI_RHOMBUS_OUTLINE "  Control",
        };
        // THE SAME SEGMENTS WITHOUT THEIR WORDS, for a narrow viewport. MEASURED, not guessed: at the
        // editor's default layout on a 4112 px screen the Scene panel is about 1420 px wide and the four
        // written-out segments run off its right edge — "Control" was clipped to "Co", which is a mode
        // the user can neither read nor click. Dropping the words is what a toolbar can give up; dropping
        // a segment is not, because three of the four modes have no other door.
        static constexpr const char* kIcons[] = {
             ICON_MDI_CUBE_OUTLINE,
             ICON_MDI_BONE,
             ICON_MDI_HUMAN_HANDSUP,
             ICON_MDI_RHOMBUS_OUTLINE,
        };
        static_assert( std::size( kLabels ) == Core::kAuthoringModes.size() &&
                            std::size( kIcons ) == Core::kAuthoringModes.size(),
                       "every authoring mode needs a segment: the strip is the only way into three of "
                       "them, so a mode without one is a mode the user cannot reach" );

        ImGui::SameLine();
        ImGui::TextUnformatted( "|" );

        const ImGuiStyle& style  = ImGui::GetStyle();
        float             wanted = 0.0f;
        for ( const char* label : kLabels )
            wanted += ImGui::CalcTextSize( label ).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;

        // Measured AFTER the separator has been placed on the row, against the RIGHT-HAND CLUSTER's own
        // x and not against the panel's edge — that is the difference between "it fits in the window" and
        // "it fits before the eye button is painted over it", and the second is the one that is true.
        // SameLine() only moves the cursor; the loop's own SameLine() lands in the same place.
        ImGui::SameLine();
        const bool compact = ImGui::GetCursorPosX() + wanted > ToolbarRightClusterX();

        for ( std::size_t i = 0; i < Core::kAuthoringModes.size(); ++i )
        {
            const Core::AuthoringMode mode      = Core::kAuthoringModes[i];
            const auto                available = ModeAvailability( mode );
            const bool                active    = ( authoring.Mode() == mode );

            ImGui::SameLine();
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.85f, 0.45f, 0.1f, 1.0f ) );
            ImGui::BeginDisabled( !available );
            if ( ImGui::Button( compact ? kIcons[i] : kLabels[i] ) )
                EnterAuthoringMode( mode );
            ImGui::EndDisabled();
            if ( active )
                ImGui::PopStyleColor();

            // ImGuiHoveredFlags_AllowWhenDisabled: the tooltip on a DISABLED segment is the only place the
            // reason is ever shown, so suppressing it there would hide exactly the message that matters.
            // And in the compact strip it is the only place the mode's NAME is shown at all.
            if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            {
                ImGui::SetTooltip( "%s",
                                   available ? Core::AuthoringModeName( mode ) : available.GetError().c_str() );
            }
        }

        // Bone labels belong to the two modes that draw bones, and to no others.
        if ( authoring.ShowsBones() )
        {
            ImGui::SameLine();
            bool showNames = authoring.ShowBoneNames();
            if ( ImGui::Checkbox( "Names", &showNames ) )
            {
                ClaimAuthoringContext();
                if ( const auto changed = Core::ActiveAuthoringContext().SetShowBoneNames(
                          m_AuthoringOwner, m_Authoring, showNames );
                     !changed )
                {
                    LOG_WARN( "[Viewport] bone-name labels refused: {}", changed.GetError() );
                }
            }
        }
    }

    ViewportPanel* ViewportPanel::ActiveViewport()
    {
        // ── THE ONE AUTHORITY ON "WHERE THE USER IS", READ RATHER THAN COPIED ─────────────────────
        //
        // This used to answer "whoever holds the bone-AUTHORING context, else the first live one", and
        // those are two different questions: the authoring context is legitimately held by a Sequencer
        // document or by the Details bone tree, and while one of them held it this answer fell through
        // to `s_Live.front()` with focus never consulted at all — "Viewport Camera: Top" re-aimed a grid
        // pane while the user was driving the Scene pane.
        //
        // ImGui's own focus order is the answer and it is already maintained for us, so nothing is
        // stored here. ActiveViewportRule.hpp carries the rule (and the reason the most RECENT entry is
        // not enough: the palette that issued the command is focused, not a viewport).
        if ( s_Live.empty() )
            return nullptr;

        std::vector<std::string_view> live;
        live.reserve( s_Live.size() );
        for ( const ViewportPanel* panel : s_Live )
            live.emplace_back( panel->GetName() );

        std::vector<std::string_view> focusOrder;
        if ( const ImGuiContext* ctx = ImGui::GetCurrentContext() )
        {
            focusOrder.reserve( static_cast<size_t>( ctx->WindowsFocusOrder.Size ) );
            for ( const ImGuiWindow* window : ctx->WindowsFocusOrder )
                if ( window && window->Name )
                    focusOrder.emplace_back( window->Name );
        }

        const auto index = ActiveViewportIndex( live, focusOrder );
        return index ? s_Live[*index] : nullptr;
    }

    Common::BoolResultStr ViewportPanel::ApplyCameraPreset( ViewportCameraPreset preset )
    {
        const auto camera    = ViewCamera();
        auto*      editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( camera.get() );
        if ( !editorCam )
        {
            // NOT SILENT. ViewCamera() is empty when this panel's view has been closed under it, and the
            // camera is a GameplayCamera in Play mode — in both cases there is nothing here the user may
            // orbit, and over the control channel a silent success would read as an angle that was taken.
            return Common::MakeFormattedError<bool>(
                 "'{}' has no editor camera to aim (the view was closed, or the scene is playing).",
                 GetName() );
        }
        ApplyViewportCameraPreset( *editorCam, preset );
        return Common::MakeSuccess<bool>( true );
    }

    void ViewportPanel::DrawPilotOverlay()
    {
        m_PilotOverlayHovered = false;
        if ( !m_Pilot.IsActive() )
            return;

        // Where the preset caption normally sits, because it answers the same question — what is this
        // viewport looking through — and because the two must never be on screen together.
        const std::string caption = "Piloting: " + m_Pilot.EntityName();
        ImDrawList*       dl      = ::ImGui::GetWindowDrawList();
        const ImVec2      at( m_ViewportData.ViewportPos.x + 10.0f, m_ViewportData.ViewportPos.y + 8.0f );
        dl->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), IM_COL32( 0, 0, 0, 200 ), caption.c_str() );
        dl->AddText( at, IM_COL32( 255, 196, 64, 255 ), caption.c_str() );

        const ImVec2 textSize     = ::ImGui::CalcTextSize( caption.c_str() );
        const ImVec2 cursorBefore = ::ImGui::GetCursorScreenPos();
        ::ImGui::SetCursorScreenPos( ImVec2( at.x + textSize.x + 10.0f, at.y - 3.0f ) );
        if ( ::ImGui::SmallButton( ICON_MDI_EJECT " Eject" ) )
        {
            const auto camera = ViewCamera();
            m_Pilot.Eject( dynamic_cast<::Desert::Core::EditorCamera*>( camera.get() ) );
        }
        m_PilotOverlayHovered = ::ImGui::IsItemHovered();
        if ( m_PilotOverlayHovered )
            ::ImGui::SetTooltip( "Stop piloting: the viewport returns to where it was." );
        ::ImGui::SetCursorScreenPos( cursorBefore );
    }

    Common::BoolResultStr ViewportPanel::RequestPilot( const Common::UUID& entity )
    {
        ViewportPanel* target = ActiveViewport();
        if ( !target )
            return Common::MakeError<bool>( "there is no viewport to pilot with." );
        const auto camera    = target->ViewCamera();
        auto*      editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( camera.get() );
        if ( !editorCam || !target->m_Scene )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' has no editor camera to pilot with (the view was closed, or the scene is playing).",
                 target->GetName() );
        }
        return target->m_Pilot.Begin( *target->m_Scene, entity, *editorCam );
    }

    Common::BoolResultStr ViewportPanel::RequestEject()
    {
        bool any = false;
        for ( ViewportPanel* panel : s_Live )
        {
            if ( !panel->m_Pilot.IsActive() )
                continue;
            const auto camera = panel->ViewCamera();
            panel->m_Pilot.Eject( dynamic_cast<::Desert::Core::EditorCamera*>( camera.get() ) );
            any = true;
        }
        return any ? Common::MakeSuccess<bool>( true )
                   : Common::MakeError<bool>( "no viewport is piloting a camera." );
    }

    bool ViewportPanel::IsPilotingAnywhere( const Common::UUID& entity )
    {
        for ( const ViewportPanel* panel : s_Live )
            if ( panel->m_Pilot.Entity() && *panel->m_Pilot.Entity() == entity )
                return true;
        return false;
    }

    Common::BoolResultStr PilotCameraEntity( const Common::UUID& entity )
    {
        return ViewportPanel::RequestPilot( entity );
    }

    Common::BoolResultStr EjectPilot()
    {
        return ViewportPanel::RequestEject();
    }

    bool IsPiloted( const Common::UUID& entity )
    {
        return ViewportPanel::IsPilotingAnywhere( entity );
    }

    Common::BoolResultStr ViewportPanel::RequestCameraPreset( ViewportCameraPreset preset )
    {
        ViewportPanel* target = ActiveViewport();
        if ( !target )
            return Common::MakeError<bool>( "there is no viewport to aim." );
        return target->ApplyCameraPreset( preset );
    }

    Common::BoolResultStr ViewportPanel::SetCameraPreset( uint64_t sceneViewId, ViewportCameraPreset preset )
    {
        for ( ViewportPanel* panel : s_Live )
            if ( panel->m_SceneViewId == sceneViewId )
                return panel->ApplyCameraPreset( preset );
        return Common::MakeFormattedError<bool>( "no viewport is named {}.", sceneViewId );
    }

    Common::BoolResultStr ViewportPanel::RequestAuthoringMode( Core::AuthoringMode mode )
    {
        ViewportPanel* target = ActiveViewport();
        if ( !target )
            return Common::MakeError<bool>( "there is no viewport to put into a rig mode." );

        if ( const auto available = target->ModeAvailability( mode ); !available )
        {
            return Common::MakeFormattedError<bool>( "cannot enter {} mode: {}", Core::AuthoringModeName( mode ),
                                                     available.GetError() );
        }

        target->EnterAuthoringMode( mode );
        if ( Core::ActiveAuthoringContext().Mode() != mode )
        {
            // SAYS SO INSTEAD OF REPORTING A SUCCESS IT DID NOT HAVE. The write can be refused by the
            // election — another surface holds the context — and over the wire a silent success is
            // indistinguishable from the mode actually being entered.
            return Common::MakeFormattedError<bool>(
                 "the viewport could not take the authoring context; {} holds it.",
                 Core::ActiveAuthoringContext().Holder().Describe() );
        }
        return Common::MakeSuccess<bool>( true );
    }

    void ViewportPanel::DrawViewportToolbar()
    {
        // Godot-style strip directly ABOVE the image. Left cluster = how you EDIT (mode, transform
        // tools, snap, contextual skeleton toggle); right edge = the camera gear. Small icons —
        // the viewport pixels are the star, the tools are furniture.
        TakeAuthoringContextIfFocused();

        // THE MODE FALLS BACK WHEN WHAT IT NEEDS IS GONE, and the same predicate that greys a segment out
        // decides it — one derivation, so a mode can never be enterable by the strip and un-fallen-back by
        // this guard, or the other way round.
        //
        // ONLY WHILE THIS VIEW HOLDS THE CONTEXT. The guard is about what THIS viewport's selection can be
        // authored as; a Sequencer authoring its own character is not this view's business, and forcing it
        // back to Object from here is precisely the "everybody writes one global" shape that was removed —
        // SetMode refuses it for us rather than this call site having to remember.
        if ( !ModeAvailability( Core::ActiveAuthoringContext().Mode() ) )
            (void)Core::ActiveAuthoringContext().SetMode( m_AuthoringOwner, m_Authoring,
                                                          Core::AuthoringMode::Object );

        // ── 07 §1.3, FIXED HERE ───────────────────────────────────────────────────────────────────
        //
        // While the RIG is edited the mesh must be drawn in BIND pose, or a clip playing on the entity
        // overwrites every bone-gizmo edit before it can be seen. While a CLIP is authored the opposite is
        // true — the animator's pose buffer is what the gizmo writes and what the user is looking at — and
        // this asked `ShowsBones()`, which is true for both, so Pose mode got the rig's answer and the
        // edit under the cursor was hidden by the preview. `PreviewsBindPose()` is Skeleton alone, and it
        // has a name so the decision is asserted in a suite: this file is compiled by none.
        if ( Core::ActiveAuthoringContext().PreviewsBindPose() )
            Runtime::SelectionContext::SetBindPosePreview( Core::SelectionManager::GetSelected() );
        else
            Runtime::SelectionContext::SetBindPosePreview( std::nullopt );

        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 4.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4.0f, 4.0f ) );

        // Breathing room: the row must not sit flush against the panel's left wall.
        ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 6.0f );

        // The editor mode (Select / Modeling / Foliage) is chosen on the MAIN toolbar's mode rail
        // (EditorLayer::DrawToolbar) and nowhere else. A duplicate combo lived here and drove the same
        // Core::ViewportMode, so the two could never disagree — but two controls for one value is still
        // two places to look when the answer surprises you, and the rail is the one the mock keeps.
        // Removed rather than hidden: a control kept "just in case" is the legacy path this tree does
        // not carry.

        // --- Transform-tool toggles (GizmoState is the single source of truth; hotkeys mirror it) ---
        const auto opButton = [&]( const char* icon, Core::GizmoState::Operation op, const char* tip )
        {
            const bool active = Core::GizmoState::Get() == op;
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            if ( ImGui::Button( icon ) )
                Core::GizmoState::Set( op );
            if ( active )
                ImGui::PopStyleColor();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", tip );
            ImGui::SameLine();
        };
        opButton( ICON_MDI_CURSOR_DEFAULT_OUTLINE, Core::GizmoState::Operation::None, "Select (Esc)" );
        opButton( ICON_MDI_AXIS_ARROW, Core::GizmoState::Operation::Translate, "Move (T)" );
        opButton( ICON_MDI_ROTATE_ORBIT, Core::GizmoState::Operation::Rotate, "Rotate (R)" );
        opButton( ICON_MDI_ARROW_EXPAND_ALL, Core::GizmoState::Operation::Scale, "Scale (C)" );

        ImGui::TextDisabled( "|" );
        ImGui::SameLine();

        // --- Snap: magnet toggle (persistent) + right-click popup with the increments.
        // Ctrl during a drag temporarily inverts the toggle.
        {
            const bool snapOn = Core::GizmoState::PersistentSnap();
            if ( snapOn )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            if ( ImGui::Button( ICON_MDI_MAGNET "##SnapToggle" ) )
                Core::GizmoState::SetPersistentSnap( !snapOn );
            if ( snapOn )
                ImGui::PopStyleColor();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Snap %s (Ctrl inverts while dragging).\nRight-click: snap steps",
                                   snapOn ? "ON" : "OFF" );
            if ( ImGui::IsItemClicked( ImGuiMouseButton_Right ) )
                ImGui::OpenPopup( "##SnapSettings" );
            if ( ImGui::BeginPopup( "##SnapSettings" ) )
            {
                ImGui::TextUnformatted( "Snap steps" );
                ImGui::Separator();

                // THE OWNING FIELDS, EDITED IN PLACE. These three used to go through a local and a
                // GizmoState setter, back when GizmoState kept its own copy of the value — which is
                // why a step set here was live for the session and gone on the next launch. The
                // snap's one storage is EditorPreferences, so a drag writes it directly and the
                // gizmo follows on the same frame.
                //
                // A local would not work even now: DragFloat accumulates the drag IN the value it is
                // handed, so one re-seeded from the owner every frame never moves. And the save waits
                // for ImGui::IsItemDeactivatedAfterEdit() rather than riding the drag, because a
                // DragFloat reports a change on every frame the mouse moves and that would rewrite
                // editor.json (and log a line) sixty times a second.
                auto& snapPrefs = EditorPreferences::Get();

                ImGui::SetNextItemWidth( 130.0f );
                ImGui::DragFloat( "Move (cm)", &snapPrefs.TranslateSnap, 1.0f, 1.0f, 10000.0f, "%.1f" );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                    EditorPreferences::Save();

                ImGui::SetNextItemWidth( 130.0f );
                ImGui::DragFloat( "Rotate (deg)", &snapPrefs.RotateSnapDeg, 0.5f, 0.1f, 180.0f, "%.1f" );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                    EditorPreferences::Save();

                ImGui::SetNextItemWidth( 130.0f );
                ImGui::DragFloat( "Scale", &snapPrefs.ScaleSnap, 0.01f, 0.01f, 10.0f, "%.2f" );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                    EditorPreferences::Save();

                ImGui::EndPopup();
            }
        }

        // --- 07 §14.2: Object / Skeleton / Pose / Control, one switcher ---
        DrawAuthoringModeSwitch();

        // --- In-scene UI authoring (Godot/UE-style): create + parent UI elements without the UI Editor panel.
        // New elements parent under the selected UI element if one is selected, else under the canvas. ---
        {
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_VIEW_DASHBOARD "  UI" ) )
                ImGui::OpenPopup( "ui_create" );
            if ( ImGui::BeginPopup( "ui_create" ) )
            {
                auto& reg = m_Scene->GetRegistry();

                // WHICH CANVAS THE NEW ELEMENT GOES INTO. The selection answers it exactly when there is
                // one — an element names its own canvas — and only a scene with a single canvas has an
                // answer without it. With two canvases and nothing selected there is genuinely no answer,
                // and the menu says so rather than dropping the element into the first canvas, which is
                // what this did before and where it would then be invisible to the author who asked.
                // ONE derivation, shared with the prefab drop below (UIElementFactory.hpp).
                entt::entity canvas = entt::null;
                std::string  canvasRefusal;
                if ( const auto chosen = UICanvasForCreate( *m_Scene ) )
                {
                    canvas = chosen.GetValue();
                }
                else
                {
                    canvasRefusal = chosen.GetError();
                }

                if ( canvas == entt::null && ::Desert::UI::CanvasCount( reg ) == 0 )
                {
                    if ( ImGui::MenuItem( ICON_MDI_PLUS "  UI Canvas" ) )
                    {
                        // Through the factory so the creation is recorded: this used to build the
                        // entity inline and Ctrl+Z walked straight past it (see UIElementFactory.hpp).
                        if ( const entt::entity h = CreateUICanvas( *m_Scene ); h != entt::null )
                            Core::SelectionManager::SetSelected( reg.get<ECS::UUIDComponent>( h ).UUID );
                    }
                }
                else if ( canvas == entt::null )
                {
                    ImGui::TextDisabled( "Select an element of the canvas you want to add to" );
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "%s", canvasRefusal.c_str() );
                }
                else
                {
                    const entt::entity parent = UIParentForCreate( *m_Scene, canvas );
                    for ( std::size_t i = 0; i < kUIElementCount; ++i )
                    {
                        const UIElementEntry& entry = kUIElements[i];
                        const std::string     label = std::string( entry.Icon ) + "  " + entry.Label;
                        if ( ImGui::MenuItem( label.c_str() ) )
                        {
                            const entt::entity h = CreateUIElement( *m_Scene, parent, i );
                            if ( h != entt::null )
                                Core::SelectionManager::SetSelected( reg.get<ECS::UUIDComponent>( h ).UUID );
                        }
                    }

                    // An OVERLAY is not an element and is deliberately not in the element catalog: it is a
                    // CANVAS of its own, with its own Sort Order, and it is never a child of the canvas the
                    // rest of this menu adds to. Its own submenu, and each entry builds a working example
                    // (see CreateUIOverlay) rather than an empty rectangle with a component on it.
                    ImGui::Separator();
                    if ( ImGui::BeginMenu( ICON_MDI_LAYERS_OUTLINE "  Overlay" ) )
                    {
                        const std::array<std::pair<const char*, ECS::UIOverlayKind>, 4> kinds{ {
                             { ICON_MDI_TOOLTIP_TEXT_OUTLINE "  Tooltip", ECS::UIOverlayKind::Tooltip },
                             { ICON_MDI_MENU "  Context Menu", ECS::UIOverlayKind::ContextMenu },
                             { ICON_MDI_WINDOW_MAXIMIZE "  Modal Dialog", ECS::UIOverlayKind::Modal },
                             { ICON_MDI_BELL_OUTLINE "  Toast Stack", ECS::UIOverlayKind::Toast },
                        } };
                        for ( const auto& [label, kind] : kinds )
                            if ( ImGui::MenuItem( label ) )
                            {
                                const entt::entity h = CreateUIOverlay( *m_Scene, kind );
                                if ( h != entt::null )
                                    Core::SelectionManager::SetSelected( reg.get<ECS::UUIDComponent>( h ).UUID );
                            }
                        ImGui::EndMenu();
                    }
                }
                ImGui::EndPopup();
            }
        }

        // --- In-scene UI: anchor presets (Unity/UE RectTransform) + 2D toggle. Shown only when relevant so
        // the toolbar stays clean for pure-3D scenes. ---
        {
            // The row is about whether this scene DOES UI at all, so it counts canvases instead of electing
            // one — counting cannot pick a wrong winner, and two canvases must not read as one.
            auto&        reg       = m_Scene->GetRegistry();
            const bool   hasCanvas = ::Desert::UI::CanvasCount( reg ) > 0;
            entt::entity selUI     = entt::null;
            if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
                if ( auto ref = m_Scene->FindEntityByID( *sel ) )
                    if ( reg.has<ECS::UILayoutComponent>( ref->get().GetHandle() ) )
                        selUI = ref->get().GetHandle();

            const ::Desert::UI::Rect viewRect{ m_ViewportData.ViewportPos.x, m_ViewportData.ViewportPos.y,
                                               m_ViewportData.Size.x, m_ViewportData.Size.y };

            if ( selUI != entt::null && viewRect.W > 1.0f )
            {
                ImGui::SameLine();
                if ( ImGui::Button( ICON_MDI_ARROW_EXPAND_ALL "  Fill" ) )
                    ApplyAnchorPreset( reg, selUI, viewRect, AnchorAxis::Stretch, AnchorAxis::Stretch );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Stretch to fill the parent (anchors 0,0 - 1,1, offsets 0)" );

                ImGui::SameLine();
                if ( ImGui::Button( ICON_MDI_ANCHOR "  Anchors" ) )
                    ImGui::OpenPopup( "ui_anchors" );
                if ( ImGui::BeginPopup( "ui_anchors" ) )
                {
                    const AnchorAxis modes[4] = { AnchorAxis::Min, AnchorAxis::Center, AnchorAxis::Max,
                                                  AnchorAxis::Stretch };
                    const char*      cl[4]    = { "L", "C", "R", "<->" };
                    const char*      rl[4]    = { "T", "M", "B", "^v" };
                    ImGui::TextDisabled( "Anchor preset (keeps size; stretch fills the axis)" );
                    for ( int r = 0; r < 4; ++r )
                        for ( int c = 0; c < 4; ++c )
                        {
                            if ( c > 0 )
                                ImGui::SameLine();
                            const std::string lbl =
                                 std::string( rl[r] ) + cl[c] + "##a" + std::to_string( r * 4 + c );
                            if ( ImGui::Button( lbl.c_str(), ImVec2( 40.0f, 28.0f ) ) )
                            {
                                ApplyAnchorPreset( reg, selUI, viewRect, modes[c], modes[r] );
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    ImGui::EndPopup();
                }
            }

            // The toolbar row is shown for a scene that HAS a canvas — and also whenever 2D mode is
            // already on, whatever the scene holds. The palette can turn the mode on from outside
            // (ToggleUIMode) and a canvas can be deleted while the mode is running; without the second
            // condition either leaves a viewport in a mode whose only OFF switch has disappeared.
            if ( hasCanvas || m_Modes.UI2D )
            {
                ImGui::SameLine();
                // A MODE, AND IT TOUCHES NOTHING BUT ITSELF. What 2D mode hides is applied to a COPY of
                // the user's view state on its way to the renderer (EffectiveDebugView), so this toggle
                // writes no preference and saves no file — and no save anywhere else in the editor can
                // therefore carry "the grid is off" to disk on its behalf. It used to write
                // EditorPreferences::DebugView.ShowGrid = false and park the real answer in a member of
                // this panel; see Editor/Core/ViewportModes.hpp for why a fence around each Save() was
                // the wrong shape of fix.
                ImGui::Checkbox( "2D", &m_Modes.UI2D );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "2D UI mode: hide the grid + orientation gizmo" );

                // Design <-> Preview: Preview feeds the viewport mouse/keyboard into the canvas so buttons /
                // toggles / sliders react in the editor (like UMG preview); Design keeps drag/select/handles.
                ImGui::SameLine();
                const bool prevActive = m_UIPreview;
                if ( prevActive )
                    ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.85f, 0.45f, 0.1f, 1.0f ) );
                if ( ImGui::Button( m_UIPreview ? ICON_MDI_PLAY "  Preview"
                                                : ICON_MDI_CURSOR_DEFAULT_OUTLINE "  Design" ) )
                    m_UIPreview = !m_UIPreview;
                if ( prevActive )
                    ImGui::PopStyleColor();
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( m_UIPreview
                                            ? "Preview: UI is interactive (click buttons). Toggle for Design."
                                            : "Design: drag/select UI. Toggle for interactive Preview." );
            }
        }

        // --- Right edge: View Mode (UE-style) + editor camera gear ---
        // One dropdown driving the engine's existing debug visualizations behind a single control.
        // Each mode maps to the underlying flags; the current selection is derived back from them so
        // external edits stay in sync.
        //
        // IT EDITS THE USER'S VIEW STATE, NOT THE SCENE (К2). These flags used to be SceneSettings fields,
        // so choosing "Albedo" here dirtied the level and a Ctrl+S shipped it. They are
        // EditorPreferences::DebugView now, pushed to the renderer each frame by EditorLayer.
        //
        // AND IT IS NOW THE ONLY PLACE THEY ARE EDITED. Scene Settings used to carry a "Shadow Debug"
        // combo and a "Deferred Debug" combo of its own, offering Shadow Factor and GI, which this
        // dropdown did not — two controls over one piece of state, disagreeing about its vocabulary.
        // Those two entries were added here rather than dropped, so the panel could lose the combos
        // without losing a visualization.
        {
            // Nothing in this popup is scene data any more. `auto& s = m_Scene->GetSettings()` stood here
            // for Mesh LOD, "the one Show-popup entry that IS scene data" — К3 showed it was not.
            auto& view = EditorPreferences::Get().DebugView;
            enum ViewMode
            {
                VM_Lit,
                VM_Wireframe,
                VM_Normals,
                // К7: the entry that closes the OTHER half of a dead setting. `DebugView.LightingDebug` was
                // read all the way down to a twenty-five-line branch in three PBR shaders and written by no
                // widget anywhere, so the only way to see it was to edit editor.json by hand — working code
                // a person could not reach. §1.3 names the mirror case (a control that does nothing) and
                // not this one; they are the same gap between what the code does and what is available,
                // read from opposite ends.
                VM_Lighting,
                VM_Albedo,
                VM_Metallic,
                VM_Roughness,
                VM_AO,
                VM_GI,
                VM_LightComplexity,
                VM_Overdraw,
                VM_MaterialComplexity,
                VM_ShadowCascades,
                VM_ShadowFactor,
                VM_Count
            };
            const char* kViewModes[] = { ICON_MDI_LIGHTBULB_ON "  Lit",
                                         ICON_MDI_VECTOR_TRIANGLE "  Wireframe",
                                         ICON_MDI_AXIS_ARROW "  Normals",
                                         ICON_MDI_LIGHTBULB_MULTIPLE "  Lighting",
                                         ICON_MDI_PALETTE "  Albedo (Unlit)",
                                         ICON_MDI_CIRCLE_HALF_FULL "  Metallic",
                                         ICON_MDI_BLUR "  Roughness",
                                         ICON_MDI_WEATHER_NIGHT "  Ambient Occlusion",
                                         ICON_MDI_LIGHTBULB_OUTLINE "  Global Illumination",
                                         ICON_MDI_FIRE "  Light Complexity",
                                         ICON_MDI_LAYERS_TRIPLE "  Overdraw",
                                         ICON_MDI_TEXTURE "  Material Complexity",
                                         ICON_MDI_LAYERS "  Shadow Cascades",
                                         ICON_MDI_BRIGHTNESS_6 "  Shadow Factor" };
            static_assert( IM_ARRAYSIZE( kViewModes ) == VM_Count,
                           "every view mode needs a label - a short array silently truncates the combo" );

            // Derive the active mode from the current view state (last-wins order matches the enum).
            int vm = VM_Lit;
            if ( view.WireframeMode )
                vm = VM_Wireframe;
            else if ( view.LightingDebug )
                vm = VM_Lighting;
            else if ( view.ShadowDebug == Graphic::ShadowDebugMode::Cascades )
                vm = VM_ShadowCascades;
            else if ( view.ShadowDebug == Graphic::ShadowDebugMode::ShadowFactor )
                vm = VM_ShadowFactor;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::Albedo )
                vm = VM_Albedo;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::Normal )
                vm = VM_Normals;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::Metallic )
                vm = VM_Metallic;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::Roughness )
                vm = VM_Roughness;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::AO )
                vm = VM_AO;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::GI )
                vm = VM_GI;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::LightComplexity )
                vm = VM_LightComplexity;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::Overdraw )
                vm = VM_Overdraw;
            else if ( view.DeferredDebug == Graphic::DeferredDebugMode::MaterialComplexity )
                vm = VM_MaterialComplexity;
            else if ( view.ShowNormals )
                vm = VM_Normals;

            const float vmX = ToolbarViewModeX();

            // The "Show" flags. Every toggle here edits the USER's view state and is written to
            // editor.json on the spot — these are single clicks scattered through a session, and losing
            // them to a crash would be worse than the write (the same argument EditorPreferences makes for
            // its favourite-field and collapsed-section lists).
            //
            // "Mesh LOD (auto)" is the odd one out and stays on the SCENE: it is a machine-quality knob,
            // not a debug overlay, and it belongs to the group SceneSettings names as awaiting a decision
            // about who owns quality. It is drawn here because this is where a user looks for it, under a
            // separator that says which side of the fence it is on.
            ImGui::SameLine( ToolbarRightClusterX() );
            if ( ImGui::Button( ICON_MDI_EYE_OUTLINE "##DebugShowFlags" ) )
                ImGui::OpenPopup( "##DebugShowFlagsPopup" );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Show flags (grid, bounding boxes, colliders, wireframe, LOD)" );
            // Roomy padding + item spacing so the toggles aren't cramped against the popup border.
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12.0f, 10.0f ) );
            if ( ImGui::BeginPopup( "##DebugShowFlagsPopup" ) )
            {
                ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8.0f, 8.0f ) );
                ImGui::TextUnformatted( "Show" );
                ImGui::Separator();
                bool viewChanged = false;
                // THIS CHECKBOX IS THE USER'S ANSWER AND ALWAYS SHOWS IT, including while 2D UI mode is
                // hiding the grid — the suppression lives in the mode now and never in this field, so
                // there is nothing left for the control to lie about. It used to be DISABLED in 2D mode
                // and read a shadow copy, because the field underneath had been overwritten with the
                // suppression; a user in 2D mode therefore could not state a grid preference at all.
                // The note beside it is what explains an empty viewport with the box ticked.
                viewChanged |= ImGui::Checkbox( "Grid", &view.ShowGrid );
                if ( m_Modes.UI2D )
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled( "(hidden while 2D is on)" );
                }
                viewChanged |= ImGui::Checkbox( "Bounding Boxes", &view.ShowBoundingBoxes );
                ImGui::BeginDisabled( !view.ShowBoundingBoxes );
                viewChanged |= ImGui::ColorEdit3( "BB Color", &view.BoundingBoxColor.x );
                viewChanged |= ImGui::SliderFloat( "BB Width", &view.BoundingBoxLineWidth, 1.0f, 10.0f, "%.1f" );
                ImGui::EndDisabled();
                viewChanged |= ImGui::Checkbox( "Colliders", &view.ShowColliders );
                viewChanged |= ImGui::Checkbox( "Wireframe", &view.WireframeMode );
                // A PLAIN SAVE, and that is the deliverable of К10. This call used to be wrapped in three
                // lines that put the user's real grid answer back before writing and took it away again
                // afterwards, because 2D mode kept its suppression in the field being written. Eleven
                // other Save() call sites had no such wrapper and would each have made the suppression
                // permanent. There is nothing left to wrap: the struct always holds the user's answer.
                if ( viewChanged )
                    EditorPreferences::Save();

                ImGui::Separator();
                // "Scene" is what this heading said until К3, and it was wrong about the one control
                // under it: distance LOD is what a MACHINE can afford (LOD0 is byte-identical geometry
                // near the camera), not what the level is. It saves to the machine store on the click,
                // like the Show flags above save to editor.json.
                ImGui::TextDisabled( "This machine" );
                if ( ImGui::Checkbox( "Mesh LOD (auto)", &Common::Settings::MachineSettings::Get().MeshLOD ) )
                    Common::Settings::MachineSettings::Save();
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(); // WindowPadding (pushed unconditionally before BeginPopup)

            ImGui::SameLine( vmX );
            ImGui::SetNextItemWidth( 154.0f );
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 6.0f ) );
            if ( ImGui::Combo( "##ViewMode", &vm, kViewModes, IM_ARRAYSIZE( kViewModes ) ) )
            {
                // Reset every debug channel, then set the one this mode needs. LightingDebug is in the
                // list because it is a channel like the others: left out, "Lighting" would be the one mode
                // you could not leave, and the dropdown would keep reporting it whatever else was picked.
                view.WireframeMode = false;
                view.ShowNormals   = false;
                view.LightingDebug = false;
                view.DeferredDebug = Graphic::DeferredDebugMode::Off;
                view.ShadowDebug   = Graphic::ShadowDebugMode::Off;
                switch ( vm )
                {
                    case VM_Wireframe:
                        view.WireframeMode = true;
                        break;
                    case VM_Lighting:
                        // No DeferredDebug partner, unlike Normals below: the deferred path zeroes
                        // u_DebugParams, so SceneRenderer forces the FORWARD path while this is on — the
                        // same treatment Wireframe gets, for the same reason.
                        view.LightingDebug = true;
                        break;
                    case VM_Normals:
                        view.ShowNormals   = true;
                        view.DeferredDebug = Graphic::DeferredDebugMode::Normal;
                        break;
                    case VM_Albedo:
                        view.DeferredDebug = Graphic::DeferredDebugMode::Albedo;
                        break;
                    case VM_Metallic:
                        view.DeferredDebug = Graphic::DeferredDebugMode::Metallic;
                        break;
                    case VM_Roughness:
                        view.DeferredDebug = Graphic::DeferredDebugMode::Roughness;
                        break;
                    case VM_AO:
                        view.DeferredDebug = Graphic::DeferredDebugMode::AO;
                        break;
                    case VM_GI:
                        view.DeferredDebug = Graphic::DeferredDebugMode::GI;
                        break;
                    case VM_LightComplexity:
                        view.DeferredDebug = Graphic::DeferredDebugMode::LightComplexity;
                        break;
                    case VM_Overdraw:
                        view.DeferredDebug = Graphic::DeferredDebugMode::Overdraw;
                        break;
                    case VM_MaterialComplexity:
                        view.DeferredDebug = Graphic::DeferredDebugMode::MaterialComplexity;
                        break;
                    case VM_ShadowCascades:
                        view.ShadowDebug = Graphic::ShadowDebugMode::Cascades;
                        break;
                    case VM_ShadowFactor:
                        view.ShadowDebug = Graphic::ShadowDebugMode::ShadowFactor;
                        break;
                    default:
                        break; // VM_Lit
                }
                // No guard, for the same reason as the Show popup above: a viewport mode no longer has a
                // field in the struct this writes.
                EditorPreferences::Save();
            }
            ImGui::PopStyleVar();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Viewport view mode. Buffer views (Albedo/Metallic/Roughness/AO)\n"
                                   "need the Deferred render path; Wireframe and Lighting force\n"
                                   "the Forward one." );
        }

        // --- Right edge: editor camera settings (speed) behind a gear button ---
        ImGui::SameLine( ToolbarGearX() );
        if ( ImGui::Button( ICON_MDI_COG "##CamSettings" ) )
            ImGui::OpenPopup( "##EditorCameraSettings" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Editor camera settings" );
        // Roomy padding so the settings aren't cramped against the popup border.
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 14.0f, 12.0f ) );
        if ( ImGui::BeginPopup( "##EditorCameraSettings" ) )
        {
            ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8.0f, 8.0f ) );
            ImGui::TextUnformatted( "Editor Camera" );
            ImGui::Separator();
            if ( auto cam = ViewCamera() )
            {
                if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                {
                    constexpr float kW = 170.0f;

                    // ── THE NAMED ANGLE ───────────────────────────────────────────────────────────
                    //
                    // Perspective / Top / Bottom / Front / Back / Left / Right, which is UE's viewport
                    // type menu and the half of its four-up pattern that carries meaning. It sets the
                    // DIRECTION and the projection together; the Type combo below stays because an
                    // orthographic view from a user-chosen angle is a legitimate thing this does not
                    // cover, and it is the raw knob rather than a second copy of this one.
                    //
                    // THE PREVIEW IS DERIVED FROM THE CAMERA, never remembered: orbit one pixel off Top
                    // and it says "Ortho", because a label that keeps claiming "Top" after the camera
                    // has moved is worse than no label.
                    ImGui::SetNextItemWidth( kW );
                    if ( ImGui::BeginCombo( "View", ViewportCameraPresetLabel( *editorCam ) ) )
                    {
                        const auto current = PresetOfCamera( *editorCam );
                        for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
                        {
                            if ( ImGui::Selectable( row.Name, current && *current == row.Preset ) )
                                ApplyViewportCameraPreset( *editorCam, row.Preset );
                        }
                        ImGui::EndCombo();
                    }

                    // Projection type: Perspective / Orthographic (view is unchanged; only the projection).
                    const char* kTypes[] = { "Perspective", "Orthographic" };
                    int         type     = static_cast<int>( editorCam->GetProjectionType() );
                    ImGui::SetNextItemWidth( kW );
                    if ( ImGui::Combo( "Type", &type, kTypes, IM_ARRAYSIZE( kTypes ) ) )
                        editorCam->SetProjectionType( static_cast<::Desert::Core::ProjectionType>( type ) );

                    if ( editorCam->GetProjectionType() == ::Desert::Core::ProjectionType::Perspective )
                    {
                        float fov = editorCam->GetFOV();
                        ImGui::SetNextItemWidth( kW );
                        if ( ImGui::SliderFloat( "FOV", &fov, 20.0f, 120.0f, "%.0f" ) )
                            editorCam->SetFOV( fov );
                    }
                    else
                    {
                        // IN CENTIMETRES, AND IT COULD NOT REACH ITS OWN DEFAULT. The range was
                        // 1..100 against a Camera::m_OrthoSize default of 1000 — a leftover from the
                        // metre era (1 world unit = 1 cm project-wide). The slider was therefore pinned
                        // at its maximum the moment anyone opened it, and dragging it could only ever
                        // shrink the view to a 2 cm-tall sliver: an orthographic view was unusable
                        // through the one control that frames it. Logarithmic, because 10 cm and 100 m
                        // are both wanted and a linear slider spends 99 % of its travel above 1 km.
                        float size = editorCam->GetOrthoSize();
                        ImGui::SetNextItemWidth( kW );
                        if ( ImGui::SliderFloat( "Ortho Size", &size, 10.0f, 100000.0f, "%.0f cm",
                                                 ImGuiSliderFlags_Logarithmic ) )
                            editorCam->SetOrthoSize( size );
                    }

                    float nearP = editorCam->GetNear();
                    ImGui::SetNextItemWidth( kW );
                    if ( ImGui::SliderFloat( "Near", &nearP, 1.0f, 1000.0f, "%.0f cm" ) )
                        editorCam->SetNear( nearP );

                    float farP = editorCam->GetFar();
                    ImGui::SetNextItemWidth( kW );
                    // Up to 100 km: reversed-Z made the far plane cheap, and a slider that stopped at
                    // 5 km could not even reach the engine's own 50 km default (Core/Projection.hpp).
                    if ( ImGui::SliderFloat( "Far", &farP, 10000.0f, 10000000.0f, "%.0f cm" ) )
                        editorCam->SetFar( farP );

                    float spd = editorCam->GetMovementSpeed();
                    ImGui::SetNextItemWidth( kW );
                    if ( ImGui::SliderFloat( "Speed", &spd, 0.1f, 10.0f, "%.2fx" ) )
                        editorCam->SetMovementSpeed( spd );
                }
                else
                {
                    ImGui::TextDisabled( "(only in editor view, not Play)" );
                }
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(); // camera-settings WindowPadding

        ImGui::PopStyleVar( 2 );
    }

    void ViewportPanel::OnUIRender()
    {
        UpdateAsyncLoads(); // spawn any meshes whose background cook just finished (+ progress bar)

        const auto mainCamera = ViewCamera();
        if ( !mainCamera )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "Camera was not found" );
            ImGui::TextWrapped( "Please add a camera to the scene to display the view." );
            return;
        }

        // Godot-style: one compact toolbar ROW above the image (mode, transform tools, snap,
        // contextual skeleton toggle, camera gear on the right). Nothing floats over the scene
        // pixels anymore, so the picture is clean and picking never fights an overlay window.
        DrawViewportToolbar();

        // The viewport rect is whatever remains BELOW the toolbar row.
        const ImVec2 mousePos   = ::ImGui::GetMousePos();
        const ImVec2 imagePos   = ImGui::GetCursorScreenPos();
        ImVec2       imageAvail = ImGui::GetContentRegionAvail();
        imageAvail.x            = std::max( imageAvail.x, 1.0f );
        imageAvail.y            = std::max( imageAvail.y, 1.0f );

        m_ViewportData.ViewportPos   = { imagePos.x, imagePos.y };
        m_ViewportData.MousePosition = glm::vec2( mousePos.x - imagePos.x, mousePos.y - imagePos.y );
        const auto oldSize           = m_ViewportData.Size;

        m_ViewportData.Size      = { imageAvail.x, imageAvail.y };
        m_ViewportData.IsHovered = ::ImGui::IsWindowHovered();

        if ( oldSize != m_ViewportData.Size )
        {
            // Store the new size and apply it in OnPreUpdate() next frame, before any recording
            // starts. Calling Scene::Resize() here (inside OnUIRender) destroys descriptor set
            // pools while their DS are still bound to the recording command buffer.
            m_PendingViewportSize = m_ViewportData.Size;
            mainCamera->UpdateProjectionMatrix( m_ViewportData.Size.x,
                                                m_ViewportData.Size.y ); // TODO: Move to scene
        }

        m_ViewportData.IsHovered = ImGui::IsWindowHovered();

        // Multi-scene: focusing this viewport makes ITS scene the active one (Outliner/Details/gizmo
        // follow). Idempotent on the editor side, so calling it every focused frame is fine.
        if ( m_OnActivate && ImGui::IsWindowFocused() )
            m_OnActivate();

        // Feed the fly-camera its input gate: WASD/QE/arrows work while the viewport is hovered
        // and no text field owns the keyboard (otherwise typing "wasd" in a search box flies away).
        if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( mainCamera.get() ) )
        {
            editorCam->SetInputEnabled( m_ViewportData.IsHovered && !ImGui::GetIO().WantTextInput );
            // A modeling tool binds the bare Q/E (Push/Pull) and WASD, so hand it the keyboard: the camera
            // only flies while RMB is held, as in UE's Modeling Mode.
            editorCam->SetKeyboardRequiresLook( Core::ViewportMode::Get() == Core::EditorMode::Modeling &&
                                                Core::ModelingState::Get().ActiveTool !=
                                                     Core::ModelingState::Tool::None );

            // After the scene has ticked the camera with this frame's input: a pilot turns that motion
            // into the camera entity's transform and puts the entity's view back on the camera.
            m_Pilot.Update( *m_Scene, *editorCam );
        }

        // Render scene
        m_UIHelper->Image( m_Scene->GetFinalImage( ViewIndex() ),
                           { m_ViewportData.Size.x, m_ViewportData.Size.y } );

        // UI Preview vs Design. Preview: publish the viewport pointer/keyboard so the EditorUIPass drives the
        // canvas with real input (buttons interactive) and SKIP the authoring overlays/handles. Design: the
        // in-scene WYSIWYG handles (select marquee + drag/resize/anchor) as before.
        {
            auto& pv = ::Desert::Editor::Core::UIPreview::Get();
            if ( m_UIPreview )
            {
                const bool down = m_ViewportData.IsHovered && ImGui::IsMouseDown( ImGuiMouseButton_Left );
                pv.Enabled      = true;
                pv.HasInput     = m_ViewportData.IsHovered;
                pv.MousePx      = m_ViewportData.MousePosition;
                pv.DisplaySize  = m_ViewportData.Size;
                pv.Released     = pv.Down && !down; // down->up edge
                pv.Down         = down;
                pv.RightDown    = m_ViewportData.IsHovered && ImGui::IsMouseDown( ImGuiMouseButton_Right );
                pv.Escape       = ImGui::IsKeyPressed( ImGuiKey_Escape, false );
                pv.Scroll       = m_ViewportData.IsHovered ? ImGui::GetIO().MouseWheel : 0.0f;
                pv.Tab          = ImGui::IsKeyPressed( ImGuiKey_Tab, false );
                pv.Submit       = ImGui::IsKeyPressed( ImGuiKey_Enter, false );
                pv.Backspace    = ImGui::IsKeyPressed( ImGuiKey_Backspace, false );
                pv.TypedText.clear();
                for ( ImWchar c : ImGui::GetIO().InputQueueCharacters )
                    if ( c >= 32 && c < 128 ) // ASCII typed chars (matches the default font atlas)
                        pv.TypedText.push_back( static_cast<char>( c ) );
            }
            else
            {
                pv.Enabled = false;
                DrawUIInScene();
            }
        }

        // Drag a prefab file from the File Explorer onto the viewport to instantiate it into the scene.
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::PrefabFile ); payload && m_AssetManager )
            {
                const std::string path( static_cast<const char*>( payload->Data ),
                                        payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                auto& mgr = const_cast<Assets::AssetManager&>( *m_AssetManager );
                auto  prefab = mgr.FindByPath<Assets::PrefabAsset>( path );
                if ( !prefab )
                    prefab = mgr.CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High, path );
                if ( prefab )
                {
                    if ( !prefab->IsReadyForUse() )
                        prefab->Load();

                    // WHERE IT LANDS DEPENDS ON WHAT IT IS. A world prefab goes to the scene root, as it
                    // always did. A UI prefab dropped into the viewport must land inside the canvas the
                    // author is looking at — at the scene root it would be a correct, selectable,
                    // serialized tree covering zero pixels, which is the silent success PrefabPlacement
                    // exists to refuse. Same answer the "UI" create menu above uses, from the same two
                    // functions, so the drop and the menu cannot put things in different canvases.
                    ECS::Entity parentEntity;
                    if ( !prefab->GetEntities().empty() &&
                         Assets::ClassifyPrefabRoot( prefab->GetEntities().front() ) ==
                              Assets::PrefabRootKind::UIElement )
                    {
                        if ( const auto canvas = UICanvasForCreate( *m_Scene ) )
                        {
                            parentEntity = ECS::Entity{ UIParentForCreate( *m_Scene, canvas.GetValue() ),
                                                        m_Scene->GetRegistry() };
                        }
                    }

                    const auto placed =
                         prefab->Instantiate( m_Scene.get(), *m_AssetManager, parentEntity, nullptr );
                    if ( !placed )
                    {
                        LOG_ERROR( "{}", placed.GetError() );
                    }
                    else if ( ECS::Entity root = placed.GetValue() )
                    {
                        // A world prefab lands on the surface under the cursor; a UI prefab keeps the layout
                        // its canvas gives it.
                        const auto surface = SurfaceAtCursor();
                        if ( !parentEntity && surface && root.HasComponent<ECS::TransformComponent>() )
                            root.GetComponent<ECS::TransformComponent>().Translation = surface->Point;
                        Commands::NotifyCreated( { root.GetComponent<ECS::UUIDComponent>().UUID } );
                    }
                }
            }

            // Drag a mesh source (.obj/.fbx/.gltf/...) from the File Explorer onto the viewport to spawn it
            // as a new entity. Cooks the source on demand (see MeshDnD).
            if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MeshAsset ); payload && m_AssetManager )
            {
                const std::string path( static_cast<const char*>( payload->Data ),
                                        payload->DataSize > 0 ? payload->DataSize - 1 : 0 );

                // ASYNC spawn: create the (empty) entity NOW and cook the mesh on a worker thread so a heavy
                // FBX doesn't hitch the editor. UpdateAsyncLoads() assigns the mesh once the cook finishes.
                const std::string name = std::filesystem::path( path ).stem().string();
                auto&             e    = m_Scene->CreateNewEntity( std::string( name ) );
                e.AddComponent<ECS::StaticMeshComponent>(); // pending: no MeshHandle until the cook completes
                if ( const auto surface = SurfaceAtCursor() )
                    e.GetComponent<ECS::TransformComponent>().Translation = surface->Point;
                const auto uuid = e.GetComponent<ECS::UUIDComponent>().UUID;
                Core::SelectionManager::SetSelected( uuid );
                Commands::NotifyCreated( { uuid } ); // undo removes the pending entity; the async cook
                                                     // no-ops when its target entity is gone
                m_AsyncLoader->Request( path, static_cast<uint64_t>( uuid ) );
            }

            // Drag a material (.demat) onto the viewport: mouse-pick the mesh under the cursor and
            // assign the material to its elements (UE-style drop-on-object).
            if ( const ImGuiPayload* payload =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MaterialAsset );
                 payload && m_AssetManager )
            {
                const std::string path( static_cast<const char*>( payload->Data ),
                                        payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                AssignMaterialAtCursor( path );
            }

            // Drag a scene (.desce) onto the viewport to OPEN it — the document is replaced, so the load
            // itself is deferred to the editor (GPU teardown between frames, and it asks first when the
            // current scene has unsaved changes).
            if ( const ImGuiPayload* payload =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::SceneFile ) )
            {
                const std::string path( static_cast<const char*>( payload->Data ),
                                        payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                Core::SceneOpenRequest::Request( path );
            }
            ImGui::EndDragDropTarget();
        }

        const bool foliageMode  = Core::ViewportMode::Get() == Core::EditorMode::Foliage;
        const bool modelingMode = Core::ViewportMode::Get() == Core::EditorMode::Modeling;
        const bool landscapeMode = Core::ViewportMode::Get() == Core::EditorMode::Landscape;

        // UI elements are edited with the in-scene UILayout handles (DrawUIInScene), not the 3D transform
        // gizmo — suppress the object gizmo for them so the two don't overlap and fight for the mouse.
        bool selectedIsUI = false;
        if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
            if ( auto ref = m_Scene->FindEntityByID( *sel ) )
                selectedIsUI = m_Scene->GetRegistry().has<ECS::UILayoutComponent>( ref->get().GetHandle() );

        // Handle gizmos (Select mode only — Foliage/Modeling use LMB for their own tools, not gizmo/pick).
        m_Gizmo.ResetHovered();
        if ( !foliageMode && !modelingMode && !landscapeMode )
        {
            if ( Core::ActiveAuthoringContext().ShowsBones() )
            {
                // bone authoring owns the gizmo (edits the selected bone, not the object)
                m_Gizmo.RenderBone( *m_Scene, ViewCamera(), m_ViewportData.ViewportPos, m_ViewportData.Size );
            }
            else if ( m_Gizmo.IsActive() && !selectedIsUI )
            {
                m_Gizmo.RenderObject( *m_Scene, ViewCamera(), m_ViewportData.ViewportPos, m_ViewportData.Size );
            }
        }

        // --- Foliage paint mode: type panel + LMB scatter/erase (FoliagePaintTool) ---
        if ( foliageMode )
        {
            m_FoliageTool.DrawPanel( *m_Scene, m_AssetManager, m_ViewportData.ViewportPos );
            if ( m_ViewportData.IsHovered && Core::FoliagePaint::HasActive() &&
                 ImGui::IsMouseDown( ImGuiMouseButton_Left ) && !ImGui::IsAnyItemActive() )
            {
                if ( const auto camera = ViewCamera() )
                {
                    auto [mx, my]  = GetMouseViewportSpace();
                    const auto ray = Common::Math::Ray::FromScreenPosition(
                         { mx, my }, camera->GetProjectionMatrix(), camera->GetViewMatrix(),
                         camera->GetPosition(), static_cast<uint32_t>( m_ViewportData.Size.x ),
                         static_cast<uint32_t>( m_ViewportData.Size.y ) );
                    m_FoliageTool.Paint( *m_Scene, ray );
                }
            }
        }

        // --- Landscape mode: LMB strokes, Shift lowers (LandscapeSculptTool); its settings are the Landscape
        // panel ---
        if ( landscapeMode )
        {
            if ( const auto camera = ViewCamera() )
            {
                auto [mx, my]       = GetMouseViewportSpace();
                const auto width    = static_cast<uint32_t>( m_ViewportData.Size.x );
                const auto height   = static_cast<uint32_t>( m_ViewportData.Size.y );
                const auto mouseRay = Common::Math::Ray::FromScreenPosition(
                     { mx, my }, camera->GetProjectionMatrix(), camera->GetViewMatrix(), camera->GetPosition(),
                     width, height );
                const auto centreRay = Common::Math::Ray::FromScreenPosition(
                     { m_ViewportData.Size.x * 0.5f, m_ViewportData.Size.y * 0.5f }, camera->GetProjectionMatrix(),
                     camera->GetViewMatrix(), camera->GetPosition(), width, height );
                if ( Core::LandscapeSculptState::Get().Mode == Core::LandscapeEdMode::Paint )
                    m_LandscapePaintTool.Update( *m_Scene, mouseRay, centreRay, m_ViewportData.IsHovered );
                else
                    m_LandscapeTool.Update( *m_Scene, mouseRay, centreRay, m_ViewportData.IsHovered,
                                            ImGui::GetIO().DeltaTime );
            }
        }

        // --- Modeling mode: UE5-style CubeGrid blockout (add/remove grid cubes -> live DynamicMesh). ---
        if ( modelingMode )
        {
            if ( const auto camera = ViewCamera() )
            {
                auto [mx, my]  = GetMouseViewportSpace();
                const auto ray = Common::Math::Ray::FromScreenPosition(
                     { mx, my }, camera->GetProjectionMatrix(), camera->GetViewMatrix(), camera->GetPosition(),
                     static_cast<uint32_t>( m_ViewportData.Size.x ),
                     static_cast<uint32_t>( m_ViewportData.Size.y ) );
                const glm::mat4 viewProj = camera->GetProjectionMatrix() * camera->GetViewMatrix();
                m_CubeGridTool.Update( *m_Scene, ray, viewProj, m_ViewportData.ViewportPos, m_ViewportData.Size,
                                       m_ViewportData.IsHovered );
                m_PolyEditTool.Update( *m_Scene, ray, viewProj, m_ViewportData.ViewportPos, m_ViewportData.Size,
                                       m_ViewportData.IsHovered );
                m_ElementSelectTool.Update( *m_Scene, ray, viewProj, m_ViewportData.ViewportPos,
                                            m_ViewportData.Size, m_ViewportData.IsHovered );
                m_CreateShapeTool.Update( *m_Scene, ray, viewProj, m_ViewportData.ViewportPos, m_ViewportData.Size,
                                          m_ViewportData.IsHovered );
            }
        }

        // Editor gizmos (light/camera icons + frustums) are authoring aids — hide them in Play/Paused so the
        // running game view is clean.
        if ( m_Scene->GetState() == ::Desert::Core::Scene::SceneState::Edit )
            m_LightGizmoRenderer->Render( ViewCamera(), m_ViewportData.Size.x, m_ViewportData.Size.y,
                                          m_ViewportData.ViewportPos.x, m_ViewportData.ViewportPos.y,
                                          m_AuthoringOwner, m_Authoring );

        // ── WHICH PANE IS THIS ────────────────────────────────────────────────────────────────────
        //
        // The camera's angle, printed into the top-left corner of the image the way UE labels each of
        // its four panes. It is the whole reason a grid of viewports is legible: four pictures of one
        // scene from four directions are indistinguishable until each says which direction it is.
        //
        // DERIVED EVERY FRAME from the camera (ViewportCameraPreset.hpp), so orbiting off Top turns the
        // label into "Ortho" rather than leaving a claim the picture no longer supports. Hidden in 2D UI
        // mode with the triad, for the same reason: there is no world angle to name there.
        if ( !m_Modes.UI2D )
        {
            if ( const auto camera = ViewCamera() )
            {
                auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( camera.get() );
                if ( editorCam && !m_Pilot.IsActive() )
                {
                    const char* label = ViewportCameraPresetLabel( *editorCam );
                    const ImVec2 at( m_ViewportData.ViewportPos.x + 10.0f,
                                     m_ViewportData.ViewportPos.y + 8.0f );
                    ImDrawList*  dl = ::ImGui::GetWindowDrawList();
                    // One dark tap behind it, not a panel: a plate would occlude scene pixels in the one
                    // corner a modeller works in, and the label has to survive a white sky all the same.
                    dl->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), IM_COL32( 0, 0, 0, 200 ), label );
                    dl->AddText( at, IM_COL32( 235, 238, 245, 235 ), label );
                }
            }
        }

        DrawPilotOverlay();

        // Corner XYZ orientation triad — a 3D aid, so hide it in 2D UI mode (like Unity's 2D scene view).
        // THE MODEL THE GRID NOW FOLLOWS: the mode decides what it draws at the moment it draws it and
        // stores the decision nowhere, so no save can find it and no call site can forget to fence it.
        if ( !m_Modes.UI2D )
            DrawViewAxisGizmo( m_ViewportData.ViewportPos, m_ViewportData.Size );

        // Perf HUD (View -> Perf HUD): FPS + frame graph + top CPU scopes, useful in Play too.
        if ( EditorPreferences::Get().ShowPerfHud )
            m_PerfHud.Draw( ImVec2( m_ViewportData.ViewportPos.x, m_ViewportData.ViewportPos.y ),
                            ImVec2( m_ViewportData.ViewportPos.x + m_ViewportData.Size.x,
                                    m_ViewportData.ViewportPos.y + m_ViewportData.Size.y ) );
    }

    void ViewportPanel::OnPreUpdate()
    {
        if ( m_PendingViewportSize.has_value() )
        {
            m_Scene->ResizeView( ViewIndex(), (uint32_t)m_PendingViewportSize->x,
                                 (uint32_t)m_PendingViewportSize->y );
            m_PendingViewportSize.reset();
        }
    }

    std::pair<float, float> ViewportPanel::GetMouseViewportSpace() const
    {
        return { m_ViewportData.MousePosition.x, m_ViewportData.MousePosition.y };
    }

    void ViewportPanel::DrawUIInScene()
    {
        if ( !m_Scene )
            return;

        auto&                    reg = m_Scene->GetRegistry();
        const ::Desert::UI::Rect viewRect{ m_ViewportData.ViewportPos.x, m_ViewportData.ViewportPos.y,
                                           m_ViewportData.Size.x, m_ViewportData.Size.y };
        ImDrawList*              dl = ImGui::GetWindowDrawList();

        // The canvas CONTENT is drawn by the engine's own Render2D batcher (EditorUIPass, into the scene image
        // shown here) — no longer re-drawn with ImGui. This function now only adds the editor-authoring
        // overlays (canvas bounds, selection handles, anchor markers) on top of that image.

        // Canvas bounds outline so an EMPTY canvas (no Panel yet) is still visible + selectable — you can
        // see where it maps on screen. Dashed-ish subtle frame, drawn under the element handles.
        //
        // EVERY canvas, not the first: this frame is how an author sees where a canvas maps, and a second
        // canvas that is never outlined is a canvas the author cannot find. Enumerating is not electing.
        for ( const entt::entity canvas : reg.view<ECS::UICanvasComponent>() )
        {
            ::Desert::UI::Rect cr;
            if ( ::Desert::UI::GetElementRect( reg, canvas, canvas, viewRect, cr ) )
                dl->AddRect( ImVec2( cr.X, cr.Y ), ImVec2( cr.X + cr.W, cr.Y + cr.H ),
                             IM_COL32( 120, 135, 160, 170 ), 0.0f, 0, 1.5f );
        }

        // Editing handles only for a selected UI element.
        const auto& sel = Core::SelectionManager::GetSelected();
        if ( !sel.has_value() )
            return;
        auto ref = m_Scene->FindEntityByID( *sel );
        if ( !ref )
            return;
        const entt::entity e = ref->get().GetHandle();
        if ( !reg.has<ECS::UILayoutComponent>( e ) )
            return;

        // Everything below is about the selected element, so the canvas below is ITS canvas — the one it
        // actually lives in. Handles, anchors and snapping all resolve inside that tree.
        const entt::entity selCanvas = ::Desert::UI::CanvasOf( reg, e );

        // `r` is the element's rect BEFORE its render transform (the space its offsets are written in);
        // `xf` maps that rect onto the screen. Everything the author SEES has to go through xf, and
        // everything written BACK into UILayout has to come out of it — the two halves below.
        ::Desert::UI::Rect r;
        glm::mat3          xf( 1.0f );
        if ( !::Desert::UI::GetElementRect( reg, selCanvas, e, viewRect, r, &xf ) )
            return;

        namespace R2D = ::Desert::Graphic::Render2D;

        // A corner of `r` where it actually lands on screen.
        const auto OnScreen = [&xf]( float x, float y )
        {
            const glm::vec2 p = R2D::TransformPoint2D( xf, glm::vec2( x, y ) );
            return ImVec2( p.x, p.y );
        };
        // A screen point brought back into `r`'s space, for asking "is the cursor on this element".
        const auto ToLocal = [&xf]( const ImVec2& p )
        {
            const glm::vec2 q = R2D::TransformPoint2D( R2D::InverseTransform2D( xf ), glm::vec2( p.x, p.y ) );
            return ImVec2( q.x, q.y );
        };

        // Selection marquee — FOUR EDGES rather than an axis-aligned box, because a rotated element
        // whose marquee stayed square would be a selection the author cannot line up with what they see.
        {
            const ImVec2 c[4] = { OnScreen( r.X, r.Y ), OnScreen( r.X + r.W, r.Y ),
                                  OnScreen( r.X + r.W, r.Y + r.H ), OnScreen( r.X, r.Y + r.H ) };
            for ( int i = 0; i < 4; ++i )
                dl->AddLine( c[i], c[( i + 1 ) % 4], IM_COL32( 255, 170, 40, 255 ), 2.0f );
        }

        // Edit only in Select mode, over the viewport, and not while grabbing a 3D gizmo.
        const bool canEdit = Core::ViewportMode::Get() == Core::EditorMode::Select && m_ViewportData.IsHovered &&
                             !m_Gizmo.IsHovered();

        // 8 resize handles (corners + edge midpoints), screen px.
        const float cx = r.X + r.W * 0.5f, cy = r.Y + r.H * 0.5f;
        struct HandlePt
        {
            UIHandle Id;
            ImVec2   P;
        };
        const HandlePt handles[8] = {
             { UIHandle::TL, OnScreen( r.X, r.Y ) },
             { UIHandle::T, OnScreen( cx, r.Y ) },
             { UIHandle::TR, OnScreen( r.X + r.W, r.Y ) },
             { UIHandle::R, OnScreen( r.X + r.W, cy ) },
             { UIHandle::BR, OnScreen( r.X + r.W, r.Y + r.H ) },
             { UIHandle::B, OnScreen( cx, r.Y + r.H ) },
             { UIHandle::BL, OnScreen( r.X, r.Y + r.H ) },
             { UIHandle::L, OnScreen( r.X, cy ) },
        };
        const float hs = 4.0f; // half handle size

        if ( canEdit )
            for ( const auto& h : handles )
            {
                dl->AddRectFilled( ImVec2( h.P.x - hs, h.P.y - hs ), ImVec2( h.P.x + hs, h.P.y + hs ),
                                   IM_COL32( 255, 170, 40, 255 ) );
                dl->AddRect( ImVec2( h.P.x - hs, h.P.y - hs ), ImVec2( h.P.x + hs, h.P.y + hs ),
                             IM_COL32( 20, 20, 20, 255 ) );
            }

        const ImVec2 mouse  = ImGui::GetMousePos();
        const auto   scaleR = ::Desert::UI::CanvasScale( reg, selCanvas, viewRect );
        const float  scale  = std::max( 0.0001f, scaleR ? scaleR.GetValue() : 1.0f );

        // Parent rect: anchors are fractions of it. Draggable anchor markers let you re-anchor the element
        // (change how it pins to the parent) WITHOUT moving it — same idea as Unity's RectTransform anchors.
        const entt::entity parentE = reg.has<ECS::RelationshipComponent>( e )
                                          ? reg.get<ECS::RelationshipComponent>( e ).Parent
                                          : entt::null;
        ::Desert::UI::Rect pr;
        glm::mat3          pxf( 1.0f );
        const bool         haveParent =
             parentE != entt::null && ::Desert::UI::GetElementRect( reg, selCanvas, parentE, viewRect, pr, &pxf );

        // Anchors are fractions of the PARENT's rect, so their markers live in the parent's space and go
        // to the screen through the parent's transform — not this element's, which the anchors know
        // nothing about.
        const auto OnScreenP = [&pxf]( float x, float y )
        {
            const glm::vec2 p = R2D::TransformPoint2D( pxf, glm::vec2( x, y ) );
            return ImVec2( p.x, p.y );
        };
        const auto ToParent = [&pxf]( const ImVec2& p )
        {
            const glm::vec2 q = R2D::TransformPoint2D( R2D::InverseTransform2D( pxf ), glm::vec2( p.x, p.y ) );
            return ImVec2( q.x, q.y );
        };

        const auto&  aL    = reg.get<ECS::UILayoutComponent>( e ).Data;
        const ImVec2 aMinP = haveParent ? OnScreenP( pr.X + aL.AnchorMin.x * pr.W, pr.Y + aL.AnchorMin.y * pr.H )
                                        : ImVec2( 0.0f, 0.0f );
        const ImVec2 aMaxP = haveParent ? OnScreenP( pr.X + aL.AnchorMax.x * pr.W, pr.Y + aL.AnchorMax.y * pr.H )
                                        : ImVec2( 0.0f, 0.0f );
        const float  ar    = 5.0f;
        if ( canEdit && haveParent )
        {
            dl->AddCircleFilled( aMinP, ar, IM_COL32( 90, 200, 255, 235 ) );
            dl->AddCircle( aMinP, ar, IM_COL32( 15, 15, 15, 255 ) );
            dl->AddCircleFilled( aMaxP, ar, IM_COL32( 90, 200, 255, 235 ) );
            dl->AddCircle( aMaxP, ar, IM_COL32( 15, 15, 15, 255 ) );
        }

        // Handle under the cursor this frame (drives the cursor + starts a drag). Anchor markers first, then
        // the resize handles, then the body.
        UIHandle hovered = UIHandle::None;
        if ( canEdit && haveParent )
        {
            const float ah = ar + 2.0f;
            if ( std::abs( mouse.x - aMaxP.x ) <= ah && std::abs( mouse.y - aMaxP.y ) <= ah )
                hovered = UIHandle::AnchorMax;
            else if ( std::abs( mouse.x - aMinP.x ) <= ah && std::abs( mouse.y - aMinP.y ) <= ah )
                hovered = UIHandle::AnchorMin;
        }
        if ( hovered == UIHandle::None )
            for ( const auto& h : handles )
                if ( mouse.x >= h.P.x - hs - 1.0f && mouse.x <= h.P.x + hs + 1.0f &&
                     mouse.y >= h.P.y - hs - 1.0f && mouse.y <= h.P.y + hs + 1.0f )
                {
                    hovered = h.Id;
                    break;
                }
        // The body test is the one that has to be asked in the element's OWN space: the handles above are
        // screen points already, but "inside the rect" is a question about the rect, and a rotated
        // element whose body answered in screen space would be grabbable in a box beside itself.
        const ImVec2 mouseLocal = ToLocal( mouse );
        if ( hovered == UIHandle::None && mouseLocal.x >= r.X && mouseLocal.x <= r.X + r.W &&
             mouseLocal.y >= r.Y && mouseLocal.y <= r.Y + r.H )
            hovered = UIHandle::Body;

        if ( canEdit && m_UIDrag == UIHandle::None && hovered != UIHandle::None &&
             ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            m_UIDrag            = hovered;
            m_UIDragStartMouse  = glm::vec2( mouse.x, mouse.y );
            const auto& L       = reg.get<ECS::UILayoutComponent>( e ).Data;
            m_UIDragStartOffMin = L.OffsetMin;
            m_UIDragStartOffMax = L.OffsetMax;
            m_UIDragStartRect   = glm::vec4( r.X, r.Y, r.X + r.W, r.Y + r.H ); // edges to preserve on re-anchor
        }

        // Dragging an anchor marker re-anchors the element (changes AnchorMin/Max) while KEEPING its on-screen
        // rect — offsets are recomputed to hold the preserved edges. Anchors snap to 0/0.5/1 (Alt disables).
        if ( m_UIDrag == UIHandle::AnchorMin || m_UIDrag == UIHandle::AnchorMax )
        {
            if ( ImGui::IsMouseDown( ImGuiMouseButton_Left ) && haveParent && pr.W > 0.0f && pr.H > 0.0f )
            {
                auto&        L  = reg.get<ECS::UILayoutComponent>( e ).Data;
                const ImVec2 mp = ToParent( mouse ); // anchors are fractions of the parent's own rect
                float        fx = std::clamp( ( mp.x - pr.X ) / pr.W, 0.0f, 1.0f );
                float        fy = std::clamp( ( mp.y - pr.Y ) / pr.H, 0.0f, 1.0f );
                if ( !ImGui::GetIO().KeyAlt )
                    for ( float s : { 0.0f, 0.5f, 1.0f } )
                    {
                        if ( std::abs( fx - s ) < 0.03f )
                            fx = s;
                        if ( std::abs( fy - s ) < 0.03f )
                            fy = s;
                    }
                if ( m_UIDrag == UIHandle::AnchorMin )
                {
                    fx          = std::min( fx, L.AnchorMax.x );
                    fy          = std::min( fy, L.AnchorMax.y );
                    L.OffsetMin = glm::vec2( ( m_UIDragStartRect.x - pr.X - fx * pr.W ) / scale,
                                             ( m_UIDragStartRect.y - pr.Y - fy * pr.H ) / scale );
                    L.AnchorMin = glm::vec2( fx, fy );
                }
                else
                {
                    fx          = std::max( fx, L.AnchorMin.x );
                    fy          = std::max( fy, L.AnchorMin.y );
                    L.OffsetMax = glm::vec2( ( m_UIDragStartRect.z - pr.X - fx * pr.W ) / scale,
                                             ( m_UIDragStartRect.w - pr.Y - fy * pr.H ) / scale );
                    L.AnchorMax = glm::vec2( fx, fy );
                }
            }
            ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
            if ( ImGui::IsMouseReleased( ImGuiMouseButton_Left ) )
                m_UIDrag = UIHandle::None;
        }
        else if ( m_UIDrag != UIHandle::None )
        {
            if ( ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                // THE DELTA IS A SCREEN DELTA AND THE OFFSETS ARE NOT, so it is brought back through the
                // linear part of a transform — and WHICH transform differs by handle, which is the whole
                // reason this is two lines and not one:
                //   * Body moves the rect inside its PARENT, and a rect translated by d in the parent's
                //     space appears translated by pxf*d on screen (the pivot travels with it, so the
                //     element's own rotation contributes nothing);
                //   * a resize handle moves an EDGE of the rect, which the element's own transform then
                //     turns as well — so that one comes back through xf.
                // Both are the identity for an untransformed element, which is why this reads the same as
                // it always did for every scene that does not rotate anything.
                const glm::mat3 undo    = R2D::InverseTransform2D( m_UIDrag == UIHandle::Body ? pxf : xf );
                const glm::vec2 screenD = glm::vec2( mouse.x, mouse.y ) - m_UIDragStartMouse;
                glm::vec2       d       = glm::vec2( undo[0].x * screenD.x + undo[1].x * screenD.y,
                                                     undo[0].y * screenD.x + undo[1].y * screenD.y ) /
                              scale; // design px, in the space the offsets are written in
                if ( ImGui::GetIO().KeyShift && m_UIDrag == UIHandle::Body ) // Shift = lock the dominant axis
                {
                    if ( std::abs( d.x ) >= std::abs( d.y ) )
                        d.y = 0.0f;
                    else
                        d.x = 0.0f;
                }

                const bool eL = m_UIDrag == UIHandle::L || m_UIDrag == UIHandle::TL || m_UIDrag == UIHandle::BL;
                const bool eR = m_UIDrag == UIHandle::R || m_UIDrag == UIHandle::TR || m_UIDrag == UIHandle::BR;
                const bool eT = m_UIDrag == UIHandle::T || m_UIDrag == UIHandle::TL || m_UIDrag == UIHandle::TR;
                const bool eB = m_UIDrag == UIHandle::B || m_UIDrag == UIHandle::BL || m_UIDrag == UIHandle::BR;

                glm::vec2 oMin = m_UIDragStartOffMin, oMax = m_UIDragStartOffMax;
                if ( m_UIDrag == UIHandle::Body )
                {
                    oMin += d; // move: shift both edges so the element slides, whatever its anchors
                    oMax += d;
                }
                else
                {
                    if ( eL )
                        oMin.x += d.x;
                    if ( eR )
                        oMax.x += d.x;
                    if ( eT )
                        oMin.y += d.y;
                    if ( eB )
                        oMax.y += d.y;
                }

                // Snapping: align the element's edges/centre to the parent's edges/centre (hold Alt to
                // disable). Computed in screen space, the winning snap is converted back to design offsets,
                // and a cyan guide line is drawn. The dragged edge(s) / whole box shift onto the guide.
                if ( !ImGui::GetIO().KeyAlt )
                {
                    const entt::entity parent = reg.has<ECS::RelationshipComponent>( e )
                                                     ? reg.get<ECS::RelationshipComponent>( e ).Parent
                                                     : entt::null;
                    ::Desert::UI::Rect pr;
                    if ( parent != entt::null &&
                         ::Desert::UI::GetElementRect( reg, selCanvas, parent, viewRect, pr ) )
                    {
                        const auto& L0     = reg.get<ECS::UILayoutComponent>( e ).Data;
                        const float eLeft  = pr.X + L0.AnchorMin.x * pr.W + oMin.x * scale;
                        const float eRight = pr.X + L0.AnchorMax.x * pr.W + oMax.x * scale;
                        const float eTop   = pr.Y + L0.AnchorMin.y * pr.H + oMin.y * scale;
                        const float eBot   = pr.Y + L0.AnchorMax.y * pr.H + oMax.y * scale;
                        const float gx[3]  = { pr.X, pr.X + pr.W * 0.5f, pr.X + pr.W };
                        const float gy[3]  = { pr.Y, pr.Y + pr.H * 0.5f, pr.Y + pr.H };
                        const float thr    = 6.0f;
                        const bool  move   = m_UIDrag == UIHandle::Body;

                        struct Cand
                        {
                            float Pos;
                            bool  Min;
                            bool  Max;
                        };

                        Cand xs[3];
                        int  nx = 0;
                        if ( move )
                        {
                            xs[nx++] = { eLeft, true, true };
                            xs[nx++] = { ( eLeft + eRight ) * 0.5f, true, true };
                            xs[nx++] = { eRight, true, true };
                        }
                        else
                        {
                            if ( eL )
                                xs[nx++] = { eLeft, true, false };
                            if ( eR )
                                xs[nx++] = { eRight, false, true };
                        }
                        float bestX = thr, gXpos = 0.0f, addX = 0.0f;
                        bool  hitX = false, minX = false, maxX = false;
                        for ( int i = 0; i < nx; ++i )
                            for ( float g : gx )
                                if ( std::abs( xs[i].Pos - g ) < bestX )
                                {
                                    bestX = std::abs( xs[i].Pos - g );
                                    addX  = ( g - xs[i].Pos ) / scale;
                                    minX  = xs[i].Min;
                                    maxX  = xs[i].Max;
                                    gXpos = g;
                                    hitX  = true;
                                }
                        if ( hitX )
                        {
                            if ( minX )
                                oMin.x += addX;
                            if ( maxX )
                                oMax.x += addX;
                            // The guide is a line in the PARENT's space (that is where gXpos and the
                            // element's edges were compared), so it reaches the screen the same way.
                            dl->AddLine( OnScreenP( gXpos, r.Y - 40.0f ), OnScreenP( gXpos, r.Y + r.H + 40.0f ),
                                         IM_COL32( 90, 200, 255, 200 ), 1.0f );
                        }

                        Cand ys[3];
                        int  ny = 0;
                        if ( move )
                        {
                            ys[ny++] = { eTop, true, true };
                            ys[ny++] = { ( eTop + eBot ) * 0.5f, true, true };
                            ys[ny++] = { eBot, true, true };
                        }
                        else
                        {
                            if ( eT )
                                ys[ny++] = { eTop, true, false };
                            if ( eB )
                                ys[ny++] = { eBot, false, true };
                        }
                        float bestY = thr, gYpos = 0.0f, addY = 0.0f;
                        bool  hitY = false, minY = false, maxY = false;
                        for ( int i = 0; i < ny; ++i )
                            for ( float g : gy )
                                if ( std::abs( ys[i].Pos - g ) < bestY )
                                {
                                    bestY = std::abs( ys[i].Pos - g );
                                    addY  = ( g - ys[i].Pos ) / scale;
                                    minY  = ys[i].Min;
                                    maxY  = ys[i].Max;
                                    gYpos = g;
                                    hitY  = true;
                                }
                        if ( hitY )
                        {
                            if ( minY )
                                oMin.y += addY;
                            if ( maxY )
                                oMax.y += addY;
                            dl->AddLine( OnScreenP( r.X - 40.0f, gYpos ), OnScreenP( r.X + r.W + 40.0f, gYpos ),
                                         IM_COL32( 90, 200, 255, 200 ), 1.0f );
                        }
                    }
                }

                auto& L     = reg.get<ECS::UILayoutComponent>( e ).Data;
                L.OffsetMin = oMin;
                L.OffsetMax = oMax;
            }
            ImGui::SetMouseCursor( CursorForHandle( m_UIDrag ) );
            if ( ImGui::IsMouseReleased( ImGuiMouseButton_Left ) )
                m_UIDrag = UIHandle::None;
        }
        else if ( canEdit && hovered != UIHandle::None )
        {
            ImGui::SetMouseCursor( CursorForHandle( hovered ) );
        }
    }

    void ViewportPanel::DrawViewAxisGizmo( const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        const auto camera = ViewCamera();
        if ( !camera )
            return;

        // Rotation-only part of the view matrix maps world directions into view space (x=right, y=up,
        // z=toward-viewer). We only need orientation, so drop translation.
        const glm::mat3 viewRot = glm::mat3( camera->GetViewMatrix() );

        const float  radius = 34.0f;
        const ImVec2 center( viewportPos.x + viewportSize.x - radius - 18.0f, viewportPos.y + radius + 18.0f );
        ImDrawList*  dl = ::ImGui::GetWindowDrawList();

        struct Axis
        {
            glm::vec3   Dir;
            ImU32       Color;
            const char* Label;
        };
        const Axis axes[3] = {
            { { 1.0f, 0.0f, 0.0f }, IM_COL32( 232, 88, 88, 255 ), "X" },
            { { 0.0f, 1.0f, 0.0f }, IM_COL32( 120, 208, 96, 255 ), "Y" },
            { { 0.0f, 0.0f, 1.0f }, IM_COL32( 92, 152, 240, 255 ), "Z" },
        };

        // One drawable tip per axis END (+ and -). Sort back-to-front by view-space depth so the nearer
        // axis ends overpaint the farther ones (a readable 3D triad instead of flat crossing lines).
        struct Tip
        {
            ImVec2      Pos;
            float       Depth;
            ImU32       Color;
            bool        Positive;
            const char* Label;
            glm::vec3   WorldDir; // the world-space axis end this tip represents (a.Dir * s)
        };
        std::array<Tip, 6> tips2;
        int                n = 0;
        for ( const Axis& a : axes )
            for ( float s : { 1.0f, -1.0f } )
            {
                const glm::vec3 v = viewRot * ( a.Dir * s );
                tips2[n++] = Tip{ ImVec2( center.x + v.x * radius, center.y - v.y * radius ),
                                  v.z,
                                  a.Color,
                                  s > 0.0f,
                                  a.Label,
                                  a.Dir * s };
            }
        std::sort( tips2.begin(), tips2.end(), []( const Tip& l, const Tip& r ) { return l.Depth < r.Depth; } );

        // Clickable: a tip under the cursor snaps the editor camera to view FROM that axis end (forward =
        // -worldDir). Hover state suppresses picking (see OnMousePressed). Nearest-to-cursor tip wins.
        const ImVec2 mouse = ::ImGui::GetMousePos();
        auto*        editorCam =
             m_Scene ? dynamic_cast<::Desert::Core::EditorCamera*>( camera.get() ) : nullptr;
        const float  dxg = mouse.x - center.x, dyg = mouse.y - center.y;
        m_ViewAxisGizmoHovered = editorCam && ( dxg * dxg + dyg * dyg ) <= ( radius + 8.0f ) * ( radius + 8.0f );

        int   hotTip  = -1;
        float hotDist = 11.0f; // click/hover radius around a tip in pixels
        for ( int i = 0; i < n; ++i )
        {
            const float dx = mouse.x - tips2[i].Pos.x, dy = mouse.y - tips2[i].Pos.y;
            const float d  = std::sqrt( dx * dx + dy * dy );
            if ( d < hotDist )
            {
                hotDist = d;
                hotTip  = i;
            }
        }

        dl->AddCircleFilled( center, radius + 8.0f, IM_COL32( 20, 20, 24, 130 ) );
        for ( int i = 0; i < n; ++i )
        {
            const Tip& t   = tips2[i];
            const bool hot = ( i == hotTip ) && editorCam;
            if ( t.Positive )
            {
                dl->AddLine( center, t.Pos, t.Color, 2.0f );
                dl->AddCircleFilled( t.Pos, hot ? 10.0f : 8.0f, t.Color );
                if ( hot )
                    dl->AddCircle( t.Pos, 10.0f, IM_COL32( 255, 255, 255, 220 ), 0, 2.0f );
                const ImVec2 ts = ::ImGui::CalcTextSize( t.Label );
                dl->AddText( ImVec2( t.Pos.x - ts.x * 0.5f, t.Pos.y - ts.y * 0.5f ), IM_COL32( 15, 15, 18, 255 ),
                             t.Label );
            }
            else
            {
                // Negative ends: hollow dot, no label — reads as "the back of the axis".
                dl->AddCircleFilled( t.Pos, hot ? 8.0f : 6.0f, IM_COL32( 40, 40, 46, 200 ) );
                dl->AddCircle( t.Pos, hot ? 8.0f : 6.0f, hot ? IM_COL32( 255, 255, 255, 220 ) : t.Color, 0,
                               1.6f );
            }
        }

        if ( hotTip >= 0 && editorCam && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            editorCam->SnapToDirection( -tips2[hotTip].WorldDir );
    }

    void ViewportPanel::OnEvent( Common::Event& e )
    {
        // NO EventWindowResize SUBSCRIPTION. There was one, and it called an `OnWindowResize` whose whole
        // body was two commented-out lines naming members this class does not have (`m_ImGuiLayer`,
        // `m_EditorCamera`) and a `return false`. A viewport takes its size from its ImGui window, not
        // from the OS window; the handler and the subscription are both gone rather than left looking
        // like the resize is being handled somewhere.
        Common::EventManager eventManager( e );
        eventManager.Notify<Common::MouseButtonPressedEvent>( [this]( Common::MouseButtonPressedEvent& e )
                                                              { return OnMousePressed( e ); } );

        eventManager.Notify<Common::KeyPressedEvent>( [this]( Common::KeyPressedEvent& e )
                                                      { return OnKeyPressedEvent( e ); } );
    }

    bool ViewportPanel::OnMousePressed( Common::MouseButtonPressedEvent& e )
    {
        // LMB picks/selects ONLY in Select mode and when no brush is active (terrain brush / Foliage paint
        // both consume LMB in OnUIRender instead).
        // A click on the corner view-axis gizmo snaps the camera (handled in DrawViewAxisGizmo) — don't also
        // pick the object behind it.
        if ( m_ViewAxisGizmoHovered || m_PilotOverlayHovered )
            return false;

        // The pick must fire ONLY over the rendered scene image — not the toolbar/overlay widgets that sit
        // in the same viewport window. IsHovered (IsWindowHovered) is true for the whole window, so a click
        // on e.g. the "Skeleton" toolbar button used to leak here, Raycast-miss, and clear the selection.
        const ImVec2 mp        = ::ImGui::GetMousePos();
        const auto&  vp        = m_ViewportData;
        const bool   overImage = mp.x >= vp.ViewportPos.x && mp.y >= vp.ViewportPos.y &&
                                 mp.x < vp.ViewportPos.x + vp.Size.x && mp.y < vp.ViewportPos.y + vp.Size.y;

        if ( e.GetMouseButton() == Common::MouseButton::Left && !m_UIPreview &&
             Core::ViewportMode::Get() == Core::EditorMode::Select && m_ViewportData.IsHovered && overImage )
        {
            // In-scene UI: the 2D canvas overlays the 3D scene, so a click on a UI element selects it and
            // skips the 3D raycast (unless a 3D gizmo handle is being grabbed). Edit anchors/colour in Details.
            if ( !m_Gizmo.IsHovered() )
            {
                auto&                    reg = m_Scene->GetRegistry();
                const ::Desert::UI::Rect viewRect{ vp.ViewportPos.x, vp.ViewportPos.y, vp.Size.x, vp.Size.y };

                // Ask EVERY canvas IN DRAW ORDER and keep the last hit, because the last canvas drawn is
                // the one on top — the same "last writer wins" rule the renderer's own hot election uses.
                // Picking through only the first canvas is what made an overlay canvas unselectable in the
                // viewport while it was plainly on screen.
                //
                // THE ORDER HAS TO BE THE RENDERER'S, and `reg.view<>()` is not it: this loop used to walk
                // the component pool, whose order is the REVERSE of creation and knows nothing of the
                // canvas's authored Sort Order (Ю4). Two canvases overlapping at the cursor would then
                // select the one the renderer had drawn UNDERNEATH — a click landing on the element that
                // is not the one under the pointer. UI::CanvasesInDrawOrder is the single answer both
                // halves ask, and Desert/Tests/Engine/UICanvasContext asserts they agree.
                entt::entity uiHit = entt::null;
                for ( const entt::entity canvas : ::Desert::UI::CanvasesInDrawOrder( reg ) )
                    if ( const entt::entity hit =
                              ::Desert::UI::PickElement( reg, canvas, glm::vec2( mp.x, mp.y ), viewRect );
                         hit != entt::null )
                        uiHit = hit;

                // Clicking a control's content (e.g. a button's label / icon) selects the CONTROL, not the
                // child — promote the hit to its nearest interactable ancestor. Alt-click drills down to the
                // exact element under the cursor instead.
                if ( uiHit != entt::null && !::ImGui::GetIO().KeyAlt )
                {
                    auto isInteractable = [&]( entt::entity x )
                    {
                        return reg.has<ECS::UIButtonComponent>( x ) || reg.has<ECS::UIToggleComponent>( x ) ||
                               reg.has<ECS::UISliderComponent>( x ) || reg.has<ECS::UIDropdownComponent>( x ) ||
                               reg.has<ECS::UIInputFieldComponent>( x );
                    };
                    for ( entt::entity cur = uiHit; cur != entt::null; )
                    {
                        if ( isInteractable( cur ) )
                        {
                            uiHit = cur;
                            break;
                        }
                        cur = reg.has<ECS::RelationshipComponent>( cur )
                                   ? reg.get<ECS::RelationshipComponent>( cur ).Parent
                                   : entt::null;
                    }
                }

                if ( uiHit != entt::null && reg.has<ECS::UUIDComponent>( uiHit ) )
                {
                    // The lock applies HERE too, and not only to the 3D raycast below. This is a second
                    // picking path through the same click, and a lock that stopped one of them would be
                    // the "blocks two of the three things it claims to" failure the predicate exists to
                    // prevent — with the padlock still drawn closed in the Outliner either way.
                    if ( ECS::IsLocked( reg, uiHit ) )
                    {
                        ToastManager::Push( Tools::Describe( Tools::PickOutcome::RefusedLocked ), ToastLevel::Info,
                                            2.5f );
                        return false;
                    }

                    const auto uuid = reg.get<ECS::UUIDComponent>( uiHit ).UUID;
                    if ( ::ImGui::GetIO().KeyCtrl )
                        Core::SelectionManager::Toggle( uuid );
                    else
                        Core::SelectionManager::SetSelected( uuid );
                    return false;
                }
            }

            // Bone authoring: LMB selects the nearest bone joint under the cursor (keeping the skinned
            // mesh selected) rather than picking a new entity — unless the bone gizmo is being interacted with.
            if ( Core::ActiveAuthoringContext().ShowsBones() && !m_Gizmo.IsHovered() )
            {
                const int bone = m_LightGizmoRenderer->PickBone( ::ImGui::GetMousePos() );
                if ( bone >= 0 )
                {
                    ClaimAuthoringContext();
                    const auto picked = Core::ActiveAuthoringContext().SetSelectedBone(
                         m_AuthoringOwner, m_Authoring, static_cast<uint32_t>( bone ) );
                    if ( !picked.IsSuccess() )
                        LOG_WARN( "[Viewport] bone pick refused: {}", picked.GetError() );
                }
            }
            else
            {
                const auto outcome = m_Picking.Pick(
                     *m_Scene, ViewCamera(), m_ViewportData.MousePosition, m_ViewportData.Size,
                     m_Gizmo.IsHovered() || m_LightGizmoRenderer->IsLightIconHovered(), ::ImGui::GetIO().KeyCtrl );

                // A refused click has to SAY so. Clicking a locked entity and watching the selection not
                // change is indistinguishable from a broken raycast, and the fix (unlock it) lives in
                // another panel — so the refusal is put on screen rather than dropped. The other outcomes
                // are ordinary and stay quiet; only the one the user can act on speaks, through the
                // editor's existing toast queue instead of a second notification path of its own.
                if ( outcome == Tools::PickOutcome::RefusedLocked )
                    ToastManager::Push( Tools::Describe( outcome ), ToastLevel::Info, 2.5f );
            }
        }

        return false;
    }

    bool ViewportPanel::OnKeyPressedEvent( Common::KeyPressedEvent& e )
    {
        switch ( e.GetKeyCode() )
        {
            case Common::KeyCode::Escape:
                // First Esc turns the gizmo off; a second Esc (gizmo already off) clears the selection.
                if ( m_Gizmo.GetOperation() == Tools::GizmoController::Operation::None )
                    Core::SelectionManager::ClearSelection();
                else
                    m_Gizmo.SetOperation( Tools::GizmoController::Operation::None );
                break;
            case Common::KeyCode::T:
                m_Gizmo.SetOperation( Tools::GizmoController::Operation::Translate );
                break;
            case Common::KeyCode::R:
                m_Gizmo.SetOperation( Tools::GizmoController::Operation::Rotate );
                break;
            case Common::KeyCode::C:
                m_Gizmo.SetOperation( Tools::GizmoController::Operation::Scale );
                break;
            case Common::KeyCode::F:
                // Frame the selected entity (Unity/Godot 'F').
                if ( const auto sel = Core::SelectionManager::GetSelected() )
                    if ( auto cam = ViewCamera() )
                        if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                            if ( auto ref = m_Scene->FindEntityByID( *sel ) )
                                editorCam->Focus( glm::vec3( ref->get().GetWorldTransform()[3] ) );
                break;
            // A `default` and not 115 empty cases: this is a KEYBOARD, and the shortcuts it handles are a
            // deliberately small set. Enumerating the rest would make every key an editing decision and
            // would say nothing true — unlike the renderer-backend switches above, where a new enumerator
            // is a new backend and must not compile until every factory has answered for it.
            default:
                break;
        }
        return false;
    }

    void ViewportPanel::ApplySidecarMaterial( ECS::Entity& entity, const std::string& meshSourcePath )
    {
        if ( !m_AssetManager )
            return;

        const auto h = MeshMaterial::ResolveSidecar( const_cast<Assets::AssetManager&>( *m_AssetManager ),
                                                     meshSourcePath );
        if ( h.IsNull() )
            return;

        // Assign to every material slot (one per submesh).
        auto&  smc   = entity.GetComponent<ECS::StaticMeshComponent>();
        size_t count = 1;
        if ( auto* meshAsset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( smc.MeshHandle ) )
            if ( const auto n = meshAsset->GetMaterialHandles().size(); n > 0 )
                count = n;
        smc.MaterialSlots.assign( count, h );
        smc.RuntimeMaterialInstances.clear();
    }

    std::optional<::Desert::Core::RaycastHit> ViewportPanel::SurfaceAtCursor() const
    {
        const auto mainCamera = ViewCamera();
        if ( !mainCamera || !m_Scene )
            return std::nullopt;

        // Same screen->world ray the click-picker uses (mouse position is already viewport-local).
        const auto ray = Common::Math::Ray::FromScreenPosition(
             { m_ViewportData.MousePosition.x, m_ViewportData.MousePosition.y },
             mainCamera->GetProjectionMatrix(), mainCamera->GetViewMatrix(), mainCamera->GetPosition(),
             static_cast<uint32_t>( m_ViewportData.Size.x ), static_cast<uint32_t>( m_ViewportData.Size.y ) );

        ::Desert::Core::RaycastHit hit;
        if ( !m_Scene->Raycast( ray, hit ) )
            return std::nullopt;
        return hit;
    }

    void ViewportPanel::AssignMaterialAtCursor( const std::string& materialPath )
    {
        if ( !m_AssetManager )
            return;
        const auto found = SurfaceAtCursor();
        if ( !found )
            return;
        const ::Desert::Core::RaycastHit& hit = *found;
        auto ref = m_Scene->FindEntityByID( hit.Entity );
        if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
            return;
        const ECS::Entity& entity = ref->get();

        // Resolve/register the dropped material (same path the slot editor's drop target takes).
        auto& mgr   = const_cast<Assets::AssetManager&>( *m_AssetManager );
        auto  asset = mgr.FindByPath<Assets::SurfaceMaterialAsset>( materialPath );
        if ( !asset )
            asset = mgr.CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High, materialPath );
        if ( !asset )
            return;
        const auto handle = asset->GetMetadata().Handle;
        if ( !Runtime::ResourceRegistry::GetMaterialService()->Get( handle ) )
            Runtime::ResourceRegistry::GetMaterialService()->RegisterAsset( asset );

        // Assign every element (the renderer repeats the last slot anyway) and select the entity so
        // the Materials panel shows the result of the drop immediately.
        auto& smc = m_Scene->GetRegistry().get<ECS::StaticMeshComponent>( entity.GetHandle() );
        const size_t count = std::max<size_t>( size_t{ 1 }, smc.MaterialSlots.size() );
        smc.MaterialSlots.assign( count, handle );
        smc.RuntimeMaterialInstances.clear();
        Core::SelectionManager::SetSelected( hit.Entity );
    }

} // namespace Desert::Editor
