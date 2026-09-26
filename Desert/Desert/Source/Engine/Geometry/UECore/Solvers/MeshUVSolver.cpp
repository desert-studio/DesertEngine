// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Solvers/Internal/
// MeshUVSolver.cpp:14-72, 155-319, 430-601; PowerMethodSolver.cpp:9-185; Private/Solvers/
// PrecomputedMeshWeightData.cpp:8-100 (CotanTriangleData::Initialize) and Public/Solvers/
// LaplacianMatrixAssembly.h:855-975 (ConstructFullCotangentLaplacian, ECotangentAreaMode::NoArea), adapted: Eigen
// sparse types and SimplicialLDLT are replaced by SparseMatrixD/SparseLDLT (see the header); the Laplacian is
// assembled serially (UE batches rows over tasks, the entries are the same); the Voronoi areas CotanTriangleData
// also computes are dropped (NoArea never reads them).
#include "Engine/Geometry/UECore/Solvers/MeshUVSolver.hpp"

#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>

namespace Desert::Geometry
{
    namespace
    {
        using Triplet = SparseMatrixD::Triplet;

        // UE's CotanTriangleData::SmallTriangleArea (SMALL_NUMBER): the floor for triangle area.
        constexpr double SmallTriangleArea = 1.e-8;

        struct CotanTriangleData
        {
            std::array<double, 3> Cotangent{};    // at each corner
            std::array<int32_t, 3> OppositeEdge{}; // edge opposite each corner
            double                Area = 0.0;

            CotanTriangleData( const DynamicMesh3& Mesh, int32_t TriID )
            {
                const Index3i EdgeIds = Mesh.GetTriEdges( TriID );
                glm::dvec3    VertA{};
                glm::dvec3    VertB{};
                glm::dvec3    VertC{};
                Mesh.GetTriVertices( TriID, VertA, VertB, VertC );
                const glm::dvec3 EdgeAB( VertB - VertA );
                const glm::dvec3 EdgeAC( VertC - VertA );
                const glm::dvec3 EdgeBC( VertC - VertB );
                OppositeEdge           = { EdgeIds[1], EdgeIds[2], EdgeIds[0] };
                const double TwiceArea = glm::length( glm::cross( EdgeAB, EdgeAC ) );
                if ( TwiceArea > 2. * SmallTriangleArea )
                {
                    Cotangent[0] = glm::dot( EdgeAB, EdgeAC ) / TwiceArea;
                    Cotangent[1] = -glm::dot( EdgeAB, EdgeBC ) / TwiceArea;
                    Cotangent[2] = glm::dot( EdgeAC, EdgeBC ) / TwiceArea;
                    Area         = 0.5 * TwiceArea;
                }
                else
                {
                    // default small triangle - equilateral
                    const double CotOf60 = std::numbers::inv_sqrt3;
                    Cotangent            = { CotOf60, CotOf60, CotOf60 };
                    Area                 = SmallTriangleArea;
                }
            }

            [[nodiscard]] double GetOpposingCotangent( int32_t EdgeID ) const
            {
                for ( int32_t i = 0; i < 3; ++i )
                {
                    if ( OppositeEdge[i] == EdgeID )
                        return Cotangent[i];
                }
                return -1.; // UE's value for an edge not in the triangle
            }
        };

        enum class CotangentWeightMode
        {
            ClampedMagnitude,
            TriangleArea
        };

