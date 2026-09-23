#pragma once

#include "EditMesh.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    // MESH ELEMENT SELECTION - UE's Modeling Mode selection (UPolygonSelectionMechanic over a
    // FGroupTopologySelection / FDynamicMeshSelection): which vertices, edges, triangles or polygroups of ONE
    // EditMesh the next operation works on. Plain CPU data - no ECS, no ImGui, no camera: a pick is given its
    // view (PickView) by the caller.
    //
    // Why not UE's shape: FGroupTopologySelection keeps corners, group edges and groups side by side in ONE
    // object, because UE's group topology derives polygroup corners/edges from the triangles. We keep one mode
    // and one ID set - the element kinds the EditMesh itself numbers, plus polygroups - so every operation
    // below has one input kind and one output kind, and a conversion is an explicit call, not a side of the
    // object that silently goes stale. Group corners/edges become their own modes when a tool needs them.

    // Ordered from finest to coarsest; ConvertSelection relies on this order.
    enum class ElementMode : uint8_t
    {
        Vertex,
        Edge,
        Triangle,
        PolyGroup,
    };
    [[nodiscard]] const char* ToString( ElementMode mode );

    // The view a pick is made in. Pixels are in one space (ViewportPos + ViewportSize and Cursor alike); the
    // ray is the world-space ray under Cursor, normalised. Vertices and edges are picked by their PROJECTED
    // distance to the cursor, so a thin element is as easy to hit near the camera as far from it.
    struct PickView
    {
        glm::mat4 LocalToWorld{ 1.0f };
        glm::mat4 ViewProj{ 1.0f };
        glm::vec2 ViewportPos{ 0.0f };
        glm::vec2 ViewportSize{ 1.0f };
        glm::vec2 Cursor{ 0.0f };
        glm::vec3 RayOrigin{ 0.0f };
        glm::vec3 RayDirection{ 0.0f, 0.0f, -1.0f };
        float     TolerancePixels = 8.0f;
    };

    // World point -> viewport pixels (y down). False behind the camera (clip w <= 0).
    [[nodiscard]] bool ProjectToViewport( const glm::vec3& world, const glm::mat4& viewProj,
                                          const glm::vec2& viewportPos, const glm::vec2& viewportSize,
                                          glm::vec2& outPixels );

    struct ElementHit
    {
        int   Id            = InvalidId; // vertex / edge / triangle / polygroup ID by mode
        float RayT          = 0.0f;      // world distance along the ray to the picked point
        float PixelDistance = 0.0f;      // 0 for a triangle or polygroup (the ray is ON it)

        [[nodiscard]] bool IsHit() const
        {
            return Id != InvalidId;
        }
    };

    // The element under the cursor:
    //   Triangle / PolyGroup - the nearest triangle the ray crosses (either side), or its polygroup;
    //   Vertex / Edge        - the one whose projection is nearest the cursor within TolerancePixels (ties: the
    //                          nearer along the ray), among those the mesh itself does not hide: a triangle
    //                          crossing the line from the ray origin to the element, in front of it, hides it.
    // An edge with an endpoint behind the camera is not pickable (its projection is undefined).
    [[nodiscard]] ElementHit PickElement( const EditMesh& mesh, ElementMode mode, const PickView& view );

    // What Prune / Remap had to drop, by reason. Nothing is dropped silently: a caller can say it out loud.
    struct PruneReport
    {
        int Missing = 0; // the ID no longer names a live element (a polygroup: no live triangle carries it)
        int Changed = 0; // the ID is live but names a DIFFERENT element: a triangle or edge whose corners are
                         // not the ones selected (a freed ID reused by a new element, or a flipped edge)

        [[nodiscard]] int Total() const
        {
            return Missing + Changed;
        }
    };

    class ElementSelection
    {
    public:
        explicit ElementSelection( ElementMode mode = ElementMode::Triangle ) : m_Mode( mode ) {}

        [[nodiscard]] ElementMode Mode() const
        {
            return m_Mode;
        }
        // Ascending, unique.
        [[nodiscard]] std::span<const int> Ids() const
        {
            return m_Ids;
        }
        [[nodiscard]] int Size() const
        {
            return static_cast<int>( m_Ids.size() );
        }
        [[nodiscard]] bool Empty() const
        {
            return m_Ids.empty();
        }
        [[nodiscard]] bool Contains( int id ) const;

        // Refused, naming the ID and the mode, when the mesh has no such element; nothing changes then.
        [[nodiscard]] Common::BoolResultStr Add( const EditMesh& mesh, int id );
        // False when the ID was not selected.
        bool Remove( int id );
        // Removes a selected ID, adds an unselected one (refused like Add).
        [[nodiscard]] Common::BoolResultStr Toggle( const EditMesh& mesh, int id );
        void                                Clear();

        // Drops every ID the (edited) mesh no longer has, or has as a different element, and says how many.
        PruneReport Prune( const EditMesh& mesh );
        // Renumbers through EditMesh::Compact()'s maps; an ID the compaction removed is counted as Missing.
        // Polygroup IDs are not renumbered by a compaction and pass through unchanged.
        PruneReport Remap( const CompactMaps& maps );

        bool operator==( const ElementSelection& other ) const
        {
            return m_Mode == other.m_Mode && m_Ids == other.m_Ids;
        }

    private:
        // The corners an ID named when it was selected (edge: 2, triangle: 3, others unused) - what lets Prune
        // tell a reused ID from the element that was picked.
        using Key = std::array<int, 3>;
        [[nodiscard]] static Key KeyOf( const EditMesh& mesh, ElementMode mode, int id );
        [[nodiscard]] static bool Exists( const EditMesh& mesh, ElementMode mode, int id );

        ElementMode      m_Mode;
        std::vector<int> m_Ids;  // ascending
        std::vector<Key> m_Keys; // parallel to m_Ids
    };

    // The same part of the mesh in another mode. Towards a FINER mode every part of a selected element is
    // selected (a polygroup's triangles, a triangle's edges, an edge's vertices). Towards a COARSER mode an
    // element is selected only when ALL its parts are (a triangle needs its three edges, a polygroup all its
    // triangles) - so Triangle -> Vertex -> Triangle gives back the triangles, and never grows by the
    // neighbours that merely touch a selected vertex.
    [[nodiscard]] ElementSelection ConvertSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                     ElementMode target );

    // Every element of the connected pieces (across shared edges) the selection touches, same mode.
    [[nodiscard]] ElementSelection SelectConnected( const EditMesh& mesh, const ElementSelection& selection );

    // One ring out: vertices gain their edge neighbours; edges, triangles and polygroups gain every element
    // that shares a vertex with the selection.
    [[nodiscard]] ElementSelection GrowSelection( const EditMesh& mesh, const ElementSelection& selection );
    // One ring in, the inverse: an element stays only when no vertex of it is shared with an UNselected
    // element (a vertex: when all its edge neighbours are selected). A selection with no unselected
    // neighbour - the whole of a closed mesh - does not shrink.
    [[nodiscard]] ElementSelection ShrinkSelection( const EditMesh& mesh, const ElementSelection& selection );
} // namespace Desert::Geometry
