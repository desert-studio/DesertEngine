#include "FractureCommands.hpp"

#include "FractureTool.hpp"

#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        // The level the palette's level entries act on: the last one (the one Add Level just made).
        Destruction::FractureLevelSettings& LastLevel()
        {
            auto& levels = FractureTool::Get().Settings.Levels;
            if ( levels.empty() )
                levels.emplace_back();
            return levels.back();
        }

        // A cut plane through the piece's origin, perpendicular to one axis (0 = X, 1 = Y, 2 = Z).
        Destruction::CutPlane AxisPlane( int axis )
        {
            Destruction::CutPlane plane;
            plane.Normal       = glm::dvec3( 0.0 );
            plane.Normal[axis] = 1.0;
            return plane;
        }

        // The palette's "Target:" entries. Named and bound rather than a lambda: `bugprone-exception-escape`
        // fires on a parameter-less lambda that copies a string into its closure (LandscapeCommands.cpp).
        Common::BoolResultStr TargetFracture( const std::string& rel )
        {
            FractureTool& tool = FractureTool::Get();
            tool.Path          = rel;
            tool.Load();
            return PaletteCommandDone();
        }
    } // namespace

    Common::BoolResultStr GenerateFromSelection( FractureTool&                                 tool,
                                                 const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !scene || !selected.has_value() )
            return PaletteCommandOutcome( false, "Generate: select the static mesh entity to fracture." );
        auto ref = scene->FindEntityByID( *selected );
        if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
            return PaletteCommandOutcome( false, "Generate: the selected entity has no static mesh." );
        const auto& component = ref->get().GetComponent<ECS::StaticMeshComponent>();
        // The fracture names the ASSET it cut (FractureData::SourceMesh): an entity whose geometry is an
        // unaccepted modeling edit is not that asset, so its edit is accepted into the asset first.
        if ( component.EditableMesh )
            return PaletteCommandOutcome( false,
                                          "Generate: the selected mesh has a modeling edit; Accept it into its "
                                          "asset first, so the fracture names the mesh it cut." );
        const auto guid = Assets::ContentRegistry::GuidForHandle( component.MeshHandle );
        if ( !guid.has_value() || guid->IsNull() )
            return PaletteCommandOutcome( false,
                                          "Generate: the selected mesh is not a static mesh asset with a GUID." );
        const auto target = GetToolTargetMesh( component );
        if ( !target )
            return PaletteCommandOutcome( false,
                                          std::format( "Generate: no mesh to fracture: {}", target.GetError() ) );
        return tool.Generate( *target.GetValue().Mesh, *guid );
    }

    void AppendFractureCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        commands.push_back( { "Fracture", "Fracture mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Fracture );
                                  return PaletteCommandDone();
                              } } );

        // TARGET: one entry per .dfrac under Assets/Fractures (PaletteCommand::Run takes no argument, so the
        // path is in the label, as the Landscape heightmap entries do), and the panel's default new file.
        {
            const std::filesystem::path fractures = Common::Constants::Path::ASSETS_PATH / "Fractures";
            std::vector<std::string>    targets{ FractureTool().Path };
            std::error_code             ec;
            for ( const auto& entry : std::filesystem::directory_iterator( fractures, ec ) )
                if ( entry.is_regular_file() && entry.path().extension() == ".dfrac" )
                    targets.push_back( entry.path()
                                            .lexically_relative( Common::Constants::Path::ASSETS_PATH )
                                            .generic_string() );
            std::sort( targets.begin() + 1, targets.end() );
            targets.erase( std::unique( targets.begin(), targets.end() ), targets.end() );
            for ( const std::string& rel : targets )
                commands.push_back(
                     { "Fracture", std::format( "Target: {}", rel ), std::bind_front( &TargetFracture, rel ) } );
        }
        commands.push_back( { "Fracture", "Load", []
                              {
                                  FractureTool::Get().Load();
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "Fracture", "Generate from the selected static mesh",
                              [&scene] { return GenerateFromSelection( FractureTool::Get(), scene ); } } );

        // GENERATE SETTINGS (the panel's Generate section).
        commands.push_back( { "Fracture", "Random Seed: next", []
                              {
                                  ++FractureTool::Get().Settings.Seed;
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "Fracture", "Add Level", []
                              {
                                  auto& levels = FractureTool::Get().Settings.Levels;
                                  levels.push_back( levels.empty() ? Destruction::FractureLevelSettings{}
                                                                   : levels.back() );
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "Fracture", "Remove Level", []
                              {
                                  auto& levels = FractureTool::Get().Settings.Levels;
                                  if ( levels.size() <= 1 )
                                      return PaletteCommandOutcome( false,
                                                                    "Remove Level: one level is the minimum" );
                                  levels.pop_back();
                                  return PaletteCommandDone();
                              } } );
        for ( size_t m = 0; m < kFractureMethodNames.size(); ++m )
            commands.push_back( { "Fracture", std::format( "Last level method: {}", kFractureMethodNames[m] ), [m]
                                  {
                                      LastLevel().Method = static_cast<Destruction::FractureMethod>( m );
                                      return PaletteCommandDone();
                                  } } );
        for ( const bool more : { true, false } )
            commands.push_back(
                 { "Fracture",
                   more ? "Last level: double the sites / clusters" : "Last level: halve the sites / clusters",
                   [more]
                   {
                       auto& level     = LastLevel();
                       auto  step      = [more]( int n ) { return more ? n * 2 : std::max( 1, n / 2 ); };
                       level.SiteCount = step( level.SiteCount );
                       level.Clusters  = step( level.Clusters );
                       return PaletteCommandDone();
                   } } );
        for ( int axis = 0; axis < 3; ++axis )
            commands.push_back(
                 { "Fracture", std::format( "Last level: add a plane across {} through the origin", "XYZ"[axis] ),
                   [axis]
                   {
                       LastLevel().Planes.push_back( AxisPlane( axis ) );
                       return PaletteCommandDone();
                   } } );
        commands.push_back( { "Fracture", "Last level: remove the last plane", []
                              {
                                  auto& planes = LastLevel().Planes;
                                  if ( planes.empty() )
                                      return PaletteCommandOutcome( false,
                                                                    "remove plane: the level has no planes" );
                                  planes.pop_back();
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "Fracture", "Auto Cluster: toggle", []
                              {
                                  auto& enabled = FractureTool::Get().Settings.AutoCluster.Enabled;
                                  enabled       = !enabled;
                                  return PaletteCommandDone();
                              } } );

        // INTERIOR MATERIAL: the project's materials, as the panel's picker lists them.
        commands.push_back( { "Fracture", "Interior material: none",
                              [] { return FractureTool::Get().SetInteriorMaterial( {} ); } } );
        for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Material ) )
            if ( row.Guid.has_value() )
                commands.push_back(
                     { "Fracture", std::format( "Interior material: {} ({})", PickerDisplayName( row ), row.Key ),
                       [guid = *row.Guid] { return FractureTool::Get().SetInteriorMaterial( guid ); } } );

        // VIEW (preview only).
        for ( const float amount : { 0.0f, 0.5f, 1.0f } )
            commands.push_back( { "Fracture", std::format( "Explode Amount: {:.1f}", amount ), [amount]
                                  {
                                      FractureTool::Get().View.ExplodeAmount = amount;
                                      return PaletteCommandDone();
                                  } } );
        const int deepest = static_cast<int>( Destruction::DeepestLevel( FractureTool::Get().Fracture().Nodes ) );
        for ( int level = -1; level <= deepest; ++level )
            commands.push_back(
                 { "Fracture",
                   level < 0 ? std::string( "Fracture Level: all" ) : std::format( "Fracture Level: {}", level ),
                   [level]
                   {
                       FractureTool::Get().View.ViewLevel = level;
                       return PaletteCommandDone();
                   } } );
    }
} // namespace Desert::Editor
