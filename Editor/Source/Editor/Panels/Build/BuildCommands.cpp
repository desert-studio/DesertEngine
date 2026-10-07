#include "BuildCommands.hpp"

#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Packaging/GamePackager.hpp>
#include <Editor/Packaging/ProjectChunkScheme.hpp>

namespace Desert::Editor
{
    void AppendBuildCommands( std::vector<PaletteCommand>& commands )
    {
        // ── BUILD: THE ONE ACTION WHOSE PRODUCT A STRANGER RUNS, AND IT WAS UNREACHABLE ────────────────
        //
        // `PackageGame` had exactly one caller in this repository — a button in the Build Settings panel
        // — so the only way to produce a game was a mouse click. On this machine synthetic input is
        // closed at both doors (osascript and CGEventPost, measured), and the control channel's own
        // promise is that "anything a human can do, it can do": packaging was the counter-example. The
        // consequence was not theoretical. The packager has been in the tree for weeks, its output has
        // unit tests over temp fixtures, and NOBODY HAD EVER STARTED THE GAME IT PRODUCES — every
        // verification of the runtime, twenty-five runs of it, was done against loose files on disk,
        // which is the developer's path and not the player's.
        //
        // SYNCHRONOUS, unlike the panel's button, which submits to a JobSystem worker and paints a
        // spinner. A palette entry's contract is that its `Run` RETURNS the outcome (see
        // CommandPalette.hpp): the channel turns that into a refusal a script can stop on. Handing the
        // work to a worker would mean returning success the instant it was queued — the exact
        // "failure reads as success" the return type was introduced to end. The cost is a frame that
        // lasts as long as the cook does, which for an explicit "build me a game" is the honest
        // behaviour rather than a surprise.
        //
        // THE THIRD STATE IS A REFUSAL HERE, and that is a choice this entry is allowed to make where
        // `PackageResult` is not. `Complete()` exists because a package with unbaked content is neither
        // success nor failure (GamePackager.hpp says why at length, and the panel paints it amber). A
        // palette command has two outcomes and no third colour, so an incomplete package reports the
        // counts as an error string: an unattended caller that got "ok" for a package with a missing
        // font would ship it.
        commands.push_back(
             { "Build", "Package Game", []() -> Common::BoolResultStr
               {
                   const auto&    prefs = EditorPreferences::Get();
                   PackageOptions options;
                   options.OutputDir    = prefs.PackageOutputDir;
                   options.Config       = prefs.PackageConfig;
                   options.MacAppBundle = prefs.PackageAppBundle;

                   const PackageResult result = PackageGame( options );
                   if ( !result.Success )
                       return Common::MakeError<bool>( result.Message );
                   if ( !result.Complete() )
                   {
                       return Common::MakeFormattedError<bool>(
                            "packaged to '{}', but {} item(s) would not cook and {} artifact(s) never "
                            "reached the disk — the game will rebuild them on the player's machine at "
                            "every start.",
                            result.PackageDir, result.CookFailures, result.CookUnwritten );
                   }
                   return Common::MakeSuccess( true );
               } } );

        // The Build Settings panel's "Create default ContentChunks.json", reachable by a script: the
        // packager refuses a project with no chunk scheme, and the way out must not need a mouse.
        commands.push_back( { "Build", "Create Default ContentChunks.json", []() -> Common::BoolResultStr
                              { return Editor::ProjectChunkScheme().CreateDefault(); } } );
        // Re-reads the file into the one session, for a scheme edited outside the editor.
        commands.push_back( { "Build", "Reload ContentChunks.json", []() -> Common::BoolResultStr
                              {
                                  Editor::ProjectChunkScheme().Reload();
                                  return Common::MakeSuccess( true );
                              } } );
    }
} // namespace Desert::Editor
