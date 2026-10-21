// CopyToOverlays ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshTangents.cpp:295-345,
// adapted: UE Core as std/glm, namespace Desert::Geometry, instantiated for double only. The tangents themselves
// come from the engine's one generator, MikkTSpace (Engine/Geometry/MikkTangents.hpp), the frame UE's mesh build
// stores (ComputeMikktTangents) - not UE's per-triangle / averaged FMeshTangents paths, so a DynamicMesh edit
// and an import agree on the frame a baked normal map expects.
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshTangents.hpp"

#include "Engine/Geometry/MikkTangents.hpp"

using namespace Desert::Geometry;


namespace Desert::Geometry
{
    template <typename RealType>
    bool MeshTangents<RealType>::CopyToOverlays( DynamicMesh3& MeshToSet ) const
    {
        if ( !MeshToSet.HasAttributes() || MeshToSet.Attributes()->NumNormalLayers() != 3 )
        {
            return false;
        }

        // Set aliases to make iterating over tangents and bitangents easier
        DynamicMeshNormalOverlay*                 TangentOverlays[2] = { MeshToSet.Attributes()->PrimaryTangents(),
                                                                         MeshToSet.Attributes()->PrimaryBiTangents() };
        const std::vector<glm::vec<3, RealType>>* TangentValues[2]   = { &m_Tangents, &m_Bitangents };

        for ( int Idx = 0; Idx < 2; Idx++ )
        {
            // Create overlay topology
            const std::vector<glm::vec<3, RealType>>& TV = *TangentValues[Idx];
            TangentOverlays[Idx]->CreateFromPredicate(
                 [&MeshToSet, &TV]( int ParentVertexIdx, int TriIDA, int TriIDB ) -> bool
                 {
                     const Index3i TriA = MeshToSet.GetTriangle( TriIDA );
                     const Index3i TriB = MeshToSet.GetTriangle( TriIDB );
                     const int     SubA = TriA.IndexOf( ParentVertexIdx );
                     const int     SubB = TriB.IndexOf( ParentVertexIdx );
                     assert( SubA > -1 && SubB > -1 );
                     const glm::vec<3, RealType>& A = TV[TriIDA * 3 + SubA];
                     const glm::vec<3, RealType>& B = TV[TriIDB * 3 + SubB];
                     return DistanceSquared( A, B ) < ZeroTolerance<RealType>;
                 },
                 0.0f );

            // Write tangent values out for each wedge value
            // Note: shared elements will be written to multiple times, and the last value written will be used
            for ( const int TID : MeshToSet.TriangleIndicesItr() )
            {
                const Index3i ElTri = TangentOverlays[Idx]->GetTriangle( TID );
                for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
                {
                    TangentOverlays[Idx]->SetElement( ElTri[SubIdx],
                                                      static_cast<glm::vec3>( TV[TID * 3 + SubIdx] ) );
                }
            }
        }
        return true;
    }

    namespace
    {
        // The DynamicMesh3 as MikkTSpace reads it: positions from the mesh, normals and UVs from the overlays,
        // over the triangle IDs the mesh holds (the ID space may have holes).
        template <typename RealType>
        class OverlaySource final : public MikkTangentSource
        {
        public:
            OverlaySource( const DynamicMesh3& mesh, const DynamicMeshNormalOverlay& normals,
                           const DynamicMeshUVOverlay& uvs, MeshTangents<RealType>& target )
                : m_Mesh( mesh ), m_Normals( normals ), m_UVs( uvs ), m_Target( target )
            {
                for ( const int tid : mesh.TriangleIndicesItr() )
                    if ( uvs.IsSetTriangle( tid ) && normals.IsSetTriangle( tid ) )
                        m_Triangles.push_back( tid );
            }

            int TriangleCount() const override
            {
                return static_cast<int>( m_Triangles.size() );
            }
            glm::vec3 Position( int t, int c ) const override
            {
                glm::dvec3 p[3]{};
                m_Mesh.GetTriVertices( m_Triangles[t], p[0], p[1], p[2] );
                return glm::vec3( p[c] );
            }
            glm::vec3 Normal( int t, int c ) const override
            {
                glm::vec3 n[3]{};
                m_Normals.GetTriElements( m_Triangles[t], n[0], n[1], n[2] );
                return n[c];
            }
            glm::vec2 TexCoord( int t, int c ) const override
            {
                glm::vec2 uv[3]{};
                m_UVs.GetTriElements( m_Triangles[t], uv[0], uv[1], uv[2] );
                return uv[c];
            }
            void SetFrame( int t, int c, const glm::vec3& tangent, float sign ) override
            {
                const glm::vec3 bitangent = MikkBitangent( Normal( t, c ), tangent, sign );
                m_Target.SetPerTriangleTangent( m_Triangles[t], c, glm::vec<3, RealType>( tangent ),
                                                glm::vec<3, RealType>( bitangent ) );
            }

        private:
            const DynamicMesh3&              m_Mesh;
            const DynamicMeshNormalOverlay&  m_Normals;
            const DynamicMeshUVOverlay&      m_UVs;
            MeshTangents<RealType>&          m_Target;
            std::vector<int>                 m_Triangles;
        };
    } // namespace

    template <typename RealType>
    bool MeshTangents<RealType>::ComputeMikkTTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                                       const DynamicMeshUVOverlay*     UVOverlay )
    {
        InitializeTriVertexTangents( true );
        OverlaySource<RealType> source( *m_Mesh, *NormalOverlay, *UVOverlay, *this );
        return ComputeMikkTangents( source );
    }

    template class MeshTangents<double>;
} // namespace Desert::Geometry
