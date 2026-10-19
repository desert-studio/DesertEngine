#include "Editor/Core/PlayKeptChanges.hpp"

#include "Editor/Core/Commands/SceneCommands.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>

#include <utility>
#include <vector>

namespace Desert::Editor::PlayKeptChanges
{
    namespace
    {
        struct KeptChange
        {
            Common::UUID Entity;
            std::string  Key;
            Apply        Run;
        };

        // The editor's one Play session owns this list; it lives here rather than on PlaySession because
        // the widgets that keep a change (Details, the Clouds window) are drawn without a reference to it.
        std::vector<KeptChange>& Changes()
        {
            static std::vector<KeptChange> changes;
            return changes;
        }
    } // namespace

    void Keep( const Common::UUID& entity, std::string key, Apply apply )
    {
        for ( KeptChange& kept : Changes() )
        {
            if ( kept.Entity == entity && kept.Key == key )
            {
                kept.Run = std::move( apply );
                return;
            }
        }
        Changes().push_back( { entity, std::move( key ), std::move( apply ) } );
    }

    void ApplyTo( Core::Scene& restored )
    {
        // Moved out first: an apply that kept another change (it should not) must not grow the list the
        // loop walks.
        std::vector<KeptChange> changes = std::move( Changes() );
        Changes().clear();

        for ( KeptChange& kept : changes )
        {
            const auto found = restored.FindEntityByID( kept.Entity );
            if ( !found )
            {
                LOG_WARN( "[Play] '{}' kept for entity {} was dropped: the authored world has no such entity",
                          kept.Key, kept.Entity.ToString() );
                continue;
            }
            ECS::Entity entity = found->get();
            Commands::MutateEntityUndoable( kept.Entity, [&] { kept.Run( entity ); } );
            LOG_INFO( "[Play] kept '{}' on '{}' past Stop", kept.Key, kept.Entity.ToString() );
        }
    }

    void Clear()
    {
        Changes().clear();
    }
} // namespace Desert::Editor::PlayKeptChanges
