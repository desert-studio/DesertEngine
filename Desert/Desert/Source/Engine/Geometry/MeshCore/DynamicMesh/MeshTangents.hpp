// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/MeshTangents.h:1-259, adapted: only the
// per-triangle path (SetMesh, InitializeTriVertexTangents, SetPerTriangleTangent, GetPerTriangleTangent,
// ComputeSeparatePerTriangleTangents, CopyToOverlays); the averaged / MikkT paths and FComputeTangentsOptions are
// not ported (ParallelFor is a serial for loop, so bParallel has nothing to switch). UE Core as
// std/glm, namespace Desert::Geometry, instantiated for double only.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"

namespace Desert::Geometry
{
    /**
     * MeshTangents is a utility class that can calculate and store tangents and bitangents for a DynamicMesh3,
     * one pair per triangle corner (index TriangleID * 3 + corner).
     */
    template <typename RealType>
    class MeshTangents
    {
    protected:
        /** Target Mesh */
        const DynamicMesh3* m_Mesh = nullptr;
        /** Set of computed tangents */
        std::vector<glm::vec<3, RealType>> m_Tangents;
        /** Set of computed bitangents */
        std::vector<glm::vec<3, RealType>> m_Bitangents;

    public:
        MeshTangents() = default;

        explicit MeshTangents( const DynamicMesh3* MeshIn )
        {
            SetMesh( MeshIn );
        }

        void SetMesh( const DynamicMesh3* MeshIn )
        {
            this->m_Mesh = MeshIn;
        }

        const std::vector<glm::vec<3, RealType>>& GetTangents() const
        {
            return m_Tangents;
        }

        const std::vector<glm::vec<3, RealType>>& GetBitangents() const
        {
            return m_Bitangents;
        }

        /**
         * Initialize the per-triangle-vertex tangents / bitangents storage (MaxTriangleID * 3 entries).
         */
        void InitializeTriVertexTangents( bool bClearToZero )
        {
            SetTangentCount( m_Mesh->MaxTriangleID() * 3, bClearToZero );
        }

        void SetPerTriangleTangent( int TriangleID, int TriVertIdx, const glm::vec<3, RealType>& Tangent,
                                    const glm::vec<3, RealType>& Bitangent )
        {
            const int k     = TriangleID * 3 + TriVertIdx;
            m_Tangents[k]   = Tangent;
            m_Bitangents[k] = Bitangent;
        }

        void GetPerTriangleTangent( int TriangleID, int TriVertIdx, glm::vec<3, RealType>& TangentOut,
                                    glm::vec<3, RealType>& BitangentOut ) const
        {
            const int k  = TriangleID * 3 + TriVertIdx;
            TangentOut   = m_Tangents[k];
            BitangentOut = m_Bitangents[k];
        }

        /**
         * Calculate per-triangle tangent spaces from the triangle's UVs and positions, each projected into the
         * plane of the corner's overlay normal. The same triangle vertex may have different tangents on
         * different triangles. Triangles unset in UVOverlay are left unwritten.
         */
        void ComputeSeparatePerTriangleTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                                 const DynamicMeshUVOverlay*     UVOverlay );

        /**
         * Write the computed tangents and bitangents into MeshToSet's PrimaryTangents / PrimaryBiTangents
         * overlays, rebuilding their topology so corners whose values agree share one element.
         * @return false when MeshToSet has no attribute set or not exactly three normal layers
         */
        bool CopyToOverlays( DynamicMesh3& MeshToSet ) const;

    protected:
        void SetTangentCount( int Count, bool bClearToZero )
        {
            if ( static_cast<int32_t>( m_Tangents.size() ) < Count )
                m_Tangents.resize( Count );
            if ( static_cast<int32_t>( m_Bitangents.size() ) < Count )
                m_Bitangents.resize( Count );
            if ( bClearToZero )
            {
                for ( glm::vec<3, RealType>& T : m_Tangents )
                    T = glm::vec<3, RealType>( 0 );
                for ( glm::vec<3, RealType>& B : m_Bitangents )
                    B = glm::vec<3, RealType>( 0 );
            }
        }
    };

    using MeshTangentsd = MeshTangents<double>;
} // namespace Desert::Geometry