        // ConstructFullCotangentLaplacian with ECotangentAreaMode::NoArea, written into both diagonal blocks
        // (UE's FEigenDNCPSparseMatrixAssembler): [L 0; 0 L] for the 2V unknowns (U block, then V block).
        void AppendDNCPCotangentLaplacian( const DynamicMesh3& Mesh, const std::vector<int32_t>& ToVertex,
                                           const std::vector<int32_t>& ToIndex, CotangentWeightMode WeightMode,
                                           std::vector<Triplet>& Triplets )
        {
            const int32_t                  NumVerts = static_cast<int32_t>( ToVertex.size() );
            std::vector<CotanTriangleData> TriData;
            std::vector<int32_t>           ToTriIdx( Mesh.MaxTriangleID(), DynamicMesh3::InvalidID );
            for ( const int32_t tid : Mesh.TriangleIndicesItr() )
            {
                ToTriIdx[tid] = static_cast<int32_t>( TriData.size() );
                TriData.emplace_back( Mesh, tid );
            }
            for ( int32_t i = 0; i < NumVerts; ++i )
            {
                const int32_t IVertId    = ToVertex[i];
                const double WeightArea = 1.0;
                double       WeightII   = 0.;
                for ( const int32_t EdgeId : Mesh.VtxEdgesItr( IVertId ) )
                {
                    const Index2i             EdgeV      = Mesh.GetEdgeV( EdgeId );
                    const Index2i             EdgeT      = Mesh.GetEdgeT( EdgeId );
                    const int32_t             JVertId    = EdgeV.A == IVertId ? EdgeV.B : EdgeV.A;
                    const CotanTriangleData&  Tri0Data   = TriData[ToTriIdx[EdgeT.A]];
                    double                    CotanAlpha = Tri0Data.GetOpposingCotangent( EdgeId );
                    double                    CotanBeta  = EdgeT.B != DynamicMesh3::InvalidID
                                                                ? TriData[ToTriIdx[EdgeT.B]].GetOpposingCotangent( EdgeId )
                                                                : 0.0;
                    if ( WeightMode == CotangentWeightMode::TriangleArea )
                    {
                        CotanAlpha /= Tri0Data.Area;
                        if ( EdgeT.B != DynamicMesh3::InvalidID )
                            CotanBeta /= TriData[ToTriIdx[EdgeT.B]].Area;
                    }
                    double WeightIJ = CotanAlpha + CotanBeta;
                    if ( WeightMode == CotangentWeightMode::ClampedMagnitude )
                        WeightIJ = std::clamp( WeightIJ, -1.e5 * WeightArea, 1.e5 * WeightArea );
                    WeightII += WeightIJ;
                    const int32_t j = ToIndex[JVertId];
                    Triplets.push_back( { i, j, WeightIJ / WeightArea } );
                    Triplets.push_back( { NumVerts + i, NumVerts + j, WeightIJ / WeightArea } );
                }
                Triplets.push_back( { i, i, -WeightII / WeightArea } );
                Triplets.push_back( { NumVerts + i, NumVerts + i, -WeightII / WeightArea } );
            }
        }

        // One directed edge [EdgeVertex1, EdgeVertex2] of the signed-area quadratic form, scaled by Value.
        void AppendAreaEdge( int32_t U1, int32_t U2, int32_t NumVert, double Value,
                             std::vector<Triplet>& Triplets )
        {
            const int32_t V1 = U1 + NumVert;
            const int32_t V2 = U2 + NumVert;
            Triplets.push_back( { U1, V2, Value } );
            Triplets.push_back( { V1, U2, -Value } );
            // Make it symmetric
            Triplets.push_back( { V2, U1, Value } );
            Triplets.push_back( { U2, V1, -Value } );
        }
    } // namespace

    SparseMatrixD::SparseMatrixD( int32_t RowsIn, int32_t ColsIn, std::vector<Triplet> Triplets )
         : m_NumRows( RowsIn ), m_NumCols( ColsIn )
    {
        std::sort( Triplets.begin(), Triplets.end(), []( const Triplet& L, const Triplet& R )
                   { return L.Row != R.Row ? L.Row < R.Row : L.Col < R.Col; } );
        m_RowStart.assign( m_NumRows + 1, 0 );
        for ( size_t k = 0; k < Triplets.size(); )
        {
            const Triplet&  T   = Triplets[k];
            double          Sum = 0.0;
            for ( ; k < Triplets.size() && Triplets[k].Row == T.Row && Triplets[k].Col == T.Col; ++k )
                Sum += Triplets[k].Value;
            m_ColIndex.push_back( T.Col );
            m_Values.push_back( Sum );
            ++m_RowStart[T.Row + 1];
        }
        for ( int32_t r = 0; r < m_NumRows; ++r )
            m_RowStart[r + 1] += m_RowStart[r];
    }

    void SparseMatrixD::Multiply( const std::vector<double>& In, std::vector<double>& Out ) const
    {
        Out.assign( m_NumRows, 0.0 );
        for ( int32_t r = 0; r < m_NumRows; ++r )
        {
            double Sum = 0.0;
            for ( int32_t k = m_RowStart[r]; k < m_RowStart[r + 1]; ++k )
                Sum += m_Values[k] * In[m_ColIndex[k]];
            Out[r] = Sum;
        }
    }

