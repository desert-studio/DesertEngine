#include <Engine/Graphic/TextureStreamingDemand.hpp>

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <utility>

namespace Desert::Graphic
{
    /// One thread's notes, in two halves by epoch parity: a note writes the half of the epoch it published in
    /// Writing, the take merges the half of the epoch it closed. Cache-line aligned: two workers' Writing words
    /// never share a line.
    struct alignas( 64 ) TextureDemandBuffer
    {
        static constexpr uint64_t kIdle = std::numeric_limits<uint64_t>::max();

        std::atomic<uint64_t>                              Writing{ kIdle };
        std::atomic<bool>                                  Claimed{ false };
        std::vector<std::pair<Common::AssetHandle, float>> Halves[2];
    };

    /// AN INSTANCED FIELD'S BOUNDS TREE (UE's ISM cluster tree): a median-split hierarchy over the instances'
    /// origins, each node holding its origins' box and its largest instance scale. Nothing in it depends on the
    /// mesh: a node's instances' spheres lie inside its origin box grown by the largest scale x the mesh's reach
    /// (|centre|_1 + half-diagonal), so one tree serves every mesh and section drawn with the snapshot.
    struct InstanceFieldBounds
    {
        static constexpr uint32_t kLeafInstances = 16;
        static constexpr uint32_t kNoChild       = 0;

        struct Node
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
            float     MaxScale = 0.0f;
            uint32_t  First    = 0; // into Order
            uint32_t  Count    = 0;
            uint32_t  Left     = kNoChild; // kNoChild: a leaf (the root is node 0 and never a child)
            uint32_t  Right    = kNoChild;
        };

        std::weak_ptr<const std::vector<glm::mat4>> Source;
        std::vector<Node>                           Nodes;
        std::vector<uint32_t>                       Order; // instance indices, a leaf's contiguous

