// SCENE FILES — see SceneFiles.hpp. Moved out of EditorLayer.cpp unchanged (EDL-4).
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ViewportCapture.hpp"
#include <Engine/Core/PlayerStart.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Core/Core.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include "Editor/Core/AutosavePaths.hpp"
#include "Editor/Core/SceneOpenRequest.hpp"
#include "Editor/Core/SceneSaveRules.hpp"
#include <Common/Utilities/FileSystem.hpp>
#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include <stb_image/stb_image_write.h>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Panels/WorldPartition/WorldPartitionPanel.hpp"
#include "Editor/Panels/Foliage/FoliageCommands.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/NodeGraph/NodeGraphPanel.hpp"
#include "Editor/Panels/MaterialEditor/MaterialEditorPanel.hpp"
#include "Editor/Panels/Animation/AnimGraphPanel.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Sequencer/SequencerPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/Clouds/CloudModellingVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudLayoutPanel.hpp"
#include "Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp"
#include "Editor/Panels/SkyboxViewer/SkyboxViewerDocument.hpp"
#include "Editor/Panels/AnimationEditor/AnimationEditorDocument.hpp"
#include "Editor/Panels/StaticMeshViewer/StaticMeshViewerDocument.hpp"
#include "Editor/Panels/TextureViewer/TextureViewerDocument.hpp"
#include "Editor/Panels/Clouds/CloudTypePanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Core/ToastManager.hpp"
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <algorithm> // std::sort (scene list)
#include <format>
#include <functional>

namespace Desert::Editor
{
    SceneFiles::SceneFiles( SceneWorkspace& workspace, const std::shared_ptr<Assets::AssetManager>& assets,
                            ViewportCapture& capture )
         : m_Workspace( workspace ), m_Assets( assets ), m_Capture( capture )
    {
    }

    // "Unsaved changes": the CommandHistory revision at the last save/load differs from the current one.
    bool SceneFiles::HasUnsavedChanges() const
    {
        return CommandHistory::Get().Revision() != m_SavedRevision;
    }

    // A scene handed over by a panel (dropped on the viewport, double-clicked in the asset browser). It goes
    // through the SAME deferred load as the menu — but a drag is easy to do by accident, so unsaved work is
    // not thrown away silently: the confirm popup decides, and only then is the load queued.
    void SceneFiles::ConsumeOpenRequest()
    {
        if ( auto requested = Editor::Core::SceneOpenRequest::Consume() )
        {
            const Common::Filepath path( *requested );
            if ( HasUnsavedChanges() )
            {
                m_PendingOpenScene      = path;
                m_ConfirmOpenScenePopup = true;
            }
            else
            {
                RequestLoad( path );
            }
        }
    }

    bool SceneFiles::RequestReload()
    {
        if ( m_OpenScenePath.empty() )
            return false;
        Editor::Core::SceneOpenRequest::Request( m_OpenScenePath.string() );
        return true;
    }

    // Between frames, after the startup stages: the deferred load. True when a load ran — the host starts
    // the content settle then, BEFORE InitializeIfLoadRefused and New Scene (the order OnUpdate always had).
    bool SceneFiles::ServiceLoadRequest()
    {
        if ( !m_SceneLoadRequested )
            return false;
        auto path = m_SceneLoadRequested.value();
        m_SceneLoadRequested.reset();
        LoadSceneInternal( path );
        return true;
    }

    void SceneFiles::InitializeIfLoadRefused()
    {
        // THE OTHER HALF OF THE SKIPPED Init() IN OnAttach. That skip is safe only because THIS load
        // initialises the scene — and LoadSceneInternal has three early returns (the file is gone,
        // unreadable, or written by an older build) that deliberately leave the editor exactly as it
        // was. "Exactly as it was" used to mean an initialised empty scene; with the first Init()
        // pre-empted it would mean a scene whose renderer has no systems, and the frame below would
        // record against it. Asked of the SCENE rather than inferred from the load's return value,
        // which is void, and rather than tracked in a flag here, which would be a second copy of a
        // fact the scene already holds.
        if ( !m_Workspace.ActiveScene()->IsInitialized() )
        {
            if ( const auto inited = m_Workspace.ActiveScene()->Init(); !inited.IsSuccess() )
                LOG_ERROR( "[EditorLayer] the scene load was refused and the fallback initialise "
                           "failed too: {}",
                           inited.GetError() );
            // The registry follows the Init that built the framebuffers its passes bind to, exactly
            // as it does on the successful path inside LoadSceneInternal.
            m_Workspace.RebuildRenderRegistry();
        }
    }

