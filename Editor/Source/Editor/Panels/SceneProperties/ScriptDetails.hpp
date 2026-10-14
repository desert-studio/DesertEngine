#pragma once

namespace Desert::ECS
{
    class Entity;
}
namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    struct ComponentEditContext;

    // THE SCRIPT COMPONENT IN DETAILS — the customisation (UE: an IDetailCustomization) of ECS::ScriptComponent.
    // One section per script slot, the way Unity's Inspector gives every MonoBehaviour its own header:
    //   * the header: the script's name, its status (Ready / Running / Error / Not found) and its remove button;
    //   * the script itself: the picker, the path, "Open in code editor", Reload, the comment it opens with, and
    //     an error with its line and a button that opens the code editor AT that line;
    //   * its properties, drawn from what the script DECLARES (Scripting::ScriptSchema — the same metadata
    //     vocabulary a C++ PROPERTY carries): a slider for a range, a colour picker, an asset slot with its
    //     picture, an entity slot with the eyedropper, an enum combo, an array with + / - / drag to reorder,
    //     collapsible categories, EditCondition greying, tooltips, and the reset arrow — a property only holds
    //     a value on the entity while it differs from the script's default;
    //   * its CallInEditor functions as buttons.
    // Every edit is one undo step in the edit world; in a played world it reaches the running script on its
    // next frame (ScriptSystem re-applies the slot's values every frame).
    void DrawScriptComponent( ECS::Entity& entity, ::Desert::Core::Scene* scene, const ComponentEditContext& context );
} // namespace Desert::Editor
