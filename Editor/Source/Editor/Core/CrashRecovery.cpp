#include "CrashRecovery.hpp"

#include "AutosavePaths.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <sstream>
#include <string>

namespace Desert::Editor
{
    std::filesystem::path CrashRecovery::AutosaveDir()
    {
        return Autosave::Dir();
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

    Autosave::RecoveryChoice CrashRecovery::ChooseAutosave()
    {
        return Autosave::ChooseRecovery( AutosaveDir(),
                                         { Desert::Core::kSceneVersion, Desert::Core::kUnitVersion },
                                         Desert::Assets::kSceneSchemaTag, Desert::Assets::kUnitSchemaTag );
    }
} // namespace Desert::Editor
