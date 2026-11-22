#include "VertexAnimationCrowd.hpp"

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/AnimationUpdateRate.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/Systems/RayTracing/SkinnedRayTracingGeometry.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <span>

namespace Desert::Graphic::System
{
    namespace
    {
        // The skin cache's stream layout (RayTracingSkinCache.hpp, Common/SkinCacheStreams.glslh), per vertex
        // count V: current positions [0, 3V), previous positions [3V, 6V), normal / tangent / bitangent [6V, 15V).
        constexpr uint64_t kFloatsPerVertex = 15u;
    } // namespace

    VertexAnimationCrowd::VertexAnimationCrowd()  = default;
    VertexAnimationCrowd::~VertexAnimationCrowd() = default;

    void VertexAnimationCrowd::BeginFrame()
    {
        for ( auto& [key, bake] : m_Bakes )
            bake.InUse = false;
    }

    Common::BoolResultStr VertexAnimationCrowd::BakeClip( Bake& bake, const SkinnedMesh& mesh,
                                                          const Animation::AnimationClip& clip )
    {
        DESERT_PROFILE_SCOPE( "VertexAnimationCrowd::BakeClip" );
        const auto& lods = mesh.GetSkinnedLODs();
        if ( lods.empty() )
            return Common::MakeError( "the mesh has no skinned levels" );
        const SkinnedMesh::SkinnedLOD&    lowest   = lods.back();
        const std::vector<SkinnedVertex>& vertices = mesh.GetVertices();
        const uint32_t                    count    = mesh.GetVertexCount();
        if ( static_cast<std::size_t>( lowest.FirstVertex ) + lowest.VertexCount > vertices.size() ||
             vertices.size() > count )
            return Common::MakeError(
                 std::format( "the lowest level's vertices [{}, +{}) are not on the CPU ({} of {})",
                              lowest.FirstVertex, lowest.VertexCount, vertices.size(), count ) );
        const double duration = clip.DurationSeconds();
        if ( !( duration > 0.0 ) )
            return Common::MakeError( "the clip has no length" );
        const uint32_t frames =
             std::max( 1u, static_cast<uint32_t>( std::lround( duration * kBakeFramesPerSecond ) ) );

        Animation::Animator animator( mesh.GetSkeleton() );
        animator.Play( clip, true );

        const uint64_t slot = static_cast<uint64_t>( count ) * kFloatsPerVertex;
        bake.Data.assign( slot * frames, 0.0f );
        for ( uint32_t f = 0; f < frames; ++f )
        {
            animator.SetTime( static_cast<float>( f ) / kBakeFramesPerSecond );
            const std::vector<glm::mat4>& palette = animator.GetPose().Matrices;
            if ( palette.empty() )
                return Common::MakeError( std::format( "the clip's frame {} evaluated no pose", f ) );
            float* const out = bake.Data.data() + slot * f;
            for ( uint32_t v = lowest.FirstVertex; v < lowest.FirstVertex + lowest.VertexCount; ++v )
            {
                const SkinnedVertex& vertex = vertices[v];
                glm::mat4            skin( 0.0f );
                for ( std::size_t i = 0; i < SkinnedVertex::MAX_BONE_INFLUENCES; ++i )
                    if ( vertex.BoneWeights[i] != 0.0f && vertex.BoneIDs[i] < palette.size() )
                        skin += palette[vertex.BoneIDs[i]] * vertex.BoneWeights[i];
                const glm::mat3 skin3( skin );
                const glm::vec3 posed     = glm::vec3( skin * glm::vec4( vertex.StaticVertex.Position, 1.0f ) );
                const glm::vec3 normal    = skin3 * vertex.StaticVertex.Normal;
                const glm::vec3 tangent   = skin3 * vertex.StaticVertex.Tangent;
                const glm::vec3 bitangent = skin3 * vertex.StaticVertex.Bitangent;
                std::memcpy( out + v * 3u, &posed, sizeof( glm::vec3 ) );
                float* const frame = out + static_cast<uint64_t>( count ) * 6u + v * 9u;
                std::memcpy( frame + 0, &normal, sizeof( glm::vec3 ) );
                std::memcpy( frame + 3, &tangent, sizeof( glm::vec3 ) );
                std::memcpy( frame + 6, &bitangent, sizeof( glm::vec3 ) );
            }
        }
        // Previous positions: the frame before (the loop's last before frame 0), what the skin cache rotates in.
        for ( uint32_t f = 0; f < frames; ++f )
        {
            const uint32_t     before = ( f + frames - 1u ) % frames;
            const float* const from   = bake.Data.data() + slot * before + lowest.FirstVertex * 3u;
            float* const       to =
                 bake.Data.data() + slot * f + static_cast<uint64_t>( count ) * 3u + lowest.FirstVertex * 3u;
            std::memcpy( to, from, sizeof( float ) * 3u * lowest.VertexCount );
        }
        bake.Frames      = frames;
        bake.VertexCount = count;
        bake.SlotBytes   = slot * sizeof( float );

        bake.Buffer = RayTracing::CreateSkinCacheStreams();
        if ( !bake.Buffer )
            return Common::MakeError( "no device buffer for the bake" );
        if ( const Common::ResultStr<bool> prepared = bake.Buffer->Prepare( count * frames ); !prepared )
            return Common::MakeError( std::format( "the bake's buffer was refused: {}", prepared.GetError() ) );
        if ( bake.Buffer->GetBytes() < bake.Data.size() * sizeof( float ) )
            return Common::MakeError( std::format( "the bake's buffer holds {} of {} bytes",
                                                   bake.Buffer->GetBytes(), bake.Data.size() * sizeof( float ) ) );
        LOG_INFO( "[Crowd] baked clip '{}' for a skinned mesh: {} frames at {} fps of its lowest level ({} of {} "
                  "vertices, from {}), {:.1f} MB",
                  clip.AnimationName, frames, kBakeFramesPerSecond, lowest.VertexCount, count, lowest.FirstVertex,
                  static_cast<double>( bake.Data.size() * sizeof( float ) ) / ( 1024.0 * 1024.0 ) );
        return BOOLSUCCESS;
    }

