// The Deform palette's operations. Ported structure, by tool:
//   Smooth - UE 5.8 MeshModelingToolset/Operators/Private/SmoothingOps/IterativeSmoothingOp.cpp (Smooth_Forward:
//            double position buffer, per round lerp towards the one-ring centroid by SmoothAlpha, boundary
//            vertices held when bSmoothBoundary is off, normals recomputed on the result), centroids from
//            GeometryCore DynamicMesh/MeshWeights.h (UniformCentroid, CotanCentroidSafe).
//   Offset - UE 5.8 MeshModelingToolset/Operators/Private/SmoothingOps/IterativeOffsetMeshOp (UOffsetMeshTool,
//            Iterative): Steps rounds of StepDistance = Distance / Steps along the current vertex normals,
//            each followed by a smoothing round of SmoothingPerStep; boundaries held unless bOffsetBoundaries.
//   Smooth, Implicit / Diffusion - UE 5.8 SmoothingOps/CotanSmoothingOp.cpp (ComputeSmoothing_BiHarmonic: the
//            least-squares fairing |L x|^2 + w^2 |x - p|^2, UE's normalisation SmoothPower = Smoothness^2 / 50 *
//            VertexCount) and IterativeSmoothingOp Smooth_Implicit (ComputeSmoothing_Diffusion: backward-Euler
//            heat steps), both on the ClampedCotangent (or uniform) Laplacian; UE's Eigen SimplicialLDLT is our
//            SparseLDLT (MeshUVSolver.hpp). Pinned vertices are eliminated from the system (their positions
//            move to the right-hand side) instead of UE's heavily weighted soft constraints, so they hold exactly.
//            Implicit VolumeCorrection pushes each fairing target along the input normal, as UE's NormalOffset.
//   Offset, Implicit - UE 5.8 DeformationOps/MeshOffsetOps.h FLaplacianOffsetMeshOp: the constrained deformer
//            (|L x - L p|^2 kept) softly pulled to p + Distance n; open boundary held as UE's interior-only solve.
//   Warp - UE 5.8 ModelingOperators/MeshSpaceDeformerOps (FMeshSpaceDeformerOp ObjectToGizmo, FBendMeshOp after
//            Barr's "Global and Local Deformations of Solid Primitives" in the y-up gizmo space, FTwistMeshOp,
//            FFlareMeshOp); UE transforms the normal overlay analytically, here normals are recomputed in Finish
//            like the other Deform operators. A zero-length slab (Upper == Lower) is refused for Twist / Flare,
//            where UE divides by it.
// Deviation from UE: the cotan centroid clamps a negative edge weight (an obtuse opposite angle pair) to zero
// so an explicit step can never push a vertex away from its ring; UE leaves the weight signed and relies on
// small SmoothAlpha. Rounds run serially (UE ParallelFor) - Modeling Mode recomputes synchronously.
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/Solvers/MeshUVSolver.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <spdlog/fmt/fmt.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <utility>
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

        // ---- implicit solvers ----
        using LaplacianRow = std::vector<std::pair<int32_t, double>>; // (vertex ID, value), diagonal included

        // The symmetric positive semi-definite Laplacian, rows by vertex ID: L_ii = sum w, L_ij = -w. Uniform:
        // w = 1; Cotan: UE's ClampedCotangent w = max( 0, (cot a + cot b) / 2 ), a degenerate corner adds 0.
        std::vector<LaplacianRow> BuildLaplacian( const DynamicMesh3& mesh, SmoothWeightType type,
                                                  const std::vector<glm::dvec3>& p )
        {
            std::vector<LaplacianRow> rows( static_cast<size_t>( mesh.MaxVertexID() ) );
            std::vector<double>       diagonal( rows.size(), 0.0 );
            for ( const int eid : mesh.EdgeIndicesItr() )
            {
                const Index2i ev     = mesh.GetEdgeV( eid );
                double        weight = 1.0;
                if ( type == SmoothWeightType::Cotan )
                {
                    weight = 0.0;
                    for ( const int o : { mesh.GetEdgeOpposingV( eid ).A, mesh.GetEdgeOpposingV( eid ).B } )
                    {
                        double cot = 0.0;
                        if ( o != DynamicMesh3::InvalidID &&
                             CotAt( p[static_cast<size_t>( o )], p[static_cast<size_t>( ev.A )],
                                    p[static_cast<size_t>( ev.B )], cot ) )
                            weight += 0.5 * cot;
                    }
                    weight = std::max( weight, 0.0 );
                }
                if ( weight == 0.0 )
                    continue;
                rows[static_cast<size_t>( ev.A )].push_back( { ev.B, -weight } );
                rows[static_cast<size_t>( ev.B )].push_back( { ev.A, -weight } );
                diagonal[static_cast<size_t>( ev.A )] += weight;
                diagonal[static_cast<size_t>( ev.B )] += weight;
            }
            for ( const int vid : mesh.VertexIndicesItr() )
                rows[static_cast<size_t>( vid )].push_back( { vid, diagonal[static_cast<size_t>( vid )] } );
            return rows;
        }

        // Unknowns are the movable vertices only: vertex ID -> compact index, -1 for a pinned or absent one.
        std::vector<int32_t> CompactMovable( const std::vector<uint8_t>& movable, int32_t& count )
        {
            std::vector<int32_t> index( movable.size(), -1 );
            count = 0;
            for ( size_t i = 0; i < movable.size(); ++i )
                if ( movable[i] != 0 )
                    index[i] = count++;
            return index;
        }

        // Factor @p a and solve it for the three coordinates of @p rhs; the solution is written to the movable
        // vertices of @p p. False when the system is not positive definite.
        bool SolveInto( const SparseMatrixD& a, const std::vector<int32_t>& index, int32_t count,
                        const std::array<std::vector<double>, 3>& rhs, std::vector<glm::dvec3>& p,
                        const SparseLDLT* factored = nullptr )
        {
            SparseLDLT        local;
            const SparseLDLT* solver = factored;
            if ( solver == nullptr )
            {
                if ( !local.Factorize( a ) )
                    return false;
                solver = &local;
            }
            std::vector<double> x( static_cast<size_t>( count ) );
            for ( int c = 0; c < 3; ++c )
            {
                solver->Solve( rhs[static_cast<size_t>( c )], x );
                for ( size_t vid = 0; vid < index.size(); ++vid )
                    if ( index[vid] >= 0 )
                        p[vid][c] = x[static_cast<size_t>( index[vid] )];
            }
            return true;
        }

        // UE IConstrainedMeshSolver with soft position constraints of one weight w (UE AddConstraint):
        // x = argmin |L x - d|^2 + w^2 |x_m - t_m|^2 over the movable x_m, pinned vertices fixed at p, where d = 0
        // (UE ConstructConstrainedMeshSmoother: fairing) or d = L p (UE ConstructConstrainedMeshDeformer: the
        // input's Laplacian vectors are kept, so detail survives the move). L is taken from the input positions.
        // The weight enters SQUARED, as UE FConstrainedSolver::SetConstraintWeights ("the soft constrained system
        // uses the square of the weight"). Normal equations ( L_m^T L_m + w^2 I ) x_m = w^2 t_m - L_m^T ( L_pin
        // p_pin - d ).
        Common::BoolResultStr LaplacianFit( const DynamicMesh3& mesh, SmoothWeightType weightType,
                                            const std::vector<uint8_t>& movable, bool keepLaplacian, double w,
                                            const std::vector<glm::dvec3>& targets, std::vector<glm::dvec3>& p )
        {
            int32_t                             count = 0;
            const std::vector<int32_t>          index = CompactMovable( movable, count );
            const std::vector<LaplacianRow>     rows  = BuildLaplacian( mesh, weightType, p );
            std::vector<SparseMatrixD::Triplet> triplets;
            std::array<std::vector<double>, 3>  rhs;
            for ( auto& r : rhs )
                r.assign( static_cast<size_t>( count ), 0.0 );
            for ( const int k : mesh.VertexIndicesItr() )
            {
                const LaplacianRow& row = rows[static_cast<size_t>( k )];
                glm::dvec3          fixedPart( 0.0 ); // L_k,pin p_pin - d_k
                for ( const auto& [j, b] : row )
                {
                    if ( index[static_cast<size_t>( j )] < 0 )
                        fixedPart += b * p[static_cast<size_t>( j )];
                    if ( keepLaplacian )
                        fixedPart -= b * p[static_cast<size_t>( j )];
                }
                for ( const auto& [i, a] : row )
                {
                    const int32_t ci = index[static_cast<size_t>( i )];
                    if ( ci < 0 )
                        continue;
                    for ( const auto& [j, b] : row )
                        if ( const int32_t cj = index[static_cast<size_t>( j )]; cj >= 0 )
                            triplets.push_back( { ci, cj, a * b } );
                    for ( int c = 0; c < 3; ++c )
                        rhs[static_cast<size_t>( c )][static_cast<size_t>( ci )] -= a * fixedPart[c];
                }
            }
            const double weightSquared = w * w;
            for ( size_t vid = 0; vid < index.size(); ++vid )
                if ( const int32_t ci = index[vid]; ci >= 0 )
                {
                    triplets.push_back( { ci, ci, weightSquared } );
                    for ( int c = 0; c < 3; ++c )
                        rhs[static_cast<size_t>( c )][static_cast<size_t>( ci )] +=
                             weightSquared * targets[vid][c];
                }
            if ( !SolveInto( SparseMatrixD( count, count, std::move( triplets ) ), index, count, rhs, p ) )
                return Common::MakeError<bool>( "the Laplacian system is not positive definite" );
            return Common::MakeSuccess( true );
        }

        // Vertex normals of the input, before any move (UE GetInitialVtxNormals / BaseNormals).
        std::vector<glm::dvec3> BaseNormals( const DynamicMesh3& mesh )
        {
            std::vector<glm::dvec3> normals( static_cast<size_t>( mesh.MaxVertexID() ), glm::dvec3( 0.0 ) );
            for ( const int vid : mesh.VertexIndicesItr() )
                normals[static_cast<size_t>( vid )] = MeshNormals::ComputeVertexNormal( mesh, vid );
            return normals;
        }

        // UE FCotanSmoothingOp::Smooth (ComputeSmoothing_BiHarmonic): fairing towards targets p + VolumeCorrection n
        // with weight 1 / SmoothPower, SmoothPower = Smoothness^2 * VertexCount / 50 (UE USmoothMeshTool).
        Common::BoolResultStr ImplicitSmooth( const DynamicMesh3& mesh, const SmoothSettings& settings,
                                              const std::vector<uint8_t>& movable, std::vector<glm::dvec3>& p )
        {
            const double smoothness = static_cast<double>( settings.Smoothness );
            const double s          = smoothness * smoothness / 50.0 * static_cast<double>( mesh.VertexCount() );
            if ( s < 0.0001 )
                return Common::MakeSuccess( true ); // UE: SmoothPower < 0.0001 leaves the positions unchanged
            std::vector<glm::dvec3> targets = p;
            if ( settings.VolumeCorrection != 0.0f )
            {
                const std::vector<glm::dvec3> normals = BaseNormals( mesh );
                for ( const int vid : mesh.VertexIndicesItr() )
                    targets[static_cast<size_t>( vid )] +=
                         static_cast<double>( settings.VolumeCorrection ) * normals[static_cast<size_t>( vid )];
            }
            if ( auto fit = LaplacianFit( mesh, settings.WeightType, movable, false, 1.0 / s, targets, p );
                 !fit.IsSuccess() )
                return Common::MakeError<bool>( "Smooth (Implicit): " + fit.GetError() );
            return Common::MakeSuccess( true );
        }

        // UE FLaplacianOffsetMeshOp::Offset: keep the input's Laplacian vectors, softly pull every vertex to
        // p + Distance n with weight max( Softness, 0.001 ) / 100, Softness = Smoothness^2 * VertexCount / 50.
        Common::BoolResultStr ImplicitOffset( const DynamicMesh3& mesh, const OffsetSettings& settings,
                                              const std::vector<uint8_t>& movable, std::vector<glm::dvec3>& p )
        {
            const double smoothness = static_cast<double>( settings.Smoothness );
            const double softness   = smoothness * smoothness / 50.0 * static_cast<double>( mesh.VertexCount() );
            const double w          = std::max( softness, 0.001 ) / 100.0;
            const std::vector<glm::dvec3> normals = BaseNormals( mesh );
            std::vector<glm::dvec3>       targets = p;
            for ( const int vid : mesh.VertexIndicesItr() )
                targets[static_cast<size_t>( vid )] +=
                     static_cast<double>( settings.Distance ) * normals[static_cast<size_t>( vid )];
            if ( auto fit = LaplacianFit( mesh, settings.WeightType, movable, true, w, targets, p ); !fit.IsSuccess() )
                return Common::MakeError<bool>( "Offset (Implicit): " + fit.GetError() );
            return Common::MakeSuccess( true );
        }

        // UE ComputeSmoothing_Diffusion: Steps backward-Euler heat steps ( I + t L_mm ) x' = x - t L_m,pin p_pin,
        // t = SmoothingPerStep, L taken from the input positions and factored once.
        Common::BoolResultStr DiffusionSmooth( const DynamicMesh3& mesh, const SmoothSettings& settings,
                                               const std::vector<uint8_t>& movable, std::vector<glm::dvec3>& p )
        {
            const double t = static_cast<double>( settings.SmoothingPerStep );
            if ( t <= 0.0 )
                return Common::MakeSuccess( true );
            int32_t                             count = 0;
            const std::vector<int32_t>          index = CompactMovable( movable, count );
            const std::vector<LaplacianRow>     rows  = BuildLaplacian( mesh, settings.WeightType, p );
            std::vector<SparseMatrixD::Triplet> triplets;
            std::vector<glm::dvec3>             pinnedTerm( index.size(), glm::dvec3( 0.0 ) ); // t L_m,pin p_pin
            for ( size_t vid = 0; vid < index.size(); ++vid )
            {
                const int32_t ci = index[vid];
                if ( ci < 0 )
                    continue;
                triplets.push_back( { ci, ci, 1.0 } );
                for ( const auto& [j, w] : rows[vid] )
                    if ( const int32_t cj = index[static_cast<size_t>( j )]; cj >= 0 )
                        triplets.push_back( { ci, cj, t * w } );
                    else
                        pinnedTerm[vid] += t * w * p[static_cast<size_t>( j )];
            }
            const SparseMatrixD a( count, count, std::move( triplets ) );
            SparseLDLT          solver;
            if ( !solver.Factorize( a ) )
                return Common::MakeError<bool>( "Smooth (Diffusion): the heat system is not positive definite" );
            std::array<std::vector<double>, 3> rhs;
            for ( auto& r : rhs )
                r.assign( static_cast<size_t>( count ), 0.0 );
            for ( int32_t step = 0; step < settings.Steps; ++step )
            {
                for ( size_t vid = 0; vid < index.size(); ++vid )
                    if ( const int32_t ci = index[vid]; ci >= 0 )
                        for ( int c = 0; c < 3; ++c )
                            rhs[static_cast<size_t>( c )][static_cast<size_t>( ci )] =
                                 p[vid][c] - pinnedTerm[vid][c];
                SolveInto( a, index, count, rhs, p, &solver );
            }
            return Common::MakeSuccess( true );
        }
        // UE FBendMeshOp on one point of its y-up copy of the gizmo frame (y = the bend axis): the slab
        // [LowerBound, UpperBound] rolls onto an arc of BendDegrees about x, and what lies beyond the slab is
        // carried along the arc's end tangent. LockBottom pins the slab's bottom instead of the frame origin.
        double BendAngle( const WarpSettings& settings )
        {
            return -static_cast<double>( settings.BendDegrees ) * 3.14159265358979323846 / 180.0;
        }

        glm::dvec3 BendYUp( const WarpSettings& settings, glm::dvec3 g )
        {
            const double totalAngle = BendAngle( settings );
            const double yMin = settings.LowerBound, yMax = settings.UpperBound, bentLength = yMax - yMin;
            const bool   sharp = bentLength == 0.0;
            const double y0    = settings.LockBottom ? yMin : 0.0;
            const double k     = sharp ? 1.0 : totalAngle / bentLength;
            const double ik    = 1.0 / k; // the arc's radius: the rotation centre's z
            const double yHat  = std::clamp( g.y, yMin, yMax );
            const double theta = sharp && g.y > y0 ? totalAngle : k * ( yHat - y0 );
            const double s0 = std::sin( theta ), c0 = std::cos( theta );
            const double zRel = g.z - ik;
            double       y    = -s0 * zRel + y0;
            double       z    = c0 * zRel + ik;
            if ( g.y > yMax || g.y < yMin )
            {
                const double yDiff = g.y - ( g.y > yMax ? yMax : yMin );
                y += c0 * yDiff;
                z += s0 * yDiff;
            }
            return { g.x, y, z };
        }
    } // namespace

    const char* ToString( SmoothType type )
    {
        switch ( type )
        {
            case SmoothType::Iterative:
                return "Iterative";
            case SmoothType::Implicit:
                return "Implicit";
            case SmoothType::Diffusion:
                return "Diffusion";
        }
        return "Unknown";
    }

    const char* ToString( OffsetType type )
    {
        switch ( type )
        {
            case OffsetType::Iterative:
                return "Iterative";
            case OffsetType::Implicit:
                return "Implicit";
        }
        return "Unknown";
    }

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

        if ( !( settings.Smoothness >= 0.0f && settings.Smoothness <= 1.0f ) )
            return Common::MakeError<DeformOutcome>(
                 fmt::format( "Smooth: Smoothness is {}, 0..1 expected", settings.Smoothness ) );
        if ( !( settings.VolumeCorrection >= 0.0f && settings.VolumeCorrection <= 10.0f ) )
            return Common::MakeError<DeformOutcome>(
                 fmt::format( "Smooth: VolumeCorrection is {}, 0..10 expected", settings.VolumeCorrection ) );

        DynamicMesh3               mesh  = input;
        int32_t                    moved = 0, pinned = 0;
        const std::vector<uint8_t> movable =
             MovableVertices( mesh, settings.SmoothBoundary, settings.PinUVSeams, moved, pinned );
        std::vector<glm::dvec3> p = Positions( mesh );
        std::string             report;
        switch ( settings.Type )
        {
            case SmoothType::Iterative:
            {
                std::vector<glm::dvec3> next = p;
                for ( int32_t step = 0; step < settings.Steps; ++step )
                {
                    SmoothRound( mesh, settings.WeightType, settings.SmoothingPerStep, movable, p, next );
                    p.swap( next );
                }
                report = fmt::format( "Iterative {} smoothing, {} steps at {:.2f}",
                                      ToString( settings.WeightType ), settings.Steps, settings.SmoothingPerStep );
                break;
            }
            case SmoothType::Implicit:
                if ( auto solved = ImplicitSmooth( mesh, settings, movable, p ); !solved.IsSuccess() )
                    return Common::MakeError<DeformOutcome>( solved.GetError() );
                report = fmt::format( "Implicit {} smoothing, smoothness {:.2f}, volume correction {:.2f} cm",
                                      ToString( settings.WeightType ), settings.Smoothness,
                                      settings.VolumeCorrection );
                break;
            case SmoothType::Diffusion:
                if ( auto solved = DiffusionSmooth( mesh, settings, movable, p ); !solved.IsSuccess() )
                    return Common::MakeError<DeformOutcome>( solved.GetError() );
                report = fmt::format( "Diffusion {} smoothing, {} steps at {:.2f}",
                                      ToString( settings.WeightType ), settings.Steps, settings.SmoothingPerStep );
                break;
        }
        WritePositions( mesh, p );
        return Finish( std::move( mesh ), "Smooth", moved, pinned, std::move( report ) );
    }

    Common::ResultStr<DeformOutcome> Offset( const DynamicMesh3& input, const OffsetSettings& settings )
    {
        if ( auto check = CheckCommon( input, "Offset", settings.Steps, settings.SmoothingPerStep );
             !check.IsSuccess() )
            return Common::MakeError<DeformOutcome>( check.GetError() );
        if ( !std::isfinite( settings.Distance ) )
            return Common::MakeError<DeformOutcome>( "Offset: Distance is not a finite number" );

        if ( !( settings.Smoothness >= 0.0f && settings.Smoothness <= 100.0f ) )
            return Common::MakeError<DeformOutcome>(
                 fmt::format( "Offset: Smoothness is {}, 0..100 expected", settings.Smoothness ) );

        DynamicMesh3 mesh  = input;
        int32_t      moved = 0, pinned = 0;
        // UE sets bFixedBoundary for the Implicit operator: its constrained solver only solves interior vertices.
        const bool                 moveBoundary = settings.Type == OffsetType::Iterative && settings.OffsetBoundary;
        const std::vector<uint8_t> movable =
             MovableVertices( mesh, moveBoundary, settings.PinUVSeams, moved, pinned );
        if ( settings.Type == OffsetType::Implicit )
        {
            std::vector<glm::dvec3> p = Positions( mesh );
            if ( auto solved = ImplicitOffset( mesh, settings, movable, p ); !solved.IsSuccess() )
                return Common::MakeError<DeformOutcome>( solved.GetError() );
            WritePositions( mesh, p );
            return Finish( std::move( mesh ), "Offset", moved, pinned,
                           fmt::format( "Implicit offset {:.2f} cm, {} weights, smoothness {:.2f}", settings.Distance,
                                        ToString( settings.WeightType ), settings.Smoothness ) );
        }
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
    const char* ToString( WarpOperation operation )
    {
        switch ( operation )
        {
            case WarpOperation::Bend:
                return "Bend";
            case WarpOperation::Twist:
                return "Twist";
            case WarpOperation::Flare:
                return "Flare";
        }
        return "?";
    }

    const char* ToString( FlareProfile profile )
    {
        switch ( profile )
        {
            case FlareProfile::SinMode:
                return "Sin Mode";
            case FlareProfile::SinSquared:
                return "Sin Squared";
            case FlareProfile::Triangle:
                return "Triangle";
        }
        return "?";
    }

    Common::ResultStr<DeformOutcome> Warp( const DynamicMesh3& input, const WarpSettings& settings,
                                           const glm::dmat4& objectToGizmo )
    {
        if ( input.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Warp: the mesh has no triangles" );
        if ( !( settings.UpperBound >= 0.0f ) || !( settings.LowerBound <= 0.0f ) ||
             !std::isfinite( settings.UpperBound ) || !std::isfinite( settings.LowerBound ) )
            return Common::MakeError<DeformOutcome>(
                 fmt::format( "Warp: bounds are {} / {} cm; Upper >= 0 and Lower <= 0 expected",
                              settings.UpperBound, settings.LowerBound ) );
        const float degrees =
             settings.Operation == WarpOperation::Bend ? settings.BendDegrees : settings.TwistDegrees;
        if ( settings.Operation != WarpOperation::Flare && !( std::abs( degrees ) <= 3600.0f ) )
            return Common::MakeError<DeformOutcome>( fmt::format( "Warp: {} degrees is {}, -3600..3600 expected",
                                                                  ToString( settings.Operation ), degrees ) );
        const double flareY = settings.FlarePercentY / 100.0;
        const double flareX =
             ( settings.LockXAndYFlaring ? settings.FlarePercentY : settings.FlarePercentX ) / 100.0;
        if ( settings.Operation == WarpOperation::Flare &&
             !( flareX >= -10.0 && flareX <= 20.0 && flareY >= -10.0 && flareY <= 20.0 ) )
            return Common::MakeError<DeformOutcome>( "Warp: Flare percent outside -1000..2000" );
        const double zMin = settings.LowerBound, zMax = settings.UpperBound;
        if ( settings.Operation != WarpOperation::Bend && zMax - zMin <= 0.0 )
            return Common::MakeError<DeformOutcome>( fmt::format(
                 "Warp: {} needs Upper > Lower (both bounds are 0)", ToString( settings.Operation ) ) );

        // UE FBendMeshOp works in a y-up copy of the gizmo frame: (x, y, z) -> (x, z, -y).
        glm::dmat4 toYUp( 0.0 );
        toYUp[0][0]           = 1.0;  // column 0 = image of x
        toYUp[1][2]           = -1.0; // y -> -z
        toYUp[2][1]           = 1.0;  // z -> y
        toYUp[3][3]           = 1.0;
        const glm::dmat4 toOp = settings.Operation == WarpOperation::Bend ? toYUp * objectToGizmo : objectToGizmo;
        if ( std::abs( glm::determinant( toOp ) ) < 1e-4 )
            return Common::MakeError<DeformOutcome>(
                 "Warp: the gizmo frame is singular for this mesh's transform" );
        const glm::dmat4 fromOp = glm::inverse( toOp );

        constexpr double        kPi  = 3.14159265358979323846;
        DynamicMesh3            mesh = input;
        std::vector<glm::dvec3> p    = Positions( mesh );
        std::string             report;
        if ( settings.Operation == WarpOperation::Bend )
        {
            if ( std::abs( BendAngle( settings ) ) >= 1e-8 ) // UE: no bend asked, the mesh is returned as is
            {
                for ( const int vid : mesh.VertexIndicesItr() )
                {
                    const glm::dvec4 g = toOp * glm::dvec4( p[static_cast<size_t>( vid )], 1.0 );
                    p[static_cast<size_t>( vid )] =
                         glm::dvec3( fromOp * glm::dvec4( BendYUp( settings, glm::dvec3( g ) ), 1.0 ) );
                }
            }
            report = fmt::format( "Bend {:.1f} deg over [{:.1f}, {:.1f}] cm{}", settings.BendDegrees, zMin, zMax,
                                  settings.LockBottom ? ", bottom locked" : "" );
        }
        else
        {
            const double theta = static_cast<double>( settings.TwistDegrees ) * kPi / 180.0;
            for ( const int vid : mesh.VertexIndicesItr() )
            {
                glm::dvec4   g = toOp * glm::dvec4( p[static_cast<size_t>( vid )], 1.0 );
                const double t = std::clamp( ( g.z - zMin ) / ( zMax - zMin ), 0.0, 1.0 );
                if ( settings.Operation == WarpOperation::Twist )
                {
                    const double angle = theta * ( t - ( settings.LockBottom ? 0.0 : 0.5 ) );
                    const double c0 = std::cos( angle ), s0 = std::sin( angle );
                    const double x = c0 * g.x - s0 * g.y;
                    const double y = s0 * g.x + c0 * g.y;
                    g.x            = x;
                    g.y            = y;
                }
                else
                {
                    double profile = 0.0;
                    switch ( settings.Profile )
                    {
                        case FlareProfile::Triangle:
                            profile = t < 0.5 ? 2.0 * t : 2.0 * ( 1.0 - t );
                            break;
                        case FlareProfile::SinMode:
                            profile = std::sin( kPi * t );
                            break;
                        case FlareProfile::SinSquared:
                            profile = std::sin( kPi * t ) * std::sin( kPi * t );
                            break;
                    }
                    g.x *= 1.0 + flareX * profile;
                    g.y *= 1.0 + flareY * profile;
                }
                p[static_cast<size_t>( vid )] = glm::dvec3( fromOp * g );
            }
            report =
                 settings.Operation == WarpOperation::Twist
                      ? fmt::format( "Twist {:.1f} deg over [{:.1f}, {:.1f}] cm{}", settings.TwistDegrees, zMin,
                                     zMax, settings.LockBottom ? ", bottom locked" : "" )
                      : fmt::format( "Flare {} X {:.0f}% Y {:.0f}% over [{:.1f}, {:.1f}] cm",
                                     ToString( settings.Profile ), flareX * 100.0, flareY * 100.0, zMin, zMax );
        }
        WritePositions( mesh, p );
        const int32_t moved = mesh.VertexCount();
        return Finish( std::move( mesh ), "Warp", moved, 0, std::move( report ) );
    }
    glm::dvec3 WarpBendGizmoPoint( const WarpSettings& settings, const glm::dvec3& gizmoPoint )
    {
        if ( std::abs( BendAngle( settings ) ) < 1e-8 )
            return gizmoPoint;
        // gizmo (x, y, z) -> UE's y-up (x, z, -y), bent, and back.
        const glm::dvec3 b = BendYUp( settings, { gizmoPoint.x, gizmoPoint.z, -gizmoPoint.y } );
        return { b.x, -b.z, b.y };
    }
    const char* ToString( LatticeInterpolation interpolation )
    {
        return interpolation == LatticeInterpolation::Cubic ? "Cubic" : "Linear";
    }

    std::vector<glm::dvec3> FFDLattice::RestPoints() const
    {
        std::vector<glm::dvec3> points( static_cast<size_t>( std::max( 0, PointCount() ) ) );
        for ( int32_t k = 0; k < Dims.z; ++k )
            for ( int32_t j = 0; j < Dims.y; ++j )
                for ( int32_t i = 0; i < Dims.x; ++i )
                    points[static_cast<size_t>( Index( i, j, k ) )] = Min + CellSize * glm::dvec3( i, j, k );
        return points;
    }

    Common::ResultStr<FFDLattice> MakeFFDLattice( const DynamicMesh3& mesh, const LatticeSettings& settings )
    {
        if ( mesh.VertexCount() == 0 )
            return Common::MakeError<FFDLattice>( "Lattice: the mesh has no vertices" );
        const glm::ivec3 dims( settings.XResolution, settings.YResolution, settings.ZResolution );
        for ( int c = 0; c < 3; ++c )
            if ( dims[c] < kLatticeMinResolution || dims[c] > kLatticeMaxResolution )
                return Common::MakeError<FFDLattice>(
                     fmt::format( "Lattice: resolution {} x {} x {}, {}..{} per axis expected", dims.x, dims.y,
                                  dims.z, kLatticeMinResolution, kLatticeMaxResolution ) );
        const auto       box  = mesh.GetBounds();
        glm::dvec3       lo   = glm::dvec3( box.Min ), hi = glm::dvec3( box.Max );
        const double     side = std::max( { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z } );
        // UE FFFDLattice: InitialBounds expanded by Padding * MaxDim on every side (the tool's padding 0.01).
        const double pad = 0.01 * ( side > 0.0 ? side : 1.0 );
        lo -= glm::dvec3( pad );
        hi += glm::dvec3( pad );
        FFDLattice lattice;
        lattice.Dims     = dims;
        lattice.Min      = lo;
        lattice.CellSize = ( hi - lo ) / glm::dvec3( dims - 1 );
        return Common::MakeSuccess( lattice );
    }

    namespace
    {
        // Catmull-Rom weights of the points at -1, 0, 1, 2 for t in [0, 1]: interpolating, and exact on a
        // linear field, so the rest lattice gives back every vertex unmoved.
        std::array<double, 4> CubicWeights( double t )
        {
            const double t2 = t * t, t3 = t2 * t;
            return { 0.5 * ( -t3 + 2.0 * t2 - t ), 0.5 * ( 3.0 * t3 - 5.0 * t2 + 2.0 ),
                     0.5 * ( -3.0 * t3 + 4.0 * t2 + t ), 0.5 * ( t3 - t2 ) };
        }
    } // namespace

    Common::ResultStr<DeformOutcome> LatticeDeform( const DynamicMesh3& input, const FFDLattice& lattice,
                                                    const std::vector<glm::dvec3>& points,
                                                    LatticeInterpolation           interpolation )
    {
        if ( input.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Lattice: the mesh has no triangles" );
        if ( lattice.PointCount() <= 0 || static_cast<int32_t>( points.size() ) != lattice.PointCount() )
            return Common::MakeError<DeformOutcome>(
                 fmt::format( "Lattice: {} control points for a {} x {} x {} lattice", points.size(),
                              lattice.Dims.x, lattice.Dims.y, lattice.Dims.z ) );
        const glm::ivec3 dims = lattice.Dims;
        // A control point by grid coordinates; outside the grid (the cubic stencil's ring) it is extrapolated
        // linearly from the edge, UE's extrapolated lattice positions, so the edge cells stay cubic.
        const auto at = [&]( int32_t i, int32_t j, int32_t k ) -> glm::dvec3
        {
            const glm::ivec3 q( i, j, k );
            glm::dvec3       offset( 0.0 );
            const glm::ivec3 c( std::clamp( i, 0, dims.x - 1 ), std::clamp( j, 0, dims.y - 1 ),
                                std::clamp( k, 0, dims.z - 1 ) );
            const glm::dvec3 base = points[static_cast<size_t>( lattice.Index( c.x, c.y, c.z ) )];
            for ( int a = 0; a < 3; ++a )
            {
                if ( q[a] == c[a] )
                    continue;
                // One step past the edge along a: edge + ( edge - its inner neighbour ).
                glm::ivec3 n = c;
                n[a]         = q[a] < 0 ? 1 : dims[a] - 2;
                offset += base - points[static_cast<size_t>( lattice.Index( n.x, n.y, n.z ) )];
            }
            return base + offset;
        };

        DynamicMesh3            mesh = input;
        std::vector<glm::dvec3> p    = Positions( mesh );
        int32_t                 moved = 0;
        for ( const int vid : mesh.VertexIndicesItr() )
        {
            glm::dvec3&      v    = p[static_cast<size_t>( vid )];
            const glm::dvec3 g    = ( v - lattice.Min ) / lattice.CellSize;
            glm::ivec3       cell;
            glm::dvec3       t;
            for ( int a = 0; a < 3; ++a )
            {
                cell[a] = std::clamp( static_cast<int32_t>( std::floor( g[a] ) ), 0, dims[a] - 2 );
                t[a]    = std::clamp( g[a] - cell[a], 0.0, 1.0 );
            }
            glm::dvec3 out( 0.0 );
            if ( interpolation == LatticeInterpolation::Linear )
            {
                for ( int c = 0; c < 8; ++c )
                {
                    const glm::ivec3 o( c & 1, ( c >> 1 ) & 1, ( c >> 2 ) & 1 );
                    const double     w = ( o.x ? t.x : 1.0 - t.x ) * ( o.y ? t.y : 1.0 - t.y ) *
                                     ( o.z ? t.z : 1.0 - t.z );
                    out += w * points[static_cast<size_t>(
                                    lattice.Index( cell.x + o.x, cell.y + o.y, cell.z + o.z ) )];
                }
            }
            else
            {
                const auto wx = CubicWeights( t.x ), wy = CubicWeights( t.y ), wz = CubicWeights( t.z );
                for ( int c = 0; c < 4; ++c )
                    for ( int b = 0; b < 4; ++b )
                        for ( int a = 0; a < 4; ++a )
                            out += wx[a] * wy[b] * wz[c] * at( cell.x - 1 + a, cell.y - 1 + b, cell.z - 1 + c );
            }
            if ( glm::length( out - v ) > 1e-9 )
                ++moved;
            v = out;
        }
        WritePositions( mesh, p );
        std::string report = fmt::format( "{} lattice {} x {} x {}", ToString( interpolation ), dims.x, dims.y,
                                          dims.z );
        return Finish( std::move( mesh ), "Lattice", moved, 0, std::move( report ) );
    }
} // namespace Desert::Geometry
