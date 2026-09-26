// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicMesh3.h:1-1946, adapted: UE Core
// via UECore.hpp, namespace Desert::Geometry, change-stamp mutex is std::mutex; NOT ported here: FArchive
// serialization, FMeshShapeGenerator construction,
// IsSameAs/MeshInfoString (1603-1707),
// vertex/triangle frames (Frame3d), debug-mesh stash; GetBounds has no parallel path.

// Port of geometry3cpp DMesh3

#pragma once

#include "Engine/Geometry/UECore/BoxTypes.hpp"
#include "Engine/Geometry/UECore/CompactMaps.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/InfoTypes.hpp"
#include "Engine/Geometry/UECore/DynamicVector.hpp"
#include "Engine/Geometry/UECore/GeometryTypes.hpp"
#include "Engine/Geometry/UECore/IndexTypes.hpp"
#include "Engine/Geometry/UECore/IndexUtil.hpp"
#include "Engine/Geometry/UECore/IteratorUtil.hpp"
#include "Engine/Geometry/UECore/MathUtil.hpp"
#include "Engine/Geometry/UECore/RefCountVector.hpp"

#include <memory>
#include "Engine/Geometry/UECore/SmallListSet.hpp"
#include "Engine/Geometry/UECore/UECore.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"
#include "Engine/Geometry/UECore/VectorUtil.hpp"

#include <atomic>
#include <initializer_list>
#include <mutex>
#include <Common/Core/Core.hpp>

namespace Desert::Geometry
{

    class DynamicMeshCompactMaps;

    enum class MeshComponents : uint8_t
    {
        None          = 0,
        VertexNormals = 1,
        VertexColors  = 2,
        VertexUVs     = 4,
        FaceGroups    = 8,
        All           = 15
    };

    /**
     * DynamicMesh3 is a dynamic triangle mesh class. The mesh has has connectivity,
     * is an indexed mesh, and allows for gaps in the index space.
     *
     * internally, all data is stored in POD-type buffers, except for the vertex->edge
     * links, which are stored as List<int>'s. The arrays of POD data are stored in
     * DynamicVector's, so they grow in chunks, which is relatively efficient. The actual
     * blocks are arrays, so they can be efficiently mem-copied into larger buffers
     * if necessary.
     *
     * Reference counts for verts/tris/edges are stored as separate RefCountVector
     * instances.
     *
     * Vertices are stored as doubles, although this should be easily changed
     * if necessary, as the internal data structure is not exposed
     *
     * Per-vertex Vertex Normals, Colors, and UVs are optional and stored as floats.
     * Note that in practice, these are generally only used as scratch space, in limited
     * circumstances, usually when needed for performance reasons. Most of our geometry
     * code instead prefers to read attributes from the per-triangle AttributeSet accessed
     * via Attributes() (see DynamicMeshOverlay for a description of the structure). For
     * instance, an empty (but existing) attribute set will take precedence over non-empty
     * vertex normals in much of our processing code.
     *
     * For each vertex, VertexEdgeLists[i] is the unordered list of connected edges. The
     * elements of the list are indices into the edges list.
     * This list is unsorted but can be traversed in-order (ie cw/ccw) at some additional cost.
     *
     * Triangles are stored as 3 ints, with optionally a per-triangle integer group id.
     * The group IDs stored here DO get widely used and preserved in our geometry code
     * (unlike the per-vertex attributes described earlier), even though the AttributeSet
     * can store group IDs as well (potentially in multiple layers).
     *
     * The edges of a triangle are similarly stored as 3 ints, in triangle_edes. If the
     * triangle is [v1,v2,v3], then the triangle edges [e1,e2,e3] are
     * e1=edge(v1,v2), e2=edge(v2,v3), e3=edge(v3,v1), where the e# are indexes into edges.
     *
     * Edges are stored as tuples of 4 ints. If the edge is between v1 and v2, with neighbour
     * tris t1 and t2, then the edge is [min(v1,v2), max(v1,v2), t1, t2]. For a boundary
     * edge, t2 is InvalidID. t1 is never InvalidID.
     *
     * Most of the class assumes that the mesh is manifold. Many functions will
     * work if the topology is non-manifold, but behavior of operators like Split/Flip/Collapse
     * edge is untested.
     *
     * The function CheckValidity() does extensive sanity checking on the mesh data structure.
     * Use this to test your code, both for mesh construction and editing!!
     */
    class DynamicMeshAttributeSet;

    class DynamicMesh3
    {

        // TODO:
        //  - Many of the iterators depend on lambda functions, can we replace these with calls to
        //    internal/static functions that do the same thing?
        //  - CompactInPlace() does not compact VertexEdgeLists

    public:
        // Inline-allocator array types optionally used for mesh queries, to reduce heap allocations
        using LocalIntArray  = std::vector<int32_t>;
        using LocalBoolArray = std::vector<bool>;

        struct Edge
        {
            Index2i     Vert;
            Index2i     Tri;
            friend bool operator!=( const Edge& e0, const Edge& e1 )
            {
                return ( e0.Vert != e1.Vert ) || ( e0.Tri != e1.Tri );
            }
        };
        /** InvalidID indicates that a vertex/edge/triangle ID is invalid */
        constexpr static int InvalidID = IndexConstants::InvalidID;
        /** NonManifoldID is returned by AppendTriangle() to indicate that the added triangle would result in
         * nonmanifold geometry and hence was ignored */
        constexpr static int NonManifoldID = -2;
        /** DuplicateTriangleID is returned by AppendTriangle() to indicate that the added triangle already exists
         * in the mesh, and was ignored because we do not support duplicate triangles */
        constexpr static int DuplicateTriangleID = -3;

        const static glm::dvec3   InvalidVertex;
        constexpr static Index3i  InvalidTriangle{ InvalidID, InvalidID, InvalidID };
        constexpr static Index2i  InvalidEdge{ InvalidID, InvalidID };

    protected:
        /** List of vertex positions */
        DynamicVector<glm::dvec3> m_Vertices{};
        /** Reference counts of vertex indices. For vertices that exist, the count is 1 +
         * num_triangle_using_vertex. Iterate over this to find out which vertex indices are valid. */
        RefCountVector m_VertexRefCounts{};
        /** (optional) List of per-vertex normals */
        std::optional<DynamicVector<glm::vec3>> m_VertexNormals{};
        /** (optional) List of per-vertex colors */
        std::optional<DynamicVector<glm::vec3>> m_VertexColors{};
        /** (optional) List of per-vertex uv's */
        std::optional<DynamicVector<glm::vec2>> m_VertexUVs{};
        /** List of per-vertex edge one-rings */
        SmallListSet m_VertexEdgeLists;

        /** List of triangle vertex-index triplets [Vert0 Vert1 Vert2]*/
        DynamicVector<Index3i> m_Triangles;
        /** Reference counts of triangle indices. Ref count is always 1 if the triangle exists. Iterate over this
         * to find out which triangle indices are valid. */
        RefCountVector m_TriangleRefCounts;
        /** List of triangle edge triplets [Edge0 Edge1 Edge2] */
        DynamicVector<Index3i> m_TriangleEdges;
        /** (optional) List of per-triangle group identifiers */
        std::optional<DynamicVector<int>> m_TriangleGroups{};
        /** Upper bound on the triangle group IDs used in the mesh (may be larger than the actual maximum if
         * triangles have been deleted) */
        int m_GroupIDCounter = 0;

        /** Extended Attributes for the Mesh (UV layers, Hard Normals, additional Polygroup Layers, etc) */
        std::unique_ptr<DynamicMeshAttributeSet> m_AttributeSet{};

        /** List of edge elements. An edge is four elements [VertA, VertB, Tri0, Tri1], where VertA < VertB, and
         * Tri1 may be InvalidID (if the edge is a boundary edge) */
        DynamicVector<Edge> m_Edges;
        /** Reference counts of edge indices. Ref count is always 1 if the edge exists. Iterate over this to find
         * out which edge indices are valid. */
        RefCountVector m_EdgeRefCounts;

    private:
        struct ChangeStamp
        {
            explicit ChangeStamp( bool bInIsEnabled ) : bIsEnabled( bInIsEnabled )
            {
            }

            ChangeStamp( const ChangeStamp& )            = delete;
            ChangeStamp& operator=( const ChangeStamp& ) = delete;

            /** Enable/disable this change stamp. */
            bool bIsEnabled = false;

            /** Guards `Value`. */
            mutable std::mutex Mutex;

            /** The change stamp is incremented when modifications occur, if `bIsEnabled` is set. It's guarded by
             * `Mutex`. */
            uint32_t Value = 1;

            /** Updates the change stamp in an thread-safe, transactionally-safe way. Does nothing if `bIsEnabled`
             * is not set. */
            void IncrementIfEnabled()
            {
                if ( bIsEnabled )
                {
                    Mutex.lock();
                    ++Value;
                    Mutex.unlock();
                }
            }

            /** Overwrites the change stamp in an thread-safe, transactionally-safe way. Does nothing if
             * `bIsEnabled` is not set. */
            void Set( uint32_t NewValue )
            {
                if ( bIsEnabled )
                {
                    Mutex.lock();
                    Value = NewValue;
                    Mutex.unlock();
                }
            }

