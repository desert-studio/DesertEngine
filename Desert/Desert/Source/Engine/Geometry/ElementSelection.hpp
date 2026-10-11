#pragma once


#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    class DynamicMesh3;

    // "No element": a miss, an unset ID.
    inline constexpr int InvalidId = -1;

    // MESH ELEMENT SELECTION - UE's Modeling Mode selection (UPolygonSelectionMechanic over a
    // GroupTopologySelection / FDynamicMeshSelection): which vertices, edges, triangles or polygroups of ONE
    // DynamicMesh3 the next operation works on. Plain CPU data - no ECS, no ImGui, no camera: a pick is given its
    // view (PickView) by the caller.
    //
    // Why not UE's shape: GroupTopologySelection keeps corners, group edges and groups side by side in ONE
    // object, because UE's group topology derives polygroup corners/edges from the triangles. We keep one mode
    // and one ID set - the element kinds the DynamicMesh3 itself numbers, plus polygroups - so every operation
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
        explicit ElementSelection( ElementMode mode = ElementMode::Triangle ) : m_Mode( mode )
        {
        }

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

        // False when the ID was not selected.
        bool Remove( int id );
        // Refused, naming the ID and the mode, when the mesh has no such element; nothing changes then. Toggle
        // removes a selected ID and adds an unselected one (refused like Add); Prune drops every ID the (edited)
        // mesh no longer has, or has as a different element, and says how many (DynamicMeshSelection.cpp).
        [[nodiscard]] Common::BoolResultStr Add( const DynamicMesh3& mesh, int id );
        [[nodiscard]] Common::BoolResultStr Toggle( const DynamicMesh3& mesh, int id );
        PruneReport                         Prune( const DynamicMesh3& mesh );
        // Add over any mesh view of ElementSelectionAlgorithms.inl (the algorithms build their results with it).
        template <class Mesh>
        [[nodiscard]] Common::BoolResultStr AddIn( const Mesh& mesh, int id );
        void                                Clear();


        bool operator==( const ElementSelection& other ) const
        {
            return m_Mode == other.m_Mode && m_Ids == other.m_Ids;
        }

    private:
        // The corners an ID named when it was selected (edge: 2, triangle: 3, others unused) - what lets Prune
        // tell a reused ID from the element that was picked.
        using Key = std::array<int, 3>;
        // Written once over a mesh view for both cores (ElementSelectionAlgorithms.inl).
        template <class Mesh>
        [[nodiscard]] static Key KeyOf( const Mesh& mesh, ElementMode mode, int id );
        template <class Mesh>
        [[nodiscard]] static bool Exists( const Mesh& mesh, ElementMode mode, int id );
        template <class Mesh>
        [[nodiscard]] Common::BoolResultStr ToggleIn( const Mesh& mesh, int id );
        template <class Mesh>
        PruneReport PruneIn( const Mesh& mesh );

        ElementMode      m_Mode;
        std::vector<int> m_Ids;  // ascending
        std::vector<Key> m_Keys; // parallel to m_Ids
    };



} // namespace Desert::Geometry
