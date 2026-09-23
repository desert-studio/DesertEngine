#pragma once

#include "EditMeshTypes.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <optional>
#include <vector>

namespace Desert::Geometry
{
    class EditMesh;

    // ATTRIBUTE LAYERS OF AN EditMesh: overlays with seams, polygroups, material IDs.
    //
    // WHY OVERLAYS AND NOT PER-VERTEX VALUES. A cube has 8 corners but 24 distinct (corner, normal) pairs, and
    // a UV-unwrapped mesh has cuts where one corner carries two UVs. Storing attributes on the EditMesh
    // vertex would force either duplicating the vertex (and losing the topology the modeling tools stand
    // on - the two copies are no longer neighbours) or averaging across the cut. So each attribute layer is
    // its own small mesh laid over the triangles, UE's DynamicMeshOverlay pattern: the layer owns ELEMENTS
    // (a value plus the one vertex it belongs to, its "parent"), and every triangle names three elements, one
    // per corner. Two triangles that share a vertex AND an element are smooth there; sharing the vertex but
    // not the element is a SEAM. A triangle may also be UNSET in a layer (all three elements InvalidId) -
    // AppendTriangle creates it that way, and the tool that appended it sets it.
    //
    // WHO KEEPS THEM CONSISTENT. The layers are owned by the EditMesh and every topology edit (split, flip,
    // collapse, remove, compact) updates them in the same call - they are not a separate structure a caller
    // must remember to patch. An edit whose attribute outcome has no sensible answer is REFUSED with
    // EditResult::AttributeSeam before anything changes, the same rule the topology follows; UE leaves those
    // cases to "higher-level code" and repairs after the fact (DynamicMeshOverlay.cpp OnFlipEdge/
    // OnCollapseEdge, "should be protected against by calling code").
    //
    // What an element ID means follows the mesh: stable across edits, a hole when freed, reused later, and
    // renumbered only by EditMesh::Compact. An element is freed when the last triangle using it lets go of it.

    enum class OverlayInterpolation : uint8_t
    {
        Linear,        // UVs, colours: plain lerp
        UnitDirection, // normals, tangents: lerp then renormalise (w of a vec4 tangent is a sign, never blended)
    };

    namespace Detail
    {
        // What EditMesh tells its layers about one side of an edge it just split / is about to collapse.
        struct SplitSide
        {
            int   Triangle = InvalidId; // edited in place: corner Slot+1 becomes the new vertex
            int   Created  = InvalidId; // the new (f, q, o) triangle
            int   Slot     = 0;         // the split edge was corners (Slot, Slot+1) of Triangle
            float TowardQ  = 0.0f;      // the new vertex's position from corner Slot to corner Slot+1
        };
        struct CollapseSide
        {
            int Triangle    = InvalidId;
            int SlotKept    = 0; // corner index of the kept / removed vertex in Triangle
            int SlotRemoved = 0;
        };
    } // namespace Detail

    template <typename T>
    class EditMeshOverlay
    {
    public:
        explicit EditMeshOverlay( OverlayInterpolation interpolation, int triangleSlots );

        // A new element with no parent vertex; it is bound to a vertex by the first SetTriangle that uses it.
        // An element no triangle ever uses is a caller defect and CheckValidity names it.
        [[nodiscard]] int AppendElement( const T& value );

        [[nodiscard]] bool IsElement( int e ) const
        {
            return m_Pool.Contains( e );
        }
        [[nodiscard]] int ElementCount() const
        {
            return m_Pool.Live;
        }
        [[nodiscard]] int MaxElementId() const
        {
            return m_Pool.Size();
        }
        [[nodiscard]] IdRange ElementIds() const
        {
            return m_Pool.Ids();
        }

        // Live IDs only, checked by assert.
        [[nodiscard]] const T& GetElement( int e ) const;
        void                   SetElement( int e, const T& value );
        [[nodiscard]] int      GetParentVertex( int e ) const;

        [[nodiscard]] bool IsSetTriangle( int t ) const;
        // Corner order follows EditMesh::GetTriangle(t); {InvalidId x3} when unset.
        [[nodiscard]] const std::array<int, 3>& GetTriangle( int t ) const;
        // Refused (layer untouched) unless t is live, every element is live, and each element is either
        // unbound or already bound to the vertex at that corner.
        [[nodiscard]] EditResult SetTriangle( const EditMesh& mesh, int t, const std::array<int, 3>& elements );
        void                     UnsetTriangle( int t );

        // The element triangle t uses at mesh vertex v, InvalidId if t is unset (v must be a corner of t).
        [[nodiscard]] int GetElementAtVertex( const EditMesh& mesh, int t, int v ) const;

        // An interior edge whose two triangles disagree about the element at either end, or where exactly
        // one of them is set. A boundary edge is never a seam.
        [[nodiscard]] bool IsSeamEdge( const EditMesh& mesh, int e ) const;

        [[nodiscard]] Common::BoolResultStr CheckValidity( const EditMesh& mesh, const char* name ) const;

    private:
        friend class EditMeshAttributes;

