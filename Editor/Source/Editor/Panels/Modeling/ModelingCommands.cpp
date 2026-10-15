#include "ModelingCommands.hpp"

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
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/MeshBooleanTool.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/MeshXformOperations.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/ModelingStateProperties.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>
#include <Editor/Panels/ViewportPanel/Tools/CubeGridTool.hpp>
#include <Editor/Panels/Modeling/ModelingPanel.hpp>
#include <array>
#include <initializer_list>
#include <utility>

namespace Desert::Editor
{
    void AppendMeshToCollisionCommands( std::vector<PaletteCommand>&                  commands,
                                        const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        // UE's Mesh To Collision (Modeling panel, Collision palette): the combo's choice and the button in one
        // entry per type, running the button's own path on the selection.
        for ( const auto& shape : Editor::ModelingPanel::kCollisionShapes )
            commands.push_back( { "Modeling", std::string( "Mesh To Collision " ) + shape.Name, [&scene, &shape]
                                  { return Editor::ModelingPanel::MeshToCollision( scene, shape ); } } );
    }

    void AppendSelectElementsCommand( std::vector<PaletteCommand>& commands )
    {
        // MESH ELEMENT SELECTION (Modeling Mode). Palette entries for the same reason as the modes above: the
        // selection a later Extrude / Delete works on must be reachable - and photographable - unattended.
        commands.push_back( { "Modeling", "Select Elements tool", []
                              {
                                  Core::ModelingState::Get().ActiveTool = Core::ModelingState::Tool::ElementSelect;
                                  Core::ViewportMode::Set( Core::EditorMode::Modeling );
                                  return PaletteCommandDone();
                              } } );
    }