            /** Returns the current change value in a thread-safe, transactionally-safe way. Returns 1 if
             * `bIsEnabled` is not set. */
            uint32_t GetValue() const
            {
                uint32_t Result = 1;

                if ( bIsEnabled )
                {
                    Mutex.lock();
                    Result = Value;
                    Mutex.unlock();
                }

                return Result;
            }
        };

        // Shape change tracking can be problematic in multi-threaded contexts so they're disabled by default.
        // In fact, it is not suggested that these be used at all. (See comment for SetShapeChangeStampEnabled.)
        ChangeStamp m_ChangeStampShape{ /*bInIsEnabled=*/false };

        // Topological change tracking is enabled by default.
        ChangeStamp m_ChangeStampTopology{ /*bInIsEnabled=*/true };

    public:
        /** Default constructor */
        DynamicMesh3();

        /** Copy/Move construction */
        DynamicMesh3( const DynamicMesh3& CopyMesh );
        DynamicMesh3( DynamicMesh3&& MoveMesh );

        /** Copy and move assignment */
        DynamicMesh3& operator=( const DynamicMesh3& CopyMesh );
        DynamicMesh3& operator=( DynamicMesh3&& MoveMesh );

        /** Destructor */
        virtual ~DynamicMesh3();

        /** Construct an empty mesh with specified attributes */
        explicit DynamicMesh3( bool bWantNormals, bool bWantColors, bool bWantUVs, bool bWantTriGroups );
        explicit DynamicMesh3( MeshComponents flags );

        /** Set internal data structures to be a copy of input mesh using the specified attributes*/
        void Copy( const DynamicMesh3& CopyMesh, bool bNormals = true, bool bColors = true, bool bUVs = true,
                   bool bAttributes = true );

        // Tracks how IDs are offset and number of elements appended by a mesh append operation
        struct AppendInfo
        {
            // Offsets for base mesh element IDs
            int32_t VertexOffset = 0, TriangleOffset = 0, EdgeOffset = 0, GroupOffset = 0;

            // Offsets for the first 3 normal overlay layers (i.e., typically normal, tangent, bitangent)
            int32_t NormalOverlayOffsets[3]{ 0, 0, 0 };

            // The number appended of each element type -- including 'invalid' slots, i.e. the amount by which
            // MaxID increased
            int32_t NumVertex = 0, NumTriangle = 0, NumEdge = 0;
        };

        /**
         * Append the ToAppend mesh geometry to this mesh, preserving element IDs aside from a constant offset.
         *
         * Attributes from the ToAppend mesh will only be included if they already exist on this mesh.
         * If more attributes are needed, consider calling EnableMatchingAttributes.
         *
         * Note that OutAppendInfo optionally provides some offsets for convenience, but callers may also compute
         * the offset for any element type by storing the corresponding MaxID before calling AppendWithOffsets.
         *
         * @param ToAppend Mesh to append
         * @param OutAppendInfo Optionally stores offsets and number of IDs appended for mesh elements.
         */
        void AppendWithOffsets( const DynamicMesh3& ToAppend, AppendInfo* OutAppendInfo = nullptr );

        /**
         * Copy input mesh while compacting, i.e. removing unused vertices/triangles/edges.
         *
         * @param CopyMesh Mesh to copy
         * @param bNormals if true, will copy normals
         * @param bColors if true, will copy colors
         * @param bUVs if true, will copy UVs
         * @param CompactInfo if not nullptr, will be filled with mapping indicating how vertex and triangle IDs
         * were changed during compaction
         */
        void CompactCopy( const DynamicMesh3& CopyMesh, bool bNormals = true, bool bColors = true,
                          bool bUVs = true, bool bAttributes = true,
                          DynamicMeshCompactMaps* CompactInfo = nullptr );

        /** Discard all data */
        void Clear();

        /**
         * Ensure that all the same extended attributes available in ToMatch are also enabled.
         * By default, clears existing attributes, so that there will be an exact match
         * If bClearExisting is passed as false, existing attributes are not removed/cleared.
         * If bDiscardExtraAttributes=true and bClearExisting=false, extra attributes not in ToMatch are discarded,
         * but existing attributes are not cleared/reset
         */
        void EnableMatchingAttributes( const DynamicMesh3& ToMatch, bool bClearExisting = true,
                                       bool bDiscardExtraAttributes = false );

    public:
        /** @return number of vertices in the mesh */
        int VertexCount() const
        {
            return static_cast<int>( m_VertexRefCounts.GetCount() );
        }
        /** @return number of triangles in the mesh */
        int TriangleCount() const
        {
            return static_cast<int>( m_TriangleRefCounts.GetCount() );
        }
        /** @return number of edges in the mesh */
        int EdgeCount() const
        {
            return static_cast<int>( m_EdgeRefCounts.GetCount() );
        }

        /** @return upper bound on vertex IDs used in the mesh, i.e. all vertex IDs in use are < MaxVertexID */
        int MaxVertexID() const
        {
            return static_cast<int>( m_VertexRefCounts.GetMaxIndex() );
        }
        /** @return upper bound on triangle IDs used in the mesh, i.e. all triangle IDs in use are < MaxTriangleID
         */
        int MaxTriangleID() const
        {
            return static_cast<int>( m_TriangleRefCounts.GetMaxIndex() );
        }
        /** @return upper bound on edge IDs used in the mesh, i.e. all edge IDs in use are < MaxEdgeID */
        int MaxEdgeID() const
        {
            return static_cast<int>( m_EdgeRefCounts.GetMaxIndex() );
        }
        /** @return upper bound on group IDs used in the mesh, i.e. all group IDs in use are < MaxGroupID */
        int MaxGroupID() const
        {
            return m_GroupIDCounter;
        }

        /** @return true if this mesh has per-vertex normals */
        bool HasVertexNormals() const
        {
            return m_VertexNormals.has_value();
        }
        /** @return true if this mesh has per-vertex colors */
        bool HasVertexColors() const
        {
            return m_VertexColors.has_value();
        }
        /** @return true if this mesh has per-vertex UVs */
        bool HasVertexUVs() const
        {
            return m_VertexUVs.has_value();
        }
        /** @return true if this mesh has per-triangle groups */
        bool HasTriangleGroups() const
        {
            return m_TriangleGroups.has_value();
        }
        /** @return true if this mesh has attribute layers */
        bool HasAttributes() const
        {
            return m_AttributeSet != nullptr;
        }
        /** @return a pointer to the attribute set, if the mesh has one, else nullptr */
        DynamicMeshAttributeSet* Attributes()
        {
            return m_AttributeSet.get();
        }
        /** @return a pointer to the attribute set, if the mesh has one, else nullptr */
        const DynamicMeshAttributeSet* Attributes() const
        {
            return m_AttributeSet.get();
        }
        /** Enable the attribute set, with one UV and one normal layer, if it does not exist */
        void EnableAttributes();
        /** Discard the attribute set */
        void DiscardAttributes();

        /** @return bitwise-or of MeshComponents flags specifying which extra data this mesh has */
        int GetComponentsFlags() const;

        /** @return true if VertexID is a valid vertex in this mesh */
        bool IsVertex( int VertexID ) const
        {
            return m_VertexRefCounts.IsValid( VertexID );
        }
        /** @return true if VertexID is a valid vertex in this mesh AND is used by at least one triangle */
        bool IsReferencedVertex( int VertexID ) const
        {
            return VertexID >= 0 && VertexID < static_cast<int>( m_VertexRefCounts.GetMaxIndex() ) &&
                   m_VertexRefCounts.GetRefCount( VertexID ) > 1;
        }
        /** @return true if TriangleID is a valid triangle in this mesh */
        bool IsTriangle( int TriangleID ) const
        {
            return m_TriangleRefCounts.IsValid( TriangleID );
        }
        /** @return true if EdgeID is a valid edge in this mesh */
        bool IsEdge( int EdgeID ) const
        {
            return m_EdgeRefCounts.IsValid( EdgeID );
        }

        //
        // Change Tracking support
        //
        // Note: May someday be removed (see comment for SetShapeChangeStampEnabled)
        //
    public:
        /**
         * Enable/Disable incrementing of the ShapeChangeStamp.
         *
         * NOTE: Both change stamps are unreliable in many contexts, and may someday get removed.
         * Specifically, they get reset on Clear(), copied to the destination rather than incremented
         * in copies and moves, and do not track any changes via access to the buffers directly (through
         * GetVerticesBuffer(), etc).
         */
        void SetShapeChangeStampEnabled( bool bEnabled )
        {
            m_ChangeStampShape.bIsEnabled = bEnabled;
        }

        /** Enable/disable incrementing of the TopologyChangeStamp. */
        void SetTopologyChangeStampEnabled( bool bEnabled )
        {
            m_ChangeStampTopology.bIsEnabled = bEnabled;
        }

        /** @return true if shape ChangeStamp is enabled (disabled by default) */
        bool HasShapeChangeStampEnabled() const
        {
            return m_ChangeStampShape.bIsEnabled;
        }

