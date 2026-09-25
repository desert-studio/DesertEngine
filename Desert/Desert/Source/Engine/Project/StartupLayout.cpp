#include "StartupLayout.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace Desert::Project
{
    namespace
    {
        namespace fs = std::filesystem;

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
        // weakly_canonical rather than absolute: the executable may be reached through a symlink,
        // and the shape below is a statement about the REAL directories the binary sits in.
        fs::path resolved = fs::weakly_canonical( executable, ec );
        if ( ec || resolved.empty() )
            resolved = executable;

        // THE EXACT SHAPE, AND NOT A WALK UPWARDS. This used to climb ancestors looking for the two
        // markers, and the first end-to-end run measured what is wrong with that: a drop unzipped
        // INSIDE a checkout (dist/DesertEngine-Release, three directories under the worktree) found
        // the worktree and registered it. Every ancestor test passed and the answer was still wrong,
        // because the question is not "is there an engine above me" — it is "is there a root the
        // launcher can start THIS binary from", and the launcher starts exactly one path:
        // `<root>/build/Bin/<config>/Editor[.exe]`, directly on Windows and through
        // `<root>/scripts/MacOS/RunEditor.sh` (which runs the same file) on macOS.
        //
        // So the relation is checked rather than searched for. A binary that is not at that path is
        // not one the launcher can start, however much of an engine sits above it — and it does not
        // register, which is the whole of what the drop needed.
        const fs::path configDirectory = resolved.parent_path(); // <config>
        const fs::path binDirectory    = configDirectory.parent_path();
        const fs::path buildDirectory  = binDirectory.parent_path();
        const fs::path root            = buildDirectory.parent_path();

        if ( binDirectory.filename() == "Bin" && buildDirectory.filename() == "build" && IsEngineRoot( root ) )
        {
            lookup.Root = root.string();
            return lookup;
        }

        // NOT AN ERROR, AND THE WORDING HAS TO SAY SO. This is the ordinary state of every
        // downloaded drop, and the message it replaced told the reader to run
        // `scripts/Windows/RunEditor.bat` — a file that is not in a drop, so the instruction could
        // not be followed and read as a broken build. What this says instead is the true
        // consequence (the launcher will not list this copy), the reason, and the one way to
        // override it — which is the environment variable's remaining legitimate job.
        lookup.Explanation =
             fmt::format( "this build is not one the launcher can start: it would run "
                          "<root>/build/Bin/<config>/Editor out of a checkout holding Templates/ and "
                          "scripts/, and this executable is '{}'. So it did not record itself in "
                          "engines.json and the launcher will not list it. That is expected for a "
                          "downloaded build, which runs perfectly well on its own. Set DESERT_ROOT to a "
                          "checkout to override.",
                          resolved.string() );
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
            // LOWER-CASED BEFORE COMPARING, because `path::extension() != ".deproj"` is a
            // case-SENSITIVE string compare on every platform — including the one whose filesystem
            // is not. A descriptor saved as `.DEPROJ` on Windows opens fine by name and would have
            // been invisible here, which is the "no project beside this executable" message for a
            // project that is sitting right there.
            //
            // `stem().empty()` on purpose too: a file literally named `.deproj` has that extension
            // and no name, and it is not a project.
            std::string extension = it->path().extension().string();
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char ch ) { return static_cast<char>( ::tolower( ch ) ); } );
            if ( extension != ".deproj" || it->path().stem().empty() )
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

    ResourceRootLookup ResolveResourceRoot( const fs::path& workingDirectory, const fs::path& executableDirectory )
    {
        ResourceRootLookup lookup;

        std::error_code ec;
        const fs::path  marker = fs::path( "Resources" ) / "Shaders";

        if ( fs::is_directory( workingDirectory / marker, ec ) )
            return lookup; // nothing moves - this is every existing launch

        if ( !executableDirectory.empty() && fs::is_directory( executableDirectory / marker, ec ) )
        {
            lookup.WorkingDirectory = executableDirectory.string();
            return lookup;
        }

        // THE CHECKOUT THIS BINARY WAS BUILT IN, by the same shape DeriveEngineRoot demands - `Bin/<config>`
        // directly under `build/` - and nothing looser: a binary merely three directories under some
        // Editor/ is not that editor's build.
        if ( !executableDirectory.empty() )
        {
            const fs::path binDirectory   = executableDirectory.parent_path();
            const fs::path buildDirectory = binDirectory.parent_path();
            const fs::path checkoutEditor = buildDirectory.parent_path() / "Editor";
            if ( binDirectory.filename() == "Bin" && buildDirectory.filename() == "build" &&
                 fs::is_directory( checkoutEditor / marker, ec ) )
            {
                lookup.WorkingDirectory = checkoutEditor.string();
                lookup.FromCheckout     = true;
                return lookup;
            }
        }

        lookup.Explanation =
             fmt::format( "the engine resources are missing: no 'Resources/Shaders' under the working "
                          "directory '{}', and none beside the executable ('{}'). A packaged build keeps "
                          "Resources/ next to its binaries and a checkout keeps it in Editor/; without it "
                          "there are no shaders, no fonts and no icons, so this stops here rather than "
                          "opening a window that can draw nothing.",
                          workingDirectory.string(),
                          executableDirectory.empty() ? std::string( "unknown" ) : executableDirectory.string() );
        return lookup;
    }
} // namespace Desert::Project
