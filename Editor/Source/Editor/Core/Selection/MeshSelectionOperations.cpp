#include "MeshSelectionOperations.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/EditMeshOperations.hpp>

#include <Common/Core/Logger.hpp>

#include <fmt/format.h>

#include <glm/matrix.hpp>

#include <cmath>
#include <memory>

namespace Desert::Editor::Core
{
    const char* ToString( MeshOperation operation )
    {
        switch ( operation )
        {
            case MeshOperation::Delete:
                return "Delete";
            case MeshOperation::Extrude:
                return "Extrude";
            case MeshOperation::PushPull:
                return "Push/Pull";
            case MeshOperation::Offset:
                return "Offset";
            case MeshOperation::Inset:
                return "Inset";
            case MeshOperation::Outset:
                return "Outset";
            case MeshOperation::Bevel:
                return "Bevel";
            case MeshOperation::InsertEdgeLoop:
                return "Insert Edge Loop";
            case MeshOperation::Cut:
                return "Cut";
            case MeshOperation::Clean:
                return "Clean";
            case MeshOperation::Subdivide:
                return "Subdivide";
            case MeshOperation::Mirror:
                return "Mirror";
            case MeshOperation::PlaneCut:
                return "Plane Cut";
            case MeshOperation::Trim:
                return "Trim";
        }
        return "Unknown";
    }

    namespace
    {
        // A plane perpendicular to an axis, `offset` cm along it from the origin, in the MESH's space (Mirror's
        // and Plane Cut's). A plane along the entity's own axis is that axis's coordinate plane whatever the
        // transform. A world plane is mapped through the inverse transform; the mesh-space plane is the world
        // one, cut or reflection, only when the transform is a similarity (rotation and uniform scale), so a
        // sheared or non-uniformly scaled AND rotated entity is refused rather than cut across a plane the
        // user did not pick.
        Common::ResultStr<Geometry::CutPlane> AxisPlane( ECS::Entity entity, int axisIndex, bool inWorld,
                                                         bool keepNegative, float offset, const char* what )
        {
            if ( axisIndex < 0 || axisIndex > 2 )
                return Common::MakeFormattedError<Geometry::CutPlane>(
                     "{}: the axis must be 0 (X), 1 (Y) or 2 (Z), not {}", what, axisIndex );
            glm::vec3 axis( 0.0f );
            axis[axisIndex] = keepNegative ? -1.0f : 1.0f;
            glm::vec3 origin( 0.0f );
            origin[axisIndex] = offset;
            if ( !inWorld )
                return Common::MakeSuccess( Geometry::CutPlane{ origin, axis } );
            // The WORLD transform, parent chain included: a child's local one would put the world plane at
            // its parent-relative image.
            const glm::mat4 world = entity.GetWorldTransform();
            const glm::mat3 linear( world );
            const glm::mat3 gram = glm::transpose( linear ) * linear;
            const float     s    = ( gram[0][0] + gram[1][1] + gram[2][2] ) / 3.0f;
            for ( int i = 0; i < 3; ++i )
                for ( int j = 0; j < 3; ++j )
                    if ( std::abs( gram[i][j] - ( i == j ? s : 0.0f ) ) > 1e-4f * s )
                        return Common::MakeFormattedError<Geometry::CutPlane>(
                             "{}: the entity's transform is not a rotation with a uniform scale (column "
                             "lengths {:.4f}, {:.4f}, {:.4f}) - a world plane has no image in its mesh; "
                             "work in local space or reset the scale",
                             what, std::sqrt( gram[0][0] ), std::sqrt( gram[1][1] ), std::sqrt( gram[2][2] ) );
            if ( !( s > 0.0f ) )
                return Common::MakeFormattedError<Geometry::CutPlane>( "{}: the entity's transform has zero scale",
                                                                       what );
            const glm::mat4 toMesh = glm::inverse( world );
            // Normals map by the inverse transpose; for a similarity that is the inverse's rotation part.
            const glm::vec3 point  = glm::vec3( toMesh * glm::vec4( origin, 1.0f ) );
            const glm::vec3 normal = glm::transpose( linear ) * axis;
            return Common::MakeSuccess( Geometry::CutPlane{ point, normal } );
        }

        // A mesh-wide operation takes no selection and leaves none (every ID may change), in the mode the
        // user is selecting in.
        Common::ResultStr<Geometry::MeshEditOutcome>
        WholeMesh( Common::ResultStr<Geometry::MeshEditOutcome> result, Geometry::ElementMode mode )
        {
            if ( !result.IsSuccess() )
                return result;
            Geometry::MeshEditOutcome outcome = result.ExtractValue();
            outcome.Selection                 = Geometry::ElementSelection( mode );
            return Common::MakeSuccess( std::move( outcome ) );
        }
    } // namespace

