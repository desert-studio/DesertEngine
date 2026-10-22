#include <Editor/Core/GizmoIdScope.hpp>
#include "GizmoController.hpp"
#include "GizmoTransformMath.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/Logger.hpp>

#include <memory>

#include <ImGui/imgui.h>

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityLock.hpp>

#include <ImGuizmo.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        // Undoable single-bone rest-pose edit. Re-resolves the skeleton via the mesh handle on each Undo/Redo,
        // so it stays valid across skeleton reloads (unlike a raw pointer into the bone array). The mesh
        // re-skins from LocalBindTransform every frame, so restoring the matrix is all that's needed.
        class BoneTransformCommand final : public ICommand
        {
        public:
            BoneTransformCommand( Assets::AssetHandle mesh, int bone, const glm::mat4& oldM,
                                  const glm::mat4& newM )
                 : m_Mesh( mesh ), m_Bone( bone ), m_Old( oldM ), m_New( newM )
            {
            }

            bool Undo() override
            {
                return Apply( m_Old );
            }
            bool Redo() override
            {
                return Apply( m_New );
            }

        private:
            bool Apply( const glm::mat4& m )
            {
                auto* base = Runtime::ResourceRegistry::GetMeshService()->Get( m_Mesh );
                if ( !base || !base->IsSkinned() )
                    return false;
                if ( m_Bone < 0 )
                    return false;
                return static_cast<SkinnedMesh*>( base )->GetSkeletonMutable()->SetLocalBindTransform(
                     static_cast<uint32_t>( m_Bone ), m );
            }

            Assets::AssetHandle m_Mesh;
            int                 m_Bone;
            glm::mat4           m_Old, m_New;
        };
    } // namespace

    bool GizmoController::RenderFrame( const char* site, glm::mat4& frame,
                                       const std::shared_ptr<::Desert::Core::Camera>& camera,
                                       const glm::vec2& viewportPos, const glm::vec2& viewportSize, int handles )
    {
        if ( !camera )
            return false;
        const Core::GizmoIdScope gizmoId( site );
        const ImGuizmo::OPERATION operation = handles == kFrameTranslateRotateScale
                                                   ? ImGuizmo::TRANSLATE | ImGuizmo::ROTATE | ImGuizmo::SCALE
                                                   : ImGuizmo::TRANSLATE | ImGuizmo::ROTATE;
        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y );
        const auto& view  = camera->GetViewMatrix();
        const auto& proj  = camera->GetProjectionMatrix();
        const auto  scale = ScaleHandles( site, frame, view, proj, viewportPos, viewportSize, operation, nullptr );
        if ( scale.Owns )
            ImGuizmo::Enable( false );
        const bool manipulated =
             ImGuizmo::Manipulate( &view[0][0], &proj[0][0], operation, ImGuizmo::LOCAL, &frame[0][0] );
        if ( scale.Owns )
            ImGuizmo::Enable( true );
        DrawScaleHandle( site );
        if ( ImGuizmo::IsOver() || ImGuizmo::IsUsing() )
            m_Hovered = true;
        return manipulated || scale.Changed;
    }

    GizmoController::ScaleHandleResult GizmoController::ScaleHandles( const char* site, glm::mat4& matrix,
                                                                      const glm::mat4& view, const glm::mat4& proj,
                                                                      const glm::vec2& viewportPos,
                                                                      const glm::vec2& viewportSize, int operation,
                                                                      const float* snap )
    {
        ScaleHandleResult result;
        const bool        held = m_ScaleAxis >= 0 && m_ScaleSite == site;
        if ( m_ScaleAxis >= 0 && !held )
            return result; // another site's handle is held
        if ( !held &&
             ( ImGuizmo::IsUsing() || IntervalOwnsInput() || viewportSize.x <= 0.0f || viewportSize.y <= 0.0f ) )
            return result;

        const ImVec2    mouseIm = ::ImGui::GetMousePos();
        const glm::vec2 mouse( mouseIm.x, mouseIm.y );
        if ( held )
        {
            result.Owns = true;
            m_Hovered   = true;
            if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                m_ScaleAxis  = -1;
                m_ScaleHover = -1;
                m_ScaleSite  = nullptr;
                return result;
            }
            // ImGuizmo's ratio: the cursor's travel along the handle's screen direction over the handle's
            // length; snapped to the step, never below 0.001 (ImGuizmo's floor).
            float factor = 1.0f + glm::dot( mouse - m_ScaleMouse, m_ScaleDir ) / m_ScalePixels;
            if ( snap && snap[0] > 0.0f )
                factor = std::round( factor / snap[0] ) * snap[0];
            factor              = std::max( factor, 0.001f );
            m_ScaleFactor       = factor;
            glm::mat4 scaled    = m_ScaleStart;
            scaled[m_ScaleAxis] = m_ScaleStart[m_ScaleAxis] * factor;
            if ( scaled != matrix )
            {
                matrix         = scaled;
                result.Changed = true;
            }
            return result;
        }

        // The handle geometry ImGuizmo draws (DrawScaleGizmo): along each orthonormalised local axis, from
        // 0.1 to 1.0 screen factors (1.0 to 1.4 when the axis also translates), flipped to the side that is
        // longer on screen, hidden when shorter than its plane limit (0.02 clip).
        const glm::mat4 viewProj = proj * view;
        const float     aspect   = viewportSize.x / viewportSize.y;
        auto            toNdc    = [&]( const glm::vec3& world, glm::vec2& ndc )
        {
            const glm::vec4 clip = viewProj * glm::vec4( world, 1.0f );
            if ( clip.w <= 0.0001f )
                return false;
            ndc = glm::vec2( clip ) / clip.w;
            return true;
        };
        auto clipLength = [&]( const glm::vec3& a, const glm::vec3& b )
        {
            glm::vec2 na, nb;
            if ( !toNdc( a, na ) || !toNdc( b, nb ) )
                return 0.0f;
            glm::vec2 d = nb - na;
            if ( aspect < 1.0f )
                d.x *= aspect;
            else
                d.y /= aspect;
            return glm::length( d );
        };
        auto toScreen = [&]( const glm::vec2& ndc )
        {
            return glm::vec2( viewportPos.x + ( ndc.x * 0.5f + 0.5f ) * viewportSize.x,
                              viewportPos.y + ( 0.5f - ndc.y * 0.5f ) * viewportSize.y );
        };
        const glm::vec3 origin( matrix[3] );
        const glm::vec3 camRight( glm::inverse( view )[0] );
        const float     rightLength = clipLength( origin, origin + camRight );
        glm::vec2       originNdc;
        if ( rightLength <= 0.0f || !toNdc( origin, originNdc ) )
            return result;
        const float screenFactor = 0.1f / rightLength; // ImGuizmo's default gizmo size, 0.1 clip

        int       best     = -1;
        float     bestDist = 12.0f; // ImGuizmo's pixel tolerance
        glm::vec2 bestBall( 0.0f );
        for ( int i = 0; i < 3; ++i )
        {
            if ( ( operation & ( ImGuizmo::SCALE_X << i ) ) == 0 )
                continue;
            glm::vec3 dir( matrix[i] );
            if ( glm::length( dir ) <= 1e-8f )
                continue;
            dir = glm::normalize( dir );
            if ( clipLength( origin, origin + dir ) < clipLength( origin, origin - dir ) )
                dir = -dir;
            if ( clipLength( origin, origin + dir * screenFactor ) <= 0.02f )
                continue;
            const bool translates = ( operation & ( ImGuizmo::TRANSLATE_X << i ) ) != 0;
            glm::vec2  startNdc, endNdc;
            if ( !toNdc( origin + dir * screenFactor * ( translates ? 1.0f : 0.1f ), startNdc ) ||
                 !toNdc( origin + dir * screenFactor * ( translates ? 1.4f : 1.0f ), endNdc ) )
                continue;
            const glm::vec2 a = toScreen( startNdc ), b = toScreen( endNdc );
            const glm::vec2 ab = b - a;
            const float     t =
                 glm::clamp( glm::dot( mouse - a, ab ) / std::max( glm::dot( ab, ab ), 1e-6f ), 0.0f, 1.0f );
            const float d = glm::length( mouse - ( a + ab * t ) );
            if ( d < bestDist )
            {
                best     = i;
                bestDist = d;
                bestBall = b;
            }
        }
        if ( best < 0 )
            return result;
        m_ScaleHover  = best;
        m_ScaleSite   = site;
        m_Hovered     = true;
        m_ScaleOrigin = toScreen( originNdc );
        m_ScalePixels = std::max( glm::length( bestBall - m_ScaleOrigin ), 1.0f );
        m_ScaleDir    = ( bestBall - m_ScaleOrigin ) / m_ScalePixels;
        m_ScaleFactor = 1.0f;
        if ( ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            m_ScaleAxis  = best;
            m_ScaleStart = matrix;
            m_ScaleMouse = mouse;
            result.Owns  = true;
        }
        return result;
    }

    void GizmoController::DrawScaleHandle( const char* site ) const
    {
        if ( m_ScaleSite != site || ( m_ScaleAxis < 0 && m_ScaleHover < 0 ) )
            return;
        ImDrawList*     dl      = ::ImGui::GetWindowDrawList();
        const glm::vec2 ball    = m_ScaleOrigin + m_ScaleDir * m_ScalePixels * m_ScaleFactor;
        constexpr auto  kActive = IM_COL32( 255, 255, 16, 255 ); // ImGuizmo's SELECTION colour
        if ( m_ScaleAxis >= 0 )
            dl->AddLine( ImVec2( m_ScaleOrigin.x, m_ScaleOrigin.y ), ImVec2( ball.x, ball.y ), kActive, 3.0f );
        dl->AddCircleFilled( ImVec2( ball.x, ball.y ), 6.0f, kActive );
    }

    bool GizmoController::RenderInterval( const glm::mat4& frame, float& lower, float& upper,
                                          const std::shared_ptr<::Desert::Core::Camera>& camera,
                                          const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        if ( !camera || viewportSize.x <= 0.0f || viewportSize.y <= 0.0f )
            return false;
        const glm::mat4 viewProj = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        const glm::vec3 origin( frame[3] );
        const glm::vec3 axis     = glm::normalize( glm::vec3( frame[2] ) );
        auto            toScreen = [&]( const glm::vec3& world, ImVec2& out )
        {
            const glm::vec4 clip = viewProj * glm::vec4( world, 1.0f );
            if ( clip.w <= 0.0001f )
                return false; // behind the camera
            out = ImVec2( viewportPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x,
                          viewportPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y );
            return true;
        };
        ImVec2 lo, hi;
        if ( !toScreen( origin + axis * lower, lo ) || !toScreen( origin + axis * upper, hi ) )
        {
            m_IntervalDrag  = 0;
            m_IntervalHover = false;
            return false;
        }
        const ImVec2 mouse  = ::ImGui::GetMousePos();
        auto         isNear = [&]( const ImVec2& p )
        { return ( p.x - mouse.x ) * ( p.x - mouse.x ) + ( p.y - mouse.y ) * ( p.y - mouse.y ) <= 81.0f; };
        // The handles sit ON the frame's axis arrow; like UE's input router (nearest hit wins, and the
        // interval handle is the smaller target), a handle under the cursor outranks the transform gizmo:
        // m_IntervalHover disables ImGuizmo on the next frame (IntervalOwnsInput), before it is rendered.
        int hover = m_IntervalDrag;
        if ( hover == 0 && !ImGuizmo::IsUsing() )
            hover = isNear( hi ) ? 2 : isNear( lo ) ? 1 : 0;
        m_IntervalHover = hover != 0;
        if ( hover != 0 )
            m_Hovered = true;

        ImDrawList*    dl       = ::ImGui::GetWindowDrawList();
        constexpr auto kLine    = IM_COL32( 230, 200, 60, 255 );
        constexpr auto kHandle  = IM_COL32( 240, 210, 70, 255 );
        constexpr auto kHovered = IM_COL32( 255, 255, 255, 255 );
        dl->AddLine( lo, hi, kLine, 2.0f );
        for ( int h = 1; h <= 2; ++h )
        {
            const ImVec2 p = h == 1 ? lo : hi;
            const float  r = hover == h ? 7.0f : 5.0f;
            dl->AddRectFilled( ImVec2( p.x - r, p.y - r ), ImVec2( p.x + r, p.y + r ),
                               hover == h ? kHovered : kHandle );
            dl->AddRect( ImVec2( p.x - r, p.y - r ), ImVec2( p.x + r, p.y + r ), IM_COL32( 20, 20, 20, 255 ) );
        }

        if ( m_IntervalDrag == 0 && hover != 0 && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            m_IntervalDrag = hover;
        if ( m_IntervalDrag == 0 )
            return false;
        if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
        {
            m_IntervalDrag = 0;
            return false;
        }
        // The mouse ray against the axis line: the axis parameter of the closest point is the new bound.
        const glm::mat4 inv = glm::inverse( viewProj );
        const float     nx  = ( mouse.x - viewportPos.x ) / viewportSize.x * 2.0f - 1.0f;
        const float     ny  = 1.0f - ( mouse.y - viewportPos.y ) / viewportSize.y * 2.0f;
        const glm::vec4 a4  = inv * glm::vec4( nx, ny, 0.1f, 1.0f );
        const glm::vec4 b4  = inv * glm::vec4( nx, ny, 0.9f, 1.0f );
        const glm::vec3 p0  = glm::vec3( a4 ) / a4.w;
        const glm::vec3 ray = glm::normalize( glm::vec3( b4 ) / b4.w - p0 );
        const glm::vec3 w0  = origin - p0;
        const float     b = glm::dot( axis, ray ), denom = 1.0f - b * b;
        if ( denom < 1e-6f )
            return false; // looking straight down the axis
        const float t     = ( b * glm::dot( ray, w0 ) - glm::dot( axis, w0 ) ) / denom;
        float&      bound = m_IntervalDrag == 2 ? upper : lower;
        const float next  = m_IntervalDrag == 2 ? std::max( 0.0f, t ) : std::min( 0.0f, t );
        if ( next == bound )
            return false;
        bound = next;
        return true;
    }

    void GizmoController::RenderObject( ::Desert::Core::Scene&                         scene,
                                        const std::shared_ptr<::Desert::Core::Camera>& camera,
                                        const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        // NOTE: ImGuizmo::BeginFrame() is issued once per frame by EditorLayer, before any panel runs.
        const auto& mainCamera = camera;
        if ( !mainCamera )
            return;

        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected )
            return;

        const auto& selectedEntityOpt = scene.FindEntityByID( *selected );
        if ( !selectedEntityOpt )
            return;

        auto& selectedEntity = selectedEntityOpt->get();

        auto& transformComponent = selectedEntity.GetComponent<ECS::TransformComponent>();
        auto& reg                = scene.GetRegistry();

        // A LOCKED primary draws no gizmo at all, which is the whole second half of the lock: the picker
        // refuses to select a locked entity, but the OUTLINER deliberately still can — that is how you
        // reach the padlock to undo it — and a selection made there must not come with draggable handles.
        // Returning before SetRect/Manipulate also leaves m_Hovered false, so picking does not stand down
        // for a gizmo that is not on screen.
        if ( ECS::IsLocked( reg, selectedEntity.GetHandle() ) )
            return;

        // The gizmo must work in WORLD space. For a CHILD entity (e.g. a camera parented to the character),
        // the world transform = parentWorld * local, and an edit must be converted back to LOCAL before
        // writing. parentWorld = identity for a root entity (so this is a no-op there).
        auto parentWorldOf = [&reg]( entt::entity e ) -> glm::mat4
        {
            glm::mat4 world( 1.0f );
            if ( reg.has<ECS::RelationshipComponent>( e ) )
            {
                std::vector<entt::entity> chain; // [parent, grandparent, ... root]
                entt::entity              cur = reg.get<ECS::RelationshipComponent>( e ).Parent;
                while ( cur != entt::null )
                {
                    chain.push_back( cur );
                    cur = reg.has<ECS::RelationshipComponent>( cur )
                               ? reg.get<ECS::RelationshipComponent>( cur ).Parent
                               : entt::null;
                }
                for ( auto it = chain.rbegin(); it != chain.rend(); ++it ) // root -> ... -> parent
                    if ( reg.has<ECS::TransformComponent>( *it ) )
                        world = world * reg.get<ECS::TransformComponent>( *it ).GetTransform();
            }
            return world;
        };

        // An entity with a selected ANCESTOR is carried by that ancestor's transform already — the group
        // logic must skip it (else it would move twice).
        auto coveredBySelection = [&reg]( entt::entity e )
        {
            entt::entity cur = e;
            while ( reg.has<ECS::RelationshipComponent>( cur ) )
            {
                const auto parent = reg.get<ECS::RelationshipComponent>( cur ).Parent;
                if ( parent == entt::null )
                    break;
                cur = parent;
                if ( reg.has<ECS::UUIDComponent>( cur ) &&
                     Core::SelectionManager::IsSelected( reg.get<ECS::UUIDComponent>( cur ).UUID ) )
                    return true;
            }
            return false;
        };

        const glm::mat4 parentWorld = parentWorldOf( selectedEntity.GetHandle() );

        auto            modelMatrix    = parentWorld * transformComponent.GetTransform(); // world, for the gizmo
        const glm::mat4 oldModelMatrix = modelMatrix;

        // SetRect MUST match the rendered scene-image rect (content region), NOT the raw window rect.
        const Core::GizmoIdScope gizmoId( "LevelSelection" );
        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y );

        const auto& view = mainCamera->GetViewMatrix();
        const auto& proj = mainCamera->GetProjectionMatrix();

        // Snap: grid for translate, fixed angles for rotate, increments for scale. Active when the
        // toolbar's magnet toggle is on OR Ctrl is held (Ctrl inverts the toggle).
        const auto  operation     = Core::GizmoState::Get();
        float       snapValues[3] = { 0.0f, 0.0f, 0.0f };
        const float snapUnit =
             ( operation == Core::GizmoState::Operation::Rotate )  ? Core::GizmoState::RotateSnapDegrees()
             : ( operation == Core::GizmoState::Operation::Scale ) ? Core::GizmoState::ScaleSnap()
                                                                   : Core::GizmoState::TranslateSnap();
        snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;
        const float* snap = Core::GizmoState::SnapActive( ::ImGui::GetIO().KeyCtrl ) ? snapValues : nullptr;

        // World or Local, from the toolbar's toggle. EffectiveSpace() and not GetSpace(): ImGuizmo ignores
        // the mode for scaling, and the toolbar reads the same function, so the handles and the button
        // cannot disagree about which space is in force. Core::GizmoState::Space mirrors ImGuizmo::MODE.
        const auto space = static_cast<ImGuizmo::MODE>( Core::GizmoState::EffectiveSpace( operation ) );

        const auto scale = ScaleHandles( "LevelSelection", modelMatrix, view, proj, viewportPos, viewportSize,
                                         static_cast<int>( operation ), snap );
        if ( scale.Owns )
            ImGuizmo::Enable( false );
        const bool manipulated =
             ImGuizmo::Manipulate( &view[0][0], &proj[0][0], static_cast<ImGuizmo::OPERATION>( operation ), space,
                                   &modelMatrix[0][0], nullptr, snap ) ||
             scale.Changed;
        if ( scale.Owns )
            ImGuizmo::Enable( true );
        DrawScaleHandle( "LevelSelection" );

        // Picking must stand down whenever the cursor is OVER the gizmo — not only mid-drag. Setting this
        // only while manipulating meant a first click on a gizmo axis drawn over another mesh SELECTED
        // that mesh instead of starting the manipulation.
        if ( ImGuizmo::IsOver() || ImGuizmo::IsUsing() )
            m_Hovered = true;

        // One undo entry per drag: when the drag STARTS this frame, capture the pre-drag TRS of every
        // selected top-level root NOW — before any of this frame's deltas are written below.
        const bool usingNow = ImGuizmo::IsUsing() || scale.Owns;
        Core::GizmoState::SetEntityInteraction( usingNow );
        if ( usingNow && !m_DragActive )
        {
            m_DragActive = true;
            m_DragEntity = *selected;
            Core::GizmoState::SetEntityGestureEntry( std::nullopt );
            m_DragSnapshots.clear();
            for ( const auto& id : Core::SelectionManager::GetSelection() )
            {
                auto ref = scene.FindEntityByID( id );
                if ( !ref )
                    continue;
                ECS::Entity e = ref->get();
                if ( !e.HasComponent<ECS::TransformComponent>() || coveredBySelection( e.GetHandle() ) )
                    continue;
                // A locked follower is not moved below, so it must not be snapshotted either — an undo
                // entry for an entity that never changed is an undo step that appears to do nothing.
                if ( ECS::IsLocked( reg, e.GetHandle() ) )
                    continue;
                const auto& tc = e.GetComponent<ECS::TransformComponent>();
                m_DragSnapshots.push_back( { id, tc.Translation, tc.Rotation, tc.Scale } );
            }
        }

        if ( manipulated )
        {
            // Convert the manipulated WORLD matrix back to the entity's LOCAL space (inverse parent) before
            // decomposing — so dragging a child entity edits its local offset correctly. The composition
            // itself lives in GizmoTransformMath.hpp so the GizmoTransformSpace suite can reach it.
            const auto local = WorldToLocalTRS( parentWorld, modelMatrix );

            transformComponent.Translation = local.Translation;
            transformComponent.Rotation    = local.Rotation;
            transformComponent.Scale       = local.Scale;

            // Group manipulation: apply the same WORLD-space delta to every other selected top-level root,
            // so the whole selection moves/rotates/scales as one rigid group around the primary's gizmo.
            const auto& allSelected = Core::SelectionManager::GetSelection();
            if ( allSelected.size() > 1 )
            {
                const glm::mat4 delta = modelMatrix * glm::inverse( oldModelMatrix );
                for ( const auto& id : allSelected )
                {
                    if ( id == *selected )
                        continue;
                    auto ref = scene.FindEntityByID( id );
                    if ( !ref )
                        continue;
                    ECS::Entity e = ref->get();
                    if ( !e.HasComponent<ECS::TransformComponent>() || coveredBySelection( e.GetHandle() ) )
                        continue;
                    // The group delta stops at a locked member. Dragging five things of which one is
                    // locked moves four — the lock is a property of the entity, not of how it happened
                    // to be selected, so a multi-selection cannot be a way around it.
                    if ( ECS::IsLocked( reg, e.GetHandle() ) )
                        continue;

                    auto&           tc = e.GetComponent<ECS::TransformComponent>();
                    const glm::mat4 pw = parentWorldOf( e.GetHandle() );

                    const auto follower = WorldToLocalTRS( pw, delta * ( pw * tc.GetTransform() ) );
                    tc.Translation      = follower.Translation;
                    tc.Rotation         = follower.Rotation;
                    tc.Scale            = follower.Scale;
                }
            }
        }

        // Commit old->current for the whole group on release. The UUID guard drops the pending capture if
        // the selection changed mid-drag.
        if ( !usingNow && m_DragActive )
        {
            m_DragActive = false;
            // The revision of the move's entry is what the level sequence's Auto Key joins its key to.
            const bool recorded = m_DragEntity == *selected && Commands::RecordTransformEdits( m_DragSnapshots );
            Core::GizmoState::SetEntityGestureEntry(
                 recorded ? std::optional<uint64_t>( CommandHistory::Get().Revision() ) : std::nullopt );
            m_DragSnapshots.clear();
        }
    }

    void GizmoController::ResetHovered()
    {
        m_Hovered = false;
        if ( m_ScaleAxis < 0 )
            m_ScaleHover = -1; // ScaleHandles re-finds it this frame
        Core::GizmoState::SetPoseInteraction( false );
        Core::GizmoState::SetEntityInteraction( false );
    }

    void GizmoController::RenderBone( ::Desert::Core::Scene&                         scene,
                                      const std::shared_ptr<::Desert::Core::Camera>& camera,
                                      const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        const auto& mainCamera = camera;
        if ( !mainCamera )
            return;

        const int boneIdx = Core::ActiveAuthoringContext().SelectedBoneIndex();
        if ( boneIdx < 0 )
            return;

        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected )
            return;
        const auto& entOpt = scene.FindEntityByID( *selected );
        if ( !entOpt )
            return;
        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::SkinnedMeshComponent>() )
            return;

        auto& smc  = entity.GetComponent<ECS::SkinnedMeshComponent>();
        auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle );
        if ( !mesh || !mesh->IsSkinned() )
            return;
        auto*       skeleton = static_cast<SkinnedMesh*>( mesh )->GetSkeletonMutable();
        const auto& bones    = skeleton->GetBones();
        if ( boneIdx >= static_cast<int>( bones.size() ) )
            return;

        // Pose mode (Sequencer authoring): the gizmo edits the Animator's EDITABLE pose buffer instead of the
        // rig's bind pose, so posing to key a clip never mutates the shared rest pose. Falls back to bind
        // editing when there is no animator. See AuthoringMode::Pose / Animator::SetBoneLocalPose.
        Animation::Animator* animator = nullptr;
        if ( Core::ActiveAuthoringContext().IsPoseAuthoring() && entity.HasComponent<ECS::AnimationComponent>() )
            animator = entity.GetComponent<ECS::AnimationComponent>().Animator.get();
        const bool usePose = ( animator != nullptr );

        const glm::mat4 entityWorld = entity.GetComponent<ECS::TransformComponent>().GetTransform();

        // Chain global per bone — the SAME space the mesh is skinned in, so the gizmo sits on the bone.
        //
        // THE LAZY COMPONENT POSE'S ONE PRODUCTION CALLER, and it is the shape the type exists for: of a
        // hundred bones this wants exactly two — the one being dragged and its parent — so the resolve costs
        // their depth rather than the rig. It used to be a memoised recursion over the whole array, one of
        // eight copies of the same eleven lines.
        Animation::LocalPose poseSource;
        if ( usePose )
        {
            poseSource.Resize( bones.size() );
            for ( uint32_t i = 0; i < bones.size(); ++i )
            {
                const auto trs = Animation::BoneTransform::FromMatrix( animator->GetBoneLocalPose( i ) );
                if ( trs.IsSuccess() )
                    poseSource[i] = trs.GetValue();
            }
        }
        else
        {
            auto bind = Animation::LocalPose::FromBindPose( *skeleton );
            if ( !bind.IsSuccess() )
            {
                LOG_ERROR( "[BoneGizmo] cannot place a gizmo on this rig: {}", bind.GetError() );
                return;
            }
            poseSource = std::move( bind.GetValue() );
        }

        Animation::ComponentPose chainGlobal( *skeleton, poseSource );

        glm::mat4 gizmoWorld = entityWorld * chainGlobal.Get( static_cast<uint32_t>( boneIdx ) );

        const Core::GizmoIdScope gizmoId( "LevelBone" );
        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y );

        const auto& view = mainCamera->GetViewMatrix();
        const auto& proj = mainCamera->GetProjectionMatrix();
        // The toolbar's operation, as for the object gizmo (Persona scales bones too); None means Translate.
        const Operation toolbarOp = Core::GizmoState::Get();
        const Operation boneOp    = ( toolbarOp == Operation::Rotate || toolbarOp == Operation::Scale )
                                         ? toolbarOp
                                         : Operation::Translate;
        const auto      op        = static_cast<ImGuizmo::OPERATION>( boneOp );

        // The bone gizmo honours the SAME toolbar toggle as the object gizmo — one control, both gizmos,
        // or the button would mean different things depending on a mode the user is not looking at.
        // Asked with the operation this call actually passes, so the space in force matches the handles drawn.
        const auto boneSpace = static_cast<ImGuizmo::MODE>( Core::GizmoState::EffectiveSpace( boneOp ) );

        // Scale goes through our screen-space handles, as RenderObject's: ImGuizmo's own scale hit misses
        // an axis seen edge-on (see ScaleHandles).
        const auto scale = ScaleHandles( "LevelBone", gizmoWorld, view, proj, viewportPos, viewportSize,
                                         static_cast<int>( op ), nullptr );
        if ( scale.Owns )
            ImGuizmo::Enable( false );
        const bool boneManipulated =
             ImGuizmo::Manipulate( &view[0][0], &proj[0][0], op, boneSpace, &gizmoWorld[0][0] ) || scale.Changed;
        if ( scale.Owns )
            ImGuizmo::Enable( true );
        DrawScaleHandle( "LevelBone" );

        // Same rule as the object gizmo: picking stands down on HOVER, not only mid-drag.
        if ( ImGuizmo::IsOver() || ImGuizmo::IsUsing() )
            m_Hovered = true;

        // One undo entry per drag: capture the bone's pre-drag rest transform when the drag STARTS, before
        // this frame's edit is written below. Rig editing only — pose-mode edits are persisted by keying.
        const bool usingNow = ImGuizmo::IsUsing() || scale.Owns;

        // AND THE POSE BRANCH PUBLISHES THE SAME BOUNDARY IT ALREADY COMPUTES. `m_BoneDragActive` above is
        // the rig-editing half of one fact — "the bone gizmo is being held" — and the pose half of it was
        // simply dropped, so the Sequencer, which is the thing that needs it, had to guess the boundary
        // from the pose CHANGING and could never see a drag end. Report 05 §971's deferral is exactly that
        // missing edge; see Core::GizmoState::PoseInteraction for why the bit lives there.
        if ( usePose && usingNow )
        {
            Core::GizmoState::SetPoseInteraction( true );
        }

        if ( !usePose && usingNow && !m_BoneDragActive )
        {
            m_BoneDragActive = true;
            m_BoneDragIndex  = boneIdx;
            m_BoneDragMesh   = smc.MeshHandle;
            m_BoneDragOld    = bones[boneIdx].LocalBindTransform;
        }

        if ( boneManipulated )
        {
            // The new local (parent-relative) transform from the gizmo's world matrix.
            const glm::mat4 newGlobalMesh = glm::inverse( entityWorld ) * gizmoWorld;
            glm::mat4       parentGlobal( 1.0f );
            if ( const uint32_t parent = skeleton->ResolveParent( static_cast<uint32_t>( boneIdx ) );
                 parent != Animation::Skeleton::NO_PARENT )
            {
                parentGlobal = chainGlobal.Get( parent );
            }
            const glm::mat4 newLocal = glm::inverse( parentGlobal ) * newGlobalMesh;

            if ( usePose )
            {
                // Pose the ANIMATED buffer (never the bind pose) and re-render it so the viewport updates.
                animator->SetBoneLocalPose( static_cast<uint32_t>( boneIdx ), newLocal );
                animator->ApplyLocalPose();
            }
            else
            {
                skeleton->SetLocalBindTransform( static_cast<uint32_t>( boneIdx ), newLocal ); // rest-pose edit
            }
        }

        // On release: record the net rig change (old -> current) as one undoable command (rig editing only).
        if ( !usePose && !usingNow && m_BoneDragActive )
        {
            m_BoneDragActive = false;
            if ( m_BoneDragIndex == boneIdx && bones[boneIdx].LocalBindTransform != m_BoneDragOld )
                CommandHistory::Get().PushCommand( std::make_unique<BoneTransformCommand>(
                     m_BoneDragMesh, boneIdx, m_BoneDragOld, bones[boneIdx].LocalBindTransform ) );
        }
    }
} // namespace Desert::Editor::Tools
