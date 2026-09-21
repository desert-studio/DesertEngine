#include "ControlRigPanel.hpp"
#include <Editor/Panels/PanelContext.hpp>

#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <ImGui/imgui.h>

#include <optional>
#include <string>
#include <utility>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    ControlRigPanel::ControlRigPanel( std::shared_ptr<::Desert::Core::Scene> scene )
         : IPanel( "Control Rig", /*showPanel=*/false ), m_Scene( std::move( scene ) )
    {
    }

    void ControlRigPanel::Author( const Common::UUID& entity, const char* what,
                                  const std::function<Common::BoolResultStr( Core::AuthoringContext& )>& write )
    {
        // Focus() ADOPTS when the live context is about the same entity, so taking it over here keeps the
        // mode and the selections whatever window the user came from had. With one character on screen
        // that is indistinguishable from the statics this replaced; with two, the other one keeps its own.
        m_Authoring.Entity = entity;
        (void)Core::ActiveAuthoringContext().Focus( m_AuthoringOwner, m_Authoring );

        if ( const auto done = write( m_Authoring ); !done )
        {
            // Unreachable while the Focus() above succeeds, and logged rather than dropped because the day
            // it IS reachable the symptom is "the control does nothing" with no other trace at all.
            LOG_WARN( "[Control Rig] {} refused: {}", what, done.GetError() );
        }
    }

    void ControlRigPanel::OnUIRender()
    {
        if ( !m_Scene )
        {
            ImGui::TextDisabled( "No active scene." );
            return;
        }

        const auto& sel = Core::SelectionManager::GetSelected();
        if ( !sel )
        {
            ImGui::TextDisabled( "Select a rigged entity." );
            return;
        }
        const auto entOpt = m_Scene->FindEntityByID( *sel );
        if ( !entOpt )
        {
            return;
        }
        const ECS::Entity& entity = entOpt->get();

        if ( !entity.HasComponent<ECS::ControlRigComponent>() )
        {
            // NAMES THE MISSING PIECE rather than saying "nothing here". Before this tier the only way to
            // reach a rig was C++, so "add the component" is genuinely the next thing to do and genuinely
            // not obvious.
            ImGui::TextDisabled( "The selected entity has no Control Rig component." );
            ImGui::TextWrapped( "Add one in Details (Add Component -> Control Rig) and point it at a .derig." );
            return;
        }

        if ( !entity.HasComponent<ECS::AnimationComponent>() )
        {
            ImGui::TextDisabled( "The selected entity has a Control Rig but no Animation component." );
            ImGui::TextWrapped( "The rig is the last stage of the pose pipeline, and the pipeline lives in "
                                "the Animator — without one nothing evaluates it." );
            return;
        }

        auto& anim = entity.GetComponent<ECS::AnimationComponent>();
        if ( !anim.Animator )
        {
            ImGui::TextDisabled( "Animator not ready (the entity's skinned mesh is still loading)." );
            return;
        }

        Animation::ControlRigStage* rig = anim.Animator->GetRig();
        if ( rig == nullptr )
        {
            // THE TWO REASONS ARE DIFFERENT AND BOTH ARE STATED. A handle of zero is a deliberate "no rig";
            // a non-zero handle with no stage is a rig that refused to build, and the log carries the
            // sentence saying why. Drawing one message for both is how "it does not work" gets reported
            // with nothing to act on.
            const auto handle = entity.GetComponent<ECS::ControlRigComponent>().Data.Rig;
            if ( static_cast<uint64_t>( handle ) == 0 )
            {
                ImGui::TextDisabled( "No rig picked." );
                ImGui::TextWrapped( "Pick a .derig in the Control Rig section of Details. Until then this "
                                    "entity is posed by its clips alone." );
            }
            else
            {
                ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.35f, 1.0f ), "The rig did not build." );
                ImGui::TextWrapped( "The reason is in the log, once, naming the rig and the bone or control "
                                    "it could not resolve against this entity's skeleton." );
            }
            return;
        }

        Animation::ControlHierarchy& hierarchy = rig->GetHierarchy();

        auto&              authoring = Core::ActiveAuthoringContext();
        const Common::UUID entityId  = *sel;

        // THE PANEL SHOWS THIS ENTITY'S AUTHORING STATE OR NONE AT ALL. The context names the character
        // it is about, so a Control mode declared over a DIFFERENT character must not light this panel's
        // checkbox or highlight a row — the index would be an index into the other rig's hierarchy.
        const bool mine    = authoring.Entity() == entityId;
        bool       overlay = mine && authoring.ShowsControls();
        if ( ImGui::Checkbox( "Show controls in viewport", &overlay ) )
        {
            const Core::AuthoringMode wanted =
                 overlay ? Core::AuthoringMode::Control : Core::AuthoringMode::Object;
            Author( entityId, "the control overlay",
                    [&]( Core::AuthoringContext& context )
                    { return authoring.SetMode( m_AuthoringOwner, context, wanted ); } );
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "(?)" );
        if ( ImGui::IsItemHovered() )
        {
            ImGui::SetTooltip( "Draws each control's shape over the viewport and lets you grab it. Clicking "
                               "a control selects it here too." );
        }

        const bool rotate = mine && authoring.ControlRotate();
        if ( ImGui::RadioButton( "Translate", !rotate ) )
        {
            Author( entityId, "the manipulator mode",
                    [&]( Core::AuthoringContext& context )
                    { return authoring.SetControlRotate( m_AuthoringOwner, context, false ); } );
        }
        ImGui::SameLine();
        if ( ImGui::RadioButton( "Rotate", rotate ) )
        {
            Author( entityId, "the manipulator mode",
                    [&]( Core::AuthoringContext& context )
                    { return authoring.SetControlRotate( m_AuthoringOwner, context, true ); } );
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "(no scale)" );
        if ( ImGui::IsItemHovered() )
        {
            ImGui::SetTooltip( "A scale drag is not hard; a scale CONTROL is a question about what a scale "
                               "channel keys to, and shipping the drag before the answer would be a knob "
                               "whose meaning changes under it." );
        }

        if ( const std::string& structure = hierarchy.GetStructureError(); !structure.empty() )
        {
            // A RIG BUILT AGAINST ONE SKELETON AND EVALUATED AGAINST ANOTHER. Shown here because a
            // per-frame failure that exists only in a log line is a failure an editor cannot show.
            ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.35f, 1.0f ), "%s", structure.c_str() );
        }

        ImGui::Separator();
        ImGui::Text( "Controls (%zu)", hierarchy.Size() );

        uint32_t selected = mine ? authoring.SelectedControl().value_or( Animation::ControlHierarchy::INVALID )
                                 : Animation::ControlHierarchy::INVALID;
        if ( selected != Animation::ControlHierarchy::INVALID && selected >= hierarchy.Size() )
        {
            // The stage was rebuilt under the selection (the file changed, the slot was re-pointed, or the
            // mesh was swapped). An index into a hierarchy that no longer has it is the stale-handle defect
            // with a smaller name.
            Author( entityId, "forgetting a stale control",
                    [&]( Core::AuthoringContext& context )
                    { return authoring.SetSelectedControl( m_AuthoringOwner, context, std::nullopt ); } );
            selected = Animation::ControlHierarchy::INVALID;
        }

        if ( ImGui::BeginChild( "##controls", ImVec2( 0.0f, 160.0f ), true ) )
        {
            for ( uint32_t i = 0; i < static_cast<uint32_t>( hierarchy.Size() ); ++i )
            {
                const Animation::ControlElement& control = hierarchy.Get( i );
                const bool                       isSel   = ( i == selected );
                if ( ImGui::Selectable( control.Name.c_str(), isSel ) )
                {
                    Author( entityId, "the control selection",
                            [&]( Core::AuthoringContext& context )
                            { return authoring.SetSelectedControl( m_AuthoringOwner, context, i ); } );
                    selected = i;
                }
                if ( ImGui::IsItemHovered() && !control.ShapeName.empty() )
                {
                    // THE SIZE IS SHOWN AND NOT EDITED, and that is the honest shape of it. The shape
                    // transform is authored in the `.derig`; an in-panel drag would write a value that
                    // lives until the next load and then vanish, which is worse than no control at all.
                    // Shown because a control drawn too small looks exactly like a control that is not
                    // there, and the number is the one thing that tells them apart.
                    const glm::vec3& size = control.ShapeTransform.Scale;
                    ImGui::SetTooltip( "Shape: %s\nShape size (cm): %.3g, %.3g, %.3g", control.ShapeName.c_str(),
                                       size.x, size.y, size.z );
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();

        if ( selected == Animation::ControlHierarchy::INVALID )
        {
            ImGui::TextDisabled( "Select a control to see and edit its pose." );
        }
        else
        {
            const Animation::ControlElement& control = hierarchy.Get( selected );

            // THE POSE, EDITABLE — and it is the same value the viewport drag writes, through the same
            // setter. Two ways to move a control that wrote to two places would be the one-source-of-truth
            // defect with a keyboard on one side and a mouse on the other.
            Animation::BoneTransform pose        = control.Pose;
            glm::vec3                translation = pose.Translation;
            if ( ImGui::DragFloat3( "Translation (cm)", &translation.x, 0.5f ) )
            {
                pose.Translation = translation;
                if ( const auto ok = hierarchy.SetPose( selected, pose ); !ok )
                {
                    LOG_WARN( "[Animation] Control '{}' refused the pose: {}", control.Name, ok.GetError() );
                }
            }

            ImGui::Separator();
            ImGui::Text( "Parent spaces (%zu)", control.Parents.size() );
            for ( const Animation::ControlSpace& space : control.Parents )
            {
                // A lookup rather than nested ternaries: three kinds read as three rows, and the analyser
                // refuses a conditional inside a conditional in any case.
                const char* kind = "Component";
                if ( space.Kind == Animation::ControlSpaceKind::Bone )
                {
                    kind = "Bone";
                }
                else if ( space.Kind == Animation::ControlSpaceKind::Control )
                {
                    kind = "Control";
                }
                ImGui::BulletText( "%s [%u]  weight %.2f", kind, space.Index, space.Weight );
            }
        }

        ImGui::Separator();
        ImGui::Text( "Drives (%zu)", rig->GetDrives().size() );
        ImGui::TextDisabled( "Each row is one control's transform becoming one bone's." );
        for ( const Animation::ControlBoneDrive& drive : rig->GetDrives() )
        {
            // "(gone)" cannot be reached while the stage is the one the drives were sorted against; it is
            // written rather than asserted because this panel draws every frame and a crash in a label is
            // the worst possible way to report an index that has gone stale.
            const char* name = "(gone)";
            if ( drive.Control < hierarchy.Size() )
            {
                name = hierarchy.Get( drive.Control ).Name.c_str();
            }
            ImGui::BulletText( "%s -> bone %u", name, drive.Bone );
        }

        if ( const std::string& last = rig->GetLastError(); !last.empty() )
        {
            ImGui::Separator();
            ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.35f, 1.0f ), "%s", last.c_str() );
        }
    }

    bool ControlRigPanel::IsRelevant() const
    {
        return SelectionHas<ECS::ControlRigComponent>( m_Scene );
    }
} // namespace Desert::Editor
