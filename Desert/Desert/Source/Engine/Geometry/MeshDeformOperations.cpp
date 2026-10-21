// The Deform palette's operations. Ported structure, by tool:
//   Smooth - UE 5.8 MeshModelingToolset/Operators/Private/SmoothingOps/IterativeSmoothingOp.cpp (Smooth_Forward:
//            double position buffer, per round lerp towards the one-ring centroid by SmoothAlpha, boundary
//            vertices held when bSmoothBoundary is off, normals recomputed on the result), centroids from
//            GeometryCore DynamicMesh/MeshWeights.h (UniformCentroid, CotanCentroidSafe).
//   Offset - UE 5.8 MeshModelingToolset/Operators/Private/SmoothingOps/IterativeOffsetMeshOp (UOffsetMeshTool,
//            Iterative): Steps rounds of StepDistance = Distance / Steps along the current vertex normals,
//            each followed by a smoothing round of SmoothingPerStep; boundaries held unless bOffsetBoundaries.
// Deviation from UE: the cotan centroid clamps a negative edge weight (an obtuse opposite angle pair) to zero
// so an explicit step can never push a vertex away from its ring; UE leaves the weight signed and relies on
// small SmoothAlpha. Rounds run serially (UE ParallelFor) - Modeling Mode recomputes synchronously.
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <spdlog/fmt/fmt.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        constexpr double kDegenerateCross = 1e-12; // |cross|^2 below this: the face has no angle to take a cot of

        // Which vertices the operation may move: valid, and not held by the boundary / UV-seam pins.
        std::vector<uint8_t> MovableVertices( const DynamicMesh3& mesh, bool moveBoundary, bool pinUVSeams,
                                              int32_t& movedOut, int32_t& pinnedOut )
        {
            std::vector<uint8_t>           movable( static_cast<size_t>( mesh.MaxVertexID() ), 0 );
            const DynamicMeshAttributeSet* attributes = mesh.HasAttributes() ? mesh.Attributes() : nullptr;
            movedOut = pinnedOut = 0;
            for ( const int vid : mesh.VertexIndicesItr() )
            {
                bool pinned = !moveBoundary && mesh.IsBoundaryVertex( vid );
                if ( !pinned && pinUVSeams && attributes != nullptr )
                    for ( int layer = 0; layer < attributes->NumUVLayers() && !pinned; ++layer )
                        pinned = attributes->GetUVLayer( layer )->IsSeamVertex( vid, false );
                movable[static_cast<size_t>( vid )] = pinned ? 0 : 1;
                ++( pinned ? pinnedOut : movedOut );
            }
            return movable;
        }

        std::vector<glm::dvec3> Positions( const DynamicMesh3& mesh )
        {
            std::vector<glm::dvec3> out( static_cast<size_t>( mesh.MaxVertexID() ), glm::dvec3( 0.0 ) );
            for ( const int vid : mesh.VertexIndicesItr() )
                out[static_cast<size_t>( vid )] = mesh.GetVertex( vid );
            return out;
        }

        // UE FMeshWeights::UniformCentroid.
        bool UniformCentroid( const DynamicMesh3& mesh, int vid, const std::vector<glm::dvec3>& p,
                              glm::dvec3& out )
        {
            glm::dvec3 sum( 0.0 );
            int        count = 0;
            for ( const int nbr : mesh.VtxVerticesItr( vid ) )
            {
                sum += p[static_cast<size_t>( nbr )];
                ++count;
            }
            if ( count == 0 )
                return false;
            out = sum / static_cast<double>( count );
            return true;
        }

        // cot of the angle at @p o between the edges to @p a and @p b; false when the corner is degenerate.
        bool CotAt( const glm::dvec3& o, const glm::dvec3& a, const glm::dvec3& b, double& cot )
        {
            const glm::dvec3 ea    = a - o;
            const glm::dvec3 eb    = b - o;
            const double     cross = glm::length( glm::cross( ea, eb ) );
            if ( cross * cross < kDegenerateCross )
                return false;
            cot = glm::dot( ea, eb ) / cross;
            return true;
        }

        // UE FMeshWeights::CotanCentroidSafe: w(v,n) = cot(alpha) + cot(beta) at the two vertices opposite the
        // edge; a degenerate face anywhere in the ring falls back to the uniform centroid, as UE does.
        bool CotanCentroid( const DynamicMesh3& mesh, int vid, const std::vector<glm::dvec3>& p, glm::dvec3& out )
        {
            const glm::dvec3& pv = p[static_cast<size_t>( vid )];
            glm::dvec3        sum( 0.0 );
            double            weightSum = 0.0;
            for ( const int nbr : mesh.VtxVerticesItr( vid ) )
            {
                const int eid = mesh.FindEdge( vid, nbr );
                if ( eid == DynamicMesh3::InvalidID )
                    continue;
                const Index2i     opposite = mesh.GetEdgeOpposingV( eid );
                const glm::dvec3& pn       = p[static_cast<size_t>( nbr )];
                double            weight   = 0.0;
                for ( const int o : { opposite.A, opposite.B } )
                {
                    if ( o == DynamicMesh3::InvalidID )
                        continue;
                    double cot = 0.0;
                    if ( !CotAt( p[static_cast<size_t>( o )], pv, pn, cot ) )
                        return UniformCentroid( mesh, vid, p, out );
                    weight += cot;
                }
                weight = std::max( weight, 0.0 );
                sum += weight * pn;
                weightSum += weight;
            }
            if ( weightSum <= 1e-12 )
                return UniformCentroid( mesh, vid, p, out );
            out = sum / weightSum;
            return true;
        }

        // One Jacobi smoothing round: every movable vertex moves @p alpha of the way to its centroid, all
        // centroids taken from @p p; the result goes to @p next.
        void SmoothRound( const DynamicMesh3& mesh, SmoothWeightType type, double alpha,
                          const std::vector<uint8_t>& movable, const std::vector<glm::dvec3>& p,
                          std::vector<glm::dvec3>& next )
        {
            for ( const int vid : mesh.VertexIndicesItr() )
            {
                const size_t i = static_cast<size_t>( vid );
                next[i]        = p[i];
                if ( movable[i] == 0 )
                    continue;
                glm::dvec3 centroid;
                const bool found = type == SmoothWeightType::Cotan ? CotanCentroid( mesh, vid, p, centroid )
                                                                   : UniformCentroid( mesh, vid, p, centroid );
                if ( found )
                    next[i] = p[i] + alpha * ( centroid - p[i] );
            }
        }

        void WritePositions( DynamicMesh3& mesh, const std::vector<glm::dvec3>& p )
        {
            for ( const int vid : mesh.VertexIndicesItr() )
                mesh.SetVertex( vid, p[static_cast<size_t>( vid )] );
        }

        // Normals and tangents are derived from positions (UE recomputes them on the op's result).
        Common::ResultStr<DeformOutcome> Finish( DynamicMesh3&& mesh, const char* name, int32_t moved,
                                                 int32_t pinned, std::string report )
        {
            if ( mesh.HasAttributes() && mesh.Attributes()->PrimaryNormals() != nullptr )
                MeshNormals::QuickRecomputeOverlayNormals( mesh );
            if ( mesh.HasVertexNormals() )
                MeshNormals::QuickComputeVertexNormals( mesh );
            if ( auto tangents = RecomputeTangentSpace( mesh, name ); !tangents.IsSuccess() )
                return Common::MakeError<DeformOutcome>( tangents.GetError() );
            DeformOutcome out;
            out.MovedVertices  = moved;
            out.PinnedVertices = pinned;
            out.Report         = fmt::format( "{} vertices moved, {} pinned; {}", moved, pinned, report );
            out.Mesh           = std::move( mesh );
            return Common::MakeSuccess( std::move( out ) );
        }

        Common::BoolResultStr CheckCommon( const DynamicMesh3& mesh, const char* tool, int32_t steps, float alpha )
        {
            if ( mesh.TriangleCount() == 0 )
                return Common::MakeError<bool>( fmt::format( "{}: the mesh has no triangles", tool ) );
            if ( steps < 1 )
                return Common::MakeError<bool>(
                     fmt::format( "{}: Steps is {}, at least 1 expected", tool, steps ) );
            if ( !( alpha >= 0.0f && alpha <= 1.0f ) )
                return Common::MakeError<bool>(
                     fmt::format( "{}: SmoothingPerStep is {}, 0..1 expected", tool, alpha ) );
            return Common::MakeSuccess( true );
        }
    } // namespace

    const char* ToString( SmoothWeightType type )
    {
        switch ( type )
        {
            case SmoothWeightType::Uniform:
                return "Uniform";
            case SmoothWeightType::Cotan:
                return "Cotan";
        }
        return "Unknown";
    }

    Common::ResultStr<DeformOutcome> Smooth( const DynamicMesh3& input, const SmoothSettings& settings )
    {
        if ( auto check = CheckCommon( input, "Smooth", settings.Steps, settings.SmoothingPerStep );
             !check.IsSuccess() )
            return Common::MakeError<DeformOutcome>( check.GetError() );

        DynamicMesh3               mesh  = input;
        int32_t                    moved = 0, pinned = 0;
        const std::vector<uint8_t> movable =
             MovableVertices( mesh, settings.SmoothBoundary, settings.PinUVSeams, moved, pinned );
        std::vector<glm::dvec3> p    = Positions( mesh );
        std::vector<glm::dvec3> next = p;
        for ( int32_t step = 0; step < settings.Steps; ++step )
        {
            SmoothRound( mesh, settings.WeightType, settings.SmoothingPerStep, movable, p, next );
            p.swap( next );
        }
        WritePositions( mesh, p );
        return Finish( std::move( mesh ), "Smooth", moved, pinned,
                       fmt::format( "{} smoothing, {} steps at {:.2f}", ToString( settings.WeightType ),
                                    settings.Steps, settings.SmoothingPerStep ) );
    }

    Common::ResultStr<DeformOutcome> Offset( const DynamicMesh3& input, const OffsetSettings& settings )
    {
        if ( auto check = CheckCommon( input, "Offset", settings.Steps, settings.SmoothingPerStep );
             !check.IsSuccess() )
            return Common::MakeError<DeformOutcome>( check.GetError() );
        if ( !std::isfinite( settings.Distance ) )
            return Common::MakeError<DeformOutcome>( "Offset: Distance is not a finite number" );

        DynamicMesh3               mesh  = input;
        int32_t                    moved = 0, pinned = 0;
        const std::vector<uint8_t> movable =
             MovableVertices( mesh, settings.OffsetBoundary, settings.PinUVSeams, moved, pinned );
        const double            stepDistance = static_cast<double>( settings.Distance ) / settings.Steps;
        std::vector<glm::dvec3> p            = Positions( mesh );
        std::vector<glm::dvec3> next         = p;
        for ( int32_t step = 0; step < settings.Steps; ++step )
        {
            // Normals of the current shell: the mesh holds the previous round's positions.
            for ( const int vid : mesh.VertexIndicesItr() )
                if ( movable[static_cast<size_t>( vid )] != 0 )
                    next[static_cast<size_t>( vid )] =
                         p[static_cast<size_t>( vid )] +
                         stepDistance * MeshNormals::ComputeVertexNormal( mesh, vid );
                else
                    next[static_cast<size_t>( vid )] = p[static_cast<size_t>( vid )];
            p.swap( next );
            if ( settings.SmoothingPerStep > 0.0f )
            {
                SmoothRound( mesh, settings.WeightType, settings.SmoothingPerStep, movable, p, next );
                p.swap( next );
            }
            WritePositions( mesh, p );
        }
        return Finish( std::move( mesh ), "Offset", moved, pinned,
                       fmt::format( "offset {:.2f} cm in {} steps, {} smoothing {:.2f} per step",
                                    settings.Distance, settings.Steps, ToString( settings.WeightType ),
                                    settings.SmoothingPerStep ) );
    }
} // namespace Desert::Geometry