        /** @return true if topology ChangeStamp is enabled (disabled by default) */
        bool HasTopologyChangeStampEnabled() const
        {
            return m_ChangeStampTopology.bIsEnabled;
        }

        /** Increment the specified ChangeStamps, if they are enabled. Thread-safe. */
        void UpdateChangeStamps( bool bShapeChange, bool bTopologyChange )
        {
            if ( bShapeChange || bTopologyChange )
            {
                m_ChangeStampShape.IncrementIfEnabled();
            }
            if ( bTopologyChange )
            {
                m_ChangeStampTopology.IncrementIfEnabled();
            }
        }

        /**
         * Returns the current ShapeChangeStamp. This is incremented any time a mesh vertex position is changed
         * _or_ the mesh topology is modified. Change stamps are disabled by default.
         */
        uint32_t GetShapeChangeStamp() const
        {
            DESERT_VERIFY_WARN( m_ChangeStampShape.bIsEnabled,
                                "Shape change tracking is not enabled on this mesh. "
                                "Use SetShapeChangeStampEnabled() to enable." );
            return m_ChangeStampShape.GetValue();
        }

        /**
         * Returns the current TopologyChangeStamp. This is incremented when the mesh topology is modified.
         * Change stamps are disabled by default.
         */
        uint32_t GetTopologyChangeStamp() const
        {
            DESERT_VERIFY_WARN( m_ChangeStampTopology.bIsEnabled,
                                "Topology change tracking is not enabled on this mesh. Use "
                                "SetTopologyChangeStampEnabled() to enable." );
            return m_ChangeStampTopology.GetValue();
        }

        /** ChangeStamp is a combination of the Shape and Topology ChangeStamps. If neither flag is enabled, this
         * value will never change. */
        uint64_t GetChangeStamp() const
        {
            return m_ChangeStampShape.GetValue() + m_ChangeStampTopology.GetValue();
        }

        //
        // Mesh Element Iterators
        //   The functions VertexIndicesItr() / TriangleIndicesItr() / EdgeIndicesItr() allow you to do:
        //      for ( int eid : EdgeIndicesItr() ) { ... }
        //   and other related begin() / end() idioms
    public:
        // simplify names for iterations
        using vertex_iterator   = typename RefCountVector::IndexEnumerable;
        using triangle_iterator = typename RefCountVector::IndexEnumerable;
        using edge_iterator     = typename RefCountVector::IndexEnumerable;
        template <typename T>
        using value_iteration          = RefCountVector::MappedEnumerable<T>;
        using vtx_triangles_enumerable = PairExpandEnumerable<SmallListSet::ValueIterator>;

        /** @return enumerable object for valid vertex indices suitable for use with range-based for, ie for ( int
         * i : VertexIndicesItr() ) */
        vertex_iterator VertexIndicesItr() const
        {
            return m_VertexRefCounts.Indices();
        }

        /** @return enumerable object for valid triangle indices suitable for use with range-based for, ie for (
         * int i : TriangleIndicesItr() ) */
        triangle_iterator TriangleIndicesItr() const
        {
            return m_TriangleRefCounts.Indices();
        }

        /** @return enumerable object for valid edge indices suitable for use with range-based for, ie for ( int i
         * : EdgeIndicesItr() ) */
        edge_iterator EdgeIndicesItr() const
        {
            return m_EdgeRefCounts.Indices();
        }

        // TODO: write helper functions that allow us to do these iterations w/o lambdas

        /** @return enumerable object for boundary edge indices suitable for use with range-based for, ie for ( int
         * i : BoundaryEdgeIndicesItr() ) */
        RefCountVector::FilteredEnumerable BoundaryEdgeIndicesItr() const
        {
            return m_EdgeRefCounts.FilteredIndices( [this]( int EdgeID )
                                                    { return m_Edges[EdgeID].Tri[1] == InvalidID; } );
        }

        /** Enumerate positions of all vertices in mesh */
        value_iteration<glm::dvec3> VerticesItr() const
        {
            return m_VertexRefCounts.MappedIndices<glm::dvec3>( [this]( int VertexID )
                                                                { return m_Vertices[VertexID]; } );
        }

        /** Enumerate all triangles in the mesh */
        value_iteration<Index3i> TrianglesItr() const
        {
            return m_TriangleRefCounts.MappedIndices<Index3i>( [this]( int TriangleID )
                                                               { return m_Triangles[TriangleID]; } );
        }

        /** Enumerate edges. Each returned element is [v0,v1,t0,t1], where t1 will be InvalidID if this is a
         * boundary edge */
        value_iteration<Edge> EdgesItr() const
        {
            return m_EdgeRefCounts.MappedIndices<Edge>( [this]( int EdgeID ) { return m_Edges[EdgeID]; } );
        }

        /** @return enumerable object for one-ring vertex neighbours of a vertex, suitable for use with range-based
         * for, ie for ( int i : VtxVerticesItr(VertexID) ) */
        SmallListSet::MappedValueEnumerable VtxVerticesItr( int VertexID ) const
        {
            assert( m_VertexRefCounts.IsValid( VertexID ) );
            return m_VertexEdgeLists.MappedValues( VertexID, [VertexID, this]( int eid )
                                                   { return GetOtherEdgeVertex( eid, VertexID ); } );
        }

        /** Call VertexFunc for each one-ring vertex neighbour of a vertex. Currently this is more efficient than
         * VtxVerticesItr() due to overhead in the Values() enumerable */
        void EnumerateVertexVertices( int32_t VertexID, std::function<void( int32_t )> VertexFunc ) const
        {
            assert( m_VertexRefCounts.IsValid( VertexID ) );
            m_VertexEdgeLists.Enumerate( VertexID, [this, &VertexFunc, VertexID]( int32_t eid )
                                         { VertexFunc( GetOtherEdgeVertex( eid, VertexID ) ); } );
        }

        /** @return enumerable object for one-ring edges of a vertex, suitable for use with range-based for, ie for
         * ( int i : VtxEdgesItr(VertexID) ) */
        SmallListSet::ValueEnumerable VtxEdgesItr( int VertexID ) const
        {
            assert( m_VertexRefCounts.IsValid( VertexID ) );
            return m_VertexEdgeLists.Values( VertexID );
        }

        /** Call EdgeFunc for each one-ring edge of a vertex. Currently this is more efficient than VtxEdgesItr()
         * due to overhead in the Values() enumerable */
        void EnumerateVertexEdges( int32_t VertexID, const std::function<void( int32_t )>& EdgeFunc ) const
        {
            assert( m_VertexRefCounts.IsValid( VertexID ) );
            m_VertexEdgeLists.Enumerate( VertexID, EdgeFunc );
        }

        /** @return enumerable object for one-ring triangles of a vertex, suitable for use with range-based for, ie
         * for ( int i : VtxTrianglesItr(VertexID) ) */
        vtx_triangles_enumerable VtxTrianglesItr( int VertexID ) const
        {
            assert( m_VertexRefCounts.IsValid( VertexID ) );
            return { m_VertexEdgeLists.Values( VertexID ),
                     [this, VertexID]( int EdgeID ) { return GetOrderedOneRingEdgeTris( VertexID, EdgeID ); } };
        }

        /** Call ApplyFunc for each one-ring triangle of a vertex. Currently this is significantly more efficient
         * than VtxTrianglesItr() in many use cases. */
        void EnumerateVertexTriangles( int32_t VertexID, std::function<void( int32_t )> ApplyFunc ) const;

        /** @return a single triangle connected to the given vertex, or INDEX_NONE if the vertex has no triangles
         */
        int32_t GetSingleVertexTriangle( int32_t VID ) const;

        /** Call ApplyFunc for each triangle connected to an Edge (1 or 2 triangles) */
        void EnumerateEdgeTriangles( int32_t EdgeID, const std::function<void( int32_t )>& ApplyFunc ) const;

        //
        // Mesh Construction
        //
    public:
        /** Append vertex at position and other fields, returns vid */
        int AppendVertex( const VertexInfo& VertInfo );

        /** Append vertex at position, returns vid */
        int AppendVertex( const glm::dvec3& Position )
        {
            return AppendVertex( VertexInfo( Position ) );
        }

        /** Copy vertex SourceVertexID from existing SourceMesh, returns new vertex id */
        int AppendVertex( const DynamicMesh3& SourceMesh, int SourceVertexID );

        /** TriVertices must be distinct and refer to existing, valid vertices */
        int AppendTriangle( const Index3i& TriVertices, int GroupID = 0 );

        /** Vertex0, Vertex1, and Vertex2 must be distinct and refer to existing, valid vertices */
        int AppendTriangle( int Vertex0, int Vertex1, int Vertex2, int GroupID = 0 )
        {
            return AppendTriangle( Index3i( Vertex0, Vertex1, Vertex2 ), GroupID );
        }

        //
        // Support for inserting vertex and triangle at specific IDs. This is a bit tricky
        // because we likely will need to update the free lists in the RefCountVectors, which
        // can be expensive. If you are going to do many inserts (eg inside a loop), wrap in
        // BeginUnsafe / EndUnsafe calls, and pass bUnsafe = true to the InsertX() calls, to
        // the defer free list rebuild until you are done.
        //