    bool SparseLDLT::Factorize( const SparseMatrixD& Matrix )
    {
        const int32_t N = static_cast<int32_t>( Matrix.m_RowStart.size() ) - 1;
        // reverse Cuthill-McKee: breadth-first from a minimum-degree vertex, neighbours by increasing degree
        std::vector<int32_t> Degree( N, 0 );
        for ( int32_t r = 0; r < N; ++r )
            Degree[r] = Matrix.m_RowStart[r + 1] - Matrix.m_RowStart[r];
        std::vector<int32_t> Order;
        Order.reserve( N );
        std::vector<char>  Visited( N, 0 );
        std::vector<int32_t> Seeds( N );
        for ( int32_t r = 0; r < N; ++r )
            Seeds[r] = r;
        std::stable_sort( Seeds.begin(), Seeds.end(),
                          [&Degree]( int32_t L, int32_t R ) { return Degree[L] < Degree[R]; } );
        std::vector<int32_t> Neighbours;
        for ( const int32_t Seed : Seeds )
        {
            if ( Visited[Seed] != 0 )
                continue;
            Visited[Seed] = 1;
            Order.push_back( Seed );
            for ( size_t Head = Order.size() - 1; Head < Order.size(); ++Head )
            {
                const int32_t r = Order[Head];
                Neighbours.clear();
                for ( int32_t k = Matrix.m_RowStart[r]; k < Matrix.m_RowStart[r + 1]; ++k )
                {
                    if ( Visited[Matrix.m_ColIndex[k]] == 0 )
                    {
                        Visited[Matrix.m_ColIndex[k]] = 1;
                        Neighbours.push_back( Matrix.m_ColIndex[k] );
                    }
                }
                std::stable_sort( Neighbours.begin(), Neighbours.end(),
                                  [&Degree]( int32_t L, int32_t R ) { return Degree[L] < Degree[R]; } );
                Order.insert( Order.end(), Neighbours.begin(), Neighbours.end() );
            }
        }
        m_Perm.assign( Order.rbegin(), Order.rend() );
        std::vector<int32_t> InvPerm( N );
        for ( int32_t i = 0; i < N; ++i )
            InvPerm[m_Perm[i]] = i;

        // envelope of the permuted lower triangle
        m_FirstCol.assign( N, 0 );
        m_RowOffset.assign( N + 1, 0 );
        for ( int32_t i = 0; i < N; ++i )
        {
            const int32_t r = m_Perm[i];
            m_FirstCol[i]   = i;
            for ( int32_t k = Matrix.m_RowStart[r]; k < Matrix.m_RowStart[r + 1]; ++k )
                m_FirstCol[i] = std::min( m_FirstCol[i], InvPerm[Matrix.m_ColIndex[k]] );
            m_RowOffset[i + 1] = m_RowOffset[i] + ( i - m_FirstCol[i] );
        }
        m_Lower.assign( m_RowOffset[N], 0.0 );
        m_Diagonal.assign( N, 0.0 );
        double MaxDiagonal = 0.0;
        for ( int32_t i = 0; i < N; ++i )
        {
            const int32_t r = m_Perm[i];
            for ( int32_t k = Matrix.m_RowStart[r]; k < Matrix.m_RowStart[r + 1]; ++k )
            {
                const int32_t j = InvPerm[Matrix.m_ColIndex[k]];
                if ( j == i )
                    m_Diagonal[i] += Matrix.m_Values[k];
                else if ( j < i )
                    m_Lower[m_RowOffset[i] + j - m_FirstCol[i]] += Matrix.m_Values[k];
            }
            MaxDiagonal = std::max( MaxDiagonal, std::abs( m_Diagonal[i] ) );
        }

        // row-by-row (Crout) LDL^T inside the envelope
        const double PivotFloor = 1.e-15 * MaxDiagonal;
        for ( int32_t i = 0; i < N; ++i )
        {
            double* RowI = &m_Lower[m_RowOffset[i]] - m_FirstCol[i];
            for ( int32_t j = m_FirstCol[i]; j < i; ++j )
            {
                const double* RowJ = &m_Lower[m_RowOffset[j]] - m_FirstCol[j];
                double        Sum  = RowI[j];
                for ( int32_t k = std::max( m_FirstCol[i], m_FirstCol[j] ); k < j; ++k )
                    Sum -= RowI[k] * m_Diagonal[k] * RowJ[k];
                RowI[j] = Sum / m_Diagonal[j];
            }
            for ( int32_t k = m_FirstCol[i]; k < i; ++k )
                m_Diagonal[i] -= RowI[k] * RowI[k] * m_Diagonal[k];
            if ( !std::isfinite( m_Diagonal[i] ) || m_Diagonal[i] <= PivotFloor )
            {
                m_Perm.clear();
                return false;
            }
        }
        return true;
    }