    bool TakesDistance( MeshOperation operation )
    {
        switch ( operation )
        {
            case MeshOperation::Delete:
            case MeshOperation::InsertEdgeLoop:
            case MeshOperation::Cut:
            case MeshOperation::Clean:
            case MeshOperation::Subdivide:
            case MeshOperation::Mirror:
            case MeshOperation::PlaneCut:
            case MeshOperation::Trim:
                return false;
            case MeshOperation::Extrude:
            case MeshOperation::PushPull:
            case MeshOperation::Offset:
            case MeshOperation::Inset:
            case MeshOperation::Outset:
            case MeshOperation::Bevel:
                return true;
        }
        return false;
    }

    MeshOperationArgs ArgsFromModelingState()
    {
        const auto&       ms = ModelingState::Get();
        MeshOperationArgs args;
        args.Distance      = ms.ElementOpDistance;
        args.LoopPosition  = ms.ElementLoopPosition;
        args.WeldTolerance = ms.ElementWeldTolerance;
        args.SubdivideLevels    = ms.ElementSubdivideLevels;
        args.SubdivideScheme    = ms.ElementSubdivideScheme;
        args.MirrorAxis         = ms.ElementMirrorAxis;
        args.MirrorWorld        = ms.ElementMirrorWorld;
        args.MirrorKeepNegative = ms.ElementMirrorKeepNegative;
        args.MirrorMode         = ms.ElementMirrorMode;
        args.PlaneCutAxis         = ms.ElementPlaneCutAxis;
        args.PlaneCutOffset       = ms.ElementPlaneCutOffset;
        args.PlaneCutWorld        = ms.ElementPlaneCutWorld;
        args.PlaneCutKeepNegative = ms.ElementPlaneCutKeepNegative;
        args.PlaneCutFill         = ms.ElementPlaneCutFill;
        args.PlaneCutMode         = ms.ElementPlaneCutMode;
        args.TrimCutter           = ms.ElementTrimCutter;
        args.TrimSide             = ms.ElementTrimSide;
        return args;
    }