        /** Call this before a set of unsafe InsertVertex() calls */
        virtual void BeginUnsafeVerticesInsert()
        {
            // do nothing...
        }

        /** Call after a set of unsafe InsertVertex() calls to rebuild free list */
        virtual void EndUnsafeVerticesInsert()
        {
            m_VertexRefCounts.RebuildFreeList();
        }

        /**
         * Insert vertex at given index, assuming it is unused.
         * If bUnsafe, we use fast id allocation that does not update free list.
         * You should only be using this between BeginUnsafeVerticesInsert() / EndUnsafeVerticesInsert() calls
         */
        MeshResult InsertVertex( int VertexID, const VertexInfo& VertInfo, bool bUnsafe = false );

        /** Call this before a set of unsafe InsertTriangle() calls */
        virtual void BeginUnsafeTrianglesInsert()
        {
            // do nothing...
        }

        /** Call after a set of unsafe InsertTriangle() calls to rebuild free list */
        virtual void EndUnsafeTrianglesInsert()
        {
            m_TriangleRefCounts.RebuildFreeList();
        }

        /**
         * Insert triangle at given index, assuming it is unused.
         * If bUnsafe, we use fast id allocation that does not update free list.
         * You should only be using this between BeginUnsafeTrianglesInsert() / EndUnsafeTrianglesInsert() calls
         */
        MeshResult InsertTriangle( int TriangleID, const Index3i& TriVertices, int GroupID = 0,
                                   bool bUnsafe = false );

        //
        // Vertex/Tri/Edge accessors
        //
    public:
        /** @return the vertex position */
        glm::dvec3 GetVertex( int VertexID ) const
        {
            assert( IsVertex( VertexID ) );
            return m_Vertices[VertexID];
        }

        /** @return the vertex position */
        const glm::dvec3& GetVertexRef( int VertexID ) const
        {
            assert( IsVertex( VertexID ) );
            return m_Vertices[VertexID];
        }

        /**
         * Set vertex position
         * @param bTrackChange if true, ShapeChangeStamp will be incremented (if enabled)
         */
        void SetVertex( int VertexID, const glm::dvec3& vNewPos, bool bTrackChange = true )
        {
            assert( VectorUtil::IsFinite( vNewPos ) );
            assert( IsVertex( VertexID ) );
            if ( VectorUtil::IsFinite( vNewPos ) )
            {
                m_Vertices[VertexID] = vNewPos;
                if ( bTrackChange )
                {
                    UpdateChangeStamps( true, false );
                }
            }
        }

        /** Get extended vertex information */
        bool GetVertex( int VertexID, VertexInfo& VertInfo, bool bWantNormals, bool bWantColors,
                        bool bWantUVs ) const;

        /** Get all vertex information available */
        VertexInfo GetVertexInfo( int VertexID ) const;

        /** @return the valence of a vertex (the number of connected edges) */
        int GetVtxEdgeCount( int VertexID ) const
        {
            return m_VertexRefCounts.IsValid( VertexID ) ? m_VertexEdgeLists.GetCount( VertexID ) : -1;
        }

        /** @return the max valence of all vertices in the mesh */
        int GetMaxVtxEdgeCount() const;

        /** Get triangle vertices */
        Index3i GetTriangle( int TriangleID ) const
        {
            assert( IsTriangle( TriangleID ) );
            return m_Triangles[TriangleID];
        }

        /** Get triangle vertices */
        const Index3i& GetTriangleRef( int TriangleID ) const
        {
            assert( IsTriangle( TriangleID ) );
            return m_Triangles[TriangleID];
        }

        /** Get triangle edges */
        Index3i GetTriEdges( int TriangleID ) const
        {
            assert( IsTriangle( TriangleID ) );
            return m_TriangleEdges[TriangleID];
        }

        /** Get triangle edges */
        const Index3i& GetTriEdgesRef( int TriangleID ) const
        {
            assert( IsTriangle( TriangleID ) );
            return m_TriangleEdges[TriangleID];
        }

        /** Get one of the edges of a triangle */
        int GetTriEdge( int TriangleID, int j ) const
        {
            assert( IsTriangle( TriangleID ) );
            return m_TriangleEdges[TriangleID][j];
        }

        /**  Applies a given function to both TriEdgeIDs which each EdgeID in a given Triangle is associated with
         */
        void
        EnumerateTriEdgeIDsFromTriID( const int                                             TriID,
                                      const std::function<void( MeshTriEdgeID TriEdgeID )>& TriEdgeFunc ) const
        {
            Index3i TriEdges = GetTriEdges( TriID );
            for ( int TriEdgesIndex = 0; TriEdgesIndex <= 2; TriEdgesIndex++ )
            {
                EnumerateTriEdgeIDsFromEdgeID( TriEdges[TriEdgesIndex], TriEdgeFunc );
            }
        }

        /** Find the neighbour triangles of a triangle (any of them might be InvalidID) */
        Index3i GetTriNeighbourTris( int TriangleID ) const;

        /** Get the three vertex positions of a triangle */
        template <typename VecType>
        void GetTriVertices( int TriangleID, VecType& v0, VecType& v1, VecType& v2 ) const
        {
            const Index3i& Triangle = m_Triangles[TriangleID];
            v0                      = m_Vertices[Triangle[0]];
            v1                      = m_Vertices[Triangle[1]];
            v2                      = m_Vertices[Triangle[2]];
        }

        /** Get the position of one of the vertices of a triangle */
        glm::dvec3 GetTriVertex( int TriangleID, int j ) const
        {
            return m_Vertices[m_Triangles[TriangleID][j]];
        }

        /** Get the vertices and triangles of an edge, returned as [v0,v1,t0,t1], where t1 may be InvalidID */
        Edge GetEdge( int EdgeID ) const
        {
            assert( IsEdge( EdgeID ) );
            return m_Edges[EdgeID];
        }

        /** Get the vertices and triangles of an edge, returned as [v0,v1,t0,t1], where t1 may be InvalidID */
        const Edge& GetEdgeRef( int EdgeID ) const
        {
            assert( IsEdge( EdgeID ) );
            return m_Edges[EdgeID];
        }

        /** Get the vertex pair for an edge */
        Index2i GetEdgeV( int EdgeID ) const
        {
            assert( IsEdge( EdgeID ) );
            return m_Edges[EdgeID].Vert;
        }

        /** Get the vertex positions of an edge */
        bool GetEdgeV( int EdgeID, glm::dvec3& a, glm::dvec3& b ) const
        {
            assert( IsEdge( EdgeID ) );

            const Index2i Verts = m_Edges[EdgeID].Vert;

            a = m_Vertices[Verts[0]];
            b = m_Vertices[Verts[1]];

            return true;
        }

        /** Get the triangle pair for an edge. The second triangle may be InvalidID */
        Index2i GetEdgeT( int EdgeID ) const
        {
            assert( IsEdge( EdgeID ) );
            return m_Edges[EdgeID].Tri;
        }

        /** Return edge vertex indices, but oriented based on attached triangle (rather than min-sorted) */
        Index2i GetOrientedBoundaryEdgeV( int EdgeID ) const;

        /** Return (triangle, edge_index) representation for given Edge ID */
        MeshTriEdgeID GetTriEdgeIDFromEdgeID( int EdgeID ) const
        {
            assert( IsEdge( EdgeID ) );
            int32_t const TriIndex = m_Edges[EdgeID].Tri.A;
            Index3i const TriEdges = m_TriangleEdges[TriIndex];
            if ( TriEdges.A == EdgeID )
            {
                return { TriIndex, 0 };
            }
            {
                return { TriIndex, ( TriEdges.B == EdgeID ) ? 1 : 2 };
            }
        }

        /** Applies a given function to both TriEdgeIDs which a given EdgeID is associated with*/
        void
        EnumerateTriEdgeIDsFromEdgeID( const int32_t                                         EdgeID,
                                       const std::function<void( MeshTriEdgeID TriEdgeID )>& TriEdgeFunc ) const
        {
            const MeshTriEdgeID FirstTriEdgeID = GetTriEdgeIDFromEdgeID(
                 EdgeID ); // function gets MeshTriEdgeID for edge included in EdgeTri.A only
            TriEdgeFunc( FirstTriEdgeID );

            // have to get MeshTriEdgeID for edge included in EdgeTri.B
            const int OtherTriID = GetEdgeT( EdgeID ).B;
            if ( OtherTriID != IndexConstants::InvalidID )
            {
                MeshTriEdgeID SecondTriEdgeID;
                const Index3i SecondTriEdges = GetTriEdges( OtherTriID );
                if ( SecondTriEdges.A == EdgeID )
                {
                    SecondTriEdgeID = MeshTriEdgeID( OtherTriID, 0 );
                }
                else
                {
                    SecondTriEdgeID = MeshTriEdgeID( OtherTriID, ( SecondTriEdges.B == EdgeID ) ? 1 : 2 );
                }
                TriEdgeFunc( SecondTriEdgeID );
            }
        }

