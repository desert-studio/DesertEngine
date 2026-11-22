#pragma once

#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>

#include <memory>
#include <Engine/ECS/UUIDComponent.hpp>

namespace Desert::Editor
{
    // Shared relevance test for CONTEXTUAL panels (IPanel::IsContextual): does the current selection in
    // `scene` carry component T? Keeps every such panel's IsRelevant() a one-liner instead of repeating
    // the selection -> entity -> component dance.
    template <typename T>
    bool SelectionHas( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        if ( !scene )
            return false;
        const auto& sel = Core::SelectionManager::GetSelected();
        if ( !sel )
            return false;
        auto ref = scene->FindEntityByID( *sel );
        return ref && ref->get().HasComponent<T>();
    }

    // THE ENTITY'S SERIALIZED IDENTITY — the id a component-subject document is keyed on
    // (Editor/Core/EditorSubject.hpp), and the same one SelectionManager and Scene::FindEntityByID speak.
    //
    // GUARDED, and the guard is not paranoia. Scene::CreateNewEntity attaches a UUIDComponent, but nothing
    // in the type system says an entity has one, and EntitySerializer already has to cope with one that
    // does not. An UNGUARDED GetComponent here would be an entt assertion on a path whose whole purpose is
    // to answer "which entity is this?" — the null UUID is the honest answer, and a subject built from it
    // is refused by SubjectEditorRegistry::Create by name rather than opening a window over nothing.
    [[nodiscard]] inline Common::UUID EntityId( const ECS::Entity& entity )
    {
        if ( !entity.HasComponent<ECS::UUIDComponent>() )
            return Common::UUID::Null();
        return entity.GetComponent<ECS::UUIDComponent>().UUID;
    }
} // namespace Desert::Editor
