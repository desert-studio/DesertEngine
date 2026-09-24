#include "MeshSelectionOperations.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>

#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshSelection.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/Geometry/UECore/Operations/InsetMeshRegion.hpp>
#include <Engine/Geometry/UECore/Operations/OffsetMeshRegion.hpp>

#include <Common/Core/Logger.hpp>

#include <spdlog/fmt/fmt.h>

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

        bool IsRegionOperation( MeshOperation operation )
        {
            return operation == MeshOperation::Extrude || operation == MeshOperation::PushPull ||
                   operation == MeshOperation::Inset || operation == MeshOperation::Outset;
        }

        struct RegionOutcome
        {
            std::shared_ptr<const Geometry::FDynamicMesh3> Mesh;
            Geometry::ElementSelection                     Selection{ Geometry::ElementMode::Triangle };
        };

        // Extrude / Push-Pull / Inset / Outset, as UE's PolyEdit runs them on a copy of the mesh:
        // FExtrudeOp (ModelingOperators/Private/DeformationOps/ExtrudeOp.cpp:94-146) drives FOffsetMeshRegion,
        // UPolyEditInsetOutsetActivity (PolyEditInsetOutsetActivity.cpp:181) drives FInsetMeshRegion with the
        // distance negated for Outset. The result's selection is the moved region (OffsetTids) or the inset
        // region (InitialTriangles), in the mode the user selected in.
        Common::ResultStr<RegionOutcome> RunRegionOperation( MeshOperation                     operation,
                                                             const Geometry::FDynamicMesh3&    before,
                                                             const Geometry::ElementSelection& selection,
                                                             float                             distance )
        {
            const char* name = ToString( operation );
            if ( distance == 0.0f )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: distance 0 cm changes nothing", name );
            if ( distance < 0.0f && operation != MeshOperation::PushPull )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh {}: distance {} cm is negative - {}", name, distance,
                     operation == MeshOperation::Extrude ? "Push/Pull takes a signed distance"
                     : operation == MeshOperation::Inset ? "use Outset"
                                                         : "use Inset" );
            const Geometry::FGroupTopology   topology( &before, true );
            const Geometry::ElementSelection triangles =
                 Geometry::ConvertSelection( before, topology, selection, Geometry::ElementMode::Triangle );
            if ( triangles.Empty() )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh {}: the {} selected {} cover no whole triangle", name, selection.Size(),
                     Geometry::ToString( selection.Mode() ) );
            Geometry::TArray<Geometry::int32> regionTriangles;
            for ( const int t : triangles.Ids() )
                regionTriangles.Add( t );

            auto                              mesh = std::make_shared<Geometry::FDynamicMesh3>( before );
            Geometry::TArray<Geometry::int32> resultTriangles;
            if ( operation == MeshOperation::Extrude || operation == MeshOperation::PushPull )
            {
                Geometry::FOffsetMeshRegion extruder( mesh.get() );
                extruder.Triangles = regionTriangles;
                // UE's Extrude default (SelectedTriangleNormalsEven): every selected face moves by the full
                // distance. Push/Pull is FExtrudeOp's SingleDirection: one direction, the region's area-weighted
                // normal, so the walls are parallel and a push into a solid cannot fold them.
                extruder.ExtrusionVectorType = Geometry::FOffsetMeshRegion::EVertexExtrusionVectorType::
                     SelectionTriNormalsAngleWeightedAdjusted;
                extruder.DefaultOffsetDistance = distance;
                if ( operation == MeshOperation::PushPull )
                {
                    Geometry::FVector3d direction( 0.0, 0.0, 0.0 );
                    for ( const int t : triangles.Ids() )
                        direction += before.GetTriNormal( t ) * before.GetTriArea( t );
                    if ( direction.Length() <= 0.0 )
                        return Common::MakeFormattedError<RegionOutcome>(
                             "Mesh {}: the {} selected triangles have no average normal (they cancel out)", name,
                             triangles.Size() );
                    Geometry::Normalize( direction );
                    const double signedDistance = distance;
                    extruder.OffsetPositionFunc = [direction, signedDistance]( const Geometry::FVector3d& position,
                                                                               const Geometry::FVector3d&, int )
                    { return position + direction * signedDistance; };
                }
                extruder.bIsPositiveOffset = distance > 0.0f;
                if ( !extruder.Apply() )
                    return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name,
                                                                      extruder.FailureReason );
                for ( const auto& region : extruder.OffsetRegions )
                    for ( const Geometry::int32 t : region.OffsetTids )
                        resultTriangles.Add( t );
            }
            else
            {
                Geometry::FInsetMeshRegion inset( mesh.get() );
                inset.Triangles     = regionTriangles;
                inset.InsetDistance = operation == MeshOperation::Outset ? -distance : distance;
                if ( !inset.Apply() )
                    return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name, inset.FailureReason );
                for ( const auto& region : inset.InsetRegions )
                    for ( const Geometry::int32 t : region.InitialTriangles )
                        resultTriangles.Add( t );
            }

            Geometry::ElementSelection result( Geometry::ElementMode::Triangle );
            for ( const Geometry::int32 t : resultTriangles )
                if ( auto added = result.Add( *mesh, t ); !added.IsSuccess() )
                    return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: result triangle {}: {}", name, t,
                                                                      added.GetError() );
            if ( selection.Mode() != Geometry::ElementMode::Triangle )
            {
                const Geometry::FGroupTopology afterTopology( mesh.get(), true );
                result = Geometry::ConvertSelection( *mesh, afterTopology, result, selection.Mode() );
            }
            return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), std::move( result ) } );
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
        if ( !e.HasComponent<ECS::StaticMeshComponent>() )
            return Common::MakeFormattedError<bool>( "Mesh {}: entity {} has no static mesh",
                                                     ToString( operation ), static_cast<uint64_t>( entity ) );
        auto& smc    = e.GetComponent<ECS::StaticMeshComponent>();
        auto  target = GetToolTargetMesh( smc );
        if ( !target.IsSuccess() )
            return Common::MakeFormattedError<bool>( "Mesh {}: entity {}: {}", ToString( operation ),
                                                     static_cast<uint64_t>( entity ), target.GetError() );
        // The component may have moved on since the tool last looked (an undo this frame): the selection is
        // pruned against the mesh the operation will actually run on.
        state.Track( entity, target.GetValue().Mesh );
        const std::shared_ptr<const Geometry::FDynamicMesh3> before = target.GetValue().Mesh;
        // What undo restores: null for a lifted asset, so one undo removes the lift and the edit together.
        const std::shared_ptr<const Geometry::FDynamicMesh3> committed = target.GetValue().Committed;
        const Geometry::ElementSelection                     selection = state.Selection();
        std::shared_ptr<const Geometry::FDynamicMesh3>       otherHalf; // Plane Cut, Keep Both Halves
        std::shared_ptr<const Geometry::FDynamicMesh3>       after;
        Geometry::MeshEditOutcome                            outcome;
        if ( IsRegionOperation( operation ) )
        {
            auto region = RunRegionOperation( operation, *before, selection, distance );
            if ( !region.IsSuccess() )
                return Common::MakeError<bool>( region.GetError() );
            RegionOutcome done = region.ExtractValue();
            after              = std::move( done.Mesh );
            outcome.Selection  = std::move( done.Selection );
        }
        else
        {
            auto view = Geometry::Bridge::EditMeshView( before );
            if ( !view.IsSuccess() )
                return Common::MakeFormattedError<bool>( "Mesh {}: {}", ToString( operation ), view.GetError() );
            const Geometry::EditMesh& beforeMesh = *view.GetValue();
            // The operations still run on the EditMesh (P11-P19): the selection in its edge IDs.
            auto inEditIds = Geometry::Bridge::ToEditMeshSelection( *before, beforeMesh, selection );
            if ( !inEditIds.IsSuccess() )
                return Common::MakeFormattedError<bool>( "Mesh {}: {}", ToString( operation ),
                                                         inEditIds.GetError() );
            const Geometry::ElementSelection editSelection = inEditIds.ExtractValue();

            Common::ResultStr<Geometry::MeshEditOutcome> result;
            switch ( operation )
            {
                case MeshOperation::Delete:
                    result = Geometry::DeleteSelection( beforeMesh, editSelection );
                    break;
                case MeshOperation::Extrude:
                case MeshOperation::PushPull:
                case MeshOperation::Inset:
                case MeshOperation::Outset:
                    return Common::MakeFormattedError<bool>( "Mesh {}: runs on FDynamicMesh3, not the EditMesh",
                                                             ToString( operation ) );
                case MeshOperation::Offset:
                    result = Geometry::OffsetSelection( beforeMesh, editSelection, distance );
                    break;
                case MeshOperation::Bevel:
                    result = Geometry::BevelSelection( beforeMesh, editSelection, distance );
                    break;
                case MeshOperation::InsertEdgeLoop:
                    result = Geometry::InsertEdgeLoop( beforeMesh, editSelection, args.LoopPosition );
                    break;
                case MeshOperation::Cut:
                    if ( !args.CutPlane )
                        return Common::MakeError<bool>( "Mesh Cut: no cut line - draw it in the viewport with the "
                                                        "knife (Alt+K, then two clicks)" );
                    result = Geometry::CutSelection( beforeMesh, editSelection, *args.CutPlane );
                    break;
                case MeshOperation::Clean:
                {
                    // Clean is mesh-wide: it takes no selection and leaves none (every ID may change).
                    auto cleaned = Geometry::CleanMesh( beforeMesh, args.WeldTolerance );
                    if ( cleaned.IsSuccess() )
                    {
                        Geometry::CleanOutcome clean = cleaned.ExtractValue();
                        clean.Edit.Selection         = Geometry::ElementSelection( editSelection.Mode() );
                        result                       = Common::MakeSuccess( std::move( clean.Edit ) );
                    }
                    else
                        result = Common::MakeError<Geometry::MeshEditOutcome>( cleaned.GetError() );
                    break;
                }
                case MeshOperation::Subdivide:
                    result = WholeMesh(
                         Geometry::SubdivideMesh( beforeMesh, args.SubdivideLevels, args.SubdivideScheme ),
                         editSelection.Mode() );
                    break;
                case MeshOperation::Mirror:
                {
                    auto plane = AxisPlane( e, args.MirrorAxis, args.MirrorWorld, args.MirrorKeepNegative, 0.0f,
                                            "Mesh Mirror" );
                    if ( !plane.IsSuccess() )
                        return Common::MakeError<bool>( plane.GetError() );
                    result = WholeMesh(
                         Geometry::MirrorMesh( beforeMesh, plane.GetValue(), args.MirrorMode, args.WeldTolerance ),
                         editSelection.Mode() );
                    break;
                }
                case MeshOperation::PlaneCut:
                {
                    auto plane = AxisPlane( e, args.PlaneCutAxis, args.PlaneCutWorld, args.PlaneCutKeepNegative,
                                            args.PlaneCutOffset, "Mesh Plane Cut" );
                    if ( !plane.IsSuccess() )
                        return Common::MakeError<bool>( plane.GetError() );
                    auto cut = Geometry::PlaneCutMesh( beforeMesh, plane.GetValue(), args.PlaneCutMode,
                                                       args.PlaneCutFill );
                    if ( !cut.IsSuccess() )
                        return Common::MakeError<bool>( cut.GetError() );
                    Geometry::PlaneCutOutcome halves = cut.ExtractValue();
                    if ( halves.OtherHalf )
                    {
                        auto other = Geometry::Bridge::FromEditMesh( std::move( *halves.OtherHalf ) );
                        if ( !other.IsSuccess() )
                            return Common::MakeError<bool>( "Mesh Plane Cut, other half: " + other.GetError() );
                        otherHalf = other.ExtractValue();
                    }
                    result = Common::MakeSuccess( std::move( halves.Kept ) );
                    break;
                }
                case MeshOperation::Trim:
                {
                    if ( args.TrimCutter.IsNull() )
                        return Common::MakeError<bool>(
                             "Mesh Trim: no cutter - select the cutter entity (a closed "
                             "convex mesh) and press Pick Cutter in the Modeling panel" );
                    if ( args.TrimCutter == entity )
                        return Common::MakeError<bool>( "Mesh Trim: the cutter is the mesh being edited - a mesh "
                                                        "cannot trim itself" );
                    auto cutterRef = scene.FindEntityByID( args.TrimCutter );
                    if ( !cutterRef )
                        return Common::MakeFormattedError<bool>(
                             "Mesh Trim: the cutter entity {} is not in the scene",
                             static_cast<uint64_t>( args.TrimCutter ) );
                    ECS::Entity cutter = cutterRef->get();
                    if ( !cutter.HasComponent<ECS::StaticMeshComponent>() )
                        return Common::MakeFormattedError<bool>(
                             "Mesh Trim: the cutter entity {} has no static mesh",
                             static_cast<uint64_t>( args.TrimCutter ) );
                    auto cutterTarget = GetToolTargetMesh( cutter.GetComponent<ECS::StaticMeshComponent>() );
                    if ( !cutterTarget.IsSuccess() )
                        return Common::MakeError<bool>( "Mesh Trim, cutter: " + cutterTarget.GetError() );
                    // WORLD transforms, parent chains included: either entity may be a child.
                    auto cutterView = Geometry::Bridge::EditMeshView( cutterTarget.GetValue().Mesh );
                    if ( !cutterView.IsSuccess() )
                        return Common::MakeError<bool>( "Mesh Trim, cutter: " + cutterView.GetError() );
                    const glm::mat4 cutterToMesh =
                         glm::inverse( e.GetWorldTransform() ) * cutter.GetWorldTransform();
                    result = WholeMesh(
                         Geometry::TrimMesh( beforeMesh, *cutterView.GetValue(), cutterToMesh, args.TrimSide ),
                         editSelection.Mode() );
                    break;
                }
            }
            if ( !result.IsSuccess() )
                return Common::MakeError<bool>( result.GetError() );
            outcome = result.ExtractValue();

            auto converted = Geometry::Bridge::FromEditMesh( std::move( outcome.Mesh ), &outcome.Selection );
            if ( !converted.IsSuccess() )
                return Common::MakeFormattedError<bool>( "Mesh {}: {}", ToString( operation ),
                                                         converted.GetError() );
            after = converted.ExtractValue();
        }
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
            auto split = Commands::RecordEditMeshSplit( entity, label, committed, otherHalf,
                                                        std::move( selectionChange ) );
            if ( !split.IsSuccess() )
            {
                // Nothing was recorded: the source goes back to the mesh it had. The copy's creation may have
                // moved the component storage, so the component is looked up again.
                auto                  source = scene.FindEntityByID( entity );
                Common::BoolResultStr back   = Common::MakeError<bool>( "the entity is gone" );
                if ( source && source->get().HasComponent<ECS::StaticMeshComponent>() )
                {
                    auto& sourceMesh = source->get().GetComponent<ECS::StaticMeshComponent>();
                    back             = Common::MakeSuccess( true );
                    switch ( PlanMeshRestore( sourceMesh.EditableMesh, committed ) )
                    {
                        case MeshRestore::Set:
                            back = ECS::SetEditableMesh( sourceMesh, committed );
                            break;
                        case MeshRestore::Clear:
                            ECS::ClearEditableMesh( sourceMesh );
                            break;
                        case MeshRestore::Unchanged:
                            break;
                    }
                }
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
            Commands::RecordEditMeshChange( entity, label, committed, std::move( selectionChange ) );
        LOG_INFO( "[Mesh Selection] {0}: {1} triangles ({2:+d}), {3} vertices ({4:+d})", label,
                  after->TriangleCount(), after->TriangleCount() - before->TriangleCount(), after->VertexCount(),
                  after->VertexCount() - before->VertexCount() );
        if ( !outcome.Report.empty() )
            LOG_INFO( "[Mesh Selection] {0}: {1}", label, outcome.Report );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::Core
