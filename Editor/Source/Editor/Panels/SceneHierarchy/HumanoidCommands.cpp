#include "HumanoidCommands.hpp"

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp>
#include <filesystem>
#include <format>
#include <string>
#include <vector>
#include <Engine/Geometry/ProceduralCharacterFactory.hpp>
#include <Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp>

namespace Desert::Editor
{
    void AppendHumanoidCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        // The outliner's Add > Animation > Character (Procedural), through the same spawn.
        commands.push_back( { "Scene", "Spawn Procedural Humanoid", [&scene]
                              {
                                  if ( !scene )
                                      return PaletteCommandOutcome( false, "no scene is open" );
                                  (void)Editor::SceneHierarchyPanel::SpawnProceduralHumanoid( *scene );
                                  return PaletteCommandDone();
                              } } );
        // The humanoid's mesh and clips are engine content GENERATED from the factory: run once, commit the files.
        commands.push_back( { "Tools", "Generate Humanoid Engine Assets", []
                              {
                                  const auto written = Geometry::ProceduralCharacterFactory::WriteEngineAssets();
                                  if ( !written )
                                      return PaletteCommandOutcome( false, written.GetError() );
                                  for ( const auto& file : written.GetValue() )
                                      LOG_INFO( "[Humanoid] wrote engine asset '{}'", file.generic_string() );
                                  return PaletteCommandDone();
                              } } );
    }
} // namespace Desert::Editor
