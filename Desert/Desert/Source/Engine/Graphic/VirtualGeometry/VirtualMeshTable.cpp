#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>

#include <Engine/Geometry/ClusterDag.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/GeometryPool.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Desert::Graphic::VirtualGeometry
{
    static_assert( VirtualMeshTable::kRetireFrames == GeometryPool::kRetireFrames,
                   "a row retires on the frames its pool spans retire" );
    static_assert( kInvalidIndex == Geometry::kClusterHierarchyNone, "an empty hierarchy child is the same word" );
    static_assert( kHierarchyLeaf == Geometry::kClusterHierarchyLeaf, "the leaf bit is the same bit" );

    namespace
    {
        constexpr uint32_t kMinCapacityRows = 64u;

        // A first-fit row allocator with coalescing; it grows at the end (the device buffer follows at upload).
        struct RowSpans
        {
            std::vector<std::pair<uint32_t, uint32_t>> Free; // (first, count), sorted by first
            uint32_t                                   End  = 0;
            uint32_t                                   Used = 0;

            [[nodiscard]] std::optional<uint32_t> Take( const uint32_t count )
            {
                for ( auto it = Free.begin(); it != Free.end(); ++it )
                {
                    if ( it->second < count )
                        continue;
                    const uint32_t first = it->first;
                    it->first += count;
                    it->second -= count;
                    if ( it->second == 0 )
                        Free.erase( it );
                    Used += count;
                    return first;
                }
                if ( uint64_t{ End } + count > kHierarchyLeaf ) // a cluster row must leave the leaf bit free
                    return std::nullopt;
                const uint32_t first = End;
                End += count;
                Used += count;
                return first;
            }
            void Give( const uint32_t first, const uint32_t count )
            {
                if ( count == 0 )
                    return;
                Used -= count;
                auto it = std::lower_bound( Free.begin(), Free.end(), std::make_pair( first, 0u ) );
                it      = Free.insert( it, { first, count } );
                if ( it + 1 != Free.end() && it->first + it->second == ( it + 1 )->first )
                {
                    it->second += ( it + 1 )->second;
                    Free.erase( it + 1 );
                }
                if ( it != Free.begin() && ( it - 1 )->first + ( it - 1 )->second == it->first )
                {
                    ( it - 1 )->second += it->second;
                    Free.erase( it );
                }
            }
        };

        // One GPU table: its CPU image, its persistent device buffer and the rows written since the last upload.
        template <typename TRow>
        struct Table
        {
            const char*                                     Name    = "";
            uint32_t                                        Binding = 0;
            std::vector<TRow>                               Rows;
            RowSpans                                        Spans;
            std::vector<std::pair<uint32_t, uint32_t>>      Dirty; // (first, count)
            std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
            uint32_t                                        CapacityRows = 0;
            mutable RDG::ExternalBuffer                     Import;

            [[nodiscard]] std::optional<uint32_t> Take( const uint32_t count )
            {
                const auto first = Spans.Take( count );
                if ( first && Rows.size() < Spans.End )
                    Rows.resize( Spans.End );
                return first;
            }
            void MarkDirty( const uint32_t first, const uint32_t count )
            {
                if ( count != 0 )
                    Dirty.emplace_back( first, count );
            }

            // New rows reach the device; a table past its buffer gets a new one holding every row (the old one
            // retires on the frame ring by its API object, a frame in flight still reads it). Rows written into
            // the existing buffer are rows no in-flight frame reads: new ones, or ones released kRetireFrames ago.
            [[nodiscard]] Common::BoolResultStr Upload()
            {
                const uint32_t needed = std::max<uint32_t>( Spans.End, 1u );
                if ( !Buffer || CapacityRows < needed )
                {
                    const uint32_t capacity = std::max( { needed, CapacityRows * 2u, kMinCapacityRows } );
                    const uint64_t bytes    = uint64_t{ capacity } * sizeof( TRow );
                    if ( bytes > std::numeric_limits<uint32_t>::max() )
                        return Common::MakeError( std::format(
                             "[VirtualMeshTable] {}: {} rows ({} bytes) pass the 4 GiB a buffer is sized by", Name,
                             capacity, bytes ) );
                    auto buffer = ShaderResources::StorageBuffer::Create(
                         std::string( "VirtualMeshTable." ) + Name, static_cast<uint32_t>( bytes ), Binding,
                         /*persistent=*/true );
                    if ( !buffer )
                        return Common::MakeError( std::format(
                             "[VirtualMeshTable] {}: the device buffer of {} rows ({} bytes) could not be created",
                             Name, capacity, bytes ) );
                    Buffer       = std::move( buffer );
                    CapacityRows = capacity;
                    Dirty.assign( 1, { 0u, static_cast<uint32_t>( Rows.size() ) } );
                }
                for ( const auto& [first, count] : Dirty )
                {
                    if ( count == 0 )
                        continue;
                    if ( const auto wrote = Buffer->SetData( Rows.data() + first,
                                                             static_cast<uint32_t>( count * sizeof( TRow ) ),
                                                             static_cast<uint32_t>( first * sizeof( TRow ) ) );
                         !wrote )
                        return Common::MakeError(
                             std::format( "[VirtualMeshTable] {}: rows [{}, +{}) did not upload: {}", Name, first,
                                          count, wrote.GetError() ) );
                }
                Dirty.clear();
                return BOOLSUCCESS;
            }

            [[nodiscard]] RDG::BufferRef ImportInto( RDG::Builder& graph ) const
            {
                if ( !Buffer )
                    return {};
                if ( const auto imported = Renderer::ImportBuffer( Buffer, Import ); !imported )
                {
                    LOG_ERROR( "[VirtualMeshTable] {}: the table could not be imported: {}", Name,
                               imported.GetError() );
                    return {};
                }
                return graph.RegisterExternal( Import, std::string( "VirtualMeshTable." ) + Name );
            }

            [[nodiscard]] uint64_t DeviceBytes() const
            {
                return Buffer ? uint64_t{ CapacityRows } * sizeof( TRow ) : 0u;
            }

            void Drop()
            {
                Buffer.reset();
                CapacityRows = 0;
                Import       = {};
                Dirty.assign( 1, { 0u, static_cast<uint32_t>( Rows.size() ) } );
            }
        };

        [[nodiscard]] glm::vec3 Center( const float ( &min )[3], const float ( &max )[3] )
        {
            return { 0.5f * ( min[0] + max[0] ), 0.5f * ( min[1] + max[1] ), 0.5f * ( min[2] + max[2] ) };
        }
        [[nodiscard]] glm::vec3 HalfSize( const float ( &min )[3], const float ( &max )[3] )
        {
            return { 0.5f * ( max[0] - min[0] ), 0.5f * ( max[1] - min[1] ), 0.5f * ( max[2] - min[2] ) };
        }
        [[nodiscard]] glm::vec4 Sphere( const Geometry::ClusterSphere& sphere )
        {
            return { sphere.Center[0], sphere.Center[1], sphere.Center[2], sphere.Radius };
        }
    } // namespace

    // One registered mesh: its row ranges in the four tables.
    struct MeshRows
    {
        std::string Name;
        uint32_t    FirstDag     = 0;
        uint32_t    DagCount     = 0;
        uint32_t    FirstCluster = 0;
        uint32_t    ClusterCount = 0;
        uint32_t    FirstNode    = 0;
        uint32_t    NodeCount    = 0;
        uint64_t    ReleasedAt   = 0; // the frame it was released (when Retiring)
        bool        Live         = false;
        bool        Retiring     = false;
        bool        Uploaded     = false; // its rows reached the device buffers (RT_Upload after RT_Register)
    };

    struct VirtualMeshTable::Storage
    {
        mutable std::mutex             Mutex;
        Table<GpuVirtualMesh>          Meshes{ "Meshes", 0 };
        Table<GpuClusterDag>           Dags{ "Dags", 0 };
        Table<GpuCluster>              Clusters{ "Clusters", 0 };
        Table<GpuClusterHierarchyNode> Hierarchy{ "Hierarchy", 0 };
        std::vector<MeshRows>          Rows; // index = VirtualMeshIndex = its row in Meshes
        uint64_t                       Frame = 0;
        uint32_t                       Live  = 0;

        void Free( MeshRows& rows, const VirtualMeshIndex index )
        {
            Dags.Spans.Give( rows.FirstDag, rows.DagCount );
            Clusters.Spans.Give( rows.FirstCluster, rows.ClusterCount );
            Hierarchy.Spans.Give( rows.FirstNode, rows.NodeCount );
            Meshes.Spans.Give( index, 1u );
            rows = MeshRows{};
        }
    };

    // --- VirtualMeshRegistration ---------------------------------------------------------------------------

    VirtualMeshRegistration::~VirtualMeshRegistration()
    {
        Reset();
    }
    VirtualMeshRegistration::VirtualMeshRegistration( VirtualMeshRegistration&& other ) noexcept
         : m_Table( std::exchange( other.m_Table, nullptr ) ), m_Index( other.m_Index )
    {
    }
    VirtualMeshRegistration& VirtualMeshRegistration::operator=( VirtualMeshRegistration&& other ) noexcept
    {
        if ( this != &other )
        {
            Reset();
            m_Table = std::exchange( other.m_Table, nullptr );
            m_Index = other.m_Index;
        }
        return *this;
    }
    void VirtualMeshRegistration::Reset()
    {
        if ( m_Table )
            std::exchange( m_Table, nullptr )->Release( m_Index );
    }

    // --- VirtualMeshTable ------------------------------------------------------------------------------------

    VirtualMeshTable::VirtualMeshTable() : m_Storage( std::make_unique<Storage>() )
    {
    }
    VirtualMeshTable::~VirtualMeshTable() = default;

    VirtualMeshTable& VirtualMeshTable::Get()
    {
        static VirtualMeshTable s_Table;
        return s_Table;
    }

    Common::ResultStr<VirtualMeshRegistration>
    VirtualMeshTable::RT_Register( const Mesh& mesh, const std::span<const Geometry::ClusterDag> dags,
                                   const std::string_view name )
    {
        using Result                  = VirtualMeshRegistration;
        const MeshGeometry& geometry  = mesh.GetGeometry();
        const auto&         submeshes = mesh.GetSubmeshes();
        if ( !geometry.Pooled || !geometry.IsResident() || !geometry.Indices )
            return Common::MakeFormattedError<Result>(
                 "[VirtualMeshTable] '{}': the mesh is not resident in the GeometryPool", name );
        if ( geometry.Pooled->Arena != GeometryArena::Static )
            return Common::MakeFormattedError<Result>(
                 "[VirtualMeshTable] '{}': only the Static arena carries virtual geometry", name );
        if ( dags.size() != submeshes.size() )
            return Common::MakeFormattedError<Result>(
                 "[VirtualMeshTable] '{}': {} cluster DAGs for {} submeshes (one per submesh)", name, dags.size(),
                 submeshes.size() );

        uint64_t words = 0, clusterCount = 0, groupCount = 0, nodeCount = 0;
        for ( std::size_t i = 0; i < dags.size(); ++i )
        {
            const Geometry::ClusterDag& dag = dags[i];
            if ( dag.Clusters.empty() || dag.Hierarchy.empty() )
                return Common::MakeFormattedError<Result>(
                     "[VirtualMeshTable] '{}': submesh {} has an empty cluster DAG ({} clusters, {} nodes)", name,
                     i, dag.Clusters.size(), dag.Hierarchy.size() );
            words += dag.VertexRefs.size() + dag.Triangles.size();
            clusterCount += dag.Clusters.size();
            groupCount += dag.Groups.size();
            nodeCount += dag.Hierarchy.size();
        }
        if ( words != geometry.ClusterWordCount )
            return Common::MakeFormattedError<Result>(
                 "[VirtualMeshTable] '{}': the DAGs hold {} words, the mesh's index span {} cluster words", name,
                 words, geometry.ClusterWordCount );

        const uint64_t vertexBase = geometry.Vertices->GetDeviceAddress();
        const uint64_t streamBase = geometry.Streams ? geometry.Streams->GetDeviceAddress() : 0u;
        const uint64_t indexBase  = geometry.Indices->GetDeviceAddress();
        if ( vertexBase == 0 || streamBase == 0 || indexBase == 0 )
            return Common::MakeFormattedError<Result>(
                 "[VirtualMeshTable] '{}': a pool page has no device address (vertices {:#x}, streams {:#x}, "
                 "indices {:#x}); the device lacks bufferDeviceAddress",
                 name, vertexBase, streamBase, indexBase );

        Storage&        s = *m_Storage;
        std::lock_guard lock( s.Mutex );
        if ( clusterCount > kHierarchyLeaf || nodeCount > kHierarchyLeaf )
            return Common::MakeFormattedError<Result>(
                 "[VirtualMeshTable] '{}': {} clusters / {} nodes pass a row index", name, clusterCount,
                 nodeCount );
        const auto meshRow   = s.Meshes.Take( 1u );
        const auto firstDag  = meshRow ? s.Dags.Take( static_cast<uint32_t>( dags.size() ) ) : std::nullopt;
        const auto firstClus = firstDag ? s.Clusters.Take( static_cast<uint32_t>( clusterCount ) ) : std::nullopt;
        const auto firstNode = firstClus ? s.Hierarchy.Take( static_cast<uint32_t>( nodeCount ) ) : std::nullopt;
        MeshRows   rows;
        rows.Name         = std::string( name );
        rows.FirstDag     = firstDag.value_or( 0u );
        rows.DagCount     = firstDag ? static_cast<uint32_t>( dags.size() ) : 0u;
        rows.FirstCluster = firstClus.value_or( 0u );
        rows.ClusterCount = firstClus ? static_cast<uint32_t>( clusterCount ) : 0u;
        rows.FirstNode    = firstNode.value_or( 0u );
        rows.NodeCount    = firstNode ? static_cast<uint32_t>( nodeCount ) : 0u;
        const auto refuse = [&]( std::string reason ) -> Common::ResultStr<Result>
        {
            if ( meshRow )
                s.Free( rows, *meshRow ); // the local ranges back to the free lists; nothing was marked dirty
            return Common::MakeError<Result>( std::move( reason ) );
        };
        if ( !firstNode )
            return refuse( std::format( "[VirtualMeshTable] '{}': the tables have no room for {} clusters", name,
                                        clusterCount ) );

        // Rows, submesh by submesh. The words sit after the mesh's indices in its index span, every DAG's
        // VertexRefs then its Triangles, in submesh order (StaticMesh's StageGeometry).
        const uint32_t vertexStride = geometry.VertexStride;
        uint64_t       wordAddress  = indexBase + geometry.IndexByteOffset + uint64_t{ geometry.IndexCount } * 4u;
        uint32_t       clusterRow   = rows.FirstCluster;
        uint32_t       nodeRow      = rows.FirstNode;
        for ( std::size_t i = 0; i < dags.size(); ++i )
        {
            const Geometry::ClusterDag& dag     = dags[i];
            const Submesh&              submesh = submeshes[i];
            const uint32_t              dagRow  = rows.FirstDag + static_cast<uint32_t>( i );

            GpuClusterDag& row = s.Dags.Rows[dagRow];
            row                = GpuClusterDag{};
            row.VertexAddress =
                 vertexBase + geometry.VertexByteOffset + uint64_t{ submesh.VertexOffset } * vertexStride;
            row.StreamAddress =
                 streamBase + geometry.StreamByteOffset + uint64_t{ submesh.VertexOffset } * kGeometryStreamStride;
            row.VertexRefAddress = wordAddress;
            wordAddress += dag.VertexRefs.size() * 4u;
            row.TriangleAddress = wordAddress;
            wordAddress += dag.Triangles.size() * 4u;
            row.RootNode     = nodeRow; // node 0 of the DAG is its root
            row.FirstCluster = clusterRow;
            row.ClusterCount = static_cast<uint32_t>( dag.Clusters.size() );
            row.Submesh      = static_cast<uint32_t>( i );
            row.VertexStride = vertexStride;

            for ( std::size_t c = 0; c < dag.Clusters.size(); ++c )
            {
                const Geometry::ClusterRecord& record = dag.Clusters[c];
                if ( record.VertexCount > Geometry::kClusterMaxVertices ||
                     record.TriangleCount > Geometry::kClusterMaxTriangles ||
                     record.Level > kClusterMaxLevel ||
                     uint64_t{ record.FirstVertexRef } + record.VertexCount > dag.VertexRefs.size() ||
                     uint64_t{ record.FirstTriangle } + record.TriangleCount > dag.Triangles.size() )
                    return refuse( std::format(
                         "[VirtualMeshTable] '{}': submesh {} cluster {} ({} vertices from {}, {} triangles from "
                         "{}, level {}) passes the caps or its DAG's {} refs / {} triangles",
                         name, i, c, record.VertexCount, record.FirstVertexRef, record.TriangleCount,
                         record.FirstTriangle, record.Level, dag.VertexRefs.size(), dag.Triangles.size() ) );
                GpuCluster& cluster  = s.Clusters.Rows[clusterRow + c];
                const float lodError = record.Level == 0 ? 0.0f : record.LodError;
                cluster.BoxCenter    = glm::vec4( Center( record.BoxMin, record.BoxMax ), lodError );
                cluster.BoxExtent    = glm::vec4( HalfSize( record.BoxMin, record.BoxMax ), record.MaxEdgeLength );
                cluster.LodBounds    = Sphere( record.LodBounds );
                cluster.FirstVertexRef = record.FirstVertexRef;
                cluster.FirstTriangle  = record.FirstTriangle;
                cluster.Counts = PackClusterCounts( record.VertexCount, record.TriangleCount, record.Level );
                cluster.Dag            = dagRow;
            }

            for ( std::size_t n = 0; n < dag.Hierarchy.size(); ++n )
            {
                const Geometry::ClusterHierarchyNode& source = dag.Hierarchy[n];
                GpuClusterHierarchyNode&              node   = s.Hierarchy.Rows[nodeRow + n];
                node                                         = GpuClusterHierarchyNode{};
                for ( uint32_t k = 0; k < Geometry::kClusterHierarchyFanout; ++k )
                {
                    const uint32_t child = source.Child[k];
                    if ( child == Geometry::kClusterHierarchyNone )
                        continue;
                    node.BoxCenter[k] =
                         glm::vec4( Center( source.BoxMin[k], source.BoxMax[k] ), source.MinLodError[k] );
                    node.BoxExtent[k] =
                         glm::vec4( HalfSize( source.BoxMin[k], source.BoxMax[k] ), source.MaxParentLodError[k] );
                    node.LodBounds[k] = Sphere( source.LodBounds[k] );
                    if ( ( child & Geometry::kClusterHierarchyLeaf ) != 0 )
                    {
                        // A leaf names a group; the GPU walks straight to its member clusters.
                        const uint32_t group = child & ~Geometry::kClusterHierarchyLeaf;
                        if ( group >= dag.Groups.size() ||
                             uint64_t{ dag.Groups[group].FirstCluster } + dag.Groups[group].ClusterCount >
                                  dag.Clusters.size() )
                            return refuse( std::format(
                                 "[VirtualMeshTable] '{}': submesh {} node {} child {} names group {} of {} (or "
                                 "its clusters pass the DAG's {})",
                                 name, i, n, k, group, dag.Groups.size(), dag.Clusters.size() ) );
                        node.Child[k]        = kHierarchyLeaf | ( clusterRow + dag.Groups[group].FirstCluster );
                        node.ClusterCount[k] = dag.Groups[group].ClusterCount;
                    }
                    else
                    {
                        if ( child >= dag.Hierarchy.size() )
                            return refuse( std::format(
                                 "[VirtualMeshTable] '{}': submesh {} node {} child {} names node {} of {}", name,
                                 i, n, k, child, dag.Hierarchy.size() ) );
                        node.Child[k]        = nodeRow + child;
                        node.ClusterCount[k] = 0u;
                    }
                }
            }
            clusterRow += static_cast<uint32_t>( dag.Clusters.size() );
            nodeRow += static_cast<uint32_t>( dag.Hierarchy.size() );
        }

        GpuVirtualMesh& meshRowData = s.Meshes.Rows[*meshRow];
        meshRowData                 = GpuVirtualMesh{};
        meshRowData.FirstDag        = rows.FirstDag;
        meshRowData.DagCount        = rows.DagCount;
        s.Meshes.MarkDirty( *meshRow, 1u );
        s.Dags.MarkDirty( rows.FirstDag, rows.DagCount );
        s.Clusters.MarkDirty( rows.FirstCluster, rows.ClusterCount );
        s.Hierarchy.MarkDirty( rows.FirstNode, rows.NodeCount );

        rows.Live = true;
        if ( s.Rows.size() <= *meshRow )
            s.Rows.resize( *meshRow + 1u );
        s.Rows[*meshRow] = std::move( rows );
        ++s.Live;
        const MeshRows& placed = s.Rows[*meshRow];
        LOG_INFO(
             "[VirtualMeshTable] '{}' registered {} clusters, {} groups, rows mesh {} dags [{}, +{}) clusters "
             "[{}, +{}) nodes [{}, +{}) ({} meshes resident)",
             name, clusterCount, groupCount, *meshRow, placed.FirstDag, placed.DagCount, placed.FirstCluster,
             placed.ClusterCount, placed.FirstNode, placed.NodeCount, s.Live );
        return Common::MakeSuccess( VirtualMeshRegistration( this, *meshRow ) );
    }

    void VirtualMeshTable::Release( const VirtualMeshIndex mesh )
    {
        Storage&        s = *m_Storage;
        std::lock_guard lock( s.Mutex );
        if ( mesh >= s.Rows.size() || !s.Rows[mesh].Live )
        {
            LOG_ERROR( "[VirtualMeshTable] release of mesh row {} that is not registered", mesh );
            return;
        }
        MeshRows& rows  = s.Rows[mesh];
        rows.Live       = false;
        rows.Retiring   = true;
        rows.ReleasedAt = s.Frame;
        --s.Live;
        LOG_INFO( "[VirtualMeshTable] '{}' released {} clusters ({} meshes resident)", rows.Name,
                  rows.ClusterCount, s.Live );
    }

    Common::BoolResultStr VirtualMeshTable::RT_Upload()
    {
        Storage&        s = *m_Storage;
        std::lock_guard lock( s.Mutex );
        ++s.Frame;
        for ( VirtualMeshIndex i = 0; i < s.Rows.size(); ++i )
            if ( s.Rows[i].Retiring && s.Frame - s.Rows[i].ReleasedAt > kRetireFrames )
                s.Free( s.Rows[i], i );
        if ( s.Meshes.Spans.End == 0 )
            return BOOLSUCCESS; // nothing ever registered: no table is made
        if ( auto uploaded = s.Meshes.Upload(); !uploaded )
            return uploaded;
        if ( auto uploaded = s.Dags.Upload(); !uploaded )
            return uploaded;
        if ( auto uploaded = s.Clusters.Upload(); !uploaded )
            return uploaded;
        if ( auto uploaded = s.Hierarchy.Upload(); !uploaded )
            return uploaded;
        for ( MeshRows& rows : s.Rows )
            rows.Uploaded = rows.Live;
        return BOOLSUCCESS;
    }

    std::vector<VirtualMeshDagSpan> VirtualMeshTable::DescribeUploaded( const VirtualMeshIndex mesh ) const
    {
        const Storage&                  s = *m_Storage;
        std::lock_guard                 lock( s.Mutex );
        std::vector<VirtualMeshDagSpan> spans;
        if ( mesh >= s.Rows.size() || !s.Rows[mesh].Live || !s.Rows[mesh].Uploaded )
            return spans;
        const MeshRows& rows = s.Rows[mesh];
        spans.reserve( rows.DagCount );
        for ( uint32_t d = rows.FirstDag; d < rows.FirstDag + rows.DagCount; ++d )
        {
            const GpuClusterDag& dag = s.Dags.Rows[d];
            spans.push_back( { .Dag          = d,
                               .Submesh      = dag.Submesh,
                               .FirstCluster = dag.FirstCluster,
                               .ClusterCount = dag.ClusterCount } );
        }
        return spans;
    }

    VirtualMeshBuffers VirtualMeshTable::Import( RDG::Builder& graph ) const
    {
        const Storage&     s = *m_Storage;
        std::lock_guard    lock( s.Mutex );
        VirtualMeshBuffers out;
        out.Meshes       = s.Meshes.ImportInto( graph );
        out.Dags         = s.Dags.ImportInto( graph );
        out.Clusters     = s.Clusters.ImportInto( graph );
        out.Hierarchy    = s.Hierarchy.ImportInto( graph );
        out.ClusterCount = s.Clusters.Spans.End;
        out.DagCount     = s.Dags.Spans.End;
        return out;
    }

    VirtualMeshTableStats VirtualMeshTable::GetStats() const
    {
        const Storage&        s = *m_Storage;
        std::lock_guard       lock( s.Mutex );
        VirtualMeshTableStats stats;
        stats.Meshes   = s.Live;
        stats.Clusters = s.Clusters.Spans.Used;
        stats.Bytes =
             s.Meshes.DeviceBytes() + s.Dags.DeviceBytes() + s.Clusters.DeviceBytes() + s.Hierarchy.DeviceBytes();
        return stats;
    }

    void VirtualMeshTable::RT_Shutdown()
    {
        Storage&        s = *m_Storage;
        std::lock_guard lock( s.Mutex );
        s.Meshes.Drop();
        s.Dags.Drop();
        s.Clusters.Drop();
        s.Hierarchy.Drop();
    }
} // namespace Desert::Graphic::VirtualGeometry
