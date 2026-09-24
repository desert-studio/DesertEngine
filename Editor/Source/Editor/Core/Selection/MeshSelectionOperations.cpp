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
        }
        return "Unknown";
    }

    bool TakesDistance( MeshOperation operation )
    {
        switch ( operation )
        {
            case MeshOperation::Delete:
            case MeshOperation::InsertEdgeLoop:
            case MeshOperation::Cut:
            case MeshOperation::Clean:
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
        // Selection first, then the mesh: Track prunes the NEW selection against the new mesh (nothing to
        // drop), instead of the old one against it (every re-created triangle reported as lost).
        state.Restore( entity, outcome.Selection );
        state.Track( entity, after );
        Commands::RecordEditMeshChange(
             entity, label, before,
             MeshElementSelection::MakeSelectionChange( entity, selection, outcome.Selection, label ) );
        LOG_INFO( "[Mesh Selection] {0}: {1} triangles ({2:+d}), {3} vertices ({4:+d})", label,
                  after->TriangleCount(), after->TriangleCount() - before->TriangleCount(), after->VertexCount(),
                  after->VertexCount() - before->VertexCount() );
        if ( !outcome.Report.empty() )
            LOG_INFO( "[Mesh Selection] {0}: {1}", label, outcome.Report );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::Core
