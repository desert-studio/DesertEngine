#include "StartupLayout.hpp"

#include <algorithm>
#include <vector>

namespace Desert::Project
{
    namespace
    {
        namespace fs = std::filesystem;

        // HOW FAR UP TO WALK, and why there is a limit at all. A development executable sits three
        // directories under the root (`<root>/build/Bin/<Config>/Editor`), so three would do for the
        // layout this repository builds. The limit is larger than that because a checkout can be
        // built somewhere else, and it is FINITE because the alternative — walking to `/` — makes the
        // answer depend on directories that have nothing to do with this program: one stray
        // `Templates/` plus `scripts/` anywhere above `$HOME` would register somebody's home
        // directory as an engine.
        constexpr int kMaxAncestorsSearched = 8;

        // THE TWO NAMES THAT MAKE A DIRECTORY AN ENGINE ROOT — see the header for who reads each and
        // what they do with it. Both, not either: a folder with one of them is not a root.
        bool IsEngineRoot( const fs::path& directory )
        {
            std::error_code ec;
            if ( !fs::is_directory( directory / "Templates", ec ) )
                return false;
            return fs::is_directory( directory / "scripts", ec );
        }
    } // namespace

    EngineRootLookup DeriveEngineRoot( const fs::path& executable )
    {
        EngineRootLookup lookup;

        if ( executable.empty() )
        {
            // Common::Utils::FileSystem::ExecutablePath() returns an empty path when the platform
            // call fails. Saying that outright beats reporting it as "no engine tree above ''",
            // which sends the reader looking at directories when the fault is in the process.
            lookup.Explanation = "this process could not find out where its own executable is, so it "
                                 "could not work out which engine it belongs to and did not record "
                                 "itself in engines.json - the launcher will not list it";
            return lookup;
        }

        std::error_code ec;
        // weakly_canonical rather than absolute: the executable may be reached through a symlink
        // (a `build/Bin/Release/Editor` handed a shortcut), and walking up from the LINK lands in
        // the directory holding the link rather than in the tree the binary belongs to.
        fs::path directory = fs::weakly_canonical( executable, ec ).parent_path();
        if ( ec || directory.empty() )
            directory = executable.parent_path();

        const fs::path started = directory;
        for ( int step = 0; step < kMaxAncestorsSearched; ++step )
        {
            if ( IsEngineRoot( directory ) )
            {
                lookup.Root = directory.string();
                return lookup;
            }
            const fs::path parent = directory.parent_path();
            if ( parent.empty() || parent == directory )
                break;
            directory = parent;
        }

        // NOT AN ERROR, AND THE WORDING HAS TO SAY SO. This is the ordinary state of every
        // downloaded drop, and the message it replaced told the reader to run
        // `scripts/Windows/RunEditor.bat` — a file that is not in a drop, so the instruction could
        // not be followed and read as a broken build. What this says instead is the true
        // consequence (the launcher will not list this copy), the reason (it is not a checkout), and
        // the one way to override it.
        lookup.Explanation =
             fmt::format( "this copy is not part of an engine checkout - no directory holding both "
                          "Templates/ and scripts/ was found above '{}' - so it did not record itself in "
                          "engines.json and the launcher will not list it. That is expected for a "
                          "downloaded build: the launcher starts an engine from a checkout, and this "
                          "editor runs perfectly well on its own. Set DESERT_ROOT to a checkout to "
                          "override.",
                          started.string() );
        return lookup;
    }

    Common::ResultStr<std::string> ProjectBesideExecutable( const fs::path& directory )
    {
        std::error_code ec;
        if ( !fs::is_directory( directory, ec ) )
            return Common::MakeFormattedError<std::string>(
                 "No project given, and '{}' - the directory holding this executable - is not a "
                 "directory this process can read. Pass: --project <path/to/.deproj>",
                 directory.string() );

        std::vector<std::string> descriptors;
        for ( fs::directory_iterator it( directory, fs::directory_options::skip_permission_denied, ec ), end;
              !ec && it != end; it.increment( ec ) )
        {
            if ( it->is_directory( ec ) )
                continue;
            // `.deproj` compared through path::extension() rather than by string suffix: on Windows
            // the entry's own spelling may differ in case, and a suffix compare would also accept a
            // file literally named ".deproj" with no stem.
            if ( it->path().extension() != ".deproj" || it->path().stem().empty() )
                continue;
            descriptors.push_back( it->path().string() );
        }

        // Sorted so that a refusal lists the same names in the same order on every platform and in
        // every filesystem's own iteration order — a message that reorders itself between runs reads
        // as a different failure each time.
        std::sort( descriptors.begin(), descriptors.end() );

        if ( descriptors.size() == 1 )
            return Common::MakeSuccess( std::move( descriptors.front() ) );

        if ( descriptors.empty() )
            return Common::MakeFormattedError<std::string>(
                 "No project given, and no .deproj file sits beside this executable (looked in '{}'). A "
                 "packaged engine build carries its project there; a development build does not, and its "
                 "run script passes the flag. Pass: --project <path/to/.deproj>",
                 directory.string() );

        std::string names;
        for ( const std::string& one : descriptors )
        {
            if ( !names.empty() )
                names += ", ";
            names += fs::path( one ).filename().string();
        }
        // NOT "take the first one". Two descriptors beside one executable is a question this process
        // has no way to answer, and answering it anyway would open a project the person is not
        // looking at while everything downstream - the title bar, the recent list, the scene - agreed
        // with each other and with nobody.
        return Common::MakeFormattedError<std::string>(
             "No project given, and '{}' holds {} .deproj files ({}) - which of them is meant is not "
             "something this can work out. Pass: --project <path/to/.deproj>",
             directory.string(), descriptors.size(), names );
    }

    ResourceRootLookup ResolveResourceRoot( const fs::path& workingDirectory,
                                            const fs::path& executableDirectory )
    {
        ResourceRootLookup lookup;

        std::error_code ec;
        const fs::path   marker = fs::path( "Resources" ) / "Shaders";

        if ( fs::is_directory( workingDirectory / marker, ec ) )
            return lookup; // nothing moves - this is every existing launch

        if ( !executableDirectory.empty() && fs::is_directory( executableDirectory / marker, ec ) )
        {
            lookup.WorkingDirectory = executableDirectory.string();
            return lookup;
        }

        lookup.Explanation =
             fmt::format( "the engine resources are missing: no 'Resources/Shaders' under the working "
                          "directory '{}', and none beside the executable ('{}'). A packaged build keeps "
                          "Resources/ next to its binaries and a checkout keeps it in Editor/; without it "
                          "there are no shaders, no fonts and no icons, so this stops here rather than "
                          "opening a window that can draw nothing.",
                          workingDirectory.string(),
                          executableDirectory.empty() ? std::string( "unknown" )
                                                      : executableDirectory.string() );
        return lookup;
    }
} // namespace Desert::Project
