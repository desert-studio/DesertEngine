#include "Editor/LevelEditor/SessionRecovery.hpp"

#include "Editor/Core/AutosavePaths.hpp"
#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/CrashRecovery.hpp"
#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/LevelEditor/DockLayout.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"

#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        // The recovery saves (autosave, device-lost) go through the one scene writer and only need to know
        // whether it landed; what it counted is the editor save's business.
        Common::BoolResultStr
        WrittenOrError( const Common::ResultStr<Desert::Core::ExternalEntities::WriteOutcome>& r )
        {
            if ( !r )
                return Common::MakeError( r.GetError() );
            return BOOLSUCCESS;
        }

        // Both recovery saves land the same way: the Autosaves directory first, then the one scene writer.
        Common::BoolResultStr WriteRecoveryFile( const std::filesystem::path& path, const std::string& text )
        {
            const auto      dir = path.parent_path();
            std::error_code ec;
            std::filesystem::create_directories( dir, ec );
            if ( ec )
                return Common::MakeFormattedError( "could not create {}: {}", dir.string(), ec.message() );
            return WrittenOrError( Desert::Core::ExternalEntities::WriteSceneText( path, text ) );
        }
    } // namespace

    SessionRecovery::SessionRecovery( SceneWorkspace& workspace, SceneFiles& sceneFiles, PlaySession& play,
                                      const std::shared_ptr<Assets::AssetManager>& assetManager )
         : m_Workspace( workspace ), m_SceneFiles( sceneFiles ), m_Play( play ), m_AssetManager( assetManager )
    {
    }

    void SessionRecovery::OfferAndArm( DockLayout& dock )
    {
        // If the previous session left its lock behind (unclean exit) and an autosave exists, arm a prompt
        // to reopen it. Then (re)arm the lock for THIS session; a clean shutdown (Disarm) removes it.
        if ( CrashRecovery::WasUncleanExit() )
        {
            // Only a copy at this build's scene schema is offered; autosaves are never migrated, so one
            // from another generation is named here and left as it is (CrashRecovery::ChooseAutosave).
            const Autosave::RecoveryChoice choice = CrashRecovery::ChooseAutosave();
            for ( const Autosave::NotOfferedCopy& copy : choice.NotOffered )
            {
                LOG_WARN( "[Recovery] not offered: '{}' states scene schema v{} / world units v{}; this build "
                          "opens v{} / v{} only. Autosaves are not migrated -- the file is left as it is.",
                          copy.Path.string(), copy.Stated.Scene, copy.Stated.Unit, Desert::Core::kSceneVersion,
                          Desert::Core::kUnitVersion );
            }
            dock.OfferRecovery( choice.Offered );
        }
        if ( !CrashRecovery::ArmSession() )
            Editor::ToastManager::Push( "Crash recovery is OFF for this session — the lock file could "
                                        "not be written (see the log)",
                                        Editor::ToastLevel::Error );
    }

    void SessionRecovery::Tick( const float seconds )
    {
        // Edit mode only, only when something actually changed since the last autosave. Writes a SEPARATE
        // file under <Project>/Saved/Autosaves (Autosave::PathFor) — never the main save, and never anything
        // under the assets root.
        const auto& prefs = EditorPreferences::Get();
        if ( prefs.AutosaveMinutes <= 0 ||
             m_Workspace.ActiveScene()->GetState() != ::Desert::Core::Scene::SceneState::Edit )
            return;

        // The wall clock: the period is minutes of the user's time, and an autosave never feeds the scene,
        // so a `--play` capture is not made less reproducible by it.
        m_AutosaveAccum += seconds;
        if ( m_AutosaveAccum < static_cast<float>( prefs.AutosaveMinutes ) * 60.0f )
            return;
        m_AutosaveAccum    = 0.0f;
        const uint64_t rev = CommandHistory::Get().Revision();
        if ( rev == m_LastAutosaveRevision )
            return;

        const Desert::Core::SceneSerializer serializer( m_Workspace.ActiveScene().get(), m_AssetManager.get() );
        const auto                          path = Autosave::PathFor(
             m_SceneFiles.OpenScenePath(), m_Workspace.ActiveScene()->GetSceneName(), Autosave::kPeriodicSuffix );
        const auto written = WriteRecoveryFile( path, serializer.SerializeToJson() );
        // BRACES ARE REQUIRED ON BOTH ARMS: the LOG_ macros are not single statements.
        if ( written )
        {
            // The revision is marked done ONLY on a write that landed. It used to be marked before the
            // write, so a failed autosave was never retried: the next tick saw the same revision, decided
            // nothing had changed, and skipped — and the log said the autosave had happened. A user going
            // for their autosave after a crash found an old file or none.
            m_LastAutosaveRevision = rev;
            LOG_INFO( "[Autosave] {}", path.string() );
        }
        else
        {
            LOG_ERROR( "[Autosave] {} was NOT written: {}. The next autosave tick will try this revision again.",
                       path.string(), written.GetError() );
        }
    }

    void SessionRecovery::SaveOnDeviceLost() const
    {
        // THE DEVICE DIED, AND THIS IS THE LAST MOMENT THE USER'S WORK EXISTS ANYWHERE.
        //
        // Not left to the autosave timer, which has three separate reasons not to have run recently: it
        // fires every AutosaveMinutes (default 5), it skips when the command revision has not moved, and
        // it runs in Edit mode only. This one runs ONCE, unconditionally, at the moment of loss.
        //
        // It writes a SEPARATE file so that a good periodic autosave is never clobbered by it. The name
        // still contains "_autosave", which is what Autosave::SceneFor matches on, and it is
        // the newest file there, so the recovery prompt offers this one.
        //
        // IN PLAY MODE THE AUTHORED SCENE IS WHAT GETS WRITTEN — PlaySession's snapshot, the same text Stop
        // would have restored. The live scene at that instant holds runtime mutations nobody authored and
        // nobody wants back; saving those under the user's scene name would be the wrong answer wearing the
        // right filename.
        if ( !Graphic::DeviceLost::IsLost() || !m_Workspace.ActiveScene() )
            return;

        using SceneState = ::Desert::Core::Scene::SceneState;
        const Desert::Core::SceneSerializer serializer( m_Workspace.ActiveScene().get(), m_AssetManager.get() );
        const std::string                   text = m_Workspace.ActiveScene()->GetState() == SceneState::Edit
                                                        ? serializer.SerializeToJson()
                                                        : m_Play.AuthoredSnapshot();
        if ( text.empty() )
        {
            // An empty file under a recovery name is a silent wrong answer: the prompt would offer it and the
            // user would open nothing. Say so instead.
            LOG_ERROR( "[DeviceLost] nothing could be serialized to save — the scene is in {} and its "
                       "authored snapshot is empty. Your periodic autosave, if any, is untouched.",
                       m_Workspace.ActiveScene()->GetState() == SceneState::Edit ? "Edit" : "Play" );
            return;
        }

        const auto path =
             Autosave::PathFor( m_SceneFiles.OpenScenePath(), m_Workspace.ActiveScene()->GetSceneName(),
                                Autosave::kDeviceLostSuffix );
        const auto written = WriteRecoveryFile( path, text );
        if ( written )
        {
            LOG_INFO( "[DeviceLost] your work was saved to {} before shutting down; the next start will offer it.",
                      path.string() );
        }
        else
        {
            LOG_ERROR( "[DeviceLost] the emergency save FAILED: {}. The periodic autosave in {} is the newest "
                       "copy that exists.",
                       written.GetError(), path.parent_path().string() );
        }
    }

    void SessionRecovery::Disarm()
    {
        // After a device loss CrashRecovery::DisarmSession refuses, on purpose — see its own comment.
        CrashRecovery::DisarmSession();
    }
} // namespace Desert::Editor
