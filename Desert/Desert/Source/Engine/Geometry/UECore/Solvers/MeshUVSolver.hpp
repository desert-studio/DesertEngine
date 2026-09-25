// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Solvers/Internal/
// MeshUVSolver.h:23-66, 68-147, 192-279 (FConstrainedMeshUVSolver, FConformalMeshUVSolver,
// FSpectralConformalMeshUVSolver), PowerMethodSolver.h/.cpp (FPowerMethod, FSparsePowerMethod) and
// MatrixSolver.h (FLDLTMatrixSolver), adapted: UE's Eigen types are replaced by a minimal CSR matrix and an
// envelope LDLT with reverse Cuthill-McKee ordering (the tree has no Eigen); only the spectral solver is ported
// (the least-squares one has no caller here); constraints are just the free-boundary vertex set, because the
// spectral solve reads only which vertices are constrained, never their positions or weights; the power method
// starts from a fixed-seed vector instead of a fresh random seed, so a solve is reproducible.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

#include <vector>

namespace Desert::Geometry
{
    /** Compressed-row sparse matrix built from (row, col, value) triplets; duplicates are summed. */
    class FSparseMatrixD
    {
    public:
        struct FTriplet
        {
            int32  Row;
            int32  Col;
            double Value;
        };

        FSparseMatrixD() = default;
        FSparseMatrixD( int32 RowsIn, int32 ColsIn, std::vector<FTriplet> Triplets );

        void Multiply( const std::vector<double>& In, std::vector<double>& Out ) const;

        std::vector<int32>  RowStart; // NumRows + 1 offsets into ColIndex/Values
        std::vector<int32>  ColIndex;
        std::vector<double> Values;

    private:
        int32 NumRows = 0;
        int32 NumCols = 0;
    };

    /**
     * LDL^T of a symmetric positive-definite sparse matrix (UE's FastestPSD = Eigen::SimplicialLDLT). The rows are
     * reordered by reverse Cuthill-McKee and factored inside their envelope, which keeps the fill of a mesh
     * Laplacian to the bandwidth of that ordering.
     */
    class FSparseLDLT
    {
    public:
        /** False (with nothing factored) when a pivot is not finite or not above a relative floor. */
        [[nodiscard]] bool Factorize( const FSparseMatrixD& Matrix );
        void Solve( const std::vector<double>& B, std::vector<double>& X ) const;

    private:
        std::vector<int32>  Perm;      // new index -> old index
        std::vector<int32>  FirstCol;  // first stored column of each permuted row
        std::vector<int32>  RowOffset; // start of each permuted row's envelope in Lower
        std::vector<double> Lower;     // strictly-lower envelope rows of L
        std::vector<double> Diagonal;  // D
    };

    /**
     * Spectral Conformal Parameterization [Mullen 2008]: the UVs are the generalized eigenvector of
     * (E_c, B - E E^T) with the smallest eigenvalue, where E_c is the conformal energy (Dirichlet minus signed
     * area), B selects the free-boundary vertices and E their centroid; nothing pins the boundary.
     */
    class FSpectralConformalMeshUVSolver
    {
    public:
        /** bPreserveIrregularity: area-weighted energy (UE's ECotangentWeightMode::TriangleArea) */
        FSpectralConformalMeshUVSolver( const FDynamicMesh3& MeshIn, bool bPreserveIrregularityIn );

        /** Marks a free-boundary vertex (UE's AddConstraint: its weight and position are unused by this solve). */
        void AddBoundaryVertex( int32 VertexID );

        /** OutUVs is indexed by vertex ID (size MaxVertexID). False if the factorization or the iteration failed.
         */
        bool SolveUVs( TArray<FVector2d>& OutUVs );

    private:
        const FDynamicMesh3& Mesh;
        bool                 bPreserveIrregularity;
        TArray<int32>        ToIndex;  // vertex ID -> compact index, InvalidID for gaps
        TArray<int32>        ToVertex; // compact index -> vertex ID
        TArray<int32>        Boundary; // compact indices, in insertion order, no duplicates
    };
} // namespace Desert::Geometry