        //
        // Vertex and Triangle attribute arrays
        //
    public:
        /**
         * Enable requested set of mesh components (triangle groups and vertex normals/colors/UVs)
         * and discard any that are not requested
         * @param MeshComponentsFlags A 'bitwise or' of requested MeshComponents flags
         */
        void EnableMeshComponents( int MeshComponentsFlags );

        void EnableVertexNormals( const glm::vec3& InitialNormal );
        void DiscardVertexNormals();

        glm::vec3 GetVertexNormal( int vID ) const
        {
            if ( !m_VertexNormals.has_value() )
            {
                return { 0, 1, 0 };
            }
            assert( IsVertex( vID ) );
            return ( *m_VertexNormals )[vID];
        }

        void SetVertexNormal( int vID, const glm::vec3& vNewNormal )
        {
            if ( m_VertexNormals.has_value() )
            {
                assert( IsVertex( vID ) );
                ( *m_VertexNormals )[vID] = vNewNormal;
            }
        }

        void EnableVertexColors( const glm::vec3& InitialColor );
        void DiscardVertexColors();

        glm::vec3 GetVertexColor( int vID ) const
        {
            if ( !m_VertexColors.has_value() )
            {
                return glm::vec3( 1 );
            }
            assert( IsVertex( vID ) );
            return ( *m_VertexColors )[vID];
        }

        void SetVertexColor( int vID, const glm::vec3& vNewColor )
        {
            if ( m_VertexColors.has_value() )
            {
                assert( IsVertex( vID ) );
                ( *m_VertexColors )[vID] = vNewColor;
            }
        }

        void EnableVertexUVs( const glm::vec2& InitialUV );
        void DiscardVertexUVs();

        glm::vec2 GetVertexUV( int vID ) const
        {
            if ( !m_VertexUVs.has_value() )
            {
                return glm::vec2( 0 );
            }
            assert( IsVertex( vID ) );
            return ( *m_VertexUVs )[vID];
        }

        void SetVertexUV( int vID, const glm::vec2& vNewUV )
        {
            if ( m_VertexUVs.has_value() )
            {
                assert( IsVertex( vID ) );
                ( *m_VertexUVs )[vID] = vNewUV;
            }
        }

        void EnableTriangleGroups( int InitialGroupID = 0 );
        void DiscardTriangleGroups();

        int AllocateTriangleGroup()
        {
            return m_GroupIDCounter++;
        }

        int GetTriangleGroup( int tID ) const
        {
            if ( !m_TriangleGroups.has_value() )
            {
                return -1;
            }
            if ( !m_TriangleRefCounts.IsValid( tID ) )
            {
                return 0;
            }
            return ( *m_TriangleGroups )[tID];
        }

        void SetTriangleGroup( int tid, int group_id )
        {
            if ( m_TriangleGroups.has_value() )
            {
                assert( IsTriangle( tid ) );
                ( *m_TriangleGroups )[tid]    = group_id;
                m_GroupIDCounter              = std::max( m_GroupIDCounter, group_id + 1 );
            }
        }

        //
        // topological queries
        //
    public:
        /** Returns true if edge is on the mesh boundary, ie only connected to one triangle */
        bool IsBoundaryEdge( int EdgeID ) const
        {
            assert( IsEdge( EdgeID ) );
            return m_Edges[EdgeID].Tri[1] == InvalidID;
        }

        /** Returns true if the vertex is part of any boundary edges */
        bool IsBoundaryVertex( int VertexID ) const;

        /** Returns true if any edge of triangle is a boundary edge */
        bool IsBoundaryTriangle( int TriangleID ) const;

        /** Find id of edge connecting A and B */
        int FindEdge( int VertexA, int VertexB ) const;

        /** Find edgeid for edge [a,b] from triangle that contains the edge. Faster than FindEdge() because it is
         * constant-time. */
        int FindEdgeFromTri( int VertexA, int VertexB, int TriangleID ) const;

        /** Find edgeid for edge connecting two triangles */
        int FindEdgeFromTriPair( int TriangleA, int TriangleB ) const;

        /** Find triangle made up of any permutation of vertices [a,b,c] */
        int FindTriangle( int A, int B, int C ) const;

        /**
         * If edge has vertices [a,b], and is connected two triangles [a,b,c] and [a,b,d],
         * this returns [c,d], or [c,InvalidID] for a boundary edge
         */
        Index2i GetEdgeOpposingV( int EdgeID ) const;

        /**
         * Given an edge and vertex on that edge, returns other vertex of edge, the two opposing verts, and the two
         * connected triangles (OppVert2Out and Tri2Out are be InvalidID for boundary edge)
         */
        void GetVtxNbrhood( int EdgeID, int VertexID, int& OtherVertOut, int& OppVert1Out, int& OppVert2Out,
                            int& Tri1Out, int& Tri2Out ) const;

        /**
         * Returns count of boundary edges at vertex, and the first two boundary
         * edges if found. If return is > 2, call GetAllVtxBoundaryEdges
         */
        int GetVtxBoundaryEdges( int VertexID, int& Edge0Out, int& Edge1Out ) const;

        /**
         * Find edge ids of boundary edges connected to vertex.
         * @param vID Vertex ID
         * @param EdgeListOut boundary edge IDs are appended to this list
         * @return count of number of elements of e that were filled
         * Note: ArrayType must by std::vector<int> or LocalIntArray
         */
        template <typename ArrayType = LocalIntArray>
        int GetAllVtxBoundaryEdges( int VertexID, ArrayType& EdgeListOut ) const;

        /**
         * return # of triangles attached to vID, or -1 if invalid vertex
         */
        int GetVtxTriangleCount( int VertexID ) const;

        /**
         * Get triangle one-ring at vertex.
         * Note: ArrayType must by std::vector<int> or LocalIntArray
         */
        template <typename ArrayType = LocalIntArray>
        MeshResult GetVtxTriangles( int VertexID, ArrayType& TrianglesOut ) const;

        /**
         * @return Triangle ID for a single triangle connected to VertexID, or InvalidID if VertexID does not exist
         * or has no attached triangles
         */
        int GetVtxSingleTriangle( int VertexID ) const;

        /**
         * Get triangles connected to vertex in contiguous order, with multiple groups if vertex is a bowtie.
         * @param VertexID Vertex ID to search around
         * @param TrianglesOut All triangles connected to the vertex, in contiguous order; if there are multiple
         * contiguous groups they are packed one after another
         * @param ContiguousGroupLengths Lengths of contiguous groups packed into TrianglesOut (if not a bowtie,
         * this will just be a length-one array w/ {TrianglesOut.Num()})
         * @param GroupIsLoop Indicates whether each contiguous group is a loop (first triangle connected to last)
         * or not Note: ArrayTypes must by std::vector<int>/<bool> or LocalIntArray/FLocalBoolArry
         */
        template <typename IntArrayType = LocalIntArray, typename BoolArrayType = LocalBoolArray>
        MeshResult GetVtxContiguousTriangles( int VertexID, IntArrayType& TrianglesOut,
                                              IntArrayType&  ContiguousGroupLengths,
                                              BoolArrayType& GroupIsLoop ) const;

        /** Returns true if the two triangles connected to edge have different group IDs */
        bool IsGroupBoundaryEdge( int EdgeID ) const;

        /** Returns true if vertex has more than one tri group in its tri nbrhood */
        bool IsGroupBoundaryVertex( int VertexID ) const;

        /** Returns true if more than two group boundary edges meet at vertex (ie 3+ groups meet at this vertex) */
        bool IsGroupJunctionVertex( int VertexID ) const;

        /** Returns up to 4 group IDs at vertex. Returns false if > 4 encountered */
        bool GetVertexGroups( int VertexID, Index4i& GroupsOut ) const;

        /** Returns all group IDs at vertex. ArrayType must by std::vector<int> or LocalIntArray */
        template <typename ArrayType = LocalIntArray>
        bool GetAllVertexGroups( int VertexID, ArrayType& GroupsOut ) const;

        /** returns true if vID is a "bowtie" vertex, ie multiple disjoint triangle sets in one-ring */
        bool IsBowtieVertex( int VertexID ) const;

        /** returns true if vertices, edges, and triangles are all dense (Count == MaxID) **/
        bool IsCompact() const
        {
            return m_VertexRefCounts.IsDense() && m_EdgeRefCounts.IsDense() && m_TriangleRefCounts.IsDense();
        }

        /** @return true if vertex count == max vertex id */
        bool IsCompactV() const
        {
            return m_VertexRefCounts.IsDense();
        }

        /** @return true if triangle count == max triangle id */
        bool IsCompactT() const
        {
            return m_TriangleRefCounts.IsDense();
        }

        /** returns measure of compactness in range [0,1], where 1 is fully compacted */
        double CompactMetric() const
        {
            return ( static_cast<double>( VertexCount() ) / static_cast<double>( MaxVertexID() ) +
                     static_cast<double>( TriangleCount() ) / static_cast<double>( MaxTriangleID() ) ) *
                   0.5;
        }

        /** @return true if mesh has no boundary edges */
        bool IsClosed() const;

        //
        // Geometric queries
        //
    public:
        /** Returns bounding box of all mesh vertices (including unreferenced vertices) */
        AxisAlignedBox3d GetBounds() const;

