#include "VertexAnimationCrowd.hpp"

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/AnimationUpdateRate.hpp>
#include <Engine/Assets/VertexAnimationDerivedData.hpp>
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
    VertexAnimationCrowd::VertexAnimationCrowd()  = default;
    VertexAnimationCrowd::~VertexAnimationCrowd() = default;

    void VertexAnimationCrowd::BeginFrame()
    {
        for ( auto& [key, bake] : m_Bakes )
            bake.InUse = false;
    }

    Common::BoolResultStr VertexAnimationCrowd::LoadClip( Bake& bake, const SkinnedMesh& mesh,
                                                          const Animation::AnimationClip& clip )
    {
        DESERT_PROFILE_SCOPE( "VertexAnimationCrowd::LoadClip" );
        auto loaded = Assets::LoadVertexAnimation(
             { .Vertices = mesh.GetVertices(), .LODs = mesh.GetSkinnedLODs(), .Skeleton = mesh.GetSkeleton() },
             clip );
        if ( !loaded )
            return Common::MakeError( loaded.GetError() );
        const auto header = Assets::ReadVertexAnimationHeader( loaded.GetValue().Bytes );
        if ( !header )
            return Common::MakeError( header.GetError() );
        const Assets::VertexAnimationHeader& h = header.GetValue();
        bake.Frames      = h.Frames;
        bake.FirstVertex = h.FirstVertex;
        bake.VertexCount = h.VertexCount;
        bake.SlotBytes   = static_cast<uint64_t>( h.VertexCount ) * Assets::kVertexAnimationFloatsPerVertex * sizeof( float );
        bake.Data        = std::move( loaded.ExtractValue().Bytes );
        bake.Data.erase( 0, sizeof( Assets::VertexAnimationHeader ) );

        bake.Buffer = RayTracing::CreateSkinCacheStreams();
        if ( !bake.Buffer )
            return Common::MakeError( "no device buffer for the vertex animation" );
        if ( const Common::ResultStr<bool> prepared = bake.Buffer->Prepare( h.VertexCount * h.Frames ); !prepared )
            return Common::MakeError( std::format( "the vertex animation's buffer was refused: {}", prepared.GetError() ) );
        if ( bake.Buffer->GetBytes() < bake.Data.size() )
            return Common::MakeError( std::format( "the vertex animation's buffer holds {} of {} bytes",
                                                   bake.Buffer->GetBytes(), bake.Data.size() ) );
        LOG_INFO( "[Crowd] clip '{}' vertex animation {:016x} {}: {} frames at {} fps of the lowest level ({} vertices "
                  "from {} of {}), {:.3f} MB",
                  clip.AnimationName, loaded.GetValue().Key,
                  loaded.GetValue().DerivedNow ? "derived now into the DDC" : "derived (DDC hit)", h.Frames,
                  h.FramesPerSecond, h.VertexCount, h.FirstVertex, mesh.GetVertexCount(),
                  static_cast<double>( bake.Data.size() ) / ( 1024.0 * 1024.0 ) );
        return BOOLSUCCESS;
    }

    VertexAnimationCrowd::Frame VertexAnimationCrowd::FrameAddress( const SkinnedMesh&             mesh,
                                                                    const Animation::AnimationClip& clip,
                                                                    const float                     seconds )
    {
        const auto [found, fresh] = m_Bakes.try_emplace( { &mesh, &clip } );
        Bake& bake                = found->second;
        if ( fresh )
        {
            if ( const Common::BoolResultStr baked = LoadClip( bake, mesh, clip ); !baked )
            {
                LOG_ERROR( "[Crowd] clip '{}' has no vertex animation: its figures stay skeletal: {}",
                           clip.AnimationName, baked.GetError() );
                bake.Refused = true;
                bake.Data    = {};
                bake.Buffer.reset();
            }
        }
        if ( bake.Refused || bake.Buffer == nullptr || bake.Buffer->GetDeviceAddress() == 0 )
            return {};
        bake.InUse = true;
        const auto at = static_cast<int64_t>(
             std::floor( std::max( seconds, 0.0f ) * Assets::kVertexAnimationFramesPerSecond + 0.5f ) );
        const uint32_t frame = static_cast<uint32_t>( at % static_cast<int64_t>( bake.Frames ) );
        // THE RANGE'S BIASED BASE: the streams hold the baked level's vertices only, and the vertex stage fetches
        // by gl_VertexIndex in the mesh's vertex order (Common/SkinCacheStreams.glslh), so the address names where
        // vertex 0 would sit - every fetch of [FirstVertex, +VertexCount) lands inside the frame's slot.
        return { bake.Buffer->GetDeviceAddress() + bake.SlotBytes * frame -
                      static_cast<uint64_t>( bake.FirstVertex ) * 3u * sizeof( float ),
                 bake.VertexCount };
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
            graph.QueueBufferUpload( ref, std::as_bytes( std::span( bake.Data.data(), bake.Data.size() ) ) );
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
                  skeletal, vat, draws, grown, maxScale, Animation::AnimationCpuMsPerFrame() );
    }
} // namespace Desert::Graphic::System
