#define NOMINMAX // engine headers below use std::min/max; keep the windows.h macros out

#include "NewAssetMenu.hpp"

#include <Editor/Core/AssetFileOps.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Editor/Core/MaterialAssetUtils.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Panels/Clouds/CloudDocumentOpen.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Panels/FileExplorer/NewCloudAsset.hpp>
#include <Editor/Panels/NodeGraph/NodeGraphPanel.hpp>
#include <Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <utility>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // The open folder's next free "<stem><ext>" — creating a second one never silently overwrites a file.
        std::filesystem::path UniqueIn( const DirectoryInformation& folder, const std::string& stem,
                                        const std::string& ext )
        {
            const std::filesystem::path dir  = folder.AssetPath;
            const std::string           name = AssetFileOps::UniqueName( stem, ext, [&]( const std::string& n )
                                                                         { return std::filesystem::exists( dir / n ); } );
            return dir / name;
        }
    } // namespace

    // Port pattern: UE Editor/ContentBrowser/Private/NewAssetOrClassContextMenu.cpp (the "New…" section built
    // for one target folder, every item a factory into that folder).
    NewAssetMenu::NewAssetMenu( Assets::AssetManager*                assetManager,
                                std::weak_ptr<::Desert::Core::Scene> viewportScene, Delegates delegates )
         : m_AssetManager( assetManager ), m_ViewportScene( std::move( viewportScene ) ),
           m_On( std::move( delegates ) )
    {
    }

    NewAssetMenu::~NewAssetMenu()
    {
        // CANCEL, THEN WAIT. The worker writes into m_BakeProgress and into a path this object owns, so this
        // object must outlive it; waiting rather than detaching is the difference between a slow editor
        // shutdown and a use-after-free. The same discipline CloudModellingVolumePanel keeps.
        //
        // The cancel bounds the wait for a `.dcmv` (the bake asks the callback whether to carry on between
        // slabs). A `.dcnv` cannot be cancelled — Assets::GenerateCloudNoiseVolume takes a progress pointer
        // and no stop condition — so closing the editor during one waits out the remainder (8.7 s in Debug),
        // as CloudNoiseVolumePanel does.
        m_BakeCancelled.store( true );
        if ( m_Bake.valid() )
            m_Bake.wait();
    }

    void NewAssetMenu::Draw( const DirectoryInformation& folder )
    {
        if ( ImGui::Selectable( "New folder" ) )
            CreateNewFolder( folder );

        if ( ImGui::Selectable( "New Material" ) )
            CreateNewMaterial( folder );

        if ( ImGui::Selectable( std::string( kNewLevelSequenceLabel ).c_str() ) )
            if ( const auto created = CreateNewLevelSequence( &folder ); !created )
                LOG_ERROR( "[Content] {}", created.GetError() );

        // Pick the domain up front (like Unreal's Material Domain / Godot's Mode): it decides the output node,
        // vertex contract and palette of the new graph.
        if ( ImGui::BeginMenu( "New Shader Graph" ) )
        {
            auto createGraph = [&]( ShaderGraph::Domain domain )
            {
                const auto path = NodeGraphPanel::CreateNewGraphFile( folder.AssetPath, domain );
                if ( path.empty() ) // not written — nothing to open, nothing new to list
                    return;
                // Through the SAME opener the double-click uses, so a graph created here and a graph opened
                // from the tile reach one window by one route.
                if ( RequestShaderGraphDocument( m_AssetManager, path ) != ShaderGraphDocumentRequest::Requested )
                {
                    LOG_ERROR( "[ShaderGraph] '{}' was created but would not open.", path );
                }
                m_On.OnRefresh();
            };
            if ( ImGui::MenuItem( "Surface" ) )
                createGraph( ShaderGraph::Domain::Surface );
            if ( ImGui::MenuItem( "Post Process" ) )
                createGraph( ShaderGraph::Domain::PostProcess );
            // The cloud medium. Named for what an artist is authoring rather than for the engine's domain
            // token: "Volume" means nothing beside "Surface" and "Post Process" until you already know it.
            if ( ImGui::MenuItem( "Cloud Medium" ) )
                createGraph( ShaderGraph::Domain::Volume );
            ImGui::EndMenu();
        }

        // THE FOUR CLOUD FORMATS. All four editors are contextual documents keyed on an asset handle, so
        // without this menu the double-click seam has nothing to open and an artist could edit the shipped
        // assets and author none of their own. A submenu for the reason "New Shader Graph" is one.
        if ( ImGui::BeginMenu( "New Cloud Asset" ) )
        {
            // Disabled while a volume is being generated: only one creation is tracked at a time, and a second
            // click would detach the first bake's thread.
            ImGui::BeginDisabled( m_BakeRunning );

            if ( ImGui::MenuItem( "Cloud Type" ) )
                CreateNewCloudAsset( folder, CloudAssetKind::Type );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "A kind of cloud: altitudes, silhouette curve, "
                                   "density. Starts from the built-in congestus." );

            if ( ImGui::MenuItem( "Cloud Layout" ) )
                CreateNewCloudAsset( folder, CloudAssetKind::Layout );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "A blank 512x512 painting of where clouds are. "
                                   "Draw on it in the layout document." );

            if ( ImGui::MenuItem( "Cloud Noise Volume" ) )
                CreateNewCloudAsset( folder, CloudAssetKind::NoiseVolume );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "The 3D noise cloud edges are eroded with, 128^3 "
                                   "RGBA8. Generated in the background - it takes "
                                   "several seconds and a progress bar appears above." );

            if ( ImGui::MenuItem( "Cloud Modelling Volume" ) )
                CreateNewCloudAsset( folder, CloudAssetKind::ModellingVolume );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "A hero cloud's sculpted body, 128x64x128. Starts "
                                   "from the shipped congestus and is baked in the "
                                   "background." );

            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
    }

    bool NewAssetMenu::CanAddPrefabToScene() const
    {
        return !m_ViewportScene.expired() && m_AssetManager != nullptr;
    }

    void NewAssetMenu::AddPrefabToScene( const std::string& prefabPath )
    {
        auto scene = m_ViewportScene.lock();
        if ( !scene || m_AssetManager == nullptr )
            return;

        auto prefab = m_AssetManager->FindByPath<Assets::PrefabAsset>( prefabPath );
        if ( !prefab )
            prefab = m_AssetManager->CreateAsset<Assets::PrefabAsset>( prefabPath );
        if ( !prefab )
            return;
        if ( !prefab->IsReadyForUse() )
            prefab->Load();

        // Same instantiate path the viewport-drop uses (undoable, selects the new root). Under the SELECTED
        // entity when there is one, so a UI prefab inserted from here lands inside the canvas the author is
        // working in rather than at the scene root, where it would draw nothing.
        ECS::Entity parentEntity;
        if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
        {
            if ( auto ref = scene->FindEntityByID( *sel ) )
            {
                parentEntity = ref->get();
            }
        }

        const auto placed = prefab->Instantiate( scene.get(), *m_AssetManager, parentEntity, nullptr );
        if ( !placed )
        {
            LOG_ERROR( "{}", placed.GetError() );
            return;
        }
        const ECS::Entity root = placed.GetValue();
        if ( root )
        {
            const auto uuid = root.GetComponent<ECS::UUIDComponent>().UUID;
            Core::SelectionManager::SetSelected( uuid );
            Commands::NotifyCreated( { uuid } );
        }
    }

    void NewAssetMenu::CreateNewFolder( const DirectoryInformation& folder ) const
    {
        std::filesystem::create_directory( folder.AssetPath + "/NewFolder" );
        m_On.OnRefresh();
    }

    void NewAssetMenu::CreateNewMaterial( const DirectoryInformation& folder )
    {
        const auto path =
             UniqueIn( folder, "NewMaterial", std::string( Common::Constants::Extensions::MATERIAL_EXTENSION ) );
        // A new material states the project's default surface template (never a default by absence); no params,
        // no textures. The refresh below is what puts the new material in front of the user, so it runs only
        // when there is a file to show: a refresh over a failed write leaves the user believing the item did
        // nothing at all.
        Assets::MaterialData data;
        if ( m_AssetManager == nullptr )
        {
            LOG_ERROR( "[Content] '{}' was not created: no asset manager is bound", path.generic_string() );
            return;
        }
        if ( const auto stated = MaterialAssetUtils::StateDefaultSurface( data, *m_AssetManager ); !stated )
        {
            LOG_ERROR( "[Content] '{}' was not created: {}", path.generic_string(), stated.GetError() );
            return;
        }
        if ( const auto written = Assets::WriteMaterialFile( path, data ); !written )
        {
            LOG_ERROR( "[Content] '{}' was not created: {}", path.generic_string(), written.GetError() );
            return;
        }
        m_On.OnRefresh();
    }

    Common::BoolResultStr NewAssetMenu::CreateNewLevelSequence( const DirectoryInformation* folder ) const
    {
        if ( folder == nullptr )
            return Common::MakeError( "New Level Sequence: the Assets window has no folder open" );
        const auto path = UniqueIn( *folder, "NewLevelSequence",
                                    std::string( ::Desert::Animation::Timeline::kLevelSequenceExtension ) );
        // UE's new ULevelSequence: no bindings, no tracks, a five-second playback range on the project's tick
        // rate. The range is stated, never implied by an empty 0..0 a player would clamp to one frame.
        ::Desert::Animation::Timeline::Sequence sequence;
        sequence.Host = ::Desert::Animation::Timeline::SequenceHost::LevelSequence;
        sequence.End  = ::Desert::Animation::SecondsToFrameTime( 5.0, sequence.TickRate ).Frame;
        if ( const auto saved =
                  Assets::LevelSequenceAsset::Save( path, sequence, Common::Content::AssetGuid::Generate() );
             !saved )
            return Common::MakeFormattedError( "New Level Sequence: {}", saved.GetError() );
        // Selected once the refresh lists it (UE selects the new asset in the Content Browser).
        m_On.OnSelectAfterRefresh( path.generic_string() );
        m_On.OnRefresh();
        return Common::MakeSuccess( true );
    }

    void NewAssetMenu::CreateNewCloudAsset( const DirectoryInformation& folder, CloudAssetKind kind )
    {
        // The menu items are disabled while a generation is in flight; this is the second lock and it is not
        // redundant, because overwriting m_Bake would detach a thread still storing into m_BakeProgress.
        if ( m_BakeRunning )
            return;

        const char* stem = nullptr;
        const char* ext  = nullptr;
        switch ( kind )
        {
            case CloudAssetKind::Type:
                stem = "NewCloudType";
                ext  = Assets::kCloudTypeExtension;
                break;
            case CloudAssetKind::Layout:
                stem = "NewCloudLayout";
                ext  = Assets::kCloudLayoutExtension;
                break;
            case CloudAssetKind::NoiseVolume:
                stem = "NewCloudNoise";
                ext  = Assets::kCloudNoiseVolumeExtension;
                break;
            case CloudAssetKind::ModellingVolume:
                stem = "NewCloudBody";
                ext  = Assets::kCloudModellingVolumeExtension;
                break;
        }

        // THE DIRECTORY IS THE ONE THE ARTIST IS LOOKING AT, not Constants::Path::CLOUD_*_PATH: those name where
        // the SHIPPED library lives; where somebody puts their own asset is their business.
        const std::filesystem::path path = UniqueIn( folder, stem, ext );

        m_BakePath  = path.string();
        m_BakeLabel = path.filename().string();

        // ── The two that are numbers, written where they were asked for — through the format's own `Save`,
        //    so there is exactly one statement of each format. ─────────────────────────────────────────────
        if ( kind == CloudAssetKind::Type )
        {
            FinishCloudAsset(
                 Assets::CloudTypeAsset::Save( path, NewCloudAsset::DefaultType( path.stem().string() ) ) );
            return;
        }

        if ( kind == CloudAssetKind::Layout )
        {
            auto layout = NewCloudAsset::DefaultLayout();
            if ( !layout )
            {
                FinishCloudAsset( Common::MakeFormattedError<bool>( "{}", layout.GetError() ) );
                return;
            }

            FinishCloudAsset( Assets::CloudLayoutAsset::Save( path, layout.GetValue() ) );
            return;
        }

        // ── The two that are voxels, and cost seconds ──────────────────────────────────────────────────────
        //
        // MEASURED (Debug, minimum of three runs): the default 128^3 noise volume takes 8 730 ms to generate and
        // the shipped modelling recipe 1 585 ms — three orders of magnitude past a frame, so neither runs in
        // this handler. std::async, matching CloudNoiseVolumePanel and CloudModellingVolumePanel, which run
        // these same two bakes this same way; GenerateCloudNoiseVolume already splits itself across the
        // hardware threads, so a JobSystem worker would only nest two pools.
        m_BakeProgress.store( 0.0f );
        m_BakeCancelled.store( false );
        m_BakeRunning = true;

        m_Bake = std::async(
             std::launch::async,
             [this, path, kind]() -> Common::BoolResultStr
             {
                 // A throw here would surface from m_Bake.get() in Poll, on the UI thread, with
                 // nothing to catch it; the worker turns it into the error this menu already shows.
                 try
                 {
                     // SAVED ON THE WORKER: all four `Save`s are pure file I/O plus a log line —
                     // no AssetManager, no ECS, no GPU — exactly the set a job may touch.
                     if ( kind == CloudAssetKind::NoiseVolume )
                     {
                         auto volume = NewCloudAsset::DefaultNoiseVolume( &m_BakeProgress );
                         if ( !volume )
                             return Common::MakeFormattedError<bool>( "{}", volume.GetError() );

                         return Assets::CloudNoiseVolumeAsset::Save( path, volume.GetValue() );
                     }

                     auto body = NewCloudAsset::DefaultModellingVolume(
                          [this]( float fraction )
                          {
                              m_BakeProgress.store( fraction );
                              return !m_BakeCancelled.load();
                          } );
                     if ( !body )
                         return Common::MakeFormattedError<bool>( "{}", body.GetError() );

                     return Assets::CloudModellingVolumeAsset::Save( path, body.GetValue() );
                 }
                 catch ( const std::exception& error )
                 {
                     return Common::MakeFormattedError<bool>( "[NewAssetMenu] bake failed: {}", error.what() );
                 }
                 catch ( ... )
                 {
                     return Common::MakeFormattedError<bool>( "[NewAssetMenu] bake failed: unknown exception" );
                 }
             } );
    }

    void NewAssetMenu::Poll()
    {
        if ( !m_BakeRunning || !m_Bake.valid() )
            return;

        if ( m_Bake.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            return;

        const Common::BoolResultStr written = m_Bake.get();
        m_BakeRunning                       = false;
        FinishCloudAsset( written );
    }

    void NewAssetMenu::FinishCloudAsset( const Common::BoolResultStr& written )
    {
        if ( !written )
        {
            // NEVER SILENT (contract §1.4): `Save` refuses an unwritable directory, a full disk and data that
            // would not load back, each with the reason.
            LOG_ERROR( "[Assets] '{}' could not be created: {}", m_BakePath, written.GetError() );
            m_On.OnStatus( "Could not create '" + m_BakeLabel + "': " + written.GetError() );
            return;
        }

        m_On.OnRefresh();

        // OPENED STRAIGHT AWAY, because creating one of these is the only way to reach its editor at all: the
        // four cloud documents are contextual, keyed on an asset handle, with no View-menu entry.
        // RequestCloudDocument logs its own failures with the path.
        if ( m_AssetManager != nullptr &&
             RequestCloudDocument( m_AssetManager, m_BakePath ) != CloudDocumentRequest::Requested )
        {
            m_On.OnStatus( "Created '" + m_BakeLabel + "' but it would not open — the log says why." );
            return;
        }

        // The status line is the RED error line; a success has the file, the opened document and Save's own log
        // entry to show for itself, so it clears rather than colours one.
        m_On.OnStatus( {} );
    }

    void NewAssetMenu::DrawBakeStatus()
    {
        if ( !m_BakeRunning )
            return;

        const std::string line = "Creating '" + m_BakeLabel + "' - this takes a few seconds.";
        ImGui::TextUnformatted( line.c_str() );
        ImGui::ProgressBar( m_BakeProgress.load(), ImVec2( -1.0f, 0.0f ) );
    }
} // namespace Desert::Editor