    // New (empty) scene — deferred like a load so it never tears down resources mid-frame.
    void SceneFiles::ServiceNewRequest()
    {
        if ( !m_NewSceneRequested )
            return;
        m_NewSceneRequested = false;
        NewSceneInternal();
    }

    void SceneFiles::RequestLoad( const Common::Filepath& path )
    {
        m_SceneLoadRequested = path;
    }

    bool SceneFiles::SaveSceneTo( const std::string& path )
    {
        // THROUGH SaveToFile AND NOT A SECOND COPY OF IT. This function used to create the directory and
        // write SerializeToJson() itself, which was the same save spelled twice — and the moment the save
        // grew a step (a landscape writes its tile files beside the scene before the scene names them),
        // this copy would have written a .desce naming tile files that were never written.
        const auto                          previousHeader = ForgetAssetIdentityUnlessSameFile( path );
        const Desert::Core::SceneSerializer serializer( m_Workspace.ActiveScene().get(), m_Assets.get() );
        if ( const auto written = serializer.SaveToFile( Common::Filepath( path ) ); !written )
        {
            // Nothing was written, so the scene is still the asset it was.
            m_Workspace.ActiveScene()->SetAssetHeader( previousHeader );
            LOG_ERROR( "[Scene] Could not write '{}': {}", path, written.GetError() );
            return false;
        }
        return true;
    }

    std::optional<Common::Content::TextAssetHeaderSerialized>
    SceneFiles::ForgetAssetIdentityUnlessSameFile( const std::string& destination )
    {
        auto previous = m_Workspace.ActiveScene()->GetAssetHeader();
        // A save under another path is a new asset (Rules::SaveKeepsAssetIdentity): dropping the header
        // makes the serializer mint a fresh GUID instead of copying this one into a second file.
        if ( !Editor::Core::Rules::SaveKeepsAssetIdentity( m_OpenScenePath.generic_string(), destination ) )
            m_Workspace.ActiveScene()->SetAssetHeader( std::nullopt );
        return previous;
    }

    Common::Filepath SceneFiles::SceneSaveDestination() const
    {
        // The rule itself is a pure function in Editor/Core/SceneSaveRules.hpp so that a test can drive
        // the case that matters — a file whose name disagrees with the scene's — without an editor. This
        // call site supplies the two project paths it cannot know.
        return { Editor::Core::Rules::SceneSaveDestination( m_OpenScenePath.generic_string(),
                                                            m_Workspace.ActiveScene()->GetSceneName(),
                                                            Common::Constants::Path::SCENE_PATH.generic_string(),
                                                            Common::Constants::Extensions::SCENE_EXTENSION ) };
    }

    bool SceneFiles::SaveOpenScene()
    {
        const Common::Filepath destination    = SceneSaveDestination();
        const auto             previousHeader = ForgetAssetIdentityUnlessSameFile( destination.generic_string() );
        const auto             verdict        = Editor::Core::Rules::DecideAfterSceneSave(
             m_Workspace.ActiveScene()->Serialize( m_Assets.get(), destination ),
             m_Workspace.ActiveScene()->GetSceneName(), destination.string() );
        if ( !verdict.MarkSceneSaved )
            m_Workspace.ActiveScene()->SetAssetHeader( previousHeader );

        if ( verdict.MarkSceneSaved )
        {
            m_SavedRevision = CommandHistory::Get().Revision();
            // The scene now IS this file, whether it already was or was just given one. Without this a
            // new scene would re-derive its path on every save and a rename between two saves would
            // leave the user's work split across two files.
            m_OpenScenePath = destination;
        }

        if ( verdict.IsError )
        {
            LOG_ERROR( "[Scene] {}", verdict.Message );
        }
        else
        {
            LOG_INFO( "[Scene] {}", verdict.Message );
        }

        // Refresh the launcher's tile picture for this project. Only on a save that actually
        // happened — a refused save must not leave the launcher showing a world that was never
        // written. The failure is a toast and nothing more: the scene IS saved, and a launcher tile
        // without a picture is a state the launcher already draws.
        if ( verdict.MarkSceneSaved )
            if ( const auto thumbnail = m_Capture.WriteProjectThumbnail(); !thumbnail.IsSuccess() )
                Editor::ToastManager::Push(
                     std::format( "The scene was saved, but the project thumbnail was not: {}",
                                  thumbnail.GetError() ),
                     Editor::ToastLevel::Warning );

        Editor::ToastManager::Push( verdict.Message,
                                    verdict.IsError ? Editor::ToastLevel::Error : Editor::ToastLevel::Success );
        return verdict.MayDiscardScene;
    }