    void SparseLDLT::Solve( const std::vector<double>& B, std::vector<double>& X ) const
    {
        const auto          N = static_cast<int32_t>( m_Perm.size() );
        std::vector<double> Y( N );
        for ( int32_t i = 0; i < N; ++i )
        {
            const double* RowI = &m_Lower[m_RowOffset[i]] - m_FirstCol[i];
            double        Sum  = B[m_Perm[i]];
            for ( int32_t k = m_FirstCol[i]; k < i; ++k )
                Sum -= RowI[k] * Y[k];
            Y[i] = Sum;
        }
        for ( int32_t i = 0; i < N; ++i )
            Y[i] /= m_Diagonal[i];
        for ( int32_t i = N - 1; i >= 0; --i )
        {
            const double* RowI = &m_Lower[m_RowOffset[i]] - m_FirstCol[i];
            for ( int32_t k = m_FirstCol[i]; k < i; ++k )
                Y[k] -= RowI[k] * Y[i];
        }
        X.assign( N, 0.0 );
        for ( int32_t i = 0; i < N; ++i )
            X[m_Perm[i]] = Y[i];
    }

    SpectralConformalMeshUVSolver::SpectralConformalMeshUVSolver( const DynamicMesh3& MeshIn,
                                                                  bool                bPreserveIrregularityIn )
         : m_Mesh( MeshIn ), m_bPreserveIrregularity( bPreserveIrregularityIn )
    {
        // UE's FVertexLinearization(Mesh, false): compact indices in vertex-ID order
        m_ToIndex.assign( m_Mesh.MaxVertexID(), DynamicMesh3::InvalidID );
        for ( const int32_t vid : m_Mesh.VertexIndicesItr() )
        {
            m_ToIndex[vid] = static_cast<int32_t>( m_ToVertex.size() );
            m_ToVertex.push_back( vid );
        }
    }

    void SpectralConformalMeshUVSolver::AddBoundaryVertex( int32_t VertexID )
    {
        if ( VertexID < 0 || VertexID >= static_cast<int32_t>( m_ToIndex.size() ) ||
             m_ToIndex[VertexID] == DynamicMesh3::InvalidID )
            return;
        const int32_t Index = m_ToIndex[VertexID];
        if ( std::find( m_Boundary.begin(), m_Boundary.end(), Index ) == m_Boundary.end() )
            m_Boundary.push_back( Index );
    }

