#include "TransformComponentWidget.hpp"
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/Controls/Controls.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>

#include <ImGui/imgui.h>
#include <glm/gtc/constants.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    TransformComponentWidget::TransformComponentWidget() : IComponentWidget( "Transform" )
    {
    }

    void TransformComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* /*scene*/ )
    {
        auto& transform = entity.GetComponent<ECS::TransformComponent>();

        // A directional light has no meaningful position — its transform IS a direction, and the dial
        // says so far better than three numbers.
        if ( entity.HasComponent<ECS::DirectionLightComponent>() )
        {
            ImGui::Dummy( ImVec2( 0, 4 ) );
            Widgets::DrawDirectionWidget( "Direction", transform.Translation );
            return;
        }

        // UE's three rows, on the panel's shared grid and with its own names: Location / Rotation /
        // Scale, each an axis-coloured vector field. The label column is the panel's, so the transform
        // lines up with every other component instead of owning a 100px column of its own.
        Utils::ImGuiUtilities::ResetPropertyRows();

        Utils::ImGuiUtilities::BeginPropertyRow( "Location", "World position in centimetres" );
        Utils::ImGuiUtilities::VectorField( "loc", &transform.Translation.x, 3, 0.5f, "%.1f" );
        Utils::ImGuiUtilities::EndPropertyRow();

        // Stored in RADIANS (TransformComponent feeds glm::quat directly) but shown in degrees with the
        // degree sign, like UE and like the Sequencer's key editor — nobody authors a rotation in radians.
        Utils::ImGuiUtilities::BeginPropertyRow( "Rotation", "Euler angles in degrees" );
        glm::vec3 degrees = glm::degrees( transform.Rotation );
        if ( Utils::ImGuiUtilities::VectorField( "rot", &degrees.x, 3, 0.5f, "%.1f\xc2\xb0" ) )
            transform.Rotation = glm::radians( degrees );
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Scale", "Multiplier per axis (1 = the mesh's own size)" );
        Utils::ImGuiUtilities::VectorField( "scale", &transform.Scale.x, 3, 0.01f, "%.3f" );
        Utils::ImGuiUtilities::EndPropertyRow();
    }

    DESERT_REGISTER_CUSTOM_COMPONENT(
         ECS::TransformComponent, "Transform", false,
         ( []( ECS::Entity& e, ::Desert::Core::Scene* s, const ComponentEditContext& )
           { TransformComponentWidget().Render( e, s ); } ) )

} // namespace Desert::Editor