#include "LanguageCommands.hpp"

#include <Engine/Localization/LocalizationService.hpp>

#include <string>

namespace Desert::Editor
{
    void AppendLanguageCommands( std::vector<PaletteCommand>& commands )
    {
        // LANGUAGE — one entry per language the compiled locale table knows, generated from the table so
        // a language added there is offered here the moment it exists and cannot be forgotten.
        //
        // OFFERED FOR EVERY KNOWN LANGUAGE, not only the ones this project has strings in, and that is
        // deliberate: switching to a language with no translations is how an author SEES what is missing
        // (every keyed label draws its own key, and the log names each one). Hiding the entry would hide
        // the hole.
        //
        // It is also the only way a language change can be photographed on this machine, where synthetic
        // input is closed at the OS: the control channel runs these entries, so `run` then `shot.window`
        // captures the switched interface in one session.
        for ( const Localization::LocaleRow& row : Localization::Locales() )
        {
            const std::string tag = std::string( row.Tag );
            commands.push_back( { "Language", tag + " - " + std::string( row.Endonym ),
                                  [tag] { return Localization::Localization::Get().SetLanguage( tag ); } } );
        }
    }
} // namespace Desert::Editor