        // The last answer: one view of one mesh's bounds. Every section and view of a frame asks the same.
        std::mutex         MemoLock;
        bool               MemoValid = false;
        glm::vec3          MemoOrigin{ 0.0f };
        glm::mat4          MemoProjection{ 1.0f };
        uint32_t           MemoHeight = 0;
        Common::Math::AABB MemoLocal;
        float              MemoPixels = 0.0f;
    };

    namespace
    {
        float LargestScale( const glm::mat4& transform )
        {
            return glm::max(
                 glm::length( glm::vec3( transform[0] ) ),
                 glm::max( glm::length( glm::vec3( transform[1] ) ), glm::length( glm::vec3( transform[2] ) ) ) );
        }

        uint32_t BuildNode( InstanceFieldBounds& field, const std::vector<glm::mat4>& transforms,
                            const std::vector<float>& scales, const uint32_t first, const uint32_t count )
        {
            const uint32_t            index = static_cast<uint32_t>( field.Nodes.size() );
            InstanceFieldBounds::Node node;
            node.First = first;
            node.Count = count;
            node.Min   = glm::vec3( std::numeric_limits<float>::max() );
            node.Max   = glm::vec3( std::numeric_limits<float>::lowest() );
            for ( uint32_t i = first; i < first + count; ++i )
            {
                const uint32_t  instance = field.Order[i];
                const glm::vec3 origin( transforms[instance][3] );
                node.Min      = glm::min( node.Min, origin );
                node.Max      = glm::max( node.Max, origin );
                node.MaxScale = std::max( node.MaxScale, scales[instance] );
            }
            field.Nodes.push_back( node );
            if ( count <= InstanceFieldBounds::kLeafInstances )
                return index;
            // The median along the box's longest axis: a balanced tree, depth log2(n / leaf).
            const glm::vec3 extent = node.Max - node.Min;
            int             axis   = 0;
            if ( extent.y > extent[axis] )
                axis = 1;
            if ( extent.z > extent[axis] )
                axis = 2;
            const uint32_t half  = count / 2;
            auto           begin = field.Order.begin() + first;
            std::nth_element( begin, begin + half, begin + count,
                              [&transforms, axis]( const uint32_t a, const uint32_t b )
                              { return transforms[a][3][axis] < transforms[b][3][axis]; } );
            const uint32_t left      = BuildNode( field, transforms, scales, first, half );
            const uint32_t right     = BuildNode( field, transforms, scales, first + half, count - half );
            field.Nodes[index].Left  = left;
            field.Nodes[index].Right = right;
            return index;
        }

        std::shared_ptr<InstanceFieldBounds>
        BuildField( const std::shared_ptr<const std::vector<glm::mat4>>& transforms )
        {
            auto                          field = std::make_shared<InstanceFieldBounds>();
            const std::vector<glm::mat4>& all   = *transforms;
            const uint32_t                count = static_cast<uint32_t>( all.size() );
            field->Source                       = transforms;
            field->Order.resize( count );
            std::iota( field->Order.begin(), field->Order.end(), 0u );
            std::vector<float> scales( count );
            for ( uint32_t i = 0; i < count; ++i )
                scales[i] = LargestScale( all[i] );
            field->Nodes.reserve( 2u * ( count / InstanceFieldBounds::kLeafInstances + 1u ) );
            (void)BuildNode( *field, all, scales, 0, count );
            return field;
        }

        float DistanceToBox( const glm::vec3& point, const glm::vec3& boxMin, const glm::vec3& boxMax )
        {
            const glm::vec3 outside = glm::max( glm::vec3( 0.0f ), glm::max( boxMin - point, point - boxMax ) );
            return glm::length( outside );
        }

        // The largest ScreenPixels any WorldSphere( local, t ) over the field can reach: branch and bound over
        // the tree. A node's bound is ScreenPixels' measure with the largest radius the node holds and the
        // smallest distance any of its spheres can be at (to its grown origin box), so it is never below an
        // instance's own value; a leaf measures its instances exactly, through ScreenPixels itself.
        float MeasureField( const InstanceFieldBounds& field, const std::vector<glm::mat4>& transforms,
                            const Common::Math::AABB& local, const Geometry::LODView& view,
                            const uint32_t viewHeight )
        {
            const glm::vec3 centre = 0.5f * ( local.Min + local.Max );
            const float     half   = glm::length( local.Max - local.Min ) * 0.5f;
            // |M c| <= sum_k |c_k| |M col k| <= scale |c|_1: the centre's offset from the instance origin.
            const float reach       = std::abs( centre.x ) + std::abs( centre.y ) + std::abs( centre.z ) + half;
            const float perspective = std::abs( view.Projection[2][3] );
            const float multiple =
                 std::max( 0.5f * std::abs( view.Projection[0][0] ), 0.5f * std::abs( view.Projection[1][1] ) );
            const float height    = static_cast<float>( viewHeight );
            const auto  nodeBound = [&]( const InstanceFieldBounds::Node& node )
            {
                const glm::vec3 grow( node.MaxScale * reach );
                const float     toSurface =
                     DistanceToBox( view.Origin, node.Min - grow, node.Max + grow ) * perspective;
                return 2.0f * ( multiple * node.MaxScale * half / std::max( 1.0f, toSurface ) ) * height;
            };
            // A bound within float rounding of the best may still hide an instance equal to it: open it unless
            // the bound is clearly below.
            constexpr float kSlack = 1.001f;

            float                    best = 0.0f;
            std::array<uint32_t, 64> stack{};
            uint32_t                 depth = 0;
            stack[depth++]                 = 0;
            while ( depth > 0 )
            {
                const InstanceFieldBounds::Node& node = field.Nodes[stack[--depth]];
                if ( nodeBound( node ) * kSlack <= best )
                    continue;
                if ( node.Left == InstanceFieldBounds::kNoChild )
                {
                    for ( uint32_t i = node.First; i < node.First + node.Count; ++i )
                    {
                        const auto [c, r] =
                             TextureStreamingDemand::WorldSphere( local, transforms[field.Order[i]] );
                        best = std::max( best, TextureStreamingDemand::ScreenPixels( c, r, view, viewHeight ) );
                    }
                    continue;
                }
                // The nearer child is popped first: the best rises early and prunes the rest. The tree is
                // median-balanced (depth <= 29 for 2^32 instances at 16 a leaf), and one entry per level stays
                // pending, so the stack never holds more than 30.
                const InstanceFieldBounds::Node& left       = field.Nodes[node.Left];
                const InstanceFieldBounds::Node& right      = field.Nodes[node.Right];
                const bool                       leftNearer = DistanceToBox( view.Origin, left.Min, left.Max ) <=
                                        DistanceToBox( view.Origin, right.Min, right.Max );
                stack[depth++] = leftNearer ? node.Right : node.Left;
                stack[depth++] = leftNearer ? node.Left : node.Right;
            }
            return best;
        }

        bool SameBounds( const Common::Math::AABB& a, const Common::Math::AABB& b )
        {
            return a.Min == b.Min && a.Max == b.Max;
        }

        // Releases this thread's buffer for the next new thread; its notes stay to be merged.
        struct BufferLease
        {
            TextureDemandBuffer* Buffer = nullptr;
            ~BufferLease()
            {
                if ( Buffer != nullptr )
                    Buffer->Claimed.store( false, std::memory_order_release );
            }
        };
    } // namespace

    TextureStreamingDemand::TextureStreamingDemand()  = default;
    TextureStreamingDemand::~TextureStreamingDemand() = default;

    TextureStreamingDemand& TextureStreamingDemand::Get()
    {
        static TextureStreamingDemand demand;
        return demand;
    }

    float TextureStreamingDemand::ScreenPixels( const glm::vec3& center, const float radius,
                                                const Geometry::LODView& view, const uint32_t viewHeight )
    {
        // Geometry::BoundsScreenRadiusSquared's measure (UE ComputeBoundsScreenRadiusSquared), with the distance
        // taken to the sphere's surface as UE's texture streaming does (FRenderAssetInstanceView: the distance
        // from the view to the bounds, not to their centre) -- a wall the camera stands at is a wall at distance
        // 0.
        const float perspective = std::abs( view.Projection[2][3] );
        const float toSurface   = std::max( 0.0f, glm::length( center - view.Origin ) - radius ) * perspective;
        const float multiple =
             std::max( 0.5f * std::abs( view.Projection[0][0] ), 0.5f * std::abs( view.Projection[1][1] ) );
        const float screenRadius = multiple * radius / std::max( 1.0f, toSurface );
        return 2.0f * screenRadius * static_cast<float>( viewHeight );
    }

    std::pair<glm::vec3, float> TextureStreamingDemand::WorldSphere( const Common::Math::AABB& local,
                                                                     const glm::mat4&          transform )
    {
        const float scale = LargestScale( transform );
        return { glm::vec3( transform * glm::vec4( 0.5f * ( local.Min + local.Max ), 1.0f ) ),
                 glm::length( local.Max - local.Min ) * 0.5f * scale };
    }

    float
    TextureStreamingDemand::FieldScreenPixels( const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                               const Common::Math::AABB& local, const Geometry::LODView& view,
                                               const uint32_t viewHeight )
    {
        if ( !transforms || transforms->empty() )
            return 0.0f;
        std::shared_ptr<InstanceFieldBounds> field;
        {
            std::shared_lock<std::shared_mutex> lock( m_FieldsLock );
            const auto                          found = m_Fields.find( transforms.get() );
            // The same address is the same snapshot only while the same owner holds it.
            if ( found != m_Fields.end() && !found->second->Source.owner_before( transforms ) &&
                 !transforms.owner_before( found->second->Source ) )
                field = found->second;
        }
        if ( !field )
        {
            field = BuildField( transforms );
            std::unique_lock<std::shared_mutex> lock( m_FieldsLock );
            std::erase_if( m_Fields, []( const auto& entry ) { return entry.second->Source.expired(); } );
            m_Fields[transforms.get()] = field;
        }
        std::lock_guard<std::mutex> memo( field->MemoLock );
        if ( field->MemoValid && field->MemoHeight == viewHeight && field->MemoOrigin == view.Origin &&
             field->MemoProjection == view.Projection && SameBounds( field->MemoLocal, local ) )
            return field->MemoPixels;
        field->MemoPixels     = MeasureField( *field, *transforms, local, view, viewHeight );
        field->MemoValid      = true;
        field->MemoHeight     = viewHeight;
        field->MemoOrigin     = view.Origin;
        field->MemoProjection = view.Projection;
        field->MemoLocal      = local;
        return field->MemoPixels;
    }

    TextureDemandBuffer& TextureStreamingDemand::LocalBuffer()
    {
        thread_local BufferLease lease;
        if ( lease.Buffer != nullptr )
            return *lease.Buffer;
        std::lock_guard<std::mutex> lock( m_BuffersLock );
        for ( const auto& buffer : m_Buffers )
        {
            bool unclaimed = false;
            if ( buffer->Claimed.compare_exchange_strong( unclaimed, true, std::memory_order_acq_rel ) )
            {
                lease.Buffer = buffer.get();
                return *lease.Buffer;
            }
        }
        m_Buffers.push_back( std::make_unique<TextureDemandBuffer>() );
        m_Buffers.back()->Claimed.store( true, std::memory_order_relaxed );
        lease.Buffer = m_Buffers.back().get();
        return *lease.Buffer;
    }

    void TextureStreamingDemand::Append( const std::span<const Common::AssetHandle> assets,
                                         const float                                screenPixels )
    {
        TextureDemandBuffer& buffer = LocalBuffer();
        // Publish the epoch, then confirm it is still current: a take that advanced it in between either sees
        // this Writing and waits, or is seen here and the note moves to the new epoch.
        uint64_t epoch = m_Epoch.load( std::memory_order_seq_cst );
        for ( ;; )
        {
            buffer.Writing.store( epoch, std::memory_order_seq_cst );
            const uint64_t now = m_Epoch.load( std::memory_order_seq_cst );
            if ( now == epoch )
                break;
            epoch = now;
        }
        auto& half = buffer.Halves[epoch & 1u];
        for ( const Common::AssetHandle& asset : assets )
            if ( static_cast<uint64_t>( asset ) != 0 )
                half.emplace_back( asset, screenPixels );
        buffer.Writing.store( TextureDemandBuffer::kIdle, std::memory_order_release );
    }

    void TextureStreamingDemand::Note( const Material* material, const float screenPixels )
    {
        if ( material == nullptr || !( screenPixels > 0.0f ) )
            return;
        // The cell's 2D slots live on its executor (MaterialExecutor::GetTexture2DProperties); the registered
        // properties are the reflected MPROPERTY fields only and never name a texture (p1 found nothing through
        // them: 3.9 MB wanted of 992 MB).
        const MaterialExecutor* executor = material->GetMaterialExecutor();
        if ( executor == nullptr )
            return;
        const auto textures = executor->GetStreamedTextureAssets();
        if ( textures && !textures->empty() )
            Append( *textures, screenPixels );
    }

    void TextureStreamingDemand::NoteAsset( const Common::AssetHandle& asset, const float screenPixels )
    {
        if ( static_cast<uint64_t>( asset ) == 0 || !( screenPixels > 0.0f ) )
            return;
        Append( std::span<const Common::AssetHandle>( &asset, 1 ), screenPixels );
    }

    const std::unordered_map<Common::AssetHandle, float>& TextureStreamingDemand::TakeFrame()
    {
        std::lock_guard<std::mutex> lock( m_BuffersLock );
        const uint64_t              taken = m_Epoch.fetch_add( 1, std::memory_order_seq_cst );
        m_Demand.clear();
        for ( const auto& buffer : m_Buffers )
        {
            // A note still writing the taken epoch is a handful of appends away.
            while ( buffer->Writing.load( std::memory_order_seq_cst ) == taken )
                std::this_thread::yield();
            auto& half = buffer->Halves[taken & 1u];
            for ( const auto& [asset, pixels] : half )
            {
                float& wanted = m_Demand[asset];
                wanted        = std::max( wanted, pixels );
            }
            half.clear();
        }
        return m_Demand;
    }
} // namespace Desert::Graphic