    Common::BoolResultStr ApplyMeshOperation( ::Desert::Core::Scene& scene, MeshOperation operation,
                                              const MeshOperationArgs& args )
    {
        const float        distance = args.Distance;
        auto&              state  = MeshElementSelection::Get();
        const Common::UUID entity = state.Entity();
        if ( !state.HasMesh() )
            return Common::MakeFormattedError<bool>(
                 "Mesh {}: no editable mesh is selected - select an entity with one in Select Elements first",
                 ToString( operation ) );
        auto ref = scene.FindEntityByID( entity );
        if ( !ref )
            return Common::MakeFormattedError<bool>( "Mesh {}: entity {} is not in the scene",
                                                     ToString( operation ), static_cast<uint64_t>( entity ) );
        ECS::Entity e = ref->get();
        if ( !e.HasComponent<ECS::StaticMeshComponent>() ||
             !e.GetComponent<ECS::StaticMeshComponent>().EditableMesh )
            return Common::MakeFormattedError<bool>( "Mesh {}: entity {} has no editable mesh",
                                                     ToString( operation ), static_cast<uint64_t>( entity ) );
        auto& smc = e.GetComponent<ECS::StaticMeshComponent>();
        // The component may have moved on since the tool last looked (an undo this frame): the selection is
        // pruned against the mesh the operation will actually run on.
        state.Track( entity, smc.EditableMesh );
        const std::shared_ptr<const Geometry::EditMesh> before    = smc.EditableMesh;
        const Geometry::ElementSelection                selection = state.Selection();

        Common::ResultStr<Geometry::MeshEditOutcome> result;
        std::shared_ptr<const Geometry::EditMesh>    otherHalf; // Plane Cut, Keep Both Halves
        switch ( operation )
        {
            case MeshOperation::Delete:
                result = Geometry::DeleteSelection( *before, selection );
                break;
            case MeshOperation::Extrude:
                result = Geometry::ExtrudeSelection( *before, selection, distance,
                                                     Geometry::ExtrudeDirection::VertexNormals );
                break;
            case MeshOperation::PushPull:
                result = Geometry::PushPullSelection( *before, selection, distance );
                break;
            case MeshOperation::Offset:
                result = Geometry::OffsetSelection( *before, selection, distance );
                break;
            case MeshOperation::Inset:
                result = Geometry::InsetSelection( *before, selection, distance );
                break;
            case MeshOperation::Outset:
                result = Geometry::OutsetSelection( *before, selection, distance );
                break;
            case MeshOperation::Bevel:
                result = Geometry::BevelSelection( *before, selection, distance );
                break;
            case MeshOperation::InsertEdgeLoop:
                result = Geometry::InsertEdgeLoop( *before, selection, args.LoopPosition );
                break;
            case MeshOperation::Cut:
                if ( !args.CutPlane )
                    return Common::MakeError<bool>( "Mesh Cut: no cut line - draw it in the viewport with the "
                                                    "knife (Alt+K, then two clicks)" );
                result = Geometry::CutSelection( *before, selection, *args.CutPlane );
                break;
            case MeshOperation::Clean:
            {
                // Clean is mesh-wide: it takes no selection and leaves none (every ID may change).
                auto cleaned = Geometry::CleanMesh( *before, args.WeldTolerance );
                if ( cleaned.IsSuccess() )
                {
                    Geometry::CleanOutcome clean = cleaned.ExtractValue();
                    clean.Edit.Selection         = Geometry::ElementSelection( selection.Mode() );
                    result                       = Common::MakeSuccess( std::move( clean.Edit ) );
                }
                else
                    result = Common::MakeError<Geometry::MeshEditOutcome>( cleaned.GetError() );
                break;
            }
            case MeshOperation::Subdivide:
                result = WholeMesh( Geometry::SubdivideMesh( *before, args.SubdivideLevels, args.SubdivideScheme ),
                                    selection.Mode() );
                break;
            case MeshOperation::Mirror:
            {
                auto plane = AxisPlane( e, args.MirrorAxis, args.MirrorWorld, args.MirrorKeepNegative, 0.0f,
                                        "Mesh Mirror" );
                if ( !plane.IsSuccess() )
                    return Common::MakeError<bool>( plane.GetError() );
                result = WholeMesh(
                     Geometry::MirrorMesh( *before, plane.GetValue(), args.MirrorMode, args.WeldTolerance ),
                     selection.Mode() );
                break;
            }
            case MeshOperation::PlaneCut:
            {
                auto plane = AxisPlane( e, args.PlaneCutAxis, args.PlaneCutWorld, args.PlaneCutKeepNegative,
                                        args.PlaneCutOffset, "Mesh Plane Cut" );
                if ( !plane.IsSuccess() )
                    return Common::MakeError<bool>( plane.GetError() );
                auto cut =
                     Geometry::PlaneCutMesh( *before, plane.GetValue(), args.PlaneCutMode, args.PlaneCutFill );
                if ( !cut.IsSuccess() )
                    return Common::MakeError<bool>( cut.GetError() );
                Geometry::PlaneCutOutcome halves = cut.ExtractValue();
                if ( halves.OtherHalf )
                    otherHalf = std::make_shared<const Geometry::EditMesh>( std::move( *halves.OtherHalf ) );
                result = Common::MakeSuccess( std::move( halves.Kept ) );
                break;
            }
            case MeshOperation::Trim:
            {
                if ( args.TrimCutter.IsNull() )
                    return Common::MakeError<bool>( "Mesh Trim: no cutter - select the cutter entity (a closed "
                                                    "convex mesh) and press Pick Cutter in the Modeling panel" );
                if ( args.TrimCutter == entity )
                    return Common::MakeError<bool>( "Mesh Trim: the cutter is the mesh being edited - a mesh "
                                                    "cannot trim itself" );
                auto cutterRef = scene.FindEntityByID( args.TrimCutter );
                if ( !cutterRef )
                    return Common::MakeFormattedError<bool>( "Mesh Trim: the cutter entity {} is not in the scene",
                                                             static_cast<uint64_t>( args.TrimCutter ) );
                ECS::Entity cutter = cutterRef->get();
                if ( !cutter.HasComponent<ECS::StaticMeshComponent>() ||
                     !cutter.GetComponent<ECS::StaticMeshComponent>().EditableMesh )
                    return Common::MakeFormattedError<bool>(
                         "Mesh Trim: the cutter entity {} has no editable mesh",
                         static_cast<uint64_t>( args.TrimCutter ) );
                // WORLD transforms, parent chains included: either entity may be a child.
                const glm::mat4 cutterToMesh = glm::inverse( e.GetWorldTransform() ) * cutter.GetWorldTransform();
                result                       = WholeMesh(
                     Geometry::TrimMesh( *before, *cutter.GetComponent<ECS::StaticMeshComponent>().EditableMesh,
                                                               cutterToMesh, args.TrimSide ),
                     selection.Mode() );
                break;
            }
        }
        if ( !result.IsSuccess() )
            return Common::MakeError<bool>( result.GetError() );
        Geometry::MeshEditOutcome outcome = result.ExtractValue();

        auto after = std::make_shared<const Geometry::EditMesh>( std::move( outcome.Mesh ) );
        if ( auto set = ECS::SetEditableMesh( smc, after ); !set.IsSuccess() )
            return Common::MakeFormattedError<bool>( "Mesh {}: the result could not be put on the entity: {}",
                                                     ToString( operation ), set.GetError() );

        std::string label = fmt::format( "Mesh {}", ToString( operation ) );
        if ( TakesDistance( operation ) )
            label = fmt::format( "Mesh {} {} cm", ToString( operation ), distance );
        else if ( operation == MeshOperation::InsertEdgeLoop )
            label = fmt::format( "Mesh {} at {:.2f}", ToString( operation ), args.LoopPosition );
        else if ( operation == MeshOperation::Clean )
            label = fmt::format( "Mesh {} {} cm", ToString( operation ), args.WeldTolerance );
        else if ( operation == MeshOperation::Subdivide )
            label = fmt::format( "Mesh {} {} x{}", ToString( operation ),
                                 Geometry::ToString( args.SubdivideScheme ), args.SubdivideLevels );
        else if ( operation == MeshOperation::Mirror )
            label = fmt::format( "Mesh {} {} {}{}", ToString( operation ), Geometry::ToString( args.MirrorMode ),
                                 args.MirrorWorld ? "world " : "", "XYZ"[args.MirrorAxis] );
        else if ( operation == MeshOperation::PlaneCut )
            label = fmt::format( "Mesh {} {} {}{}{} {:+g} cm", ToString( operation ),
                                 Geometry::ToString( args.PlaneCutMode ), args.PlaneCutWorld ? "world " : "",
                                 args.PlaneCutKeepNegative ? "-" : "+", "XYZ"[args.PlaneCutAxis],
                                 args.PlaneCutOffset );
        else if ( operation == MeshOperation::Trim )
            label = fmt::format( "Mesh {} {}", ToString( operation ), Geometry::ToString( args.TrimSide ) );
        // Selection first, then the mesh: Track prunes the NEW selection against the new mesh (nothing to
        // drop), instead of the old one against it (every re-created triangle reported as lost).
        state.Restore( entity, outcome.Selection );
        state.Track( entity, after );
        auto selectionChange =
             MeshElementSelection::MakeSelectionChange( entity, selection, outcome.Selection, label );
        if ( otherHalf )
        {
            auto split =
                 Commands::RecordEditMeshSplit( entity, label, before, otherHalf, std::move( selectionChange ) );
            if ( !split.IsSuccess() )
            {
                // Nothing was recorded: the source goes back to the mesh it had. The copy's creation may have
                // moved the component storage, so the component is looked up again.
                auto                  source = scene.FindEntityByID( entity );
                Common::BoolResultStr back   = Common::MakeError<bool>( "the entity is gone" );
                if ( source && source->get().HasComponent<ECS::StaticMeshComponent>() )
                    back = ECS::SetEditableMesh( source->get().GetComponent<ECS::StaticMeshComponent>(), before );
                state.Restore( entity, selection );
                state.Track( entity, before );
                if ( !back.IsSuccess() )
                    return Common::MakeFormattedError<bool>( "{} (and putting the uncut mesh back failed: {})",
                                                             split.GetError(), back.GetError() );
                return Common::MakeError<bool>( split.GetError() );
            }
            LOG_INFO( "[Mesh Selection] {0}: the other half ({1} triangles) is on the new entity {2}", label,
                      otherHalf->TriangleCount(), static_cast<uint64_t>( split.GetValue() ) );
        }
        else
            Commands::RecordEditMeshChange( entity, label, before, std::move( selectionChange ) );
        LOG_INFO( "[Mesh Selection] {0}: {1} triangles ({2:+d}), {3} vertices ({4:+d})", label,
                  after->TriangleCount(), after->TriangleCount() - before->TriangleCount(), after->VertexCount(),
                  after->VertexCount() - before->VertexCount() );
        if ( !outcome.Report.empty() )
            LOG_INFO( "[Mesh Selection] {0}: {1}", label, outcome.Report );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::Core
