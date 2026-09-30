#include "FoliageCommands.hpp"

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp>
#include <filesystem>
#include <format>
#include <string>
#include <vector>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliagePaintTool.hpp>
#include <functional>
#include <utility>

namespace Desert::Editor
{
    void AppendFoliageCommands( std::vector<PaletteCommand>&                  commands,
                                const std::shared_ptr<::Desert::Core::Scene>& scene,
                                const std::shared_ptr<Assets::AssetManager>&  assets )
    {
        // THE FOLIAGE PALETTE WITHOUT A MOUSE: the mode, and one entry per collection running the palette's own
        // collection drop (FO-2), so a frame can show types that came from a collection unattended.
        commands.push_back( { "Foliage", "Foliage mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Foliage );
                                  return PaletteCommandDone();
                              } } );
        {
            std::error_code ec;
            for ( const auto& dir :
                  std::filesystem::directory_iterator( Common::Constants::Path::COLLECTIONS_PATH, ec ) )
            {
                const std::filesystem::path manifest = dir.path() / "collection.json";
                if ( !dir.is_directory() || !std::filesystem::exists( manifest, ec ) )
                    continue;
                const std::string path = manifest.generic_string();
                // NOLINTBEGIN(bugprone-exception-escape)
                commands.push_back(
                     { "Foliage", "Add collection to the palette: " + dir.path().filename().string(),
                       [&scene, &assets, path]
                       {
                           if ( !scene || !assets )
                               return PaletteCommandOutcome( false, "no scene or no asset manager" );
                           Core::ViewportMode::Set( Core::EditorMode::Foliage );
                           return Tools::FoliagePaintTool::AddCollection( *scene, *assets, path );
                       } } );
                // NOLINTEND(bugprone-exception-escape)
            }
        }
        // FOLIAGE (FO-3): a type file into the palette, and the stroke a hand gives at the viewport centre.
        {
            // Through the one content enumeration (loose files and a mounted .dpak alike).
            for ( const std::filesystem::path& file :
                  Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH ) )
            {
                const std::string ext  = file.extension().string();
                const std::string path = file.generic_string();
                // NOLINTBEGIN(bugprone-exception-escape)
                // FO-UI1: the "+ Foliage" picker's static meshes (a type is found or made for the mesh).
                if ( ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" )
                    commands.push_back(
                         { "Foliage", "Add mesh to the palette: " + file.stem().string(), [&scene, &assets, path]
                           {
                               if ( !scene || !assets )
                                   return PaletteCommandOutcome( false, "no scene or no asset manager" );
                               Core::ViewportMode::Set( Core::EditorMode::Foliage );
                               return Tools::FoliagePaintTool::AddMeshFile( *scene, *assets, path );
                           } } );
                // FO-8: the "+ Foliage" picker's prefabs (a Prefab type is found or made for the prefab).
                if ( ext == ".deprefab" )
                    commands.push_back(
                         { "Foliage", "Add prefab to the palette: " + file.stem().string(), [&scene, &assets, path]
                           {
                               if ( !scene || !assets )
                                   return PaletteCommandOutcome( false, "no scene or no asset manager" );
                               Core::ViewportMode::Set( Core::EditorMode::Foliage );
                               return Tools::FoliagePaintTool::AddPrefabFile( *scene, *assets, path );
                           } } );
                if ( ext != Assets::Serialization::kFoliageTypeExtension )
                    continue;
                commands.push_back(
                     { "Foliage", "Add type to the palette: " + file.stem().string(), [&scene, &assets, path]
                       {
                           if ( !scene || !assets )
                               return PaletteCommandOutcome( false, "no scene or no asset manager" );
                           Core::ViewportMode::Set( Core::EditorMode::Foliage );
                           return Tools::FoliagePaintTool::AddTypeFile( *scene, *assets, path );
                       } } );
                // FO-UI1: the row menu's Replace.
                commands.push_back(
                     { "Foliage", "Replace the edited type with: " + file.stem().string(), [&scene, &assets, path]
                       {
                           const auto editing = Core::FoliagePaint::EditingType();
                           if ( !scene || !assets || !editing )
                               return PaletteCommandOutcome( false, "no scene, asset manager or edited type" );
                           return Tools::FoliagePaintTool::ReplaceType( *scene, *assets, *editing, path );
                       } } );
                // NOLINTEND(bugprone-exception-escape)
            }
        }
        commands.push_back( { "Foliage", "Stroke at viewport centre",
                              [] { return ViewportPanel::StrokeFoliageInActiveViewport(); } } );
        // FOLIAGE (FO-4): the tool the stroke above uses, and what UE does to the selected instances.
        // NOLINTBEGIN(bugprone-exception-escape)
        for ( const auto tool : { Core::FoliageTool::Paint, Core::FoliageTool::Single, Core::FoliageTool::Select,
                                  Core::FoliageTool::Lasso, Core::FoliageTool::Remove, Core::FoliageTool::Reapply,
                                  Core::FoliageTool::Fill } )
            commands.push_back( { "Foliage", std::string( "Tool: " ) + Core::FoliageToolName( tool ),
                                  [tool]() -> Common::BoolResultStr
                                  {
                                      Core::ViewportMode::Set( Core::EditorMode::Foliage );
                                      Core::FoliagePaint::Tool() = tool;
                                      return BOOLSUCCESS;
                                  } } );
        commands.push_back( { "Foliage", "Delete selected instances", [&scene]() -> Common::BoolResultStr
                              {
                                  if ( !scene )
                                      return PaletteCommandOutcome( false, "no scene" );
                                  return Tools::FoliagePaintTool::RemoveSelected( *scene );
                              } } );
        commands.push_back(
             { "Foliage", "Move selected instances by the panel offset", [&scene]() -> Common::BoolResultStr
               {
                   if ( !scene )
                       return PaletteCommandOutcome( false, "no scene" );
                   return Tools::FoliagePaintTool::MoveSelected( *scene, Core::FoliagePaint::MoveOffset() );
               } } );
        commands.push_back( { "Foliage", "Fill the selected mesh", [&scene, &assets]() -> Common::BoolResultStr
                              {
                                  if ( !scene || !assets )
                                      return PaletteCommandOutcome( false, "no scene or no asset manager" );
                                  const auto& selected = Core::SelectionManager::GetSelected();
                                  if ( !selected )
                                      return PaletteCommandOutcome( false, "no entity is selected" );
                                  return Tools::FoliagePaintTool::FillEntity( *scene, *assets, *selected );
                              } } );
        commands.push_back( { "Foliage", "Select no instances", [&scene]() -> Common::BoolResultStr
                              {
                                  if ( !scene )
                                      return PaletteCommandOutcome( false, "no scene" );
                                  return Tools::FoliagePaintTool::SelectNone( *scene );
                              } } );
        // FOLIAGE PANEL (FO-UI1): every control of the panel (Editor/Panels/Foliage/FoliagePanel.cpp) without a
        // mouse; the FoliagePalette suite's census holds the two in step.
        commands.push_back( { "Foliage", "Select all instances of the checked types",
                              [&scene]() -> Common::BoolResultStr
                              {
                                  if ( !scene )
                                      return PaletteCommandOutcome( false, "no scene" );
                                  return Tools::FoliagePaintTool::SelectAllInstances( *scene );
                              } } );
        commands.push_back( { "Foliage", "Add the selected entity to the palette",
                              [&scene, &assets]() -> Common::BoolResultStr
                              {
                                  if ( !scene || !assets )
                                      return PaletteCommandOutcome( false, "no scene or no asset manager" );
                                  const auto& selected = Core::SelectionManager::GetSelected();
                                  if ( !selected )
                                      return PaletteCommandOutcome( false, "no entity is selected" );
                                  Core::ViewportMode::Set( Core::EditorMode::Foliage );
                                  return Tools::FoliagePaintTool::AddFromEntity( *scene, *assets, *selected );
                              } } );
        commands.push_back( { "Foliage", "Palette: grid view", []() -> Common::BoolResultStr
                              {
                                  Core::FoliagePaint::GridView() = true;
                                  return BOOLSUCCESS;
                              } } );
        commands.push_back( { "Foliage", "Palette: list view", []() -> Common::BoolResultStr
                              {
                                  Core::FoliagePaint::GridView() = false;
                                  return BOOLSUCCESS;
                              } } );
        commands.push_back( { "Foliage", "Palette: clear the search", []() -> Common::BoolResultStr
                              {
                                  Core::FoliagePaint::Search().clear();
                                  return BOOLSUCCESS;
                              } } );
        commands.push_back( { "Foliage", "Palette: save as preset", [&scene, &assets]() -> Common::BoolResultStr
                              {
                                  if ( !scene || !assets )
                                      return PaletteCommandOutcome( false, "no scene or no asset manager" );
                                  return Tools::FoliagePaintTool::SavePreset( *scene, *assets,
                                                                              Core::FoliagePaint::PresetName() );
                              } } );
        commands.push_back( { "Foliage", "Preview the brush footprint at viewport centre",
                              [] { return ViewportPanel::PreviewFoliageInActiveViewport(); } } );
        commands.push_back( { "Foliage", "Brush filter: landscape on/off", []() -> Common::BoolResultStr
                              {
                                  Core::FoliagePaint::FilterLandscape() = !Core::FoliagePaint::FilterLandscape();
                                  return BOOLSUCCESS;
                              } } );
        commands.push_back( { "Foliage", "Brush filter: static meshes on/off", []() -> Common::BoolResultStr
                              {
                                  Core::FoliagePaint::FilterStaticMesh() = !Core::FoliagePaint::FilterStaticMesh();
                                  return BOOLSUCCESS;
                              } } );
        commands.push_back( { "Foliage", "Brush layer filter: off", []() -> Common::BoolResultStr
                              {
                                  Core::FoliagePaint::BrushLayers().clear();
                                  return BOOLSUCCESS;
                              } } );
        if ( scene )
        {
            std::set<std::string> layerNames;
            for ( const auto& entity : scene->GetAllEntities() )
                if ( entity.HasComponent<ECS::LandscapeTileComponent>() &&
                     entity.GetComponent<ECS::LandscapeTileComponent>().Heights )
                    for ( const auto& layer :
                          entity.GetComponent<ECS::LandscapeTileComponent>().Heights->WeightLayers() )
                        layerNames.insert( layer.Name );
            for ( const auto& name : layerNames )
                commands.push_back( { "Foliage", "Brush layer filter: toggle " + name,
                                      [name]() -> Common::BoolResultStr
                                      {
                                          auto&      layers = Core::FoliagePaint::BrushLayers();
                                          const auto it     = std::ranges::find( layers, name );
                                          if ( it == layers.end() )
                                              layers.push_back( name );
                                          else
                                              layers.erase( it );
                                          return BOOLSUCCESS;
                                      } } );
            // The rows: one "edit" per listed type (the row's click), then the edited type's own actions.
            for ( const auto& field : Tools::FoliagePaintTool::PaletteFields( *scene ) )
            {
                const auto uuid = field.GetComponent<ECS::UUIDComponent>().UUID;
                commands.push_back( { "Foliage", "Palette: edit " + field.GetComponent<ECS::TagComponent>().Tag,
                                      [uuid]() -> Common::BoolResultStr
                                      {
                                          Core::FoliagePaint::SetEditingType( uuid );
                                          return BOOLSUCCESS;
                                      } } );
            }
        }
        using FieldAction = std::function<Common::BoolResultStr( const Common::UUID& )>;
        const std::pair<const char*, FieldAction> editedActions[] = {
             { "Edited type: toggle checked (paint with it)",
               []( const Common::UUID& uuid ) -> Common::BoolResultStr
               {
                   Core::FoliagePaint::ToggleActive( uuid );
                   return BOOLSUCCESS;
               } },
             { "Edited type: toggle visibility", [&scene]( const Common::UUID& uuid )
               { return Tools::FoliagePaintTool::ToggleTypeVisible( *scene, uuid ); } },
             { "Edited type: select all instances", [&scene]( const Common::UUID& uuid )
               { return Tools::FoliagePaintTool::SelectTypeInstances( *scene, uuid ); } },
             { "Edited type: save as asset", [&scene, &assets]( const Common::UUID& uuid )
               { return Tools::FoliagePaintTool::SaveTypeCopy( *scene, *assets, uuid ); } },
             { "Edited type: show in Content Browser", [&scene]( const Common::UUID& uuid )
               { return Tools::FoliagePaintTool::ShowTypeInBrowser( *scene, uuid ); } },
             { "Edited type: remove from the palette", [&scene]( const Common::UUID& uuid )
               { return Tools::FoliagePaintTool::RemoveType( *scene, uuid ); } },
             { "Edited type: cast shadows on/off",
               [&scene]( const Common::UUID& uuid ) -> Common::BoolResultStr
               {
                   const auto ref = scene->FindEntityByID( uuid );
                   if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                       return Common::MakeError( "the edited type has no instanced mesh" );
                   bool& casts = ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().CastShadows;
                   casts       = !casts;
                   return BOOLSUCCESS;
               } },
        };
        for ( const auto& [label, action] : editedActions )
            commands.push_back( { "Foliage", label, [&scene, &assets, action]() -> Common::BoolResultStr
                                  {
                                      const auto editing = Core::FoliagePaint::EditingType();
                                      if ( !scene || !assets || !editing )
                                          return PaletteCommandOutcome( false,
                                                                        "no scene or no edited foliage type" );
                                      return action( *editing );
                                  } } );
        // NOLINTEND(bugprone-exception-escape)
    }
} // namespace Desert::Editor
