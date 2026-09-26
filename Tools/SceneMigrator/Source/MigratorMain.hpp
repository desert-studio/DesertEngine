#pragma once

// The WHOLE tool behind a callable signature. The migrations themselves are pure functions and have
// been testable since they moved here — but the tool's FILE loop (collect, parse, report, write
// back, exit code) lived inside main() and was compiled by nothing but the tool, so the one part of
// SceneMigrator that can destroy data was the one part no suite could reach. It did destroy data in
// principle: the write opened the scene itself with trunc, so any failure after the open cost the
// file (see the write site). Tests/Tools/SceneMigratorWritePath drives this function against a write
// that must fail and pins "non-zero exit, original untouched".
//
// The streams are parameters for the same reason the assets root is one in MigrateScene: the caller
// owns them, and a test can read the report back instead of scraping a process's stdout.

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace Desert::Migration
{
    // WHERE A SCENE'S OUTPUTS GO — derived from the SCENE'S OWN PATH, never from the working directory
    // the tool happens to be launched from.
    //
    // THE DEFECT THIS IS. The v11 -> v12 raise creates a `.demat` beside the scene and writes its
    // assets-root-relative name into the scene. The write site used to resolve that name against
    // `Constants::Path::ASSETS_PATH`, which with no project open is the relative `Resources/Assets/` —
    // i.e. it resolved against the CURRENT DIRECTORY. Run from the repository root over
    // `Editor/Resources/Assets/Scenes/Autosave/X.desce`, it created a brand-new `Resources/Assets/`
    // tree AT THE REPOSITORY ROOT and put the material there, while the scene named it relative to the
    // root it actually lives under. Two DIFFERING files, one name, one relative path, two roots — and
    // which one the engine loads decided by where somebody stood when they ran a tool.
    //
    // THE RELATION, which is what is fixed rather than the site: the file a migration produces and the
    // path it writes into the scene must be measured against ONE root, and that root is a property of
    // the scene, not of the process. `Common::Constants::Path::RootForContentPath` answers it by
    // reading the same census row the engine's own `Dir(ContentDir::Scene)` is derived from, so there
    // is no second spelling of the layout here to drift from it.
    //
    // A scene that is NOT inside a `Scenes/` folder — a fixture in a temp directory, a file handed over
    // by hand — has no census answer, and its own directory is used. That is not a fallback to the
    // working directory in disguise: it still says "beside the scene", it is still derived from the
    // scene's path, and for a bare `x.desce` the scene's directory IS the working directory, which is
    // then the honest answer rather than an accident.
    //
    // LEXICAL and pure: no disk is consulted, so the answer is the same for a scene about to be written
    // as for one that exists, and it cannot change under a concurrent run.
    std::filesystem::path SceneOutputRoot( const std::filesystem::path& scenePath );

    // The same sentence for a `.deprefab`, read off the census row for `Prefabs/` instead of `Scenes/`.
    // A prefab needs one for the same two reasons a scene does: the v16 -> v17 step measures references
    // against the assets root, and the v11 -> v12 step can produce a `.demat` that has to land under the
    // root the file it is named from actually lives beneath.
    std::filesystem::path PrefabOutputRoot( const std::filesystem::path& prefabPath );

    // WHAT A DIRECTORY WALK DOES NOT ENTER — one list, each entry with its reason, and every skip is printed
    // ("skip   <path> — <reason>") so nothing is left out silently. The tool is run over the whole repository,
    // and the repository holds files that share our extensions but are not our content: assimp's test corpus
    // carries Quake 3 `.shader` scripts and Ogre `.skeleton` files, which the shader pass would "raise" and the
    // skeleton pass refuses. An entry is a path of one or more components matched against the TRAILING
    // components of each directory or file met below a scanned root, so "ThirdParty" names every directory of
    // that name at any depth (the root's own and Editor's), and a file entry would name one file. A path given
    // on the command line is never filtered: whoever names a file asked for that file.
    struct ScanExclusion
    {
        const char* Path;
        const char* Reason;
    };
    std::span<const ScanExclusion> ScanExclusions();

    // `args` is the command line without argv[0]: any mix of "--check" and paths (a .desce, .demat or
    // .deprefab file, or a directory searched recursively). Returns the process exit code: 0 = nothing
    // to do or all raised and written; 1 = a file failed (unreadable, unparseable, at a generation this
    // build has no route from, or its write failed — the original is left untouched in every case), or
    // --check found work; 2 = usage / nothing found.
    int RunSceneMigrator( const std::vector<std::string>& args, std::ostream& out, std::ostream& err );
} // namespace Desert::Migration