    uint64_t VertexAnimationCrowd::FrameAddress( const SkinnedMesh& mesh, const Animation::AnimationClip& clip,
                                                 const float seconds )
    {
        const auto [found, fresh] = m_Bakes.try_emplace( { &mesh, &clip } );
        Bake& bake                = found->second;
        if ( fresh )
        {
            if ( const Common::BoolResultStr baked = BakeClip( bake, mesh, clip ); !baked )
            {
                LOG_ERROR( "[Crowd] clip '{}' cannot be baked: its figures stay skeletal: {}", clip.AnimationName,
                           baked.GetError() );
                bake.Refused = true;
                bake.Data    = {};
                bake.Buffer.reset();
            }
        }
        if ( bake.Refused || bake.Buffer == nullptr || bake.Buffer->GetDeviceAddress() == 0 )
            return 0;
        bake.InUse = true;
        const auto at =
             static_cast<int64_t>( std::floor( std::max( seconds, 0.0f ) * kBakeFramesPerSecond + 0.5f ) );
        const uint32_t frame = static_cast<uint32_t>( at % static_cast<int64_t>( bake.Frames ) );
        return bake.Buffer->GetDeviceAddress() + bake.SlotBytes * frame;
    }

    RDG::BufferRef VertexAnimationCrowd::RefIn( RDG::Builder& graph, Bake& bake )
    {
        if ( bake.Graph != &graph || !bake.Ref )
        {
            bake.Graph = &graph;
            bake.Ref   = graph.RegisterExternal( bake.Buffer->GetBuffer(), "Crowd.VertexAnimation" );
        }
        return *bake.Ref;
    }

    void VertexAnimationCrowd::UploadInGraph( RDG::Builder& graph )
    {
        for ( auto& [key, bake] : m_Bakes )
        {
            // The first touch of this view's graph (SyncGpuScene): a ref of an earlier graph is not one of its own,
            // even when the builder sits at the same address.
            bake.Graph = nullptr;
            bake.Ref.reset();
            if ( !bake.InUse || bake.Uploaded || bake.Refused )
                continue;
            const RDG::BufferRef ref = RefIn( graph, bake );
            graph.QueueBufferUpload( ref, std::as_bytes( std::span( bake.Data ) ) );
            bake.Uploaded = true;
            bake.Data     = {};
        }
    }

    void VertexAnimationCrowd::DeclareReads( RDG::Builder& graph, std::vector<RDG::BufferRef>& reads )
    {
        for ( auto& [key, bake] : m_Bakes )
            if ( bake.InUse && bake.Uploaded && !bake.Refused )
                reads.push_back( RefIn( graph, bake ) );
    }

    void VertexAnimationCrowd::Report( const uint32_t skeletal, const uint32_t vat, const uint32_t draws,
                                       const uint32_t grown, const float maxScale )
    {
        const uint64_t counts = static_cast<uint64_t>( skeletal ) | ( static_cast<uint64_t>( vat ) << 16u ) |
                                ( static_cast<uint64_t>( draws ) << 32u ) | ( static_cast<uint64_t>( grown ) << 48u );
        if ( counts == m_ReportedCounts && ++m_FramesSinceReport < 60u )
            return;
        m_ReportedCounts    = counts;
        m_FramesSinceReport = 0;
        LOG_INFO( "[Crowd] skeletal {}, vat {}, draws {}, grown to min screen size {} (max x{:.2f}), anim CPU {:.4f} "
                  "ms",
                  skeletal, vat, draws, grown, maxScale, Animation::g_AnimationCpuMsPerFrame.load( std::memory_order_relaxed ) );
    }
} // namespace Desert::Graphic::System