        /** Returns bounding box of all selected mesh vertices. Will use a chunked parallel implementation for
         * larger selections. */
        AxisAlignedBox3d GetBoundsForVertexSelection( std::span<const int32_t> VertexIDs ) const;

        /** Returns bounding box of all selected mesh triangles. Will use a chunked parallel implementation for
         * larger selections. */
        AxisAlignedBox3d GetBoundsForTriangleSelection( std::span<const int32_t> TriangleIDs ) const;

        /** Calculate face normal of triangle */
        glm::dvec3 GetTriNormal( int TriangleID ) const;

        /** Calculate area triangle */
        double GetTriArea( int TriangleID ) const;

        /**
         * Compute triangle normal, area, and centroid all at once. Re-uses vertex
         * lookups and computes normal & area simultaneously. *However* does not produce
         * the same normal/area as separate calls, because of this.
         */
        void GetTriInfo( int TriangleID, glm::dvec3& Normal, double& Area, glm::dvec3& Centroid ) const;

        /** Compute centroid of triangle */
        glm::dvec3 GetTriCentroid( int TriangleID ) const;

        /** Interpolate vertex positions of triangle using barycentric coordinates */
        glm::dvec3 GetTriBaryPoint( int TriangleID, double Bary0, double Bary1, double Bary2 ) const;

        /** Interpolate vertex normals of triangle using barycentric coordinates */
        glm::dvec3 GetTriBaryNormal( int TriangleID, double Bary0, double Bary1, double Bary2 ) const;

        /** Compute interpolated vertex attributes at point of triangle */
        void GetTriBaryPoint( int TriangleID, double Bary0, double Bary1, double Bary2,
                              VertexInfo& VertInfo ) const;

        /** Construct bounding box of triangle as efficiently as possible */
        AxisAlignedBox3d GetTriBounds( int TriangleID ) const;

        /** Compute solid angle of oriented triangle tID relative to point p - see WindingNumber() */
        double GetTriSolidAngle( int TriangleID, const glm::dvec3& p ) const;

        /** Compute internal angle at vertex i of triangle (where i is 0,1,2); */
        double GetTriInternalAngleR( int TriangleID, int i ) const;

        /** Compute internal angles at all vertices of triangle */
        glm::dvec3 GetTriInternalAnglesR( int TriangleID ) const;

        /** Returns average normal of connected face normals */
        glm::dvec3 GetEdgeNormal( int EdgeID ) const;

        /** Get point along edge, t clamped to range [0,1] */
        glm::dvec3 GetEdgePoint( int EdgeID, double ParameterT ) const;

        /**
         * Fastest possible one-ring centroid. This is used inside many other algorithms
         * so it helps to have it be maximally efficient
         */
        void GetVtxOneRingCentroid( int VertexID, glm::dvec3& CentroidOut ) const;

        /**
         * Compute mesh winding number, from Jacobson et. al., Robust Inside-Outside Segmentation using Generalized
         * Winding Numbers http://igl.ethz.ch/projects/winding-number/ returns ~0 for points outside a closed,
         * consistently oriented mesh, and a positive or negative integer for points inside, with value > 1
         * depending on how many "times" the point inside the mesh (like in 2D polygon winding)
         */
        double CalculateWindingNumber( const glm::dvec3& QueryPoint ) const;

        //
        // direct buffer access
        //
    public:
        const DynamicVector<glm::dvec3>& GetVerticesBuffer() const
        {
            return m_Vertices;
        }
        const RefCountVector& GetVerticesRefCounts() const
        {
            return m_VertexRefCounts;
        }
        const DynamicVector<glm::vec3>* GetNormalsBuffer() const
        {
            return m_VertexNormals.has_value() ? &*m_VertexNormals : nullptr;
        }
        const DynamicVector<glm::vec3>* GetColorsBuffer() const
        {
            return m_VertexColors.has_value() ? &*m_VertexColors : nullptr;
        }
        const DynamicVector<glm::vec2>* GetUVBuffer() const
        {
            return m_VertexUVs.has_value() ? &*m_VertexUVs : nullptr;
        }
        const DynamicVector<Index3i>& GetTrianglesBuffer() const
        {
            return m_Triangles;
        }
        const RefCountVector& GetTrianglesRefCounts() const
        {
            return m_TriangleRefCounts;
        }
        const DynamicVector<int>* GetTriangleGroupsBuffer() const
        {
            return m_TriangleGroups.has_value() ? &*m_TriangleGroups : nullptr;
        }
        const DynamicVector<Edge>& GetEdgesBuffer() const
        {
            return m_Edges;
        }
        const RefCountVector& GetEdgesRefCounts() const
        {
            return m_EdgeRefCounts;
        }
        const SmallListSet& GetVertexEdges() const
        {
            return m_VertexEdgeLists;
        }
        const DynamicVector<Index3i>& GetTriangleEdges() const
        {
            return m_TriangleEdges;
        }
        //
        // Mesh Edit operations
        //
    public:
        /**
         * Compact mesh in-place, by moving vertices around and rewriting indices.
         * Should be faster if the amount of compacting is not too significant, and is useful in some places.
         *
         * @param CompactInfo if not nullptr, will be filled with mapping indicating how vertex and triangle IDs
         * were changed during compaction
         */
        void CompactInPlace( DynamicMeshCompactMaps* CompactInfo = nullptr );

        /**
         * Remove unused vertices. Note: Does not compact the remaining vertices.
         * @return number of removed vertices
         */
        int32_t RemoveUnusedVertices();

        /**
         * @return true if any vertices are unused (not in any triangles)
         * */
        bool HasUnusedVertices() const;

        /**
         * Reverse the ccw/cw orientation of all triangles in the mesh, and
         * optionally flip the vertex normals if they exist
         */
        void ReverseOrientation( bool bFlipNormals = true );

        /**
         * Reverse the ccw/cw orientation of a triangle
         */
        MeshResult ReverseTriOrientation( int TriangleID );

        /**
         * Remove vertex VertexID and all connected triangles.
         * Returns Failed_VertexStillReferenced if VertexID is still referenced by any triangles.
         * If bPreserveManifold is true, checks that we will not create a bowtie vertex first.
         * In this case, returns Failed_WouldCreateBowtie if removing the triangles would create a bowtie.
         */
        MeshResult RemoveVertex( int VertexID, bool bPreserveManifold = false );

        /**
         * Remove a triangle from the mesh. Also removes any unreferenced edges after tri is removed.
         * If bRemoveIsolatedVertices is true, then if you remove all tris from a vert, that vert is also removed.
         * If bPreserveManifold, we check that you will not create a bow tie vertex (and return false).
         * If this check is not done, you have to make sure you don't create a bow tie, because other
         * code assumes we don't have bow ties, and will not handle it properly
         */
        MeshResult RemoveTriangle( int TriangleID, bool bRemoveIsolatedVertices = true,
                                   bool bPreserveManifold = false );

        /**
         * Rewrite the triangle to reference the new tuple of vertices.
         *
         * @todo this function currently does not guarantee that the returned mesh is well-formed. Only call if you
         * know it's OK.
         */
        virtual MeshResult SetTriangle( int TriangleID, const Index3i& NewVertices, bool bRemoveIsolatedVertices );

    public:
        using EdgeFlipInfo      = DynamicMeshInfo::EdgeFlipInfo;
        using EdgeSplitInfo     = DynamicMeshInfo::EdgeSplitInfo;
        using EdgeCollapseInfo  = DynamicMeshInfo::EdgeCollapseInfo;
        using MergeEdgesInfo    = DynamicMeshInfo::MergeEdgesInfo;
        using MergeVerticesInfo = DynamicMeshInfo::MergeVerticesInfo;
        using PokeTriangleInfo  = DynamicMeshInfo::PokeTriangleInfo;
        using VertexSplitInfo   = DynamicMeshInfo::VertexSplitInfo;