    void AppendCreateShapeCommands( std::vector<PaletteCommand>& commands )
    {
        // CREATE SHAPE (Modeling Mode -> Create). One entry per shape, and the placement a click makes, at the
        // viewport centre: placing is the whole tool, and a capability the channel cannot reach does not exist
        // for an unattended check.
        for ( const Core::ModelingState::Shape shape : Core::ModelingState::kShapes )
        {
            commands.push_back( { "Modeling",
                                  std::string( "Create shape tool: " ) + Core::ModelingState::ShapeName( shape ),
                                  [shape]
                                  {
                                      auto& ms            = Core::ModelingState::Get();
                                      ms.ActiveTool       = Core::ModelingState::Tool::CreateShape;
                                      ms.CreateShape.Kind = shape;
                                      Core::ViewportMode::Set( Core::EditorMode::Modeling );
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Modeling", "Create shape: place at the viewport centre",
                              [] { return Editor::ViewportPanel::PlaceShapeInActiveViewport(); } } );
    }

    void AppendModelingCommands( std::vector<PaletteCommand>&                  commands,
                                 const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        for ( const Geometry::ElementMode mode :
              { Geometry::ElementMode::Vertex, Geometry::ElementMode::Edge, Geometry::ElementMode::Triangle,
                Geometry::ElementMode::PolyGroup } )
        {
            commands.push_back( { "Modeling", std::string( "Mesh selection mode: " ) + Geometry::ToString( mode ),
                                  [mode] { return Core::MeshElementSelection::Get().SetMode( mode ); } } );
        }
        // UE's PolyEdit / TriEdit: which topology Vertex and Edge pick (group corners and borders, or every mesh
        // vertex and edge). The panel's two level buttons; without this a TriEdit frame cannot be taken.
        for ( const auto& [label, level] :
              { std::pair{ "Mesh selection level: PolyEdit", Geometry::TopologyLevel::Group },
                std::pair{ "Mesh selection level: TriEdit", Geometry::TopologyLevel::Triangle } } )
        {
            commands.push_back( { "Modeling", label, [level]
                                  {
                                      Core::MeshElementSelection::Get().SetLevel( level );
                                      return PaletteCommandDone();
                                  } } );
        }
        using SelectionOp = Core::MeshElementSelection::Op;
        for ( const SelectionOp op : { SelectionOp::SelectAll, SelectionOp::SelectConnected, SelectionOp::Grow,
                                       SelectionOp::Shrink, SelectionOp::Invert, SelectionOp::Clear } )
        {
            commands.push_back( { "Modeling",
                                  std::string( "Mesh selection: " ) + Core::MeshElementSelection::ToString( op ),
                                  [op] { return Core::MeshElementSelection::Get().Apply( op ); } } );
        }
        commands.push_back(
             { "Modeling", "Mesh selection: pick at the viewport centre", [&scene]
               {
                   if ( Core::ModelingState::Get().ActiveTool != Core::ModelingState::Tool::ElementSelect )
                       return PaletteCommandOutcome( false, "the Select Elements tool is not active" );
                   // The tool's own target rule, not "the tracker has a mesh": the tracker only learns the mesh
                   // on the tool's next frame, and a static mesh with no EditableMesh is a target too (P9c).
                   const auto selected = Core::SelectionManager::GetSelected();
                   if ( !scene || !selected.has_value() )
                       return PaletteCommandOutcome( false, "no entity is selected" );
                   auto ref = scene->FindEntityByID( *selected );
                   if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
                       return PaletteCommandOutcome( false, "the selected entity has no static mesh" );
                   auto target = Editor::GetToolTargetMesh( ref->get().GetComponent<ECS::StaticMeshComponent>() );
                   if ( !target.IsSuccess() )
                       return PaletteCommandOutcome( false, target.GetError() );
                   Core::MeshElementSelection::Get().ReqPickCentre = true;
                   return PaletteCommandDone();
               } } );
        // UE's Accept on a static mesh (P9b): the selected entity's edit goes INTO its asset - an imported .fbx
        // mesh included - under the same GUID, so every scene and foliage type naming the mesh draws the edit.
        commands.push_back(
             { "Modeling", "Accept: write the edit into the static mesh asset", [&scene]
               {
                   const auto selected = Core::SelectionManager::GetSelected();
                   if ( !scene || !selected.has_value() )
                       return PaletteCommandOutcome( false, "no entity is selected" );
                   auto ref = scene->FindEntityByID( *selected );
                   if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
                       return PaletteCommandOutcome( false, "the selected entity has no static mesh" );
                   auto written =
                        Editor::CommitEditableMeshToAsset( ref->get().GetComponent<ECS::StaticMeshComponent>() );
                   if ( !written.IsSuccess() )
                       return PaletteCommandOutcome( false, written.GetError() );
                   LOG_INFO( "[Modeling] the edit was written into '{}'", written.GetValue().generic_string() );
                   return PaletteCommandDone();
               } } );
        // The operations on that selection, at the panel's values (ModelingState). Cut is not here: it needs
        // a line drawn in the viewport (the knife, Alt+K).
        for ( const Core::MeshOperation op :
              { Core::MeshOperation::Delete, Core::MeshOperation::Extrude, Core::MeshOperation::PushPull,
                Core::MeshOperation::Offset, Core::MeshOperation::Inset, Core::MeshOperation::Outset,
                Core::MeshOperation::Bevel, Core::MeshOperation::InsertEdgeLoop, Core::MeshOperation::Clean,
                Core::MeshOperation::Subdivide, Core::MeshOperation::Mirror, Core::MeshOperation::PlaneCut,
                Core::MeshOperation::FillHole, Core::MeshOperation::WeldEdges, Core::MeshOperation::Simplify } )
        {
            commands.push_back( { "Modeling", std::string( "Mesh operation: " ) + Core::ToString( op ),
                                  [&scene, op]
                                  {
                                      if ( !scene )
                                          return PaletteCommandOutcome( false, "no scene is open" );
                                      return Core::ApplyMeshOperation( *scene, op, Core::ArgsFromModelingState() );
                                  } } );
        }
        // XForm (UE's XForm tab) on the scene selection's entities, at the panel's values.
        for ( const Core::XformOperation op :
              { Core::XformOperation::EditPivot, Core::XformOperation::BakeTransform, Core::XformOperation::Merge,
                Core::XformOperation::Split, Core::XformOperation::Pattern } )
        {
            commands.push_back( { "Modeling", std::string( "XForm: " ) + Core::ToString( op ), [&scene, op]
                                  {
                                      if ( !scene )
                                          return PaletteCommandOutcome( false, "no scene is open" );
                                      return Core::ApplyXformOperation( *scene, op,
                                                                        Core::XformArgsFromModelingState() );
                                  } } );
        }
        // Boolean and Trim (UE's Boolean / Trim tools) on the scene selection's two entities, at the panel's
        // values.
        for ( const Core::BooleanTool tool : { Core::BooleanTool::Boolean, Core::BooleanTool::Trim } )
        {
            commands.push_back(
                 { "Modeling", std::string( "Boolean tool: " ) + Core::ToString( tool ), [&scene, tool]
                   {
                       if ( !scene )
                           return PaletteCommandOutcome( false, "no scene is open" );
                       return Core::ApplyBooleanTool( *scene, tool, Core::BooleanArgsFromModelingState() );
                   } } );
        }
        // THE REST OF THE MODELING PANEL (M30): every button, checkbox and closed choice ModelingPanel draws,
        // so a tool is usable - and photographable - with no mouse. Each entry writes what the widget writes
        // or calls what the button calls; the dragged values are the channel's `modeling` subject
        // (ModelingStateProperties.hpp). Suite ModelingPaletteCensus holds this list to the panel's widgets.
        using MS           = Core::ModelingState;
        auto modelingOnOff = [&commands]( const char* group, const std::string& name, auto write )
        {
            for ( const bool on : { true, false } )
                commands.push_back( { group, name + ( on ? ": on" : ": off" ), [write, on]
                                      {
                                          write( MS::Get(), on );
                                          return PaletteCommandDone();
                                      } } );
        };
        auto modelingTool = [&commands]( const char* group, const char* label, MS::Tool tool )
        {
            commands.push_back( { group, label, [tool]
                                  {
                                      MS::Get().ActiveTool = tool;
                                      Core::ViewportMode::Set( Core::EditorMode::Modeling );
                                      return PaletteCommandDone();
                                  } } );
        };
        auto needCubeGrid = []
        {
            return MS::Get().ActiveTool == MS::Tool::CubeGrid
                        ? PaletteCommandDone()
                        : PaletteCommandOutcome( false, "the CubeGrid tool is not active ('CubeGrid tool')" );
        };
        modelingTool( "Modeling", "PolyEdit tool", MS::Tool::PolyEdit );
        modelingTool( "CubeGrid", "CubeGrid tool", MS::Tool::CubeGrid );
        // Reopen CubeGrid on the selected blockout (UE: the tool takes the selected mesh as its target); the
        // tool refuses, with a toast naming why, anything that does not carry its voxels.
        commands.push_back( { "CubeGrid", "Edit selected blockout", []
                              {
                                  MS::Get().ActiveTool              = MS::Tool::CubeGrid;
                                  MS::Get().ReqCubeGridEditSelected = true;
                                  Core::ViewportMode::Set( Core::EditorMode::Modeling );
                                  return PaletteCommandDone();
                              } } );

        // CubeGrid's panel buttons are the tool's own one-shot requests; Cancel also ends the tool, as the
        // viewport tool bar's Cancel does (Tools::RaiseToolRequest).
        for ( const auto& [label, request] : std::initializer_list<std::pair<const char*, bool MS::*>>{
                   { "Accept and Start New", &MS::ReqAccept },
                   { "Cancel", &MS::ReqCancel },
                   { "Reset Grid from Actor", &MS::ReqResetFromActor },
                   { "Corner Mode (Z)", &MS::ReqCornerMode },
                   { "Clear", &MS::ReqClear } } )
        {
            commands.push_back( { "CubeGrid", label, [request, needCubeGrid]
                                  {
                                      if ( auto active = needCubeGrid(); !active )
                                          return active;
                                      Tools::RaiseToolRequest( MS::Get(), request );
                                      return PaletteCommandDone();
                                  } } );
        }
        for ( int power = 0; power <= MS::MaxGridPower; ++power )
            commands.push_back( { "CubeGrid", std::format( "Grid Power {}", power ), [power]
                                  {
                                      MS::Get().SetGridPower( power );
                                      return PaletteCommandDone();
                                  } } );
        commands.push_back( { "CubeGrid", "Block Size /2", []
                              {
                                  MS::Get().HalveBlockSize();
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "CubeGrid", "Block Size x2", []
                              {
                                  MS::Get().DoubleBlockSize();
                                  return PaletteCommandDone();
                              } } );
        for ( const int div : { 2, 4, 10 } )
            commands.push_back( { "CubeGrid", std::format( "Snap Size 1/{} block", div ), [div]
                                  {
                                      MS::Get().CornerSnapDiv = div;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "CubeGrid", "Show Gizmo", []( MS& ms, bool on ) { ms.ShowGizmo = on; } );
        modelingOnOff( "CubeGrid", "Crosswise Diagonal", []( MS& ms, bool on ) { ms.CornerCrosswise = on; } );
        modelingOnOff( "CubeGrid", "Hit Unrelated Geometry", []( MS& ms, bool on ) { ms.HitUnrelated = on; } );
        modelingOnOff( "CubeGrid", "Generate Collision", []( MS& ms, bool on ) { ms.GenerateCollision = on; } );
        // The mouse's part: aim from the viewport centre, a marquee of N x N blocks there, E / Q, and the
        // corner posts Corner Mode picks (bit k = VoxelBlockout's kPosts[k]: (u-,v-) (u+,v-) (u-,v+) (u+,v+)).
        modelingOnOff( "CubeGrid", "Aim at the viewport centre",
                       []( MS& ms, bool on ) { ms.CubeGridAimCentre = on; } );
        for ( const int blocks : { 1, 2, 3, 4 } )
            commands.push_back( { "CubeGrid", std::format( "Select {0}x{0} blocks at the aim", blocks ),
                                  [blocks, needCubeGrid]
                                  {
                                      if ( auto active = needCubeGrid(); !active )
                                          return active;
                                      MS::Get().ReqCubeGridSelectBlocks = blocks;
                                      return PaletteCommandDone();
                                  } } );
        for ( const auto& [label, dir] : std::initializer_list<std::pair<const char*, int>>{
                   { "Push/Pull out (E)", +1 }, { "Push/Pull in (Q)", -1 } } )
            commands.push_back( { "CubeGrid", label, [dir, needCubeGrid]
                                  {
                                      if ( auto active = needCubeGrid(); !active )
                                          return active;
                                      MS::Get().ReqCubeGridStep = dir;
                                      return PaletteCommandDone();
                                  } } );
        for ( const auto& [label, dir] : std::initializer_list<std::pair<const char*, int>>{
                   { "Slide selection back (Shift+E)", +1 }, { "Slide selection forward (Shift+Q)", -1 } } )
            commands.push_back( { "CubeGrid", label, [dir, needCubeGrid]
                                  {
                                      if ( auto active = needCubeGrid(); !active )
                                          return active;
                                      MS::Get().ReqCubeGridSlide = dir;
                                      return PaletteCommandDone();
                                  } } );
        commands.push_back( { "CubeGrid", "Grid pivot onto the aimed face corner (Ctrl+MMB)", [needCubeGrid]
                              {
                                  if ( auto active = needCubeGrid(); !active )
                                      return active;
                                  MS::Get().ReqCubeGridPivot = true;
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "CubeGrid", "Paint the Quick Material onto the selection (Shift+B)", [needCubeGrid]
                              {
                                  if ( auto active = needCubeGrid(); !active )
                                      return active;
                                  MS::Get().ReqCubeGridPaint = true;
                                  return PaletteCommandDone();
                              } } );
        // Quick Material without the panel's list: the engine default, or the project's materials in the
        // registry's order, one step per call.
        commands.push_back( { "CubeGrid", "Quick Material: engine default", []
                              {
                                  MS::Get().QuickMaterial = Common::AssetHandle{};
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "CubeGrid", "Quick Material: next project material", []
                              {
                                  const auto rows =
                                       Assets::ContentRegistry::Rows( Common::Content::ContentKind::Material );
                                  if ( rows.empty() )
                                      return PaletteCommandOutcome( false, "the project has no material assets" );
                                  auto&  current = MS::Get().QuickMaterial;
                                  size_t next    = 0;
                                  for ( size_t i = 0; i < rows.size(); ++i )
                                      if ( rows[i].Handle == current )
                                          next = ( i + 1 ) % rows.size();
                                  current = rows[next].Handle;
                                  LOG_INFO( "[CubeGrid] Quick Material: {}", rows[next].Key );
                                  return PaletteCommandDone();
                              } } );
        for ( const auto& [label, posts] :
              std::initializer_list<std::pair<const char*, int>>{ { "Corner posts: all", 0b1111 },
                                                                  { "Corner posts: none", 0b0000 },
                                                                  { "Corner posts: U+ edge", 0b1010 },
                                                                  { "Corner posts: U- edge", 0b0101 },
                                                                  { "Corner posts: V+ edge", 0b1100 },
                                                                  { "Corner posts: V- edge", 0b0011 },
                                                                  { "Corner post: U- V-", 0b0001 },
                                                                  { "Corner post: U+ V-", 0b0010 },
                                                                  { "Corner post: U- V+", 0b0100 },
                                                                  { "Corner post: U+ V+", 0b1000 } } )
            commands.push_back( { "CubeGrid", label, [posts]
                                  {
                                      if ( !MS::Get().CornerMode )
                                          return PaletteCommandOutcome(
                                               false,
                                               "corner posts are picked in Corner Mode ('Corner Mode (Z)')" );
                                      MS::Get().ReqCornerPosts = posts;
                                      return PaletteCommandDone();
                                  } } );

        // Create Shape's closed choices.
        for ( const Geometry::ShapePolygroupMode mode :
              { Geometry::ShapePolygroupMode::PerShape, Geometry::ShapePolygroupMode::PerFace,
                Geometry::ShapePolygroupMode::PerQuad } )
            commands.push_back( { "Modeling",
                                  std::string( "Create shape polygroups: " ) + Geometry::ToString( mode ), [mode]
                                  {
                                      MS::Get().CreateShape.Groups = mode;
                                      return PaletteCommandDone();
                                  } } );
        for ( const Geometry::StairsType type : { Geometry::StairsType::Linear, Geometry::StairsType::Floating,
                                                  Geometry::StairsType::Curved, Geometry::StairsType::Spiral } )
            commands.push_back( { "Modeling", std::string( "Create shape stairs: " ) + Geometry::ToString( type ),
                                  [type]
                                  {
                                      MS::Get().CreateShape.Stairs.Type = type;
                                      return PaletteCommandDone();
                                  } } );
        for ( const Geometry::SphereType type : { Geometry::SphereType::LatLong, Geometry::SphereType::Box } )
            commands.push_back( { "Modeling", std::string( "Create shape sphere: " ) + Geometry::ToString( type ),
                                  [type]
                                  {
                                      MS::Get().CreateShape.Sphere.SubdivisionType = type;
                                      return PaletteCommandDone();
                                  } } );
        for ( const Geometry::DiscType type : { Geometry::DiscType::Disc, Geometry::DiscType::PuncturedDisc } )
            commands.push_back( { "Modeling", std::string( "Create shape disc: " ) + Geometry::ToString( type ),
                                  [type]
                                  {
                                      MS::Get().CreateShape.Disc.Type = type;
                                      return PaletteCommandDone();
                                  } } );
        for ( const Geometry::RectangleType type :
              { Geometry::RectangleType::Rectangle, Geometry::RectangleType::RoundedRectangle } )
            commands.push_back( { "Modeling",
                                  std::string( "Create shape rectangle: " ) + Geometry::ToString( type ), [type]
                                  {
                                      MS::Get().CreateShape.Rectangle.Type = type;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "Modeling", "Create shape: Maintain Dimension",
                       []( MS& ms, bool on ) { ms.CreateShape.Rectangle.MaintainDimension = on; } );
        for ( const Geometry::ShapePivot pivot :
              { Geometry::ShapePivot::Base, Geometry::ShapePivot::Centre, Geometry::ShapePivot::Top } )
            commands.push_back( { "Modeling", std::string( "Create shape pivot: " ) + Geometry::ToString( pivot ),
                                  [pivot]
                                  {
                                      MS::Get().CreateShape.Pivot = pivot;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "Modeling", "Create shape: Place on Scene", []( MS& ms, bool on )
                       { ms.CreateShape.Place = on ? MS::Placement::OnScene : MS::Placement::Ground; } );
        modelingOnOff( "Modeling", "Create shape: Align to Normal",
                       []( MS& ms, bool on ) { ms.CreateShape.AlignToNormal = on; } );
        for ( const auto& [label, type] : std::initializer_list<std::pair<const char*, MS::OutputType>>{
                   { "Output type: Static Mesh", MS::OutputType::StaticMesh },
                   { "Output type: Dynamic Mesh", MS::OutputType::Dynamic } } )
            commands.push_back( { "Modeling", label, [type]
                                  {
                                      MS::Get().Output.Type = type;
                                      return PaletteCommandDone();
                                  } } );

        // Select Elements' options for Subdivide, Mirror and Plane Cut.
        for ( const auto scheme :
              { Geometry::SubdivisionScheme::Bilinear, Geometry::SubdivisionScheme::CatmullClark,
                Geometry::SubdivisionScheme::Loop } )
            commands.push_back( { "Modeling", std::string( "Subdivide scheme: " ) + Geometry::ToString( scheme ),
                                  [scheme]
                                  {
                                      MS::Get().ElementSubdivide.Scheme = scheme;
                                      return PaletteCommandDone();
                                  } } );
        for ( const auto boundary : { Geometry::SubdivisionBoundaryScheme::SmoothCorners,
                                      Geometry::SubdivisionBoundaryScheme::SharpCorners } )
            commands.push_back(
                 { "Modeling", std::string( "Subdivide boundary: " ) + Geometry::ToString( boundary ), [boundary]
                   {
                       MS::Get().ElementSubdivide.Boundary = boundary;
                       return PaletteCommandDone();
                   } } );
        for ( const auto normals :
              { Geometry::SubdivisionOutputNormals::Interpolated, Geometry::SubdivisionOutputNormals::Generated } )
            commands.push_back( { "Modeling", std::string( "Subdivide normals: " ) + Geometry::ToString( normals ),
                                  [normals]
                                  {
                                      MS::Get().ElementSubdivide.Normals = normals;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "Modeling", "Subdivide: New PolyGroups",
                       []( MS& ms, bool on ) { ms.ElementSubdivide.NewPolyGroups = on; } );
        modelingOnOff( "Modeling", "Simplify: Preserve Sharp Edges",
                       []( MS& ms, bool on ) { ms.ElementSimplify.PreserveSharpEdges = on; } );
        for ( const auto target : { Geometry::SimplifyTarget::Percentage, Geometry::SimplifyTarget::VertexCount } )
            commands.push_back( { "Modeling", std::string( "Simplify target: " ) + Geometry::ToString( target ),
                                  [target]
                                  {
                                      MS::Get().ElementSimplify.Target = target;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "Modeling", "Simplify: Preserve PolyGroups",
                       []( MS& ms, bool on ) { ms.ElementSimplify.PreserveGroupBoundaries = on; } );
        static constexpr std::array<const char*, 3> kAxisNames = { "X", "Y", "Z" };
        for ( int axis = 0; axis < 3; ++axis )
        {
            commands.push_back( { "Modeling", std::string( "Mirror axis: " ) + kAxisNames[axis], [axis]
                                  {
                                      MS::Get().ElementMirrorAxis = axis;
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Modeling", std::string( "Plane Cut axis: " ) + kAxisNames[axis], [axis]
                                  {
                                      MS::Get().ElementPlaneCutAxis = axis;
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Modeling", std::string( "Pattern axis: " ) + kAxisNames[axis], [axis]
                                  {
                                      MS::Get().XformPattern.AxisA = axis;
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Modeling", std::string( "Pattern axis B: " ) + kAxisNames[axis], [axis]
                                  {
                                      MS::Get().XformPattern.AxisB = axis;
                                      return PaletteCommandDone();
                                  } } );
        }
        modelingOnOff( "Modeling", "Mirror: World", []( MS& ms, bool on ) { ms.ElementMirrorWorld = on; } );
        modelingOnOff( "Modeling", "Mirror: Keep -",
                       []( MS& ms, bool on ) { ms.ElementMirrorKeepNegative = on; } );
        modelingOnOff( "Modeling", "Mirror: Cut the far half first",
                       []( MS& ms, bool on ) {
                           ms.ElementMirrorMode =
                                on ? Geometry::MirrorMode::CutAndMirror : Geometry::MirrorMode::AddMirroredCopy;
                       } );
        modelingOnOff( "Modeling", "Plane Cut: World", []( MS& ms, bool on ) { ms.ElementPlaneCutWorld = on; } );
        modelingOnOff( "Modeling", "Plane Cut: Keep -",
                       []( MS& ms, bool on ) { ms.ElementPlaneCutKeepNegative = on; } );
        modelingOnOff( "Modeling", "Plane Cut: Fill", []( MS& ms, bool on ) { ms.ElementPlaneCutFill = on; } );
        modelingOnOff( "Modeling", "Plane Cut: Keep both halves",
                       []( MS& ms, bool on )
                       {
                           ms.ElementPlaneCutMode = on ? Geometry::PlaneCutMode::KeepBothHalves
                                                       : Geometry::PlaneCutMode::DiscardNegativeSide;
                       } );
        // Boolean's and Trim's closed choices.
        auto choice = [&commands]( const std::string& label, auto write )
        {
            commands.push_back( { "Modeling", label, [write]
                                  {
                                      write( MS::Get().Boolean );
                                      return PaletteCommandDone();
                                  } } );
        };
        for ( const Core::CsgOperation op : { Core::CsgOperation::DifferenceAB, Core::CsgOperation::DifferenceBA,
                                              Core::CsgOperation::Intersect, Core::CsgOperation::Union } )
            choice( std::string( "Boolean operation: " ) + Core::ToString( op ),
                    [op]( Core::BooleanToolArgs& b ) { b.Operation = op; } );
        for ( const Core::TrimTarget which : { Core::TrimTarget::TrimA, Core::TrimTarget::TrimB } )
            choice( std::string( "Trim: " ) + Core::ToString( which ),
                    [which]( Core::BooleanToolArgs& b ) { b.Trimmed = which; } );
        for ( const Core::TrimSide side : { Core::TrimSide::RemoveInside, Core::TrimSide::RemoveOutside } )
            choice( std::string( "Trim: " ) + Core::ToString( side ),
                    [side]( Core::BooleanToolArgs& b ) { b.Side = side; } );
        for ( const Core::BooleanWriteTo to : { Core::BooleanWriteTo::NewObject, Core::BooleanWriteTo::Input } )
            choice( std::string( "Boolean write to: " ) + Core::ToString( to ),
                    [to]( Core::BooleanToolArgs& b ) { b.WriteTo = to; } );
        for ( const Core::BooleanInputs in :
              { Core::BooleanInputs::Delete, Core::BooleanInputs::Hide, Core::BooleanInputs::Keep } )
            choice( std::string( "Boolean inputs: " ) + Core::ToString( in ),
                    [in]( Core::BooleanToolArgs& b ) { b.Inputs = in; } );

        // XForm's closed choices.
        for ( const auto& [label, pivot] : std::initializer_list<std::pair<const char*, Geometry::PivotLocation>>{
                   { "XForm pivot: Bounds Center", Geometry::PivotLocation::BoundsCenter },
                   { "XForm pivot: Bounds Base", Geometry::PivotLocation::BoundsBase },
                   { "XForm pivot: World Origin", Geometry::PivotLocation::WorldOrigin },
                   { "XForm pivot: World Point", Geometry::PivotLocation::WorldPoint } } )
            commands.push_back( { "Modeling", label, [pivot]
                                  {
                                      MS::Get().XformPivot = pivot;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "Modeling", "Bake: Rotation", []( MS& ms, bool on ) { ms.XformBake.Rotation = on; } );
        modelingOnOff( "Modeling", "Bake: Scale", []( MS& ms, bool on ) { ms.XformBake.Scale = on; } );
        modelingOnOff( "Modeling", "Bake: Location", []( MS& ms, bool on ) { ms.XformBake.Translation = on; } );
        modelingOnOff( "Modeling", "Split by polygroups",
                       []( MS& ms, bool on ) {
                           ms.XformSplit = on ? Geometry::SplitMethod::PolyGroups
                                              : Geometry::SplitMethod::ConnectedComponents;
                       } );
        for ( const auto& [label, shape] : std::initializer_list<std::pair<const char*, Geometry::PatternShape>>{
                   { "Pattern shape: Line", Geometry::PatternShape::Line },
                   { "Pattern shape: Grid", Geometry::PatternShape::Grid },
                   { "Pattern shape: Circle", Geometry::PatternShape::Circle } } )
            commands.push_back( { "Modeling", label, [shape]
                                  {
                                      MS::Get().XformPattern.Shape = shape;
                                      return PaletteCommandDone();
                                  } } );
        modelingOnOff( "Modeling", "Pattern: Orient",
                       []( MS& ms, bool on ) { ms.XformPattern.OrientToCircle = on; } );
        modelingOnOff( "Modeling", "Pattern: Separate entities",
                       []( MS& ms, bool on ) { ms.XformPatternSeparate = on; } );
    }
} // namespace Desert::Editor
