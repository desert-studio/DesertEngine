#include "UICommands.hpp"

#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>
#include <Engine/Core/Scene.hpp>

namespace Desert::Editor
{
    void AppendUICommands( std::vector<PaletteCommand>& commands,
                           const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        commands.push_back( { "View", "Toggle 2D UI mode", [&scene]
                              {
                                  // REFUSES RATHER THAN DOING NOTHING when there is no scene. The mode is
                                  // a property OF a scene, so "there is none" is a fact the caller has to
                                  // hear — over the channel it used to come back as a plain success.
                                  if ( !scene )
                                      return Common::MakeError<bool>( "there is no open scene to switch "
                                                                      "into 2D UI mode." );
                                  Editor::ViewportPanel::ToggleUIMode( *scene );
                                  return PaletteCommandDone();
                              } } );

    }
} // namespace Desert::Editor