    Common::Filepath SceneFiles::BasicLevelTemplate()
    {
        return Common::Constants::Path::ENGINE_CONTENT_PATH / "Maps" / "Templates" /
               Common::Filepath( "Basic" ).replace_extension( Common::Constants::Extensions::SCENE_EXTENSION );
    }

    void SceneFiles::NewSceneInternal()
    {
        // A new scene is the Basic level template opened as an untitled scene — content, not entities made
        // here. The load's own refusals (a missing template is "Scene file does not exist: <path>") leave the
        // editor as it was; InitializeIfLoadRefused covers the first boot, whose Init was deferred to this.
        LoadSceneInternal( BasicLevelTemplate(), OpenAs::Untitled );
        InitializeIfLoadRefused();
    }

    void SceneFiles::LoadSceneInternal( const Common::Filepath& requested, const OpenAs openAs )
    {
        // The old path of a moved scene opens the scene where it now lives (the registry follows the
        // redirector the move left); only a redirector the registry cannot resolve reaches the refusal below.
        const Common::Filepath path = Desert::Assets::ContentRegistry::FileToOpen( requested );
        if ( path != requested )
            LOG_INFO( "'{}' was moved; opening '{}'", requested.string(), path.string() );
        if ( !std::filesystem::exists( path ) )
        {
            LOG_ERROR( "Scene file does not exist: {0}", path.string() );
            return;
        }

        // ASKED BEFORE ANYTHING IS DESTROYED, and this is the call site that makes it worth asking. Below
        // this point the undo history is dropped and the open scene is cleared; a file the loader will
        // refuse - an old autosave, a scene saved by an older build - would then have cost the user the
        // scene they had and given them nothing. So an unloadable file leaves the editor exactly as it is
        // and says why, with the command that fixes the file.
        Desert::Core::SceneLoadPhases phases( fmt::format( "Open '{}'", path.filename().string() ) );

        const auto contentRead = Desert::Core::ExternalEntities::ReadSceneFileText( path );
        if ( !contentRead )
        {
            LOG_ERROR( "{0}", contentRead.GetError() );
            Editor::ToastManager::Push( "Scene not loaded — the file could not be read (see the log)",
                                        Editor::ToastLevel::Error );
            return;
        }
        const std::string& content = contentRead.GetValue();
        phases.Lap( "read the file", content.size() );
        // The old path of a moved scene: say where it went (the gate below could only name the GUID).
        if ( const auto moved = Common::Content::RefuseRedirectorBytes(
                  path.string(), content, Desert::Assets::ContentRegistry::KeyOfRedirectorTarget );
             !moved )
        {
            LOG_ERROR( "{0}", moved.GetError() );
            Editor::ToastManager::Push(
                 "Scene not loaded — this path is a redirector to a moved scene (see the log)",
                 Editor::ToastLevel::Error );
            return;
        }
        auto loadable = Desert::Core::ParseLoadableScene( path.string(), content );
        if ( !loadable )
        {
            LOG_ERROR( "{0}", loadable.GetError() );
            Editor::ToastManager::Push( "Scene not loaded — see the log (it names the SceneMigrator command)",
                                        Editor::ToastLevel::Error );
            return;
        }

        phases.Lap( "version gate before tearing down the open scene", content.size() );

        // Wait for GPU to be idle before destroying resources mid-frame
        EngineContext::GetInstance().GetDevice()->WaitIdle();

        // The undo history refers to entities of the OLD scene — none of it applies anymore.
        CommandHistory::Get().Clear();
        m_SavedRevision = CommandHistory::Get().Revision(); // a freshly loaded scene is "clean"

        phases.Lap( "wait for the GPU, drop the undo history", 0 );
        const std::size_t outgoing = m_Workspace.ActiveScene()->GetAllEntities().size();
        m_Workspace.ActiveScene()->Clear();
        phases.Lap( "clear the open scene", outgoing );

        const Desert::Core::SceneSerializer serializer( m_Workspace.ActiveScene().get(), m_Assets.get() );
        // Cannot fire - the same text passed the same check above, before anything was torn down. It is
        // reported and NOT returned from on purpose: the scene is already cleared by this point, so the
        // rebuild below is what leaves the editor in a coherent (empty) state rather than one holding a
        // render registry for entities that no longer exist.
        if ( const auto loaded = serializer.Deserialize( loadable.ExtractValue(), path.string() ); !loaded )
        {
            LOG_ERROR( "{0}", loaded.GetError() );
            Editor::ToastManager::Push( "Scene failed to load — see the log", Editor::ToastLevel::Error );
        }
        const std::size_t incoming = m_Workspace.ActiveScene()->GetAllEntities().size();
        phases.Lap( "deserialize (its own phases are logged above)", incoming );

        // THE SCENE'S DEPENDENCY CLOSURE, from the registry's `deps` column, is read by the loader's workers
        // while the open waits — meshes, their materials, parents and textures in one batch — so its first
        // frame is whole and no read happens in a frame (AL1-8b).
        const Runtime::ClosureResidency closure = Runtime::AwaitSceneClosure( *m_Workspace.ActiveScene() );
        phases.Lap( "wait for the scene's dependency closure (rows)", closure.Rows );
        LOG_INFO( "[SceneLoad] closure: {} row(s), {} worker read(s) awaited, {} drawable mesh(es)", closure.Rows,
                  closure.Reads, closure.DrawableMeshes );

        if ( const auto inited = m_Workspace.ActiveScene()->Init(); !inited.IsSuccess() )
        {
            LOG_ERROR( "[EditorLayer] loaded scene failed to initialise: {}", inited.GetError() );
            Editor::ToastManager::Push( "Scene could not be initialised — see the log",
                                        Editor::ToastLevel::Error );
        }
        phases.Lap( "initialise the scene", incoming );

        // Destroy the old registry FIRST: its destructor unregisters the editor passes by name, and
        // assignment would run it after the new registry already re-registered them.
        m_Workspace.ActiveSceneReplaced();
        phases.Lap( "rebuild the render registry, drop the old world's selection", incoming );
        phases.LogSummary();

        // THE OPEN SCENE IS NOW THIS FILE, and it is set HERE rather than at the top of the function on
        // purpose: every early return above leaves a scene that was NOT replaced, and adopting a path for
        // a load that refused would point the next Ctrl+S at a file the user never opened.
        //
        // A RECOVERY COPY OPENS AS THE SCENE IT STANDS FOR. Bound to the copy, Ctrl+S would write the
        // user's work back into Saved/Autosaves and the real scene would never get it; bound to the
        // original (empty for a never-saved scene: Save As), Save writes the user's file. The copy is
        // work the original does not have yet, so the scene starts DIRTY — a revision no command reaches.
        // A LEVEL TEMPLATE IS NOT THE FILE IT WAS READ FROM: no path (Ctrl+S asks where), no asset identity (a
        // save mints a new GUID instead of copying the template's), not in the recent list.
        if ( openAs == OpenAs::Untitled )
        {
            m_Workspace.ActiveScene()->SetAssetHeader( std::nullopt );
            m_Workspace.ActiveScene()->SetSceneName( "New Scene" );
            m_OpenScenePath.clear();
            Editor::ToastManager::Push( "New scene", Editor::ToastLevel::Success );
            LOG_INFO( "[Scene] New scene from the level template '{}'", path.string() );
            return;
        }

        if ( const auto original = Autosave::SceneFor( path ) )
        {
            m_OpenScenePath = *original;
            m_SavedRevision = ~CommandHistory::Get().Revision();
            LOG_INFO( "[Recovery] '{}' opened as '{}' (unsaved)", path.string(),
                      original->empty() ? std::string( "an untitled scene" ) : original->string() );
            return;
        }
        m_OpenScenePath = path;
        // The files at `path` now hold every entity as it is: the next save to it writes only what differs (WP17).
        Desert::Core::SceneSerializer( m_Workspace.ActiveScene().get(), m_Assets.get() )
             .AdoptAsSaved( Common::Filepath( path ) );

        // Update recent scenes
        auto it = std::find( m_RecentScenes.begin(), m_RecentScenes.end(), path );
        if ( it != m_RecentScenes.end() )
        {
            m_RecentScenes.erase( it );
        }
        m_RecentScenes.insert( m_RecentScenes.begin(), path );

        if ( m_RecentScenes.size() > 5 )
        {
            m_RecentScenes.pop_back();
        }
    }

