#include "CrashRecovery.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Graphic/DeviceLost.hpp>

// The migrator's own file loop, called rather than re-implemented -- see MigrateAutosaves in the header
// for why the Editor is the process that runs it over this one directory.
#include <MigratorMain.hpp>

#include <sstream>
#include <string>

namespace Desert::Editor
{
    std::filesystem::path CrashRecovery::AutosaveDir()
    {
        return Common::Constants::Path::SCENE_PATH / "Autosave";
    }

    std::filesystem::path CrashRecovery::LockPath()
    {
        return AutosaveDir() / ".session.lock";
    }

    bool CrashRecovery::WasUncleanExit()
    {
        std::error_code ec;
        return std::filesystem::exists( LockPath(), ec );
    }

    bool CrashRecovery::ArmSession()
    {
        std::error_code ec;
        std::filesystem::create_directories( AutosaveDir(), ec );
        if ( ec )
        {
            LOG_ERROR( "[Recovery] Could not create {}: {} — this session is UNPROTECTED: a crash will "
                       "not be detected on the next start.",
                       AutosaveDir().string(), ec.message() );
            return false;
        }

        // WasUncleanExit() is literally exists( LockPath() ), so a lock that failed to appear is
        // indistinguishable from a clean exit: the editor crashes, the next start sees no lock and
        // never offers the recovery. Saying so at arm time is the only moment the difference exists.
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( LockPath(), "editor session in progress" );
             !written )
        {
            LOG_ERROR( "[Recovery] Could not arm the session lock {}: {} — this session is UNPROTECTED: "
                       "a crash will not be detected on the next start.",
                       LockPath().string(), written.GetError() );
            return false;
        }
        return true;
    }

    void CrashRecovery::DisarmSession()
    {
        // A DEVICE-LOST SHUTDOWN IS NOT A CLEAN EXIT, AND THE DIFFERENCE IS THE USER'S UNSAVED WORK.
        //
        // The engine now closes in order when the GPU device is lost, which means it walks the ordinary
        // quit path — Application::Run leaves its loop, every layer is detached, and the editor's detach
        // calls this. Dropping the lock here would tell the next start that the session ended normally,
        // and the recovery prompt that offers the latest autosave would never appear. The exit was
        // orderly; the session was not.
        if ( Graphic::DeviceLost::IsLost() )
        {
            LOG_WARN( "[Recovery] the session lock is LEFT IN PLACE: this shutdown was caused by a lost "
                      "GPU device, not by you closing the editor. The next start will offer to reopen the "
                      "latest autosave." );
            return;
        }

        std::error_code ec;
        std::filesystem::remove( LockPath(), ec );
    }

    std::filesystem::path CrashRecovery::LatestAutosave()
    {
        namespace fs = std::filesystem;
        const fs::path              dir = AutosaveDir();
        fs::path                    newest;
        fs::file_time_type          newestTime{};
        std::error_code             ec;

        for ( const auto& entry : fs::directory_iterator( dir, ec ) )
        {
            if ( ec )
                break;
            const fs::path& p = entry.path();
            if ( p.extension() != Common::Constants::Extensions::SCENE_EXTENSION )
                continue;
            if ( p.filename().string().find( "_autosave" ) == std::string::npos )
                continue;

            const auto t = fs::last_write_time( p, ec );
            if ( ec )
                continue;
            if ( newest.empty() || t > newestTime )
            {
                newest     = p;
                newestTime = t;
            }
        }
        return newest;
    }

    bool CrashRecovery::MigrateAutosaves()
    {
        namespace fs = std::filesystem;

        const fs::path  dir = AutosaveDir();
        std::error_code err;
        if ( !fs::exists( dir, err ) || err )
        {
            return true; // no autosaves have ever been written for this project
        }

        // Counted first so that "nothing to migrate" and "the migrator found nothing" stay
        // distinguishable: RunSceneMigrator answers 2 for an empty search, which is a usage code and not
        // a failure, and reading it as one would put an error in the log on every clean start.
        int candidates = 0;
        for ( const auto& entry : fs::directory_iterator( dir, err ) )
        {
            if ( err )
            {
                break;
            }
            if ( entry.path().extension() == Common::Constants::Extensions::SCENE_EXTENSION )
            {
                ++candidates;
            }
        }
        if ( candidates == 0 )
        {
            return true;
        }

        std::ostringstream toolOut;
        std::ostringstream toolErr;
        const int          code = Migration::RunSceneMigrator( { dir.string() }, toolOut, toolErr );

        // The tool reports per file and says nothing at all about a file already at the head, so a quiet
        // run is the normal case and is left quiet. Anything it did say is worth a line: these are the
        // owner's own recovery copies being rewritten under him.
        const auto emit = []( const std::string& text, bool isError )
        {
            std::istringstream lines( text );
            std::string        line;
            while ( std::getline( lines, line ) )
            {
                if ( line.empty() )
                {
                    continue;
                }
                // Braced deliberately: LOG_ERROR/LOG_INFO already end in a semicolon, so an unbraced
                // if/else expands to an empty statement before the `else` and does not compile.
                if ( isError )
                {
                    LOG_ERROR( "[Autosave] {}", line );
                }
                else
                {
                    LOG_INFO( "[Autosave] {}", line );
                }
            }
        };
        emit( toolOut.str(), false );
        emit( toolErr.str(), true );

        if ( code == 1 )
        {
            LOG_ERROR( "[Autosave] at least one autosave in '{}' could NOT be raised to the current "
                       "scene schema and was left exactly as it was. It will not open in this build "
                       "until it is converted -- the lines above name which file and why.",
                       dir.string() );
            return false;
        }
        return true;
    }
} // namespace Desert::Editor
