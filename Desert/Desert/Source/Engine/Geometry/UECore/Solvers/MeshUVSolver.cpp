// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Solvers/Internal/
// MeshUVSolver.cpp:14-72, 155-319, 430-601; PowerMethodSolver.cpp:9-185; Private/Solvers/
// PrecomputedMeshWeightData.cpp:8-100 (CotanTriangleData::Initialize) and Public/Solvers/
// LaplacianMatrixAssembly.h:855-975 (ConstructFullCotangentLaplacian, ECotangentAreaMode::NoArea), adapted: Eigen
// sparse types and SimplicialLDLT are replaced by FSparseMatrixD/FSparseLDLT (see the header); the Laplacian is
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
        using FTriplet = FSparseMatrixD::FTriplet;

        // UE's CotanTriangleData::SmallTriangleArea (SMALL_NUMBER): the floor for triangle area.
        constexpr double SmallTriangleArea = 1.e-8;

        struct FCotanTriangleData
        {
            std::array<double, 3> Cotangent{};    // at each corner
            std::array<int32_t, 3> OppositeEdge{}; // edge opposite each corner
            double                Area = 0.0;

            FCotanTriangleData( const FDynamicMesh3& Mesh, int32_t TriID )
            {
                const FIndex3i EdgeIds = Mesh.GetTriEdges( TriID );
                FVector3d      VertA;
                FVector3d      VertB;
                FVector3d      VertC;
                Mesh.GetTriVertices( TriID, VertA, VertB, VertC );
                const FVector3d EdgeAB( VertB - VertA );
                const FVector3d EdgeAC( VertC - VertA );
                const FVector3d EdgeBC( VertC - VertB );
                OppositeEdge           = { EdgeIds[1], EdgeIds[2], EdgeIds[0] };
                const double TwiceArea = EdgeAB.Cross( EdgeAC ).Length();
                if ( TwiceArea > 2. * SmallTriangleArea )
                {
                    Cotangent[0] = EdgeAB.Dot( EdgeAC ) / TwiceArea;
                    Cotangent[1] = -EdgeAB.Dot( EdgeBC ) / TwiceArea;
                    Cotangent[2] = EdgeAC.Dot( EdgeBC ) / TwiceArea;
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

        enum class ECotangentWeightMode
        {
            ClampedMagnitude,
            TriangleArea
        };

        // ConstructFullCotangentLaplacian with ECotangentAreaMode::NoArea, written into both diagonal blocks
        // (UE's FEigenDNCPSparseMatrixAssembler): [L 0; 0 L] for the 2V unknowns (U block, then V block).
        void AppendDNCPCotangentLaplacian( const FDynamicMesh3& Mesh, const TArray<int32_t>& ToVertex,
                                           const TArray<int32_t>& ToIndex, ECotangentWeightMode WeightMode,
                                           std::vector<FTriplet>& Triplets )
        {
            const int32_t                   NumVerts = ToVertex.Num();
            std::vector<FCotanTriangleData> TriData;
            std::vector<int32_t>            ToTriIdx( Mesh.MaxTriangleID(), FDynamicMesh3::InvalidID );
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
                    const FIndex2i            EdgeV      = Mesh.GetEdgeV( EdgeId );
                    const FIndex2i            EdgeT      = Mesh.GetEdgeT( EdgeId );
                    const int32_t             JVertId    = EdgeV.A == IVertId ? EdgeV.B : EdgeV.A;
                    const FCotanTriangleData& Tri0Data   = TriData[ToTriIdx[EdgeT.A]];
                    double                    CotanAlpha = Tri0Data.GetOpposingCotangent( EdgeId );
                    double                    CotanBeta  = EdgeT.B != FDynamicMesh3::InvalidID
                                                                ? TriData[ToTriIdx[EdgeT.B]].GetOpposingCotangent( EdgeId )
                                                                : 0.0;
                    if ( WeightMode == ECotangentWeightMode::TriangleArea )
                    {
                        CotanAlpha /= Tri0Data.Area;
                        if ( EdgeT.B != FDynamicMesh3::InvalidID )
                            CotanBeta /= TriData[ToTriIdx[EdgeT.B]].Area;
                    }
                    double WeightIJ = CotanAlpha + CotanBeta;
                    if ( WeightMode == ECotangentWeightMode::ClampedMagnitude )
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
                             std::vector<FTriplet>& Triplets )
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

    FSparseMatrixD::FSparseMatrixD( int32_t RowsIn, int32_t ColsIn, std::vector<FTriplet> Triplets )
         : NumRows( RowsIn ), NumCols( ColsIn )
    {
        std::sort( Triplets.begin(), Triplets.end(), []( const FTriplet& L, const FTriplet& R )
                   { return L.Row != R.Row ? L.Row < R.Row : L.Col < R.Col; } );
        RowStart.assign( NumRows + 1, 0 );
        for ( size_t k = 0; k < Triplets.size(); )
        {
            const FTriplet& T   = Triplets[k];
            double          Sum = 0.0;
            for ( ; k < Triplets.size() && Triplets[k].Row == T.Row && Triplets[k].Col == T.Col; ++k )
                Sum += Triplets[k].Value;
            ColIndex.push_back( T.Col );
            Values.push_back( Sum );
            ++RowStart[T.Row + 1];
        }
        for ( int32_t r = 0; r < NumRows; ++r )
            RowStart[r + 1] += RowStart[r];
    }

    void FSparseMatrixD::Multiply( const std::vector<double>& In, std::vector<double>& Out ) const
    {
        Out.assign( NumRows, 0.0 );
        for ( int32_t r = 0; r < NumRows; ++r )
        {
            double Sum = 0.0;
            for ( int32_t k = RowStart[r]; k < RowStart[r + 1]; ++k )
                Sum += Values[k] * In[ColIndex[k]];
            Out[r] = Sum;
        }
    }

    bool FSparseLDLT::Factorize( const FSparseMatrixD& Matrix )
    {
        const int32_t N = static_cast<int32_t>( Matrix.RowStart.size() ) - 1;
        // reverse Cuthill-McKee: breadth-first from a minimum-degree vertex, neighbours by increasing degree
        std::vector<int32_t> Degree( N, 0 );
        for ( int32_t r = 0; r < N; ++r )
            Degree[r] = Matrix.RowStart[r + 1] - Matrix.RowStart[r];
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
                for ( int32_t k = Matrix.RowStart[r]; k < Matrix.RowStart[r + 1]; ++k )
                {
                    if ( Visited[Matrix.ColIndex[k]] == 0 )
                    {
                        Visited[Matrix.ColIndex[k]] = 1;
                        Neighbours.push_back( Matrix.ColIndex[k] );
                    }
                }
                std::stable_sort( Neighbours.begin(), Neighbours.end(),
                                  [&Degree]( int32_t L, int32_t R ) { return Degree[L] < Degree[R]; } );
                Order.insert( Order.end(), Neighbours.begin(), Neighbours.end() );
            }
        }
        Perm.assign( Order.rbegin(), Order.rend() );
        std::vector<int32_t> InvPerm( N );
        for ( int32_t i = 0; i < N; ++i )
            InvPerm[Perm[i]] = i;

        // envelope of the permuted lower triangle
        FirstCol.assign( N, 0 );
        RowOffset.assign( N + 1, 0 );
        for ( int32_t i = 0; i < N; ++i )
        {
            const int32_t r = Perm[i];
            FirstCol[i]   = i;
            for ( int32_t k = Matrix.RowStart[r]; k < Matrix.RowStart[r + 1]; ++k )
                FirstCol[i] = std::min( FirstCol[i], InvPerm[Matrix.ColIndex[k]] );
            RowOffset[i + 1] = RowOffset[i] + ( i - FirstCol[i] );
        }
        Lower.assign( RowOffset[N], 0.0 );
        Diagonal.assign( N, 0.0 );
        double MaxDiagonal = 0.0;
        for ( int32_t i = 0; i < N; ++i )
        {
            const int32_t r = Perm[i];
            for ( int32_t k = Matrix.RowStart[r]; k < Matrix.RowStart[r + 1]; ++k )
            {
                const int32_t j = InvPerm[Matrix.ColIndex[k]];
                if ( j == i )
                    Diagonal[i] += Matrix.Values[k];
                else if ( j < i )
                    Lower[RowOffset[i] + j - FirstCol[i]] += Matrix.Values[k];
            }
            MaxDiagonal = std::max( MaxDiagonal, std::abs( Diagonal[i] ) );
        }

        // row-by-row (Crout) LDL^T inside the envelope
        const double PivotFloor = 1.e-15 * MaxDiagonal;
        for ( int32_t i = 0; i < N; ++i )
        {
            double* RowI = &Lower[RowOffset[i]] - FirstCol[i];
            for ( int32_t j = FirstCol[i]; j < i; ++j )
            {
                const double* RowJ = &Lower[RowOffset[j]] - FirstCol[j];
                double        Sum  = RowI[j];
                for ( int32_t k = std::max( FirstCol[i], FirstCol[j] ); k < j; ++k )
                    Sum -= RowI[k] * Diagonal[k] * RowJ[k];
                RowI[j] = Sum / Diagonal[j];
            }
            for ( int32_t k = FirstCol[i]; k < i; ++k )
                Diagonal[i] -= RowI[k] * RowI[k] * Diagonal[k];
            if ( !std::isfinite( Diagonal[i] ) || Diagonal[i] <= PivotFloor )
            {
                Perm.clear();
                return false;
            }
        }
        return true;
    }

    void FSparseLDLT::Solve( const std::vector<double>& B, std::vector<double>& X ) const
    {
        const auto          N = static_cast<int32_t>( Perm.size() );
        std::vector<double> Y( N );
        for ( int32_t i = 0; i < N; ++i )
        {
            const double* RowI = &Lower[RowOffset[i]] - FirstCol[i];
            double        Sum  = B[Perm[i]];
            for ( int32_t k = FirstCol[i]; k < i; ++k )
                Sum -= RowI[k] * Y[k];
            Y[i] = Sum;
        }
        for ( int32_t i = 0; i < N; ++i )
            Y[i] /= Diagonal[i];
        for ( int32_t i = N - 1; i >= 0; --i )
        {
            const double* RowI = &Lower[RowOffset[i]] - FirstCol[i];
            for ( int32_t k = FirstCol[i]; k < i; ++k )
                Y[k] -= RowI[k] * Y[i];
        }
        X.assign( N, 0.0 );
        for ( int32_t i = 0; i < N; ++i )
            X[Perm[i]] = Y[i];
    }

    FSpectralConformalMeshUVSolver::FSpectralConformalMeshUVSolver( const FDynamicMesh3& MeshIn,
                                                                    bool                 bPreserveIrregularityIn )
         : Mesh( MeshIn ), bPreserveIrregularity( bPreserveIrregularityIn )
    {
        // UE's FVertexLinearization(Mesh, false): compact indices in vertex-ID order
        ToIndex.Init( FDynamicMesh3::InvalidID, Mesh.MaxVertexID() );
        for ( const int32_t vid : Mesh.VertexIndicesItr() )
        {
            ToIndex[vid] = ToVertex.Num();
            ToVertex.Add( vid );
        }
    }

    void FSpectralConformalMeshUVSolver::AddBoundaryVertex( int32_t VertexID )
    {
        if ( VertexID < 0 || VertexID >= ToIndex.Num() || ToIndex[VertexID] == FDynamicMesh3::InvalidID )
            return;
        const int32_t Index = ToIndex[VertexID];
        if ( std::find( Boundary.begin(), Boundary.end(), Index ) == Boundary.end() )
            Boundary.Add( Index );
    }

    bool FSpectralConformalMeshUVSolver::SolveUVs( TArray<FVector2d>& OutUVs )
    {
        const int32_t NumVerts = ToVertex.Num();
        const int32_t N        = 2 * NumVerts;
        OutUVs.Init( FVector2d::Zero(), Mesh.MaxVertexID() );
        if ( NumVerts == 0 || Boundary.Num() == 0 )
            return false;

        // conformal energy E_c = -Scale * L_cot - A (ConstructConformalEnergyMatrix), plus Eps*I to make it PSD
        std::vector<FTriplet> Triplets;
        AppendDNCPCotangentLaplacian( Mesh, ToVertex, ToIndex,
                                      bPreserveIrregularity ? ECotangentWeightMode::TriangleArea
                                                            : ECotangentWeightMode::ClampedMagnitude,
                                      Triplets );
        double Scale = 1.0;
        if ( bPreserveIrregularity )
        {
            // ConstructWeightedVectorAreaMatrix: every triangle edge, weighted by the inverse of the area
            // normalized to the largest triangle (which keeps the numerics independent of the mesh scale)
            Scale = -1.0;
            for ( const int32_t tid : Mesh.TriangleIndicesItr() )
                Scale = std::max( Scale, Mesh.GetTriArea( tid ) );
            for ( FTriplet& T : Triplets )
                T.Value *= -Scale;
            for ( const int32_t tid : Mesh.TriangleIndicesItr() )
            {
                const FIndex3i TriVert = Mesh.GetTriangle( tid );
                const double   Value   = 1.0 / ( std::max( Mesh.GetTriArea( tid ), SmallTriangleArea ) / Scale );
                // the edge is reversed to handle UE's mesh orientation, else the area term flips sign
                for ( int32_t k = 0; k < 3; ++k )
                    AppendAreaEdge( ToIndex[TriVert[( k + 1 ) % 3]], ToIndex[TriVert[k]], NumVerts, -Value,
                                    Triplets );
            }
        }
        else
        {
            for ( FTriplet& T : Triplets )
                T.Value = -T.Value;
            // ConstructVectorAreaMatrix: only the boundary loops carry the signed area
            const FMeshBoundaryLoops Loops( &Mesh, true );
            for ( const FEdgeLoop& Loop : Loops.Loops )
            {
                const int32_t NumLoopVert = Loop.Vertices.Num();
                for ( int32_t Idx = 0; Idx < NumLoopVert; ++Idx )
                    AppendAreaEdge( ToIndex[Loop.Vertices[( Idx + 1 ) % NumLoopVert]], ToIndex[Loop.Vertices[Idx]],
                                    NumVerts, -1.0, Triplets );
            }
        }
        static constexpr double Eps = 1e-8;
        for ( int32_t i = 0; i < N; ++i )
            Triplets.push_back( { i, i, Eps } );
        const FSparseMatrixD SystemMatrix( N, N, std::move( Triplets ) );

        // B selects the boundary, E (2V x 2) is its normalized centroid: the iteration uses (B - E E^T) x
        // without forming the dense E E^T
        const double InvSqrtBndr = 1.0 / std::sqrt( static_cast<double>( Boundary.Num() ) );
        const auto   ApplyB      = [&]( const std::vector<double>& In, std::vector<double>& Out )
        {
            double MeanU = 0.0;
            double MeanV = 0.0;
            for ( const int32_t b : Boundary )
            {
                MeanU += In[b] * InvSqrtBndr;
                MeanV += In[b + NumVerts] * InvSqrtBndr;
            }
            Out.assign( N, 0.0 );
            for ( const int32_t b : Boundary )
            {
                Out[b]            = In[b] - MeanU * InvSqrtBndr;
                Out[b + NumVerts] = In[b + NumVerts] - MeanV * InvSqrtBndr;
            }
        };

        FSparseLDLT Solver;
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
            OutUVs[ToVertex[Idx]] = FVector2d( X[Idx], X[Idx + NumVerts] );
        return true;
    }
} // namespace Desert::Geometry