        /**
         * Split an edge of the mesh by inserting a vertex. This creates a new triangle on either side of the edge
         * (ie a 2-4 split). If the original edge had vertices [a,b], with triangles t0=[a,b,c] and t1=[b,a,d],
         * then the split inserts new vertex f. After the split t0=[a,f,c] and t1=[f,a,d], and we have t2=[f,b,c]
         * and t3=[f,d,b]  (it's best to draw it out on paper...)
         *
         * @param EdgeAB index of the edge to be split
         * @param SplitInfo returned information about new and modified mesh elements
         * @param SplitParameterT defines the position along the edge that we split at, must be between 0 and 1,
         * and is assumed to be based on the order of vertices returned by GetEdgeV()
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult SplitEdge( int EdgeAB, EdgeSplitInfo& SplitInfo, double SplitParameterT );

        /**
         * Splits the edge between two vertices at the midpoint, if this edge exists
         * @param EdgeVertA index of first vertex
         * @param EdgeVertB index of second vertex
         * @param SplitInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        MeshResult SplitEdge( int EdgeVertA, int EdgeVertB, EdgeSplitInfo& SplitInfo );

        /**
         * Flip/Rotate an edge of the mesh. This does not change the number of edges, vertices, or triangles.
         * Boundary edges of the mesh cannot be flipped.
         *
         * On success, the edges of the new triangles are ordered such that the shared edge is on index 0. i.e.,
         * GetTriEdge(FlipInfo.Triangles[0], 0) == GetTriEdge(FlipInfo.Triangles[1], 0)
         *
         * @param EdgeAB index of edge to be flipped
         * @param FlipInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult FlipEdge( int EdgeAB, EdgeFlipInfo& FlipInfo );

        /** calls FlipEdge() on the edge between two vertices, if it exists
         * @param EdgeVertA index of first vertex
         * @param EdgeVertB index of second vertex
         * @param FlipInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult FlipEdge( int EdgeVertA, int EdgeVertB, EdgeFlipInfo& FlipInfo );

        /**
         * Clones the given vertex and updates any provided triangles to use the new vertex if/where they used the
         * old one.
         * @param VertexID the vertex to split
         * @param TrianglesToUpdate triangles that should be updated to use the new vertex anywhere they previously
         * had the old one
         * @param SplitInfo returned info about the new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult SplitVertex( int VertexID, const std::span<const int>& TrianglesToUpdate,
                                        VertexSplitInfo& SplitInfo );

        /**
         * Tests whether splitting the given vertex with the given triangles would leave no triangles attached to
         * the original vertex (creating an isolated vertex)
         * @param VertexID the vertex to split
         * @param TrianglesToUpdate triangles that should be updated to use the new vertex anywhere they previously
         * had the old one
         * @return true if calling SplitVertex with these arguments would leave an isolated vertex at the original
         * VertexID
         */
        virtual bool SplitVertexWouldLeaveIsolated( int VertexID, const std::span<const int>& TrianglesToUpdate );

        struct CollapseEdgeOptions
        {
            /**
             * When false, collapse is disallowed if the edge is the boundary of a single triangle hole,
             *  such that collapsing it would clse the hole. I.e. collapse is dissallowed if there
             *  is some vertex that connects vKeep and vRemove that is not part of the triangle(s) being
             *  collapsed (note that if such a vertex is connected by a non-boundary edge, collapse will
             *  always be disallowed regardless of bAllowHoleCollapse, as it would create non-manifold geometry).
             */
            bool bAllowHoleCollapse = false;
            /**
             * When false, collapse is disallowed if the edge is an interior edge, yet both vertices are
             *  connected to boundary edges. In some circumstances this could create a bowtie. In other
             *  cases, it could disconnected parts of a mesh that were connected by a bowtie.
             */
            bool bAllowCollapsingInternalEdgeWithBoundaryVertices = false;
            /**
             * When false, collapse is disallowed if we are collapsing the side of a tetrahedron. Note
             *  that a base edge of an open-base tetrahedron could be collapsed even if bAllowTetrahedronCollapse
             *  is false if bAllowHoleCollapse is true.
             */
            bool bAllowTetrahedronCollapse = false;
        };
        virtual MeshResult CanCollapseEdge( int vKeep, int vRemove, const CollapseEdgeOptions& Options ) const;

        /**
         * Tests whether collapsing the specified edge using the CollapseEdge function would succeed.
         *  Equivalent to calling the options overload with default options.
         * @param KeepVertID index of the vertex that should be kept
         * @param RemoveVertID index of the vertex that should be removed
         * @param EdgeParameterT vKeep is moved to Lerp(KeepPos, RemovePos, EdgeParameterT). Note: Does not
         * currently affect whether the edge is collapsable.
         * @return Ok if the edge can be collapsed, or enum value indicating why the operation cannot be applied
         */
        virtual MeshResult CanCollapseEdge( int vKeep, int vRemove, double EdgeParameterT ) const;

        /**
         * Collapse the edge between the two vertices, if topologically possible.
         * @param KeepVertID index of the vertex that should be kept
         * @param RemoveVertID index of the vertex that should be removed
         * @param EdgeParameterT vKeep is moved to Lerp(KeepPos, RemovePos, EdgeParameterT)
         * @param Options Sets options for the collapse
         * @param CollapseInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult CollapseEdge( int KeepVertID, int RemoveVertID, double EdgeParameterT,
                                         const CollapseEdgeOptions& Options, EdgeCollapseInfo& CollapseInfo );

        /**
         * Collapse the edge between the two vertices, if topologically possible. Equivalent to
         *  using the other overload with 0 for EdgeParameterT.
         */
        virtual MeshResult CollapseEdge( int KeepVertID, int RemoveVertID, const CollapseEdgeOptions& Options,
                                         EdgeCollapseInfo& CollapseInfo )
        {
            return CollapseEdge( KeepVertID, RemoveVertID, 0, Options, CollapseInfo );
        }

        /**
         * Collapse the edge between the two vertices, if topologically possible. Equivalent to calling
         *  the options overload with default options.
         * @param KeepVertID index of the vertex that should be kept
         * @param RemoveVertID index of the vertex that should be removed
         * @param EdgeParameterT vKeep is moved to Lerp(KeepPos, RemovePos, EdgeParameterT)
         * @param CollapseInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult CollapseEdge( int KeepVertID, int RemoveVertID, double EdgeParameterT,
                                         EdgeCollapseInfo& CollapseInfo );
        /**
         * Collapse the edge between the two vertices, if topologically possible. Equivalent to calling
         *  the options overload with default options and using 0 for EdgeParameterT.
         */
        virtual MeshResult CollapseEdge( int KeepVertID, int RemoveVertID, EdgeCollapseInfo& CollapseInfo )
        {
            return CollapseEdge( KeepVertID, RemoveVertID, 0, CollapseInfo );
        }

        /**
         * Given two edges of the mesh, weld both their vertices, so that one edge is removed.
         * This could result in one neighbour edge-pair attached to each vertex also collapsing,
         * so those cases are detected and handled (eg middle edge-pair in abysmal ascii drawing below)
         *
         *   ._._._.    (dots are vertices)
         *    \._./
         *
         * @param KeepEdgeID index of the edge that should be kept
         * @param DiscardEdgeID index of the edge that should be removed
         * @param InterpolationT each kept vertex is moved to Lerp(KeptPos, RemovePos, InterpolationT)
         * @param MergeInfo returned information about new and modified mesh elements
         * @param CheckValidOrientation perform edge consistency orientation checks before merging. Specifically,
         *  check that each discarded vertex is closer to the vertex it is being collapsed to than the other
         *  kept vertex (where the pairing is determined by the adjoining triangle winding).
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult MergeEdges( int KeepEdgeID, int DiscardEdgeID, double InterpolationT,
                                       MergeEdgesInfo& MergeInfo, bool bCheckValidOrientation );

        /**
         * Weld one edge to the other. Equivalent to calling the other overload with 0 for InterpolationT
         *  (i.e. the vertices stay at unmodified kept vertex positions).
         */
        virtual MeshResult MergeEdges( int KeepEdgeID, int DiscardEdgeID, MergeEdgesInfo& MergeInfo,
                                       bool bCheckValidOrientation );

        struct MergeVerticesOptions
        {
            // If false, we disallow vertex merges that attempt to merge one non-boundary vert to a
            //  a non-adjacent vert, even if this is possible through a bowtie. Note that merging
            //  boundary verts to create a bowtie on the boundary is still allowed, as this is a
            //  common intermediate step when welding edges.
            bool bAllowNonBoundaryBowtieCreation = false;
        };

        /**
         * Weld DiscardVid to KeepVid, if topologically possible and options allow. If the vertices are connected
         *  by an existing edge, this resolves as a collapse of that edge, and therefore calls OnCollapseEdge in
         *  overlays. If not, but the two vertices share a vertex neighbor, this resolves as a weld of the
         * intervening edges (failing if the edges are not boundary edges, since that would create non-manifold
         * edge), and therefore calls OnMergeEdges in the overlays. Otherwise, the merge resolves as bowtie
         * creation, and calls OnMergeVertices in the overlays.
         * @param KeepVid vertex ID of the kept vertex
         * @param DiscardVid vertex ID of the vertex whose triangles are reattached to the kept vertex
         * @param InterpolationT the kept vertex is moved to Lerp(KeptPos, RemovePos, InterpolationT)
         * @param Options set the options for the merge
         * @param MergeInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult MergeVertices( int KeepVid, int DiscardVid, double InterpolationT,
                                          const MergeVerticesOptions& Options, MergeVerticesInfo& MergeInfo );

        /**
         * Weld DiscardVid to KeepVid. Equivalent to calling the options overload with default options.
         */
        virtual MeshResult MergeVertices( int KeepVid, int DiscardVid, double InterpolationT,
                                          MergeVerticesInfo& MergeInfo )
        {
            return MergeVertices( KeepVid, DiscardVid, InterpolationT, MergeVerticesOptions(), MergeInfo );
        }

        /**
         * Weld DiscardVid to KeepVid. Equivalent to calling the options overload with default options and 0 for
         * InterpolationT.
         */
        virtual MeshResult MergeVertices( int KeepVid, int DiscardVid, MergeVerticesInfo& MergeInfo )
        {
            return MergeVertices( KeepVid, DiscardVid, 0, MergeVerticesOptions(), MergeInfo );
        }

