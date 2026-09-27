#include "MeshBooleanOperation.hpp"

#include <cstddef>

namespace Desert::Geometry
{
    const char* ToString( BooleanOperation operation )
    {
        switch ( operation )
        {
            case BooleanOperation::Union:
                return "Union";
            case BooleanOperation::Difference:
                return "Difference";
            case BooleanOperation::Intersect:
                return "Intersect";
            case BooleanOperation::TrimInside:
                return "Trim Inside";
            case BooleanOperation::TrimOutside:
                return "Trim Outside";
            case BooleanOperation::NewGroupInside:
                return "New Group Inside";
            case BooleanOperation::NewGroupOutside:
                return "New Group Outside";
        }
        return "Unknown";
    }

    namespace
    {
        std::ptrdiff_t CountOpenEdges( const DynamicMesh3& mesh )
        {
            std::ptrdiff_t count = 0;
            for ( [[maybe_unused]] int const edge : mesh.BoundaryEdgeIndicesItr() )
                ++count;
            return count;
        }
    } // namespace

    Common::ResultStr<BooleanOutcome> RunMeshBoolean( BooleanOperation operation, const DynamicMesh3& target,
                                                      const DynamicMesh3& cutter )
    {
        const char* name = ToString( operation );
        if ( target.TriangleCount() == 0 )
            return Common::MakeFormattedError<BooleanOutcome>( "Mesh {}: the mesh has no triangles", name );
        if ( cutter.TriangleCount() == 0 )
            return Common::MakeFormattedError<BooleanOutcome>( "Mesh {}: the cutter has no triangles", name );
        // The winding number of an open surface is fractional near it, so "inside" is not a property of the
        // cutter any more; the target is classified against the cutter only in the two-mesh operations.
        if ( const std::ptrdiff_t open = CountOpenEdges( cutter ); open != 0 )
            return Common::MakeFormattedError<BooleanOutcome>(
                 "Mesh {}: the cutter has {} open edges - inside and outside of an open mesh are undefined", name,
                 open );
        const bool bSingleMesh = MeshBoolean::OperatesOnSingleMesh( operation );
        if ( !bSingleMesh )
        {
            if ( const std::ptrdiff_t open = CountOpenEdges( target ); open != 0 )
                return Common::MakeFormattedError<BooleanOutcome>(
                     "Mesh {}: the mesh has {} open edges - a boolean of two meshes needs both closed (Trim "
                     "takes an open mesh)",
                     name, open );
        }

        auto        mesh = std::make_shared<DynamicMesh3>();
        MeshBoolean boolean( &target, &cutter, mesh.get(), operation );
        const bool  bWelded = boolean.Compute();
        if ( !bSingleMesh && ( !bWelded || !boolean.CreatedBoundaryEdges.empty() ) )
            return Common::MakeFormattedError<BooleanOutcome>(
                 "Mesh {}: {} edges of the cut seam could not be welded shut", name,
                 boolean.CreatedBoundaryEdges.size() );
        if ( mesh->TriangleCount() == 0 )
            return Common::MakeFormattedError<BooleanOutcome>( "Mesh {}: the result has no triangles left", name );
        // a cut adds vertices even where nothing is removed, so the unchanged case is "no cut at all": the same
        // vertex and triangle counts as the input
        const bool bTrim = operation == BooleanOperation::TrimInside || operation == BooleanOperation::TrimOutside;
        if ( bTrim && mesh->TriangleCount() == target.TriangleCount() &&
             mesh->VertexCount() == target.VertexCount() )
            return Common::MakeFormattedError<BooleanOutcome>(
                 "Mesh {}: the cutter does not cross the mesh ({} triangles unchanged)", name,
                 target.TriangleCount() );
        return Common::MakeSuccess( BooleanOutcome{ std::move( mesh ) } );
    }
} // namespace Desert::Geometry
