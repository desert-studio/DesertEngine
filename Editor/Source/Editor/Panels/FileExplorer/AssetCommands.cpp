#include "AssetCommands.hpp"

#include <Editor/Core/ActorDropPlacement.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Editor/Core/SkeletonAssignPalette.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserCommands.hpp>
#include <Editor/Panels/FileExplorer/FileExplorerPanel.hpp>
#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>
#include <Editor/Panels/WorldPartition/WorldPartitionPanel.hpp>
#include <Editor/Widgets/ThumbnailEdit.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <set>
#include <utility>

namespace Desert::Editor
{
    void AssetFileCensus::Take()
    {
        m_Files = Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH );
    }

    AssetCommands::AssetCommands( FileExplorerPanel* const& explorer, WorldPartitionPanel* const& worldPartition,
                                  const std::shared_ptr<::Desert::Core::Scene>&          mainScene,
                                  const std::shared_ptr<::Desert::Assets::AssetManager>& assets,
                                  const AssetFileCensus& files, ActiveCamera activeCamera, ShowFolder showFolder )
         : m_Explorer( &explorer ), m_WorldPartition( &worldPartition ), m_MainSceneSlot( &mainScene ),
           m_AssetsSlot( &assets ), m_Files( &files ), m_ActiveCamera( std::move( activeCamera ) ),
           m_ShowFolder( std::move( showFolder ) )
    {
    }

    void AssetCommands::AppendSelectionCommands( std::vector<PaletteCommand>& commands )
    {
        // Switching a world on (UE's "Convert Level to World Partition"). Offered UNCONDITIONALLY, unlike
        // the panel's button: a command that vanishes from the palette cannot tell the user WHY it is not
        // available, and the refusal this one returns names the scene and the grids it already has.
        commands.push_back( { "Scene", std::string( ::Desert::Core::Rules::kConvertToWorldPartitionLabel ), [this]
                              {
                                  if ( WorldPartition() == nullptr )
                                      return Common::MakeError( "convert to World Partition: the World "
                                                                "Partition window does not exist" );
                                  return WorldPartition()->ConvertSceneToWorldPartition();
                              } } );

        // The rename dialog on the Assets window's selection, with the registry's referrers listed; the
        // same dialog F2 opens.
        commands.push_back( { "Assets", "Rename the selected asset", [this]
                              {
                                  if ( Explorer() == nullptr )
                                      return Common::MakeError( "rename: the Assets window does not exist" );
                                  return Explorer()->RenameSelected();
                              } } );
        // ASSIGN SKELETON (UE's Assign Skeleton on a mesh / clip): the selected .skmesh / .anim against each
        // registered .skeleton, through the Details slot's CheckSkeletonAssignment path — a refusal lists the
        // missing and mis-parented bones and writes nothing. A clip's reference lives in its .anim, so an
        // accepted clip is saved here, as the Animation Editor saves it after the same assignment.
        {
            std::vector<std::string> skeletons;
            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Skeleton ) )
                if ( row.Guid )
                    skeletons.push_back( row.Path.generic_string() );
            const std::vector<std::string> selected =
                 Explorer() != nullptr ? Explorer()->SelectionPaths() : std::vector<std::string>{};
            for ( PaletteCommand& command : Editor::SkeletonAssignPaletteCommands(
                       selected, skeletons, std::bind_front( &AssetCommands::AssignSkeletonFromPalette, this ) ) )
                commands.push_back( std::move( command ) );
        }
        // One command per entry the Assets window shows, as a click would reach it (AF10c). Bound, not a
        // lambda: `bugprone-exception-escape` fires on a parameter-less lambda here (see RunDocumentAction).
        if ( Explorer() != nullptr )
        {
            for ( const bool folders : { false, true } )
            {
                for ( const std::string& path : Explorer()->ShownEntries( folders ) )
                {
                    commands.push_back( { "Assets", std::format( "Select asset {}", path ),
                                          std::bind_front( &FileExplorerPanel::SelectEntry,
                                                           std::to_address( Explorer() ), path ) } );
                }
            }
            // UE "Edit Thumbnail" in steps, for the selected models and materials (the tile's drag is the free
            // form). The file name addresses the entry: the selection lives in one folder, so it is unique.
            for ( const auto& subject : Explorer()->SelectedThumbnailSubjects() )
            {
                const std::string file = std::filesystem::path( subject.Asset ).filename().string();
                for ( const Editor::ThumbnailEdit::OrbitStep step : Editor::ThumbnailEdit::kOrbitSteps )
                    commands.push_back( { "Assets",
                                          std::format( "Edit Thumbnail: {} {}", file,
                                                       Editor::ThumbnailEdit::OrbitStepName( step ) ),
                                          std::bind_front( &Editor::ThumbnailEdit::EditOrbitStep,
                                                           std::filesystem::path( subject.OrbitFile ), step ) } );
            }
        }

        // Asset creation, the Assets window's context menu as commands (UE "Add Level Sequence"). Offered
        // whether or not the window exists: the refusal says why, where a missing entry would not.
        for ( PaletteCommand& command : ContentCreatePaletteCommands(
                   [this]
                   {
                       if ( Explorer() == nullptr )
                           return Common::MakeError( "New Level Sequence: the Assets window does not exist" );
                       return Explorer()->CreateNewLevelSequence();
                   },
                   [this]
                   {
                       if ( Explorer() == nullptr )
                           return Common::MakeError( "New VFX Data Channel: the Assets window does not exist" );
                       return Explorer()->CreateNewVFXDataChannel();
                   } ) )
            commands.push_back( std::move( command ) );
    }

    void AssetCommands::AppendImportCommands( std::vector<PaletteCommand>& commands )
    {
        const std::vector<std::filesystem::path>& assetFiles = m_Files->Files();
        // THE MESH DROP WITHOUT A MOUSE: one entry per model source, running the viewport's own drop body at
        // the surface the active view's centre looks at. The control channel had "Place a cube" and nothing
        // that exercised MeshDnD — AL1-5b could not check a dropped mesh for in-frame reads.
        for ( const std::filesystem::path& file : assetFiles )
        {
            std::string ext = file.extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            if ( ext != ".fbx" && ext != ".obj" && ext != ".gltf" && ext != ".glb" && ext != ".blend" )
                continue;
            const std::string path = file.generic_string();
            const std::string label =
                 std::format( "Drop into the viewport: {}",
                              file.lexically_relative( Common::Constants::Path::ASSETS_PATH ).generic_string() );
            // The same clang-tidy 18 finding as the folder entries below: the closure's implicit copy of
            // `path`, which std::function needs.
            // NOLINTBEGIN(bugprone-exception-escape)
            commands.push_back(
                 { "Assets", label, [this, path]
                   {
                       ::Desert::Core::EditorCamera* camera = ActiveEditorCamera();
                       if ( camera == nullptr || !MainScene() )
                           return PaletteCommandOutcome( false, "no viewport camera or no scene" );
                       // The view centre's ray: the surface it meets, or UE's background drop distance.
                       const Common::Math::Ray    ray( camera->GetPosition(), camera->GetDirection() );
                       ::Desert::Core::RaycastHit hit;
                       const bool                 met     = MainScene()->Raycast( ray, hit );
                       const auto                 dropped = ViewportPanel::DropMeshIntoActiveViewport(
                            path, ActorDrop::TargetFor( met ? std::optional<glm::vec3>( hit.Point ) : std::nullopt,
                                                        hit.Normal, ray.Origin, ray.Direction ) );
                       if ( !dropped )
                           return Common::MakeError<bool>( dropped.GetError() );
                       return PaletteCommandDone();
                   } } );
            // NOLINTEND(bugprone-exception-escape)
        }
        // THE IMPORT OPTIONS WINDOW WITHOUT A MOUSE: its three buttons, each running the button's own body.
        const auto importOptionsCommand = [&commands]( const char* label, Common::BoolResultStr ( *answer )() )
        {
            commands.push_back( { "Assets", label, [answer]
                                  {
                                      if ( const auto answered = answer(); !answered )
                                          return Common::MakeError<bool>( answered.GetError() );
                                      return PaletteCommandDone();
                                  } } );
        };
        importOptionsCommand( "Import Options: Import", [] { return ImportOptions::ConfirmImport( false ); } );
        importOptionsCommand( "Import Options: Import All", [] { return ImportOptions::ConfirmImport( true ); } );
        importOptionsCommand( "Import Options: Cancel", [] { return ImportOptions::CancelImport(); } );
        // ITS FIELDS WITHOUT A MOUSE: the same edits the fields make (ImportOptions::SetShown*), on the options
        // the window shows. Uniform Scale offers the unit conversions (metres, decimetres, centimetres, ...).
        for ( const float scale : { 0.01f, 0.1f, 1.0f, 10.0f, 100.0f } )
            commands.push_back( { "Assets", std::format( "Import Options: Uniform Scale {}", scale ),
                                  [scale] { return ImportOptions::SetShownUniformScale( scale ); } } );
        for ( const auto& [label, axis] :
              { std::pair{ "From File", Assets::MeshSourceUpAxis::FromFile },
                std::pair{ "Y", Assets::MeshSourceUpAxis::Y }, std::pair{ "Z", Assets::MeshSourceUpAxis::Z } } )
            commands.push_back( { "Assets", std::format( "Import Options: Up Axis {}", label ),
                                  [axis] { return ImportOptions::SetShownUpAxis( axis ); } } );
        for ( const bool on : { true, false } )
            commands.push_back( { "Assets", std::format( "Import Options: Combine Meshes {}", on ? "on" : "off" ),
                                  [on] { return ImportOptions::SetShownCombineMeshes( on ); } } );
        // UE's Content Browser > Asset Actions > Reimport (and the Import Settings fields Reimport imports with):
        // over the assets SELECTED IN THE CONTENT BROWSER, the RMB item's and the Import Settings button's own
        // ImportOptions bodies. Without a selected asset each is unavailable and says why; an asset with no import
        // source is refused by name by the body it reaches.
        const auto selectedAssets = [this]() -> Common::ResultStr<std::vector<std::string>>
        {
            if ( Explorer() == nullptr )
                return Common::MakeError<std::vector<std::string>>( "the Content Browser is not open" );
            std::vector<std::string> selected = Explorer()->SelectionPaths();
            if ( selected.empty() )
                return Common::MakeError<std::vector<std::string>>(
                     "no asset is selected in the Content Browser" );
            return Common::MakeSuccess( std::move( selected ) );
        };
        // One body over every selected asset; the first refusal is the command's answer, the others still run.
        const auto overSelectedAssets =
             [selectedAssets]( const std::function<Common::BoolResultStr( const std::filesystem::path& )>& body )
             -> Common::BoolResultStr
        {
            const auto assets = selectedAssets();
            if ( !assets )
                return Common::MakeError<bool>( assets.GetError() );
            Common::BoolResultStr outcome = PaletteCommandDone();
            for ( const std::string& asset : assets.GetValue() )
                if ( const auto done = body( asset ); !done && outcome )
                    outcome = Common::MakeError<bool>( std::format( "'{}': {}", asset, done.GetError() ) );
            return outcome;
        };
        // The Content Browser's commands — the item context menu's rows, the same executor (UE's
        // FContentBrowserCommands). Reimport over the selection is "Content Browser / Reimport".
        if ( Explorer() != nullptr )
            for ( const Editor::ContentBrowserCommand command : Editor::kContentBrowserCommandOrder )
                commands.push_back( { std::string( Editor::CommandInfo( command ).Context ),
                                      std::string( Editor::CommandInfo( command ).Label ),
                                      std::bind_front( &FileExplorerPanel::RunCommand,
                                                       std::to_address( Explorer() ), command ) } );
        // UE's SyncBrowserToFolders / SyncBrowserToAssets: the label carries the path (as "Select asset <path>"
        // does), one entry per folder / file under the browser's root, so a client reaches any asset.
        if ( Explorer() != nullptr )
        {
            for ( const std::string& folder : Explorer()->ContentFolders() )
                commands.push_back( { std::string( Editor::kContentBrowserContext ),
                                      Editor::ContentBrowserPathLabel( Editor::kGoToFolderLabel, folder ),
                                      std::bind_front( &FileExplorerPanel::GoToFolder,
                                                       std::to_address( Explorer() ), folder ) } );
            for ( const std::string& file : Explorer()->ContentFiles() )
                commands.push_back(
                     { std::string( Editor::kContentBrowserContext ),
                       Editor::ContentBrowserPathLabel( Editor::kSyncToAssetLabel, file ),
                       std::bind_front( &FileExplorerPanel::SyncToAsset, std::to_address( Explorer() ), file ) } );
        }
        for ( const float scale : { 0.01f, 0.1f, 1.0f, 10.0f, 100.0f } )
            commands.push_back( { "Assets", std::format( "Import Settings: Uniform Scale {}", scale ),
                                  [overSelectedAssets, scale]
                                  {
                                      return overSelectedAssets(
                                           [scale]( const std::filesystem::path& asset )
                                           { return ImportOptions::SetSectionUniformScale( asset, scale ); } );
                                  } } );
        for ( const auto& [label, axis] :
              { std::pair{ "From File", Assets::MeshSourceUpAxis::FromFile },
                std::pair{ "Y", Assets::MeshSourceUpAxis::Y }, std::pair{ "Z", Assets::MeshSourceUpAxis::Z } } )
            commands.push_back(
                 { "Assets", std::format( "Import Settings: Up Axis {}", label ), [overSelectedAssets, axis]
                   {
                       return overSelectedAssets( [axis]( const std::filesystem::path& asset )
                                                  { return ImportOptions::SetSectionUpAxis( asset, axis ); } );
                   } } );
    }

    void AssetCommands::AppendFolderCommands( std::vector<PaletteCommand>& commands )
    {
        const std::vector<std::filesystem::path>& assetFiles = m_Files->Files();
        // FOLDERS, one "Open folder: <path under the assets root>" entry each (the ONE folder command — the
        // Assets window's own per-shown-folder duplicate that took an absolute path is gone): brings the Assets
        // browser forward ON that folder. Derived from the SAME content enumeration as the "Open" entries above
        // (every folder that holds content, each ancestor up to the assets root included), not from a second walk
        // of the disk: that one call sees a mounted .dpak as well as loose files, and the ContentScanners gate
        // holds every content walk to it. A folder with no file anywhere beneath it is therefore not offered,
        // which is the packaged project's truth too. The label is the path under the assets root.
        {
            const std::filesystem::path assetsRoot =
                 std::filesystem::path( Common::Constants::Path::ASSETS_PATH ).lexically_normal();
            std::set<std::string> folders;
            for ( const std::filesystem::path& file : assetFiles )
            {
                std::error_code             ec;
                const std::filesystem::path rel =
                     std::filesystem::relative( file, Common::Constants::Path::ASSETS_PATH, ec );
                if ( ec || rel.empty() || *rel.begin() == std::filesystem::path( ".." ) )
                    continue; // not under the assets root (the enumeration's contract, but not trusted blind)
                for ( std::filesystem::path dir = rel.parent_path(); !dir.empty(); dir = dir.parent_path() )
                    folders.insert( dir.generic_string() );
            }
            for ( const std::string& label : folders )
            {
                const std::string folder = ( assetsRoot / label ).generic_string();
                // clang-tidy 18 reports every palette lambda that captures a std::string by copy (the "Open",
                // "Menu" and "Open Scene" entries above and below draw the same finding): it blames the
                // closure's implicit copy, which std::function needs; nothing in the body throws.
                // NOLINTBEGIN(bugprone-exception-escape)
                commands.push_back( { "Assets", std::format( "Open folder: {}", label ),
                                      [this, folder] { return ShowFolderInBrowser( folder ); } } );
                // NOLINTEND(bugprone-exception-escape)
            }
        }
    }

    Common::BoolResultStr AssetCommands::AssignSkeletonFromPalette( const std::string& subject,
                                                                    const std::string& skeleton ) const
    {
        using Common::Content::ContentKind;
        std::optional<Common::Content::AssetGuid> guid;
        for ( const auto& row : Assets::ContentRegistry::Rows( ContentKind::Skeleton ) )
            if ( row.Guid && row.Path.generic_string() == skeleton )
                guid = *row.Guid;
        if ( !guid )
            return Common::MakeError(
                 std::format( "Assign Skeleton: '{}' is not a registered .skeleton", skeleton ) );
        const std::filesystem::path path( subject );
        auto                        load = [&]<typename T>() -> Common::ResultStr<std::shared_ptr<T>>
        {
            auto asset = Manager()->FindByPath<T>( path );
            if ( !asset )
                asset = Manager()->CreateAsset<T>( path, false );
            if ( !asset )
                return Common::MakeError<std::shared_ptr<T>>(
                     std::format( "Assign Skeleton: '{}' could not be registered", subject ) );
            if ( const auto loaded = asset->EnsureLoaded( *Manager() ); !loaded )
                return Common::MakeError<std::shared_ptr<T>>(
                     std::format( "Assign Skeleton: '{}' would not load: {}", subject, loaded.GetError() ) );
            return Common::MakeSuccess( std::shared_ptr<T>( std::move( asset ) ) );
        };
        if ( path.extension() == ".skmesh" )
        {
            const auto mesh = load.template operator()<Assets::SkinnedMeshAsset>();
            if ( !mesh )
                return Common::MakeError( mesh.GetError() );
            return SkeletonSlots::AssignMeshSkeleton( *Manager(), *mesh.GetValue(), *guid );
        }
        const auto clip = load.template operator()<Assets::AnimationAsset>();
        if ( !clip )
            return Common::MakeError( clip.GetError() );
        if ( auto assigned = SkeletonSlots::AssignClipSkeleton( *Manager(), *clip.GetValue(), *guid ); !assigned )
            return assigned;
        return Assets::Serialization::SaveClipToFile( path, clip.GetValue()->GetClip() );
    }
} // namespace Desert::Editor
