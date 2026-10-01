#include "LandscapeCommands.hpp"

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Editor/Core/Commands/LandscapeLayerCommands.hpp>
#include <Editor/Import/LandscapeHeightmapIO.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <algorithm>
#include <initializer_list>
#include <utility>

namespace Desert::Editor
{
    // The palette's "Import heightmap as a new landscape" entry, with the panel's current New Landscape settings.
    // Named and bound rather than a lambda: `bugprone-exception-escape` fires on a parameter-less lambda that
    // copies a path into its closure (see DocumentHost::RunDocumentAction).
    static Common::BoolResultStr
    ImportHeightmapAsNewLandscape( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                   const std::filesystem::path&                  file )
    {
        auto made = Commands::ImportLandscapeHeightmapAsNew( scene, file,
                                                             Core::LandscapeSculptState::Get().NewLandscape );
        return made.IsSuccess() ? PaletteCommandDone() : PaletteCommandOutcome( false, made.GetError() );
    }

    void AppendLandscapeCommands( std::vector<PaletteCommand>&                  commands,
                                  const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        // LANDSCAPE SCULPT. Every setting of the Landscape panel is stepped by a row of LandscapeToolControls(),
        // offered here from that same table; the stroke itself is the click a hand would make at the viewport
        // centre, because PaletteCommand::Run takes no coordinates -- aim the camera, then stroke.
        commands.push_back( { "Landscape", "Sculpt mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Landscape );
                                  Core::LandscapeSculptState::Get().Mode = Core::LandscapeEdMode::Sculpt;
                                  return PaletteCommandDone();
                              } } );
        // NEW LANDSCAPE (UE's Manage tab): the mode, the fill, the erosion passes and Create, so a generated
        // landscape can be made and photographed unattended.
        commands.push_back( { "Landscape", "Manage mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Landscape );
                                  Core::LandscapeSculptState::Get().Mode = Core::LandscapeEdMode::Manage;
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "Landscape", "New Landscape: noise fill with erosion and hydro erosion", []
                              {
                                  auto& s        = Core::LandscapeSculptState::Get().NewLandscape;
                                  s.Fill         = World::Landscape::LandscapeGenerateFill::Noise;
                                  s.Erosion      = true;
                                  s.HydroErosion = true;
                                  return PaletteCommandDone();
                              } } );
        // The same fill without the passes: with the preset above, one seed photographed before and after erosion.
        commands.push_back( { "Landscape", "New Landscape: noise fill without erosion", []
                              {
                                  auto& s        = Core::LandscapeSculptState::Get().NewLandscape;
                                  s.Fill         = World::Landscape::LandscapeGenerateFill::Noise;
                                  s.Erosion      = false;
                                  s.HydroErosion = false;
                                  return PaletteCommandDone();
                              } } );
        // Starts the background run and answers at once; `state` reports it (BackgroundWork::LandscapeGenerate).
        commands.push_back( { "Landscape", "New Landscape: Create", [&scene] {
                                 return Commands::StartCreateLandscape(
                                      scene, Core::LandscapeSculptState::Get().NewLandscape );
                             } } );
        commands.push_back( { "Landscape", "New Landscape: Cancel", []
                              {
                                  if ( !Commands::IsCreatingLandscape() )
                                      return PaletteCommandOutcome( false,
                                                                    "new landscape: nothing is being generated" );
                                  Commands::CancelCreateLandscape();
                                  return PaletteCommandDone();
                              } } );
        // HEIGHTMAP IMPORT / EXPORT (UE's Manage mode, LS-11). PaletteCommand::Run takes no argument, so the path
        // is in the label: one Import entry per 16-bit PNG / RAW file under Assets/Landscape/Heightmaps (as "Drop
        // into the viewport:" names each mesh), and Export entries that name the file they write there.
        {
            const std::filesystem::path heightmaps =
                 Common::Constants::Path::ASSETS_PATH / "Landscape" / "Heightmaps";
            std::vector<std::filesystem::path> files;
            std::error_code                    ec;
            for ( const auto& entry : std::filesystem::directory_iterator( heightmaps, ec ) )
                if ( entry.is_regular_file() &&
                     World::Landscape::LandscapeHeightmapFormatOf( entry.path() ).IsSuccess() )
                    files.push_back( entry.path() );
            std::sort( files.begin(), files.end() );
            for ( const auto& file : files )
            {
                const std::string rel =
                     file.lexically_relative( Common::Constants::Path::ASSETS_PATH ).generic_string();
                commands.push_back(
                     { "Landscape", std::format( "Import heightmap as a new landscape: {}", rel ),
                       std::bind_front( &ImportHeightmapAsNewLandscape, std::cref( scene ), file ) } );
                commands.push_back(
                     { "Landscape", std::format( "Import heightmap into the landscape: {}", rel ),
                       std::bind_front( &Commands::ImportLandscapeHeightmap, std::cref( scene ), file ) } );
            }
            for ( const auto& [name, selected] : std::initializer_list<std::pair<const char*, bool>>{
                       { "Landscape.png", false }, { "Landscape.r16", false }, { "SelectedTiles.png", true } } )
            {
                const std::filesystem::path file = heightmaps / name;
                const std::string           rel =
                     file.lexically_relative( Common::Constants::Path::ASSETS_PATH ).generic_string();
                commands.push_back( { "Landscape",
                                      selected ? std::format( "Export heightmap of the selected tiles: {}", rel )
                                               : std::format( "Export heightmap: {}", rel ),
                                      std::bind_front( &Commands::ExportLandscapeHeightmap, std::cref( scene ),
                                                       file, selected ) } );
            }
        }
        // LANDSCAPE PAINT (UE's Paint tab): the mode, its one tool, the target layer and the "+" of the Target
        // Layers list, so a frame can show a list and a stroke unattended.
        for ( const char* label : { "Paint mode", "Tool: Paint" } )
        {
            commands.push_back( { "Landscape", label, []
                                  {
                                      Core::ViewportMode::Set( Core::EditorMode::Landscape );
                                      Core::LandscapeSculptState::Get().Mode = Core::LandscapeEdMode::Paint;
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Landscape", "Create Layer Info", [&scene]
                              {
                                  auto added = Commands::AddLandscapeLayer( scene );
                                  if ( !added.IsSuccess() )
                                      return PaletteCommandOutcome( false, added.GetError() );
                                  auto& paint = Core::LandscapeSculptState::Get().Paint;
                                  if ( paint.Layer.empty() )
                                      paint.Layer = added.GetValue();
                                  return PaletteCommandDone();
                              } } );
        // Edit Layers (LandscapePanel::DrawEditLayers): every button of a row acts on the EDITING layer here, so a
        // frame can add a layer, stroke into it and hide it unattended.
        commands.push_back( { "Landscape", "Create Edit Layer", [&scene]
                              {
                                  auto added = Commands::AddLandscapeEditLayer( scene );
                                  return added.IsSuccess() ? PaletteCommandDone()
                                                           : PaletteCommandOutcome( false, added.GetError() );
                              } } );
        {
            // The editing layer's Guid: null means the stack's bottom layer, as the brushes read it.
            const auto editingLayer = [&scene]() -> Common::UUID
            {
                const auto& editing = Core::LandscapeSculptState::Get().EditingLayer;
                if ( !editing.IsNull() || !scene )
                    return editing;
                auto&      registry  = scene->GetRegistry();
                const auto landscape = ECS::FirstLandscape( registry );
                const auto root =
                     landscape ? ECS::FindLandscapeRootEntity( registry, *landscape ) : entt::entity( entt::null );
                if ( root == entt::null ||
                     registry.get<ECS::LandscapeComponent>( root ).EditLayers.Layers.empty() )
                    return editing;
                return registry.get<ECS::LandscapeComponent>( root ).EditLayers.Layers.front().Guid;
            };
            const auto layerOf =
                 [&scene]( const Common::UUID& guid ) -> const World::Landscape::LandscapeEditLayer*
            {
                auto&      registry  = scene->GetRegistry();
                const auto landscape = ECS::FirstLandscape( registry );
                const auto root =
                     landscape ? ECS::FindLandscapeRootEntity( registry, *landscape ) : entt::entity( entt::null );
                return root == entt::null ? nullptr
                                          : registry.get<ECS::LandscapeComponent>( root ).EditLayers.Find( guid );
            };
            const auto outcome = []( const Common::BoolResultStr& r )
            { return r.IsSuccess() ? PaletteCommandDone() : PaletteCommandOutcome( false, r.GetError() ); };
            commands.push_back(
                 { "Landscape", "Editing edit layer: toggle visibility", [&scene, editingLayer, layerOf, outcome]
                   {
                       const auto  guid  = editingLayer();
                       const auto* layer = scene ? layerOf( guid ) : nullptr;
                       if ( layer == nullptr )
                           return PaletteCommandOutcome( false, "no editing edit layer" );
                       return outcome( Commands::SetLandscapeEditLayerVisible( scene, guid, !layer->Visible ) );
                   } } );
            commands.push_back(
                 { "Landscape", "Editing edit layer: toggle lock", [&scene, editingLayer, layerOf, outcome]
                   {
                       const auto  guid  = editingLayer();
                       const auto* layer = scene ? layerOf( guid ) : nullptr;
                       if ( layer == nullptr )
                           return PaletteCommandOutcome( false, "no editing edit layer" );
                       return outcome( Commands::SetLandscapeEditLayerLocked( scene, guid, !layer->Locked ) );
                   } } );
            commands.push_back( { "Landscape", "Editing edit layer: delete", [&scene, editingLayer, outcome] {
                                     return outcome( Commands::RemoveLandscapeEditLayer( scene, editingLayer() ) );
                                 } } );
            for ( const float alpha : { 0.0f, 0.5f, 1.0f } )
                commands.push_back( { "Landscape", std::format( "Editing edit layer: alphas {:.1f}", alpha ),
                                      [&scene, editingLayer, outcome, alpha] {
                                          return outcome( Commands::SetLandscapeEditLayerAlpha(
                                               scene, editingLayer(), alpha, alpha ) );
                                      } } );
            if ( scene )
            {
                auto&      registry  = scene->GetRegistry();
                const auto landscape = ECS::FirstLandscape( registry );
                const auto root =
                     landscape ? ECS::FindLandscapeRootEntity( registry, *landscape ) : entt::entity( entt::null );
                if ( root != entt::null )
                    for ( const auto& layer : registry.get<ECS::LandscapeComponent>( root ).EditLayers.Layers )
                        commands.push_back( { "Landscape", std::format( "Edit layer: {}", layer.Name ),
                                              [guid = layer.Guid]
                                              {
                                                  Core::LandscapeSculptState::Get().EditingLayer = guid;
                                                  return PaletteCommandDone();
                                              } } );
            }
        }
        // The Visibility target (UE's Visibility tool): paint cuts a hole, the lowering stroke (invert) fills it.
        commands.push_back( { "Landscape", "Target layer: Visibility (holes)", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Landscape );
                                  Core::LandscapeSculptState::Get().Mode = Core::LandscapeEdMode::Paint;
                                  Core::LandscapeSculptState::Get().Paint.Layer =
                                       std::string( World::Landscape::kLandscapeVisibilityLayerName );
                                  return PaletteCommandDone();
                              } } );
        if ( scene )
        {
            auto&      registry  = scene->GetRegistry();
            const auto landscape = ECS::FirstLandscape( registry );
            const auto root =
                 landscape ? ECS::FindLandscapeRootEntity( registry, *landscape ) : entt::entity( entt::null );
            if ( root != entt::null )
            {
                // A layer whose `.delayerinfo` is not read yet has no name to offer; it appears once it is.
                auto& layers = *Runtime::ResourceRegistry::GetLandscapeLayerInfoService();
                for ( const Assets::AssetHandle& handle : registry.get<ECS::LandscapeComponent>( root ).Layers )
                {
                    const auto* info = layers.Get( handle );
                    if ( info == nullptr )
                        continue;
                    commands.push_back(
                         { "Landscape", std::format( "Target layer: {}", info->LayerName ),
                           [&scene, handle, name = info->LayerName]
                           {
                               auto&      reg = scene->GetRegistry();
                               const auto id  = ECS::FirstLandscape( reg );
                               const auto r =
                                    id ? ECS::FindLandscapeRootEntity( reg, *id ) : entt::entity( entt::null );
                               bool found = false;
                               if ( r != entt::null )
                                   for ( const auto& l : reg.get<ECS::LandscapeComponent>( r ).Layers )
                                       found = found || l == handle;
                               if ( !found )
                                   return PaletteCommandOutcome(
                                        false, std::format( "landscape layer '{}' no longer exists", name ) );
                               Core::LandscapeSculptState::Get().Paint.Layer = name;
                               return PaletteCommandDone();
                           } } );
                }
            }
        }
        for ( auto& control : Core::LandscapeToolControls() )
        {
            if ( control.Request != Core::LandscapeStrokeRequest::None )
            {
                commands.push_back( { "Landscape", control.Label, [request = control.Request]
                                      {
                                          if ( Core::ViewportMode::Get() != Core::EditorMode::Landscape )
                                              return PaletteCommandOutcome( false,
                                                                            "the Landscape mode is not active; "
                                                                            "run 'Landscape: Sculpt mode' first" );
                                          if ( auto refusal = Core::LandscapeSculptState::Get().StrokeRefusal() )
                                              return PaletteCommandOutcome( false, *refusal );
                                          Core::LandscapeSculptState::Get().Request = request;
                                          return PaletteCommandDone();
                                      } } );
                continue;
            }
            commands.push_back( { "Landscape", control.Label, [apply = control.Apply]
                                  {
                                      apply( Core::LandscapeSculptState::Get().Settings );
                                      return PaletteCommandDone();
                                  } } );
        }
        for ( const bool lower : { false, true } )
        {
            commands.push_back(
                 { "Landscape",
                   lower ? "Stroke at the viewport centre, lowering" : "Stroke at the viewport centre", [lower]
                   {
                       if ( Core::ViewportMode::Get() != Core::EditorMode::Landscape )
                           return PaletteCommandOutcome( false, "the Landscape mode is not active; "
                                                                "run 'Landscape: Sculpt mode' first" );
                       if ( auto refusal = Core::LandscapeSculptState::Get().StrokeRefusal() )
                           return PaletteCommandOutcome( false, *refusal );
                       Core::LandscapeSculptState::Get().Request =
                            lower ? Core::LandscapeStrokeRequest::Lower : Core::LandscapeStrokeRequest::Raise;
                       return PaletteCommandDone();
                   } } );
        }
    }
} // namespace Desert::Editor
