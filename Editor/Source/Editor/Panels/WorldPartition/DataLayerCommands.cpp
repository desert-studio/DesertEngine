#include "DataLayerCommands.hpp"

#include <Editor/Core/DataLayerEditorSubsystem.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <format>
#include <functional>
#include <string>

namespace Desert::Editor
{
    namespace
    {
        // The command runs against the world it was listed for; a world closed since refuses by name.
        using WorldVerb = std::function<Common::BoolResultStr( const std::shared_ptr<::Desert::Core::Scene>& )>;

        PaletteCommand MakeCommand( std::string label, const std::weak_ptr<::Desert::Core::Scene>& world,
                                    WorldVerb verb )
        {
            return { "Scene", std::move( label ), [world, verb = std::move( verb )]() -> Common::BoolResultStr
                     {
                         const std::shared_ptr<::Desert::Core::Scene> scene = world.lock();
                         if ( !scene )
                             return Common::MakeError<bool>(
                                  std::string( "Data Layers: the world this command was listed for is closed." ) );
                         return verb( scene );
                     } };
        }

        Common::BoolResultStr Counted( const Common::ResultStr<std::size_t>& outcome )
        {
            if ( !outcome )
                return Common::MakeError<bool>( outcome.GetError() );
            return Common::MakeSuccess( outcome.GetValue() > 0 );
        }
    } // namespace

    void AppendDataLayerCommands( std::vector<PaletteCommand>&                  commands,
                                  const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        if ( !scene || !scene->GetWorldPartition().has_value() ||
             !scene->GetWorldPartition()->DataLayers.has_value() )
            return;
        const std::weak_ptr<::Desert::Core::Scene> world = scene;
        for ( const ::Desert::Core::DataLayerSerialized& layer : *scene->GetWorldPartition()->DataLayers )
        {
            const std::string name = layer.Name;
            commands.push_back( MakeCommand( std::format( "Data Layers: Make Current {}", name ), world,
                                             [name]( const auto& at )
                                             { return DataLayerEditor::SetCurrent( at, name, true ); } ) );
            commands.push_back( MakeCommand( std::format( "Data Layers: Remove {} from Current", name ), world,
                                             [name]( const auto& at )
                                             { return DataLayerEditor::SetCurrent( at, name, false ); } ) );
            commands.push_back( MakeCommand( std::format( "Data Layers: Add Selected to {}", name ), world,
                                             [name]( const auto& at ) {
                                                 return Counted( DataLayerEditor::AddEntities(
                                                      *at, Core::SelectionManager::GetSelection(), name ) );
                                             } ) );
            commands.push_back( MakeCommand( std::format( "Data Layers: Remove Selected from {}", name ), world,
                                             [name]( const auto& at ) {
                                                 return Counted( DataLayerEditor::RemoveEntities(
                                                      *at, Core::SelectionManager::GetSelection(), name ) );
                                             } ) );
            commands.push_back( MakeCommand(
                 std::format( "Data Layers: Select Actors in {}", name ), world,
                 [name]( const auto& at ) { return Counted( DataLayerEditor::SelectMembers( *at, name ) ); } ) );
            commands.push_back( MakeCommand( std::format( "Data Layers: Hide {}", name ), world,
                                             [name]( const auto& at )
                                             { return DataLayerEditor::SetVisible( at, name, false ); } ) );
            commands.push_back( MakeCommand( std::format( "Data Layers: Show {}", name ), world,
                                             [name]( const auto& at )
                                             { return DataLayerEditor::SetVisible( at, name, true ); } ) );
        }
    }
} // namespace Desert::Editor
