#include "CloudCommands.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Panels/Clouds/CloudSkyCapture.hpp>
#include <Editor/Panels/Clouds/CloudStages.hpp>
#include <Editor/Panels/Clouds/CloudsPanel.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>

#include <cstdint>
#include <format>
#include <string>

namespace Desert::Editor
{
    void AppendCloudCommands( std::vector<PaletteCommand>&                  commands,
                              const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        // THE SIX STAGES OF THE SKY, each as a command that opens the Clouds window ON that stage.
        //
        // Generated from the enum rather than typed, so a seventh stage is offered here the moment it
        // exists and cannot be forgotten — CloudStageName has no `default:`, which is what makes that safe.
        // They are the same CloudsPanel::OpenAt the Details panel's two buttons call, so a person with
        // Ctrl+P and a client on the control channel reach the window exactly the way the button does.
        for ( uint32_t i = 0; i < kCloudStageCount; ++i )
        {
            const auto stage = static_cast<CloudStage>( i );
            commands.push_back( { "Clouds", std::format( "{} {}", i + 1, CloudStageName( stage ) ), [stage]
                                  {
                                      CloudsPanel::OpenAt( stage );
                                      return PaletteCommandDone();
                                  } } );
        }

        // ON THE SELECTION, as the Details button is on the entity Details shows: one selected entity, the
        // layer whose sky is captured. Anything else is refused by name rather than guessed.
        commands.push_back( { "Clouds", "Capture current sky", [scene]() -> Common::BoolResultStr
                              {
                                  if ( !scene )
                                      return Common::MakeError<bool>( "Capture current sky: no scene is open" );
                                  const auto& selection = Core::SelectionManager::GetSelection();
                                  if ( selection.size() != 1 )
                                      return Common::MakeError<bool>( std::format(
                                           "Capture current sky: select the one cloud layer entity ({} selected)",
                                           selection.size() ) );
                                  const auto found = scene->FindEntityByID( selection.front() );
                                  if ( !found )
                                      return Common::MakeError<bool>( std::format(
                                           "Capture current sky: the selected {} is not in the scene",
                                           selection.front().ToString() ) );
                                  ECS::Entity layer = found->get();
                                  return CaptureCurrentSky( *scene, layer );
                              } } );
    }
} // namespace Desert::Editor
