// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/MeshTangents.h:1-259, adapted: the
// per-corner storage (SetMesh, InitializeTriVertexTangents, SetPerTriangleTangent, GetPerTriangleTangent,
// CopyToOverlays); the engine's frame is MikkTSpace's (ComputeMikkTTangents, through Engine/Geometry/MikkTangents.hpp,
// the generator the importer uses too); the averaged path (ComputeMikkTStyleTangents, MeshTangents.cpp:395-676) and
// FComputeTangentsOptions came with M20 for the Tangents tool. UE Core as std/glm, namespace Desert::Geometry,
// instantiated for double only.
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
        std::vector<int>                   m_DegenerateTris; // UE AllDegenerateTris (zero-UV-area triangles)

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
         * Calculate the MikkTSpace frame of every triangle corner from the mesh positions and the overlay normals
         * and UVs: unit tangent T, bitangent sign * cross(N, T) with N the corner's overlay normal. Triangles unset
         * in either overlay are left zero.
         * @return false when MikkTSpace could not allocate its working set
         */
        [[nodiscard]] bool ComputeMikkTTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                                 const DynamicMeshUVOverlay*     UVOverlay );

        /**
         * Write the computed tangents and bitangents into MeshToSet's PrimaryTangents / PrimaryBiTangents
         * overlays, rebuilding their topology so corners whose values agree share one element.
         * @return false when MeshToSet has no attribute set or not exactly three normal layers
         */
        bool CopyToOverlays( DynamicMesh3& MeshToSet ) const;

        // UE FComputeTangentsOptions.
        struct ComputeTangentsOptions
        {
            bool bAngleWeighted = true;
            bool bAveraged      = true;
        };
        // UE ComputeTriVertexTangents: averaged = the MikkTSpace-style wedge average (UE "Fast MikkTSpace"),
        // otherwise the separate per-triangle path.
        void ComputeTriVertexTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                       const DynamicMeshUVOverlay* UVOverlay, const ComputeTangentsOptions& Options );
        void ComputeMikkTStyleTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                        const DynamicMeshUVOverlay* UVOverlay, const ComputeTangentsOptions& Options );
        const std::vector<int>& GetDegenerateTris() const
        {
            return m_DegenerateTris;
        }

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
