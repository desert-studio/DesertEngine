#include "AnimGraphPanel.hpp"

#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace G     = Animation::Graph;

    namespace
    {
        // The goal the selected skeletal-control node aims at: Two Bone IK's effector goal, Look At's target.
        G::BoneControlTarget* GoalOf( G::PoseNode& node )
        {
            if ( node.TwoBoneIK )
                return &node.TwoBoneIK->Goal;
            if ( node.LookAt )
                return &node.LookAt->Target;
            return nullptr;
        }

        // The space a goal is authored in, as a component-space matrix: identity for component space, else the
        // named bone's model matrix in the pose the preview shows (an unknown bone: nullopt, no gizmo).
        std::optional<glm::mat4> GoalSpace( const G::BoneControlTarget& goal, const Animation::Animator& animator )
        {
            if ( goal.Bone.empty() )
                return glm::mat4( 1.0F );
            const auto bone = animator.GetSkeleton().FindBoneIndex( goal.Bone );
            if ( !bone )
                return std::nullopt;
            return animator.GetBoneModelMatrix( *bone );
        }
    } // namespace

    void AnimGraphPanel::DrawGoalGizmo( const glm::vec2& origin, const glm::vec2& size )
    {
        if ( !m_Preview || m_EditingMachine || m_SelectedPoseNode.empty() )
            return;
        const Animation::Animator* animator = m_Preview->GetAnimator();
        ECS::AnimationComponent*   anim     = ResolveComponent();
        if ( animator == nullptr || anim == nullptr || !anim->Graph )
            return;
        const PoseGraphTarget target = ResolvePoseTarget( *anim->Graph );
        const auto            node   = std::ranges::find_if( target.Nodes, [&]( const G::PoseNode& n )
                                                             { return n.Name == m_SelectedPoseNode; } );
        if ( node == target.Nodes.end() )
            return;
        G::BoneControlTarget* goal = GoalOf( *node );
        if ( goal == nullptr )
            return;
        const auto space = GoalSpace( *goal, *animator );
        if ( !space )
            return;

        // goal (authored space) -> component space (the bone's model matrix) -> world (the preview's target).
        const glm::mat4 model = m_Preview->GetTargetTransform();
        const glm::vec3 authored( goal->Position[0], goal->Position[1], goal->Position[2] );
        const glm::vec3 position = glm::vec3( model * *space * glm::vec4( authored, 1.0F ) );
        glm::mat4       world    = glm::translate( glm::mat4( 1.0F ), position );
        const glm::mat4 view     = m_Preview->GetView();
        const glm::mat4 proj     = m_Preview->GetProjection();

        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( origin.x, origin.y, size.x, size.y );
        const bool moved =
             ImGuizmo::Manipulate( &view[0][0], &proj[0][0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &world[0][0] );
        m_GizmoHovered = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
        if ( !moved )
            return;
        // Back into the goal's own space. The drag is ONE undo entry: OnUIRender's m_GraphEdit.Observe closes
        // the transaction only when the mouse is released.
        const glm::vec3 moved3 =
             glm::vec3( glm::inverse( model * *space ) * glm::vec4( glm::vec3( world[3] ), 1.0F ) );
        goal->Position = { moved3.x, moved3.y, moved3.z };
        MarkEdited();
    }
} // namespace Desert::Editor