    // How a scene is NAMED in the pickers: its path relative to the scenes root ("Levels/Arena.desce"),
    // not the bare filename. With subfolders in play, filenames alone are both ambiguous (two "Test.desce"
    // in different folders read identically) and lose the only structure the user gave their scenes.
    std::string SceneFiles::Label( const Common::Filepath& path )
    {
        std::error_code ec;
        std::string     rel =
             std::filesystem::relative( path, Common::Constants::Path::SCENE_PATH, ec ).generic_string();

        // Outside the scenes root (a recent scene from elsewhere): a "../../.." chain says nothing.
        if ( ec || rel.empty() || rel.starts_with( ".." ) )
            return path.filename().string();
        return rel;
    }

    std::vector<Common::Filepath> SceneFiles::CollectAvailableScenes()
    {
        std::vector<Common::Filepath> scenes;

        // THROUGH THE ONE CONTENT ENUMERATION, and this used to be a raw recursive_directory_iterator.
        //
        // FileSystem.hpp states the rule over ListFilesRecursive in as many words — "every scanner that
        // enumerates content must go through this: the font and icon services each used to walk only the
        // disk half, so a packaged game — where the loose directories do not exist at all — scanned
        // nothing and no text could resolve its font." This was the same defect in the same shape, one
        // list over: a project whose content is mounted from a .dpak had NO levels in the Open Scene
        // popup, in the Scene group of the command palette, or on the control channel, because the only
        // half this loop could see was the loose one.
        //
        // Latent today, because the editor never mounts a pak — and latency is not a mitigation. The
        // shared function exists precisely so that the day it stops being latent is not the day somebody
        // discovers it: a scanner that walks the disk itself is a second answer to "what content is
        // there", and this is the second one found. A6-2 point 2. `Desert/Tests/Engine/ContentScanners`
        // now holds the register, so a third has to be argued for rather than merely written.
        //
        // RECURSION AND THE MISSING-DIRECTORY CASE COME WITH IT: scenes live in subfolders (Levels/,
        // per-feature folders), which a flat scan simply did not list, and a missing scenes
        // directory contributes nothing rather than throwing.
        for ( const std::filesystem::path& file :
              Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SCENE_PATH ) )
        {
            if ( file.extension() == Common::Constants::Extensions::SCENE_EXTENSION )
                scenes.push_back( file );
        }