        [[nodiscard]] T Lerp( const T& a, const T& b, float t ) const;
        void            Bind( int e, int vertex ); // takes one reference, sets the parent on first use
        void            Drop( int e );             // lets go of one reference, frees on the last
        void            EnsureTriangleSlot( int t );

        void               OnTriangleAllocated( int t );
        void               OnSplitEdge( const std::array<Detail::SplitSide, 2>& sides, int newVertex );
        [[nodiscard]] bool CanFlipEdge( int t0, int j0, int t1, int j1 ) const;
        void               OnFlipEdge( int t0, int j0, int t1, int j1 );
        [[nodiscard]] bool CanCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides ) const;
        void               OnCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides,
                                           const std::vector<int>& movedTriangles, int keptVertex, int removedVertex, float t );
        std::vector<int>   OnCompact( const CompactMaps& maps );

        OverlayInterpolation            m_Interpolation;
        IdPool                          m_Pool;
        std::vector<T>                  m_Values;
        std::vector<int>                m_Parents;
        std::vector<int>                m_RefCounts;
        std::vector<std::array<int, 3>> m_Triangles; // indexed by mesh triangle ID
    };

    using NormalOverlay  = EditMeshOverlay<glm::vec3>;
    using TangentOverlay = EditMeshOverlay<glm::vec4>; // xyz unit tangent, w = bitangent sign: B = cross(N, T) * w
    using ColorOverlay   = EditMeshOverlay<glm::vec4>; // linear RGBA
    using UVOverlay      = EditMeshOverlay<glm::vec2>;

    class EditMeshAttributes
    {
    public:
        // Room for the tools that need a second channel (lightmap UVs, a detail layer) without making every
        // mesh pay for eight; the value only bounds SetUVLayerCount.
        static constexpr int MaxUVLayers = 8;

        // Enabling creates an EMPTY layer (every triangle unset); disabling drops it with its elements.
        void EnableNormals();
        void DisableNormals();
        void EnableTangents();
        void DisableTangents();
        void EnableColors();
        void DisableColors();
        // Grows with empty layers or drops the highest ones. Refused (false, nothing changes) outside
        // [0, MaxUVLayers].
        [[nodiscard]] bool SetUVLayerCount( int count );

        [[nodiscard]] NormalOverlay* Normals()
        {
            return m_Normals ? &*m_Normals : nullptr;
        }
        [[nodiscard]] const NormalOverlay* Normals() const
        {
            return m_Normals ? &*m_Normals : nullptr;
        }
        [[nodiscard]] TangentOverlay* Tangents()
        {
            return m_Tangents ? &*m_Tangents : nullptr;
        }
        [[nodiscard]] const TangentOverlay* Tangents() const
        {
            return m_Tangents ? &*m_Tangents : nullptr;
        }
        [[nodiscard]] ColorOverlay* Colors()
        {
            return m_Colors ? &*m_Colors : nullptr;
        }
        [[nodiscard]] const ColorOverlay* Colors() const
        {
            return m_Colors ? &*m_Colors : nullptr;
        }
        [[nodiscard]] int UVLayerCount() const
        {
            return static_cast<int>( m_UVs.size() );
        }
        [[nodiscard]] UVOverlay*       UV( int layer );
        [[nodiscard]] const UVOverlay* UV( int layer ) const;

        // One int per triangle each; a new triangle starts in group 0 with material 0, and a split's new
        // triangle inherits both from the triangle it was cut from. Live triangle IDs only (assert).
        [[nodiscard]] int GetPolyGroup( int t ) const;
        void              SetPolyGroup( int t, int group );
        [[nodiscard]] int GetMaterialId( int t ) const;
        void              SetMaterialId( int t, int material );

        // Any overlay seam, or the two triangles differ in polygroup or material. Boundary edges are not.
        [[nodiscard]] bool IsAttributeSeamEdge( const EditMesh& mesh, int e ) const;

        [[nodiscard]] Common::BoolResultStr CheckValidity( const EditMesh& mesh ) const;

    private:
        friend class EditMesh;

        template <typename Fn>
        void ForEachOverlay( Fn&& fn );
        template <typename Fn>
        void ForEachOverlay( Fn&& fn ) const;

        void               OnTriangleAllocated( int t );
        void               OnSplitEdge( const std::array<Detail::SplitSide, 2>& sides, int newVertex );
        [[nodiscard]] bool CanFlipEdge( int t0, int j0, int t1, int j1 ) const;
        void               OnFlipEdge( int t0, int j0, int t1, int j1 );
        [[nodiscard]] bool CanCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides ) const;
        void               OnCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides,
                                           const std::vector<int>& movedTriangles, int keptVertex, int removedVertex, float t );
        void               OnRemoveTriangle( int t );
        void               OnCompact( CompactMaps& maps );

        int                           m_TriangleSlots = 0; // == EditMesh::MaxTriangleId()
        std::vector<int>              m_PolyGroups;
        std::vector<int>              m_MaterialIds;
        std::optional<NormalOverlay>  m_Normals;
        std::optional<TangentOverlay> m_Tangents;
        std::optional<ColorOverlay>   m_Colors;
        std::vector<UVOverlay>        m_UVs;
    };
} // namespace Desert::Geometry
