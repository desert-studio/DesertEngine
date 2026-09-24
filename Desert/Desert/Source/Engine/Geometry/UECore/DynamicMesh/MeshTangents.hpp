// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/MeshTangents.h:1-259, adapted: only the
// per-triangle path (SetMesh, InitializeTriVertexTangents, SetPerTriangleTangent, GetPerTriangleTangent,
// ComputeSeparatePerTriangleTangents, CopyToOverlays); the averaged / MikkT paths and FComputeTangentsOptions are
// not ported (ParallelFor is the UECore.hpp serial shim, so bParallel has nothing to switch). UE Core via
// UECore.hpp, namespace Desert::Geometry, instantiated for double only.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"

namespace Desert::Geometry
{
    /**
     * TMeshTangents is a utility class that can calculate and store tangents and bitangents for a FDynamicMesh3,
     * one pair per triangle corner (index TriangleID * 3 + corner).
     */
    template <typename RealType>
    class TMeshTangents
    {
    protected:
        /** Target Mesh */
        const FDynamicMesh3* Mesh = nullptr;
        /** Set of computed tangents */
        TArray<TVector<RealType>> Tangents;
        /** Set of computed bitangents */
        TArray<TVector<RealType>> Bitangents;

    public:
        TMeshTangents() = default;

        explicit TMeshTangents( const FDynamicMesh3* MeshIn )
        {
            SetMesh( MeshIn );
        }

        void SetMesh( const FDynamicMesh3* MeshIn )
        {
            this->Mesh = MeshIn;
        }

        const TArray<TVector<RealType>>& GetTangents() const
        {
            return Tangents;
        }

        const TArray<TVector<RealType>>& GetBitangents() const
        {
            return Bitangents;
        }

        /**
         * Initialize the per-triangle-vertex tangents / bitangents storage (MaxTriangleID * 3 entries).
         */
        void InitializeTriVertexTangents( bool bClearToZero )
        {
            SetTangentCount( Mesh->MaxTriangleID() * 3, bClearToZero );
        }

        void SetPerTriangleTangent( int TriangleID, int TriVertIdx, const TVector<RealType>& Tangent,
                                    const TVector<RealType>& Bitangent )
        {
            const int k   = TriangleID * 3 + TriVertIdx;
            Tangents[k]   = Tangent;
            Bitangents[k] = Bitangent;
        }

        void GetPerTriangleTangent( int TriangleID, int TriVertIdx, TVector<RealType>& TangentOut,
                                    TVector<RealType>& BitangentOut ) const
        {
            const int k  = TriangleID * 3 + TriVertIdx;
            TangentOut   = Tangents[k];
            BitangentOut = Bitangents[k];
        }

        /**
         * Calculate per-triangle tangent spaces from the triangle's UVs and positions, each projected into the
         * plane of the corner's overlay normal. The same triangle vertex may have different tangents on
         * different triangles. Triangles unset in UVOverlay are left unwritten.
         */
        void ComputeSeparatePerTriangleTangents( const FDynamicMeshNormalOverlay* NormalOverlay,
                                                 const FDynamicMeshUVOverlay*     UVOverlay );

        /**
         * Write the computed tangents and bitangents into MeshToSet's PrimaryTangents / PrimaryBiTangents
         * overlays, rebuilding their topology so corners whose values agree share one element.
         * @return false when MeshToSet has no attribute set or not exactly three normal layers
         */
        bool CopyToOverlays( FDynamicMesh3& MeshToSet ) const;

    protected:
        void SetTangentCount( int Count, bool bClearToZero )
        {
            if ( Tangents.Num() < Count )
                Tangents.SetNum( Count );
            if ( Bitangents.Num() < Count )
                Bitangents.SetNum( Count );
            if ( bClearToZero )
            {
                for ( TVector<RealType>& T : Tangents )
                    T = TVector<RealType>::Zero();
                for ( TVector<RealType>& B : Bitangents )
                    B = TVector<RealType>::Zero();
            }
        }
    };

    using FMeshTangentsd = TMeshTangents<double>;
} // namespace Desert::Geometry