        // Sorted by the label the list shows, which keeps every folder's scenes contiguous (they share the
        // "Folder/" prefix) — that is what the folder headers in the popup rely on.
        std::sort( scenes.begin(), scenes.end(), []( const Common::Filepath& a, const Common::Filepath& b )
                   { return Label( a ) < Label( b ); } );
        return scenes;
    }

    void SceneFiles::PrepareScenePopup()
    {
        m_AvailableScenes    = CollectAvailableScenes();
        m_SelectedSceneIndex = -1;
        m_SceneFilter[0]     = '\0';
    }

    namespace
    {
        // NOT A LAMBDA: `bugprone-exception-escape` fires on a parameter-less lambda that captures a string
        // (DocumentHost.cpp records the same finding); bind_front leaves nothing to analyse.
        Common::BoolResultStr RequestSceneOpen( const std::string& path )
        {
            Editor::Core::SceneOpenRequest::Request( path );
            return PaletteCommandDone();
        }
    } // namespace

    void SceneFiles::AppendOpenSceneCommands( std::vector<PaletteCommand>& commands )
    {
        for ( const Common::Filepath& scene : CollectAvailableScenes() )
            commands.push_back( { "Scene", std::format( "Open Scene {}", Label( scene ) ),
                                  std::bind_front( &RequestSceneOpen, scene.string() ) } );
    }

    void SceneFiles::AppendSaveSceneCommand( std::vector<PaletteCommand>& commands )
    {
        commands.push_back( { "Action", "Save Scene", [this]
                              {
                                  return PaletteCommandOutcome(
                                       SaveOpenScene(), "the scene was NOT saved; the log line above says "
                                                        "why, and the unsaved-changes mark is still set." );
                              } } );
    }
} // namespace Desert::Editor
