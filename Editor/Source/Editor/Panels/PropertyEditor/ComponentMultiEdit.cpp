#include "ComponentWidgetRegistry.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/UUIDComponent.hpp>

namespace Desert::Editor
{
    std::vector<void*> GatherSelectionFieldPtrs(
         ::Desert::Core::Scene* scene, const ECS::Entity& primary,
         const std::function<void*( const ECS::Entity& )>& fieldPtrOrNull )
    {
        std::vector<void*> out;

        const auto& selection = Core::SelectionManager::GetSelection();
        if ( !scene || selection.size() <= 1 )
            return out; // single selection → nothing to broadcast to

        const Common::UUID primaryId = primary.GetComponent<ECS::UUIDComponent>().UUID;
        for ( const auto& uuid : selection )
        {
            if ( uuid == primaryId )
                continue;
            const auto other = scene->FindEntityByID( uuid );
            if ( !other )
                continue;
            if ( void* fieldPtr = fieldPtrOrNull( other->get() ) )
                out.push_back( fieldPtr );
        }
        return out;
    }
} // namespace Desert::Editor
