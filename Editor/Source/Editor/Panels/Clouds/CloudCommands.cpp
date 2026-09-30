#include "CloudCommands.hpp"

#include <Editor/Panels/Clouds/CloudStages.hpp>
#include <Editor/Panels/Clouds/CloudsPanel.hpp>

#include <cstdint>
#include <string>

namespace Desert::Editor
{
    void AppendCloudCommands( std::vector<PaletteCommand>& commands )
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
            commands.push_back( { "Clouds", std::to_string( i + 1 ) + " " + CloudStageName( stage ), [stage]
                                  {
                                      CloudsPanel::OpenAt( stage );
                                      return PaletteCommandDone();
                                  } } );
        }
    }
} // namespace Desert::Editor
