#include "EngineRegistration.hpp"

#include <Common/Core/Version.hpp>
#include <Common/Project/EngineRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>

namespace Desert::Project
{
    // И7 made the commit count optional so that "unknown" could never be mistaken for a very old
    // build, and the registry keeps that distinction rather than flattening it here — an engine that
    // cannot name its build registers WITHOUT a number, and the launcher's ranking says so out loud.
    static std::optional<int> CommitCountForRegistry()
    {
        const auto count = Common::Version::CommitCount();
        return count ? std::optional<int>( static_cast<int>( *count ) ) : std::nullopt;
    }

    Common::BoolResultStr RegisterThisEngine( const std::string& configDirectory, const std::string& engineRoot )
    {
        // WHAT THIS MESSAGE USED TO DO WRONG, because the repair is the whole reason it changed. It
        // said "DESERT_ROOT is not set ... start the Editor through scripts/MacOS/RunEditor.sh (or
        // scripts\\Windows\\RunEditor.bat)". A DOWNLOADED BUILD CARRIES NEITHER SCRIPT — they live in
        // the repository — so an artifact the owner unzipped and double-clicked printed an
        // instruction that could not be followed, for a variable it no longer needs: the caller now
        // derives the tree from its own executable (Engine/Project/StartupLayout.hpp) and only ever
        // reaches this refusal with an empty root it could not derive AND was not told.
        //
        // So this says what is missing and nothing about how to obtain it: the caller is the one
        // that knows where it looked, and it prints that.
        if ( engineRoot.empty() )
            return Common::MakeError<bool>(
                 "no engine root was given or could be derived, so this engine could not record itself "
                 "in engines.json and the launcher will not list it." );

        std::error_code             ec;
        const std::filesystem::path root = std::filesystem::absolute( engineRoot, ec );
        if ( ec )
            return Common::MakeFormattedError<bool>(
                 "DESERT_ROOT={} could not be resolved to an absolute path: {}", engineRoot, ec.message() );

        const std::filesystem::path file = std::filesystem::path( configDirectory ) / "engines.json";

        // Read what is there first. A registry with two installs in it belongs to the USER, not to
        // this process: clobbering it with a single entry would delete the other engine from the
        // launcher's sidebar every time this one started.
        Common::Engine::EngineRegistry registry;
        if ( std::filesystem::exists( file, ec ) )
        {
            const auto raw = Common::Utils::FileSystem::ReadFileContent( file.string() );
            if ( !raw )
                return Common::MakeFormattedError<bool>( "Could not read {} - it is left untouched",
                                                         file.string() );
            if ( !raw.GetValue().empty() )
            {
                auto parsed = Common::Engine::ReadEngineRegistry( raw.GetValue() );
                if ( !parsed.IsSuccess() )
                    // Verbatim, and NOT overwritten: a file this process cannot parse may still be a
                    // file the user (or a newer build) can, and rewriting it from scratch would
                    // silently drop whatever it held.
                    return Common::MakeFormattedError<bool>( "{}: {} - it is left untouched", file.string(),
                                                             parsed.GetError() );
                registry = parsed.ExtractValue();
            }
        }

        Common::Engine::RegisterInstall(
             registry,
             Common::Engine::EngineInstall{ root.string(), Common::Version::Full(), CommitCountForRegistry() } );

        // Atomic, for the same reason projects.json is: two processes share this file, and an
        // interrupted in-place write leaves a torn one that neither can parse.
        if ( !Common::Utils::FileSystem::WriteContentToFileAtomic(
                  file, Common::Engine::WriteEngineRegistry( registry ) ) )
            return Common::MakeFormattedError<bool>( "Could not write {} - it keeps its previous contents",
                                                     file.string() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Project