    bool SpectralConformalMeshUVSolver::SolveUVs( std::vector<glm::dvec2>& OutUVs )
    {
        const int32_t NumVerts = static_cast<int32_t>( m_ToVertex.size() );
        const int32_t N        = 2 * NumVerts;
        OutUVs.assign( m_Mesh.MaxVertexID(), glm::dvec2( 0 ) );
        if ( NumVerts == 0 || m_Boundary.empty() )
            return false;

        // conformal energy E_c = -Scale * L_cot - A (ConstructConformalEnergyMatrix), plus Eps*I to make it PSD
        std::vector<Triplet> Triplets;
        AppendDNCPCotangentLaplacian( m_Mesh, m_ToVertex, m_ToIndex,
                                      m_bPreserveIrregularity ? CotangentWeightMode::TriangleArea
                                                              : CotangentWeightMode::ClampedMagnitude,
                                      Triplets );
        double Scale = 1.0;
        if ( m_bPreserveIrregularity )
        {
            // ConstructWeightedVectorAreaMatrix: every triangle edge, weighted by the inverse of the area
            // normalized to the largest triangle (which keeps the numerics independent of the mesh scale)
            Scale = -1.0;
            for ( const int32_t tid : m_Mesh.TriangleIndicesItr() )
                Scale = std::max( Scale, m_Mesh.GetTriArea( tid ) );
            for ( Triplet& T : Triplets )
                T.Value *= -Scale;
            for ( const int32_t tid : m_Mesh.TriangleIndicesItr() )
            {
                const Index3i TriVert = m_Mesh.GetTriangle( tid );
                const double  Value   = 1.0 / ( std::max( m_Mesh.GetTriArea( tid ), SmallTriangleArea ) / Scale );
                // the edge is reversed to handle UE's mesh orientation, else the area term flips sign
                for ( int32_t k = 0; k < 3; ++k )
                    AppendAreaEdge( m_ToIndex[TriVert[( k + 1 ) % 3]], m_ToIndex[TriVert[k]], NumVerts, -Value,
                                    Triplets );
            }
        }
        else
        {
            for ( Triplet& T : Triplets )
                T.Value = -T.Value;
            // ConstructVectorAreaMatrix: only the boundary loops carry the signed area
            const MeshBoundaryLoops Loops( &m_Mesh, true );
            for ( const EdgeLoop& Loop : Loops.m_Loops )
            {
                const int32_t NumLoopVert = static_cast<int32_t>( Loop.Vertices.size() );
                for ( int32_t Idx = 0; Idx < NumLoopVert; ++Idx )
                    AppendAreaEdge( m_ToIndex[Loop.Vertices[( Idx + 1 ) % NumLoopVert]],
                                    m_ToIndex[Loop.Vertices[Idx]], NumVerts, -1.0, Triplets );
            }
        }
        static constexpr double Eps = 1e-8;
        for ( int32_t i = 0; i < N; ++i )
            Triplets.push_back( { i, i, Eps } );
        const SparseMatrixD SystemMatrix( N, N, std::move( Triplets ) );

        // B selects the boundary, E (2V x 2) is its normalized centroid: the iteration uses (B - E E^T) x
        // without forming the dense E E^T
        const double InvSqrtBndr =
             1.0 / std::sqrt( static_cast<double>( static_cast<int32_t>( m_Boundary.size() ) ) );
        const auto   ApplyB      = [&]( const std::vector<double>& In, std::vector<double>& Out )
        {
            double MeanU = 0.0;
            double MeanV = 0.0;
            for ( const int32_t b : m_Boundary )
            {
                MeanU += In[b] * InvSqrtBndr;
                MeanV += In[b + NumVerts] * InvSqrtBndr;
            }
            Out.assign( N, 0.0 );
            for ( const int32_t b : m_Boundary )
            {
                Out[b]            = In[b] - MeanU * InvSqrtBndr;
                Out[b + NumVerts] = In[b + NumVerts] - MeanV * InvSqrtBndr;
            }
        };

        SparseLDLT Solver;
        if ( !Solver.Factorize( SystemMatrix ) )
            return false;

        // inverse power iteration for the smallest generalized eigenpair (FPowerMethod::Solve, bComputeLargest
        // false): x <- A^-1 B x, normalized, until ||A x - lambda B x||_inf < Tolerance
        static constexpr double                Tolerance     = 1e-10;
        static constexpr int32_t               MaxIterations = 1000;
        std::mt19937_64                        Random( 0x5eed );
        std::uniform_real_distribution<double> Uniform( -1.0, 1.0 );
        std::vector<double>                    X( N );
        for ( double& x : X )
            x = Uniform( Random );
        std::vector<double> BX;
        std::vector<double> AX;
        for ( int32_t Iteration = 0; Iteration < MaxIterations; ++Iteration )
        {
            ApplyB( X, BX );
            Solver.Solve( BX, X );
            double SquaredMagnitude = 0.0;
            for ( const double x : X )
                SquaredMagnitude += x * x;
            if ( !std::isfinite( SquaredMagnitude ) || SquaredMagnitude < 1.e-8 )
                return false;
            const double InvLength = 1.0 / std::sqrt( SquaredMagnitude );
            for ( double& x : X )
                x *= InvLength;

            SystemMatrix.Multiply( X, AX );
            ApplyB( X, BX );
            double XAX = 0.0;
            double XBX = 0.0;
            for ( int32_t i = 0; i < N; ++i )
            {
                XAX += X[i] * AX[i];
                XBX += X[i] * BX[i];
            }
            const double Lambda = XAX / ( std::abs( XBX ) < 1.e-8 ? 1.0 : XBX );
            if ( !std::isfinite( Lambda ) )
                return false;
            double Residual = 0.0;
            for ( int32_t i = 0; i < N; ++i )
                Residual = std::max( Residual, std::abs( AX[i] - Lambda * BX[i] ) );
            if ( Residual < Tolerance )
                break;
            // UE lets a not-converged result through as usable (it only logs), and so does this port
        }
        for ( int32_t Idx = 0; Idx < NumVerts; ++Idx )
            OutUVs[m_ToVertex[Idx]] = glm::dvec2( X[Idx], X[Idx + NumVerts] );
        return true;
    }
} // namespace Desert::Geometry
