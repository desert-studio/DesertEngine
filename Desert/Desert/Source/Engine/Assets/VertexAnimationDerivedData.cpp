// UE 5.5 AnimToTexture (Plugins/Animation/AnimToTexture: UAnimToTextureBPLibrary::AnimationToTexture samples the
// skeletal mesh's chosen LOD per frame offline into vertex-position / normal textures), adapted: the samples are a
// DDC entry keyed by their inputs instead of committed textures, in the GPU skin cache's stream layout so the far
// draw reuses the skinned vertex stage's fetch.
#include <Engine/Assets/VertexAnimationDerivedData.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace Desert::Assets
{
    namespace
    {
        std::atomic<bool> g_Deriving{ false };

        struct BakedRange
        {
            uint32_t FirstVertex = 0;
            uint32_t VertexCount = 0;
        };

        Common::ResultStr<BakedRange> LowestRange( const SkinnedMesh& mesh )
        {
            const auto& lods = mesh.GetSkinnedLODs();
            if ( lods.empty() )
                return Common::MakeError<BakedRange>( "the mesh has no skinned levels" );
            const SkinnedMesh::SkinnedLOD& lowest = lods.back();
            if ( static_cast<std::size_t>( lowest.FirstVertex ) + lowest.VertexCount > mesh.GetVertices().size() ||
                 lowest.VertexCount == 0 )
                return Common::MakeError<BakedRange>(
                     std::format( "the lowest level's vertices [{}, +{}) are not on the CPU ({} there)",
                                  lowest.FirstVertex, lowest.VertexCount, mesh.GetVertices().size() ) );
            return Common::MakeSuccess( BakedRange{ lowest.FirstVertex, lowest.VertexCount } );
        }
    } // namespace

    Common::ResultStr<uint64_t> VertexAnimationKey( const SkinnedMesh& mesh, const Animation::AnimationClip& clip )
    {
        const auto range = LowestRange( mesh );
        if ( !range )
            return Common::MakeError<uint64_t>( range.GetError() );
        // THE CLIP AS THE BAKE READS IT: the skinning palettes at every sample time (frame count included). The
        // clip's written text is not stable across loads (its sequence mints ids at load), the samples are.
        const double duration = clip.DurationSeconds();
        if ( !( duration > 0.0 ) )
            return Common::MakeError<uint64_t>( "the clip has no length" );
        const uint32_t frames =
             std::max( 1u, static_cast<uint32_t>( std::lround( duration * kVertexAnimationFramesPerSecond ) ) );
        Animation::Animator animator( mesh.GetSkeleton() );
        animator.Play( clip, true );
        uint64_t clipHash = frames;
        for ( uint32_t f = 0; f < frames; ++f )
        {
            animator.SetTime( static_cast<float>( f ) / kVertexAnimationFramesPerSecond );
            const std::vector<glm::mat4>& palette = animator.GetPose().Matrices;
            const std::array<uint64_t, 2> pair    = {
                clipHash, Common::Utils::PakContentHash( palette.data(), palette.size() * sizeof( glm::mat4 ) ) };
            clipHash = Common::Utils::PakContentHash( pair.data(), sizeof( pair ) );
        }

        const SkinnedVertex* const    first   = mesh.GetVertices().data() + range.GetValue().FirstVertex;
        const std::array<uint64_t, 5> payload = {
             Common::Utils::PakContentHash( first, sizeof( SkinnedVertex ) * range.GetValue().VertexCount ),
             ( static_cast<uint64_t>( range.GetValue().FirstVertex ) << 32u ) | range.GetValue().VertexCount,
             mesh.GetSkeleton().GetContentSignature(),
             clipHash, VertexAnimationHeader{}.Version };
        const uint64_t payloadHash = Common::Utils::PakContentHash( payload.data(), sizeof( payload ) );
        const float    settings    = kVertexAnimationFramesPerSecond;
        return Common::MakeSuccess(
             Common::DDC::MakeKey( kVertexAnimationDeriver, payloadHash, &settings, sizeof( settings ) ) );
    }

    Common::ResultStr<std::string> BuildVertexAnimation( const SkinnedMesh&              mesh,
                                                         const Animation::AnimationClip& clip )
    {
        DESERT_PROFILE_SCOPE( "BuildVertexAnimation" );
        const auto range = LowestRange( mesh );
        if ( !range )
            return Common::MakeError<std::string>( range.GetError() );
        const double duration = clip.DurationSeconds();
        if ( !( duration > 0.0 ) )
            return Common::MakeError<std::string>( "the clip has no length" );

        VertexAnimationHeader header;
        header.Frames =
             std::max( 1u, static_cast<uint32_t>( std::lround( duration * kVertexAnimationFramesPerSecond ) ) );
        header.FirstVertex  = range.GetValue().FirstVertex;
        header.VertexCount  = range.GetValue().VertexCount;
        const uint64_t n    = header.VertexCount;
        const uint64_t slot = n * kVertexAnimationFloatsPerVertex;

        std::vector<float>  data( slot * header.Frames, 0.0f );
        Animation::Animator animator( mesh.GetSkeleton() );
        animator.Play( clip, true );
        const std::vector<SkinnedVertex>& vertices = mesh.GetVertices();
        for ( uint32_t f = 0; f < header.Frames; ++f )
        {
            animator.SetTime( static_cast<float>( f ) / kVertexAnimationFramesPerSecond );
            const std::vector<glm::mat4>& palette = animator.GetPose().Matrices;
            if ( palette.empty() )
                return Common::MakeError<std::string>( std::format( "the clip's frame {} evaluated no pose", f ) );
            float* const out = data.data() + slot * f;
            for ( uint64_t i = 0; i < n; ++i )
            {
                const SkinnedVertex& vertex = vertices[header.FirstVertex + i];
                glm::mat4            skin( 0.0f );
                for ( std::size_t b = 0; b < SkinnedVertex::MAX_BONE_INFLUENCES; ++b )
                    if ( vertex.BoneWeights[b] != 0.0f && vertex.BoneIDs[b] < palette.size() )
                        skin += palette[vertex.BoneIDs[b]] * vertex.BoneWeights[b];
                const glm::mat3 skin3( skin );
                const glm::vec3 posed     = glm::vec3( skin * glm::vec4( vertex.StaticVertex.Position, 1.0f ) );
                const glm::vec3 normal    = skin3 * vertex.StaticVertex.Normal;
                const glm::vec3 tangent   = skin3 * vertex.StaticVertex.Tangent;
                const glm::vec3 bitangent = skin3 * vertex.StaticVertex.Bitangent;
                std::memcpy( out + i * 3u, &posed, sizeof( glm::vec3 ) );
                std::memcpy( out + n * 6u + i * 3u, &normal, sizeof( glm::vec3 ) );
                std::memcpy( out + n * 9u + i * 3u, &tangent, sizeof( glm::vec3 ) );
                std::memcpy( out + n * 12u + i * 3u, &bitangent, sizeof( glm::vec3 ) );
            }
        }
        // Previous positions: the frame before (the loop's last before frame 0), what the skin cache rotates in.
        for ( uint32_t f = 0; f < header.Frames; ++f )
        {
            const uint32_t before = ( f + header.Frames - 1u ) % header.Frames;
            std::memcpy( data.data() + slot * f + n * 3u, data.data() + slot * before, sizeof( float ) * 3u * n );
        }

        std::string bytes( sizeof( header ) + data.size() * sizeof( float ), '\0' );
        std::memcpy( bytes.data(), &header, sizeof( header ) );
        std::memcpy( bytes.data() + sizeof( header ), data.data(), data.size() * sizeof( float ) );
        return Common::MakeSuccess( std::move( bytes ) );
    }

    Common::ResultStr<VertexAnimationHeader> ReadVertexAnimationHeader( const std::string_view bytes )
    {
        VertexAnimationHeader header;
        if ( bytes.size() < sizeof( header ) )
            return Common::MakeError<VertexAnimationHeader>(
                 std::format( "{} bytes, shorter than the entry's header", bytes.size() ) );
        std::memcpy( &header, bytes.data(), sizeof( header ) );
        const VertexAnimationHeader expected;
        if ( header.Magic != expected.Magic || header.Version != expected.Version )
            return Common::MakeError<VertexAnimationHeader>(
                 std::format( "not a version {} vertex animation (magic {:08x}, version {})", expected.Version,
                              header.Magic, header.Version ) );
        const uint64_t floats =
             static_cast<uint64_t>( header.Frames ) * header.VertexCount * kVertexAnimationFloatsPerVertex;
        if ( header.Frames == 0 || header.VertexCount == 0 ||
             bytes.size() != sizeof( header ) + floats * sizeof( float ) )
            return Common::MakeError<VertexAnimationHeader>(
                 std::format( "{} frames of {} vertices do not fill {} bytes", header.Frames, header.VertexCount,
                              bytes.size() ) );
        return Common::MakeSuccess( header );
    }

    void SetVertexAnimationDeriving( const bool deriving )
    {
        g_Deriving.store( deriving, std::memory_order_relaxed );
    }

    Common::ResultStr<LoadedVertexAnimation> LoadVertexAnimation( const SkinnedMesh&              mesh,
                                                                  const Animation::AnimationClip& clip )
    {
        const auto key = VertexAnimationKey( mesh, clip );
        if ( !key )
            return Common::MakeError<LoadedVertexAnimation>( key.GetError() );
        LoadedVertexAnimation loaded;
        loaded.Key = key.GetValue();
        if ( auto hit = Common::DDC::Get( kVertexAnimationDeriver, loaded.Key ) )
            loaded.Bytes = std::move( *hit );
        else
        {
            if ( !g_Deriving.load( std::memory_order_relaxed ) )
                return Common::MakeError<LoadedVertexAnimation>( std::format(
                     "no cooked entry {:016x} (the package cook derives every skinned mesh x clip; this one was "
                     "not cooked)",
                     loaded.Key ) );
            auto built = BuildVertexAnimation( mesh, clip );
            if ( !built )
                return Common::MakeError<LoadedVertexAnimation>( built.GetError() );
            loaded.Bytes      = built.ExtractValue();
            loaded.DerivedNow = true;
            if ( const auto put = Common::DDC::Put( kVertexAnimationDeriver, loaded.Key, loaded.Bytes ); !put )
                return Common::MakeError<LoadedVertexAnimation>(
                     std::format( "derived but not cached: {}", put.GetError() ) );
        }
        if ( const auto header = ReadVertexAnimationHeader( loaded.Bytes ); !header )
            return Common::MakeError<LoadedVertexAnimation>(
                 std::format( "entry {:016x}: {}", loaded.Key, header.GetError() ) );
        return Common::MakeSuccess( std::move( loaded ) );
    }
} // namespace Desert::Assets