        /**
         * Insert a new vertex inside a triangle, ie do a 1 to 3 triangle split
         * @param TriangleID index of triangle to poke
         * @param BaryCoordinates barycentric coordinates of poke position
         * @param PokeInfo returned information about new and modified mesh elements
         * @return Ok on success, or enum value indicates why operation cannot be applied. Mesh remains unmodified
         * on error.
         */
        virtual MeshResult PokeTriangle( int TriangleID, const glm::dvec3& BaryCoordinates,
                                         PokeTriangleInfo& PokeInfo );

        /** Call PokeTriangle at the centroid of the triangle */
        virtual MeshResult PokeTriangle( int TriangleID, PokeTriangleInfo& PokeInfo )
        {
            return PokeTriangle( TriangleID, glm::dvec3( 1 ) / 3.0, PokeInfo );
        }

    public:
        size_t GetByteCount() const;

    public:
        /**
         * Options for what the validity check will permit
         */
        struct ValidityOptions
        {
            bool bAllowNonManifoldVertices             = false;
            bool bAllowAdjacentFacesReverseOrientation = false;

            /**
             * Construct validity checking options
             */
            ValidityOptions( bool bAllowNonManifoldVertices             = false,
                             bool bAllowAdjacentFacesReverseOrientation = false )
                 : bAllowNonManifoldVertices( bAllowNonManifoldVertices ),
                   bAllowAdjacentFacesReverseOrientation( bAllowAdjacentFacesReverseOrientation )
            {
            }

            /**
             * Construct with most-permissive options that we still consider valid for processing
             */
            static ValidityOptions Permissive()
            {
                ValidityOptions ToRet;
                ToRet.bAllowAdjacentFacesReverseOrientation = true;
                ToRet.bAllowNonManifoldVertices             = true;
                return ToRet;
            }
        };

        /**
         * Checks that the mesh is well-formed, ie all internal data structures are consistent
         */
        virtual bool CheckValidity( ValidityOptions Options, ValidityCheckFailMode FailMode ) const;

        //
        // Internal functions
        //
    protected:
        void SetTriangleInternal( int TriangleID, int v0, int v1, int v2 )
        {
            m_Triangles[TriangleID] = Index3i( v0, v1, v2 );
        }
        void SetTriangleEdgesInternal( int TriangleID, int e0, int e1, int e2 )
        {
            m_TriangleEdges[TriangleID] = Index3i( e0, e1, e2 );
        }

        int AddEdgeInternal( int vA, int vB, int tA, int tB = InvalidID )
        {
            if ( vB < vA )
            {
                int const t = vB;
                vB    = vA;
                vA    = t;
            }
            int const eid = m_EdgeRefCounts.Allocate();
            m_Edges.InsertAt( Edge{ { vA, vB }, { tA, tB } }, eid );
            m_VertexEdgeLists.Insert( vA, eid );
            m_VertexEdgeLists.Insert( vB, eid );
            return eid;
        }
        int AddTriangleInternal( int a, int b, int c, int e0, int e1, int e2 );

        int ReplaceTriangleVertex( int TriangleID, int vOld, int vNew )
        {
            Index3i& Triangle = m_Triangles[TriangleID];
            for ( int const i : { 0, 1, 2 } )
            {
                if ( Triangle[i] == vOld )
                {
                    Triangle[i] = vNew;
                    return 0;
                }
            }
            return -1;
        }

        void AllocateEdgesList( int VertexID )
        {
            if ( VertexID < static_cast<int>( m_VertexEdgeLists.Size() ) )
            {
                m_VertexEdgeLists.Clear( VertexID );
            }
            m_VertexEdgeLists.AllocateAt( VertexID );
        }

        template <typename ArrayType = LocalIntArray>
        void GetVertexEdgesList( int VertexID, ArrayType& EdgesOut ) const
        {
            for ( int const eid : m_VertexEdgeLists.Values( VertexID ) )
            {
                EdgesOut.push_back( eid );
            }
        }

        void SetEdgeVerticesInternal( int EdgeID, int a, int b )
        {
            if ( a > b )
            {
                std::swap( a, b );
            }
            m_Edges[EdgeID].Vert[0] = a;
            m_Edges[EdgeID].Vert[1] = b;
        }

        void SetEdgeTrianglesInternal( int EdgeID, int t0, int t1 )
        {
            m_Edges[EdgeID].Tri[0] = t0;
            m_Edges[EdgeID].Tri[1] = t1;
        }

        int ReplaceEdgeVertex( int EdgeID, int vOld, int vNew );
        int ReplaceEdgeTriangle( int EdgeID, int tOld, int tNew );
        int ReplaceTriangleEdge( int TriangleID, int eOld, int eNew );

        bool TriangleHasVertex( int TriangleID, int VertexID ) const
        {
            return m_Triangles[TriangleID][0] == VertexID || m_Triangles[TriangleID][1] == VertexID ||
                   m_Triangles[TriangleID][2] == VertexID;
        }

        bool TriHasNeighbourTri( int CheckTriID, int NbrTriID ) const
        {
            return EdgeHasTriangle( m_TriangleEdges[CheckTriID][0], NbrTriID ) ||
                   EdgeHasTriangle( m_TriangleEdges[CheckTriID][1], NbrTriID ) ||
                   EdgeHasTriangle( m_TriangleEdges[CheckTriID][2], NbrTriID );
        }

        bool TriHasSequentialVertices( int TriangleID, int vA, int vB ) const
        {
            const Index3i& Tri = m_Triangles[TriangleID];
            return ( ( Tri.A == vA && Tri.B == vB ) || ( Tri.B == vA && Tri.C == vB ) ||
                     ( Tri.C == vA && Tri.A == vB ) );
        }

        int FindTriangleEdge( int TriangleID, int vA, int vB ) const;

        int32_t FindEdgeInternal( int32_t vA, int32_t vB, bool& bIsBoundary ) const;

        bool EdgeHasVertex( int EdgeID, int VertexID ) const
        {
            const Index2i Verts = m_Edges[EdgeID].Vert;
            return ( Verts[0] == VertexID ) || ( Verts[1] == VertexID );
        }
        bool EdgeHasTriangle( int EdgeID, int TriangleID ) const
        {
            const Index2i Tris = m_Edges[EdgeID].Tri;
            return ( Tris[0] == TriangleID ) || ( Tris[1] == TriangleID );
        }

        int GetOtherEdgeVertex( int EdgeID, int VertexID ) const
        {
            const Index2i Verts = m_Edges[EdgeID].Vert;
            return ( Verts[0] == VertexID ) ? Verts[1] : ( ( Verts[1] == VertexID ) ? Verts[0] : InvalidID );
        }
        int GetOtherEdgeTriangle( int EdgeID, int TriangleID ) const
        {
            const Index2i Tris = m_Edges[EdgeID].Tri;
            return ( Tris[0] == TriangleID ) ? Tris[1] : ( ( Tris[1] == TriangleID ) ? Tris[0] : InvalidID );
        }

        void AddTriangleEdge( int TriangleID, int v0, int v1, int j, int EdgeID )
        {
            Index3i& TriEdges =
                 m_TriangleEdges.ElementAt( TriangleID, Index3i( InvalidID, InvalidID, InvalidID ) );
            if ( EdgeID != InvalidID )
            {
                m_Edges[EdgeID].Tri[1] = TriangleID;
                TriEdges[j]          = EdgeID;
            }
            else
            {
                TriEdges[j] = AddEdgeInternal( v0, v1, TriangleID );
            }
        }

        // utility function that returns one or two triangles of edge, used to enumerate vertex one-ring triangles
        // The logic is a bit tricky to follow without drawing it out on paper, but this will only return
        // each triangle once, for the 'outgoing' edge from the vertex, and each triangle only has one such edge
        // at any vertex (including boundary triangles)
        Index2i GetOrderedOneRingEdgeTris( int VertexID, int EdgeID ) const
        {
            const Index2i Tris = m_Edges[EdgeID].Tri;

            int const vOther = GetOtherEdgeVertex( EdgeID, VertexID );
            int et1    = Tris[1];
            et1     = ( et1 != InvalidID && TriHasSequentialVertices( et1, VertexID, vOther ) ) ? et1 : InvalidID;
            int const et0    = Tris[0];
            return TriHasSequentialVertices( et0, VertexID, vOther ) ? Index2i( et0, et1 )
                                                                     : Index2i( et1, InvalidID );
        }

        void ReverseTriOrientationInternal( int TriangleID );

    protected:
        /* We keep this version of CanCollapseEdge internal because the CollapseInfo struct may only be partially
         * filled out by the function */
        virtual MeshResult CanCollapseEdgeInternal( int vKeep, int vRemove, double collapse_t,
                                                    EdgeCollapseInfo* OutCollapseInfo ) const;

    private:
        virtual MeshResult CanCollapseEdgeInternal( int vKeep, int vRemove, double collapse_t,
                                                    const CollapseEdgeOptions& Options,
                                                    EdgeCollapseInfo*          OutCollapseInfo ) const;
    };

} // namespace Desert::Geometry
