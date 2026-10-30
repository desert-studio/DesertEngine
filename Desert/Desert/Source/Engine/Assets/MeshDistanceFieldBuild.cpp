// Ported from UE 5.8 Engine/Source/Developer/MeshUtilities/Private/MeshDistanceFieldUtilities.cpp
// (FSparseMeshDistanceFieldAsyncTask::DoWork:157-288, BuildSignedDistanceField:346-470 - the per-mesh resolution
// from the voxel density and the per-mesh cap, the positive-extent bounds and the mostly-two-sided expansion, the
// per-mip indirection layout with a one-voxel border, the band-limited 8-bit encoding, brick validity, the volume
// -> virtual UV transform) and MeshRepresentationCommon.cpp (the Embree scene), ON EMBREE as UE: per sample one
// rtcPointQuery for the unsigned distance within the band (EmbreePointQueryFunction: closest point on each
// candidate triangle, the query radius shrinking to the best so far), and, when the surface lies within the band,
// the sign from 2 x 49 stratified hemisphere rays (rtcIntersect1): inside when more than a quarter of them hit a
// BACKFACE first. A TWO-SIDED triangle (its section's material is two-sided: foliage cards, leaves, awnings) stops
// a ray but never counts as a backface, as UE's IsHitTwoSided. Samples beyond the band are not traced and stay
// outside (bTraceRays). One volume per mesh, as UE: a mesh too large for the per-mesh cap at its thinnest extent
// gets the cap's voxels (UE leaves it to artists to split such a mesh).
// UE: one Embree device per mesh build (FEmbreeScene). Here one device per process (EmbreeDevice) and one scene
// per build - the device only holds the thread pool and the error state, the scene is the mesh.
// The bricks of a mip are a ParallelFor, as UE; the distance-field workers also build several fields at once
// (MeshDistanceField.cpp), and the worker building a field takes part in its loop.
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Assets/StratifiedHemisphereSamples.hpp>

#include <Common/Core/JobSystem.hpp>

#include <embree4/rtcore.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>

namespace Desert::Assets
{
    namespace
    {
        // Ericson, Real-Time Collision Detection 5.1.5: the closest point of triangle abc to p (UE
        // FMath::ClosestPointOnTriangleToPoint).
        glm::vec3 ClosestPointOnTriangle( const glm::vec3& p, const glm::vec3& a, const glm::vec3& b,
                                          const glm::vec3& c )
        {
            const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
            const float     d1 = glm::dot( ab, ap ), d2 = glm::dot( ac, ap );
            if ( d1 <= 0.0f && d2 <= 0.0f )
                return a;
            const glm::vec3 bp = p - b;
            const float     d3 = glm::dot( ab, bp ), d4 = glm::dot( ac, bp );
            if ( d3 >= 0.0f && d4 <= d3 )
                return b;
            const float vc = d1 * d4 - d3 * d2;
            if ( vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f )
                return a + ab * ( d1 / ( d1 - d3 ) );
            const glm::vec3 cp = p - c;
            const float     d5 = glm::dot( ab, cp ), d6 = glm::dot( ac, cp );
            if ( d6 >= 0.0f && d5 <= d6 )
                return c;
            const float vb = d5 * d2 - d1 * d6;
            if ( vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f )
                return a + ac * ( d2 / ( d2 - d6 ) );
            const float va = d3 * d6 - d5 * d4;
            if ( va <= 0.0f && ( d4 - d3 ) >= 0.0f && ( d5 - d6 ) >= 0.0f )
                return b + ( c - b ) * ( ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ) );
            const float denom = 1.0f / ( va + vb + vc );
            return a + ab * ( vb * denom ) + ac * ( vc * denom );
        }

        struct SdfMesh
        {
            std::vector<glm::vec3>  Positions;
            std::vector<glm::uvec3> Triangles;
            std::vector<glm::vec3>  Normal; // cross(b - a, c - a): a ray along it hits the triangle's back
            // Per triangle: 1 = its material is two-sided, so it takes no part in the inside/outside vote.
            std::vector<uint8_t> TwoSided;
        };

        // THE DEVICE: created on the first build and kept for the process (Embree devices are thread-safe; every
        // build commits and queries its own scene on it).
        RTCDevice EmbreeDevice()
        {
            static const RTCDevice device = rtcNewDevice( nullptr );
            return device;
        }

        // THE SCENE (UE MeshRepresentationCommon.cpp SetupEmbreeScene): one triangle geometry of the mesh, its
        // buffers copied into Embree's own (padded) storage. Released with the build.
        struct EmbreeScene
        {
            RTCScene Scene                               = nullptr;
            EmbreeScene()                                = default;
            EmbreeScene( const EmbreeScene& )            = delete;
            EmbreeScene& operator=( const EmbreeScene& ) = delete;
            ~EmbreeScene()
            {
                if ( Scene != nullptr )
                    rtcReleaseScene( Scene );
            }
        };

        // The error, or nothing when the scene is committed.
        std::optional<std::string> CommitEmbreeScene( const SdfMesh& mesh, EmbreeScene& out )
        {
            const RTCDevice device = EmbreeDevice();
            if ( device == nullptr )
                return std::format( "distance field: Embree device not created (error {})",
                                    static_cast<int>( rtcGetDeviceError( nullptr ) ) );
            out.Scene = rtcNewScene( device );
            rtcSetSceneFlags( out.Scene, RTC_SCENE_FLAG_NONE );
            const RTCGeometry geometry = rtcNewGeometry( device, RTC_GEOMETRY_TYPE_TRIANGLE );
            auto* vertices = static_cast<float*>( rtcSetNewGeometryBuffer( geometry, RTC_BUFFER_TYPE_VERTEX, 0,
                                                                           RTC_FORMAT_FLOAT3, 3 * sizeof( float ),
                                                                           mesh.Positions.size() ) );
            auto* indices  = static_cast<uint32_t*>(
                 rtcSetNewGeometryBuffer( geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                           3 * sizeof( uint32_t ), mesh.Triangles.size() ) );
            if ( vertices == nullptr || indices == nullptr )
            {
                rtcReleaseGeometry( geometry );
                return std::format( "distance field: Embree buffers for {} vertices, {} triangles "
                                    "not allocated (error {})",
                                    mesh.Positions.size(), mesh.Triangles.size(),
                                    static_cast<int>( rtcGetDeviceError( device ) ) );
            }
            for ( size_t v = 0; v < mesh.Positions.size(); ++v )
            {
                vertices[3 * v + 0] = mesh.Positions[v].x;
                vertices[3 * v + 1] = mesh.Positions[v].y;
                vertices[3 * v + 2] = mesh.Positions[v].z;
            }
            for ( size_t t = 0; t < mesh.Triangles.size(); ++t )
            {
                indices[3 * t + 0] = mesh.Triangles[t].x;
                indices[3 * t + 1] = mesh.Triangles[t].y;
                indices[3 * t + 2] = mesh.Triangles[t].z;
            }
            rtcCommitGeometry( geometry );
            rtcAttachGeometry( out.Scene, geometry );
            rtcReleaseGeometry( geometry );
            rtcCommitScene( out.Scene );
            const RTCError error = rtcGetDeviceError( device );
            if ( error != RTC_ERROR_NONE )
                return std::format( "distance field: Embree scene of {} triangles failed: {}",
                                    mesh.Triangles.size(), rtcGetErrorString( error ) );
            return std::nullopt;
        }

        // UE FEmbreePointQueryContext + EmbreePointQueryFunction: the closest point of each candidate triangle,
        // shrinking the query radius to the best distance so far.
        struct PointQueryContext : RTCPointQueryContext
        {
            const SdfMesh* Mesh = nullptr;
        };

        bool PointQueryFunction( RTCPointQueryFunctionArguments* args )
        {
            const auto*       context = static_cast<const PointQueryContext*>( args->context );
            float&            bestSq  = *static_cast<float*>( args->userPtr );
            const glm::uvec3& tri     = context->Mesh->Triangles[args->primID];
            const glm::vec3   p( args->query->x, args->query->y, args->query->z );
            const glm::vec3   c =
                 ClosestPointOnTriangle( p, context->Mesh->Positions[tri.x], context->Mesh->Positions[tri.y],
                                         context->Mesh->Positions[tri.z] );
            const float dSq = glm::dot( c - p, c - p );
            if ( dSq < bestSq )
            {
                bestSq              = dSq;
                args->query->radius = std::sqrt( dSq );
                return true; // the radius shrank
            }
            return false;
        }

        // UE MeshUtilities::GenerateStratifiedUniformHemisphereSamples(49) twice from one stream at seed 0, the
        // second set mirrored in Z (BuildSignedDistanceField, bUsePointQuery): 7 x 7 jittered strata per
        // hemisphere.
        constexpr int          kDfSignSamplesPerHemisphere = 49;
        std::vector<glm::vec3> SignDirections()
        {
            Common::Math::RandomStream random;
            std::vector<glm::dvec3>    directions;
            for ( const double zSign : { 1.0, -1.0 } )
                AppendStratifiedUniformHemisphereSamples( kDfSignSamplesPerHemisphere, random, zSign, directions );
            return { directions.begin(), directions.end() };
        }

        // UE FMath::LineBoxIntersection: does the segment start -> end touch the box (slabs, inclusive)?
        bool SegmentTouchesBox( const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& start,
                                const glm::vec3& end )
        {
            float           t0 = 0.0f, t1 = 1.0f;
            const glm::vec3 d = end - start;
            for ( int a = 0; a < 3; ++a )
            {
                if ( std::abs( d[a] ) < 1.0e-12f )
                {
                    if ( start[a] < lo[a] || start[a] > hi[a] )
                        return false;
                    continue;
                }
                float tn = ( lo[a] - start[a] ) / d[a], tf = ( hi[a] - start[a] ) / d[a];
                if ( tn > tf )
                    std::swap( tn, tf );
                t0 = std::max( t0, tn );
                t1 = std::min( t1, tf );
                if ( t0 > t1 )
                    return false;
            }
            return true;
        }

        // ONE SAMPLE, AS UE's DoWork: the unsigned distance by the point query (capped at the trace distance);
        // within it, the sign by the rays. Rays start pulled back by 1e-4 of the trace distance (a sample exactly
        // on a triangle - box corners - still sees it); a ray whose segment misses the volume box is not traced
        // but still counts in the quarter. Stops as soon as the remaining rays cannot change the vote.
        float SignedSampleDistance( const SdfMesh& mesh, const RTCScene scene,
                                    const std::vector<glm::vec3>& directions, const glm::vec3& p,
                                    const float trace, const glm::vec3& boxLo, const glm::vec3& boxHi )
        {
            RTCPointQuery query;
            query.x      = p.x;
            query.y      = p.y;
            query.z      = p.z;
            query.time   = 0.0f;
            query.radius = trace;
            PointQueryContext context;
            rtcInitPointQueryContext( &context );
            context.Mesh = &mesh;
            float bestSq = ( 2.0f * trace ) * ( 2.0f * trace );
            rtcPointQuery( scene, &query, &context, PointQueryFunction, &bestSq );
            const float closest = std::sqrt( bestSq );
            if ( !( closest <= trace ) )
                return trace; // bTraceRays = false: outside, the band away
            const float distance = closest;

            const int total = static_cast<int>( directions.size() );
            int       hit = 0, hitBack = 0;
            for ( int i = 0; i < total; ++i )
            {
                const glm::vec3& unit = directions[i];
                const glm::vec3  end  = p + unit * trace;
                if ( SegmentTouchesBox( boxLo, boxHi, p, end ) )
                {
                    const glm::vec3 start = p - 1.0e-4f * trace * unit;
                    RTCRayHit       ray;
                    ray.ray.org_x     = start.x;
                    ray.ray.org_y     = start.y;
                    ray.ray.org_z     = start.z;
                    ray.ray.dir_x     = unit.x * trace;
                    ray.ray.dir_y     = unit.y * trace;
                    ray.ray.dir_z     = unit.z * trace;
                    ray.ray.tnear     = 0.0f;
                    ray.ray.tfar      = 1.0f;
                    ray.ray.time      = 0.0f;
                    ray.ray.mask      = 0xFFFFFFFFu;
                    ray.ray.id        = 0;
                    ray.ray.flags     = 0;
                    ray.hit.geomID    = RTC_INVALID_GEOMETRY_ID;
                    ray.hit.primID    = RTC_INVALID_GEOMETRY_ID;
                    ray.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
                    rtcIntersect1( scene, &ray, nullptr );
                    if ( ray.hit.geomID != RTC_INVALID_GEOMETRY_ID && ray.hit.primID != RTC_INVALID_GEOMETRY_ID )
                    {
                        ++hit;
                        if ( mesh.TwoSided[ray.hit.primID] == 0 &&
                             glm::dot( unit, mesh.Normal[ray.hit.primID] ) > 0.0f )
                            ++hitBack;
                    }
                }
                if ( 4 * hitBack > total ) // Hit > 0 && HitBack > .25f * SampleDirections->Num()
                    return -distance;
                if ( 4 * ( hitBack + total - 1 - i ) <= total )
                    return distance;
            }
            return hit > 0 && 4 * hitBack > total ? -distance : distance;
        }

        uint8_t Quantize( const float localDistance, const float localToVolume, const glm::vec2 scaleBias )
        {
            const float volumeDistance = localDistance * localToVolume;
            const float rescaled       = ( volumeDistance - scaleBias.y ) / scaleBias.x;
            return static_cast<uint8_t>( std::clamp( std::floor( rescaled * 255.0f + 0.5f ), 0.0f, 255.0f ) );
        }

        // THE VOLUME over the box [lo, hi] of mesh space, mip0Dims bricks along each axis (UE
        // BuildSignedDistanceField from the bounds on): per mip and brick UE's DoWork, the bricks that are not
        // saturated, the volume -> virtual UV transform. Empty when @p cancel was raised (polled per brick, as
        // UE's running async tasks check it).
        std::optional<MeshDistanceFieldVolume> BuildVolume( const SdfMesh& mesh, const RTCScene scene,
                                                            const std::vector<glm::vec3>& directions,
                                                            const glm::vec3& lo, const glm::vec3& hi,
                                                            const glm::ivec3&              mip0Dims,
                                                            const MeshDistanceFieldCancel& cancel )
        {
            const glm::vec3         extent        = ( hi - lo ) * 0.5f;
            const glm::vec3         size          = hi - lo;
            const float             localToVolume = 1.0f / std::max( { extent.x, extent.y, extent.z } );
            MeshDistanceFieldVolume out;
            out.LocalSpaceMeshBounds     = { lo, hi };
            const glm::vec3 volumeExtent = extent * localToVolume;
            constexpr int   kUnique      = static_cast<int>( kDistanceFieldUniqueBrickSize );
            constexpr int   kBrick       = static_cast<int>( kDistanceFieldBrickSize );
            constexpr int   kBorder      = static_cast<int>( kDistanceFieldObjectBorder );

            for ( uint32_t mipIndex = 0; mipIndex < kDistanceFieldNumMips; ++mipIndex )
            {
                const int        div  = 1 << mipIndex;
                const glm::ivec3 dims = ( mip0Dims + div - 1 ) / div;
                // A one-voxel border around the mesh box, for gradients by central differences at its faces.
                const glm::vec3 texelSize   = size / glm::vec3( dims * kUnique - 2 * kBorder );
                const glm::vec3 volumeMin   = lo - texelSize;
                const glm::vec3 volumeMax   = hi + texelSize;
                const glm::vec3 indirection = ( volumeMax - volumeMin ) / glm::vec3( dims );
                const glm::vec3 voxel       = indirection / static_cast<float>( kUnique );
                const float maxEncoding = glm::length( voxel * localToVolume ) * kDistanceFieldBandSizeInVoxels;
                const float trace       = maxEncoding / localToVolume;
                const glm::vec2 scaleBias( 2.0f * maxEncoding, -maxEncoding );

                SparseDistanceFieldMip& mip        = out.Mips[mipIndex];
                mip.IndirectionDimensions          = glm::uvec3( dims );
                mip.DistanceFieldToVolumeScaleBias = scaleBias;
                mip.IndirectionTable.assign( static_cast<size_t>( dims.x ) * dims.y * dims.z,
                                             kDistanceFieldInvalidBrick );
                // UE's DoWork: the bricks of a mip are a ParallelFor (each brick its own task, the results
                // gathered in brick order afterwards, so the field is the same bytes whatever the schedule). The
                // calling distance-field worker takes part, so a nested loop never waits on a busy pool.
                const size_t                      brickCount = mip.IndirectionTable.size();
                std::vector<std::vector<uint8_t>> bricks( brickCount );
                std::atomic<bool>                 stopped{ false };
                Common::JobSystem::Get().ParallelFor(
                     brickCount,
                     [&]( const size_t index )
                     {
                         if ( stopped.load( std::memory_order_relaxed ) )
                             return;
                         if ( cancel.IsCancelled() )
                         {
                             stopped.store( true, std::memory_order_relaxed );
                             return;
                         }
                         const int bx = static_cast<int>( index % static_cast<size_t>( dims.x ) );
                         const int by = static_cast<int>( ( index / static_cast<size_t>( dims.x ) ) % dims.y );
                         const int bz = static_cast<int>( index / ( static_cast<size_t>( dims.x ) * dims.y ) );
                         const glm::vec3      brickMin = volumeMin + glm::vec3( bx, by, bz ) * indirection;
                         std::vector<uint8_t> brick( kDistanceFieldBrickVoxels );
                         uint8_t              lowest = 255, highest = 0;
                         for ( int z = 0; z < kBrick; ++z )
                             for ( int y = 0; y < kBrick; ++y )
                                 for ( int x = 0; x < kBrick; ++x )
                                 {
                                     const glm::vec3 p = brickMin + glm::vec3( x, y, z ) * voxel;
                                     const float     d = SignedSampleDistance( mesh, scene, directions, p, trace,
                                                                               volumeMin, volumeMax );
                                     const uint8_t   q = Quantize( d, localToVolume, scaleBias );
                                     brick[( z * kBrick + y ) * kBrick + x] = q;
                                     lowest                                 = std::min( lowest, q );
                                     highest                                = std::max( highest, q );
                                 }
                         // UE keeps a brick only when it is not saturated at one end: all 255 is "outside, at
                         // least the band away" and all 0 "inside, at least the band deep" - both read back as
                         // the mip's maximum encoded distance without a brick.
                         if ( lowest < 255 && highest > 0 )
                             bricks[index] = std::move( brick );
                     } );
                if ( stopped.load( std::memory_order_relaxed ) || cancel.IsCancelled() )
                    return std::nullopt;
                for ( size_t index = 0; index < brickCount; ++index )
                    if ( !bricks[index].empty() )
                    {
                        mip.IndirectionTable[index] = mip.BrickCount();
                        mip.BrickData.insert( mip.BrickData.end(), bricks[index].begin(), bricks[index].end() );
                    }
                // Account for the border voxels: [-VolumePositionExtent, VolumePositionExtent] -> [uvMin, uvMin +
                // uvSize].
                const glm::vec3 uvMin   = glm::vec3( kBorder ) / glm::vec3( dims * kUnique );
                const glm::vec3 uvSize  = glm::vec3( dims * kUnique - 2 * kBorder ) / glm::vec3( dims * kUnique );
                const glm::vec3 uvScale = uvSize / ( 2.0f * volumeExtent );
                mip.VolumeToVirtualUVScale = uvScale;
                mip.VolumeToVirtualUVAdd   = volumeExtent * uvScale + uvMin;
            }
            return out;
        }
    } // namespace

    Common::ResultStr<MeshDistanceFieldData>
    BuildMeshDistanceField( const MeshDistanceFieldBuildInput&    input,
                            const MeshDistanceFieldBuildSettings& settings, const MeshDistanceFieldCancel& cancel )
    {
        if ( input.Indices.size() % 3 != 0 )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: {} indices are not whole triangles", input.Indices.size() );
        if ( input.TriangleTwoSided.size() != input.Indices.size() / 3 )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: {} two-sided flags for {} triangles", input.TriangleTwoSided.size(),
                 input.Indices.size() / 3 );
        if ( !( settings.VoxelDensity > 0.0f ) || settings.MaxPerMeshResolution < kDistanceFieldUniqueBrickSize ||
             !( settings.ResolutionScale > 0.0f ) )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: settings out of range (density {}, max resolution {}, scale {})",
                 settings.VoxelDensity, settings.MaxPerMeshResolution, settings.ResolutionScale );

        SdfMesh mesh;
        mesh.Positions       = input.Positions;
        size_t twoSidedCount = 0;
        for ( size_t i = 0; i < input.Indices.size(); i += 3 )
        {
            const glm::uvec3 tri( input.Indices[i], input.Indices[i + 1], input.Indices[i + 2] );
            if ( tri.x >= mesh.Positions.size() || tri.y >= mesh.Positions.size() ||
                 tri.z >= mesh.Positions.size() )
                return Common::MakeFormattedError<MeshDistanceFieldData>(
                     "distance field: triangle {} names vertex {} of {}", i / 3,
                     std::max( { tri.x, tri.y, tri.z } ), mesh.Positions.size() );
            mesh.Triangles.push_back( tri );
            const glm::vec3& a = mesh.Positions[tri.x];
            mesh.Normal.push_back( glm::cross( mesh.Positions[tri.y] - a, mesh.Positions[tri.z] - a ) );
            mesh.TwoSided.push_back( input.TriangleTwoSided[i / 3] );
            twoSidedCount += input.TriangleTwoSided[i / 3] != 0 ? 1 : 0;
        }

        MeshDistanceFieldData out;
        if ( mesh.Triangles.empty() )
            return Common::MakeSuccess( std::move( out ) ); // no surface: no volume, nothing to sample

        glm::vec3 lo( std::numeric_limits<float>::max() ), hi( std::numeric_limits<float>::lowest() );
        for ( const glm::uvec3& tri : mesh.Triangles )
            for ( int c = 0; c < 3; ++c )
            {
                lo = glm::min( lo, mesh.Positions[tri[c]] );
                hi = glm::max( hi, mesh.Positions[tri[c]] );
            }
        // Positive extents, so a plane still has a volume (UE: extent at least one unit).
        {
            const glm::vec3 centre = ( lo + hi ) * 0.5f;
            const glm::vec3 extent = glm::max( ( hi - lo ) * 0.5f, glm::vec3( 1.0f ) );
            lo                     = centre - extent;
            hi                     = centre + extent;
        }

        constexpr int  kUnique = static_cast<int>( kDistanceFieldUniqueBrickSize );
        const uint32_t perMeshMax =
             settings.ResolutionScale <= 1.0f ? settings.MaxPerMeshResolution / 2 : settings.MaxPerMeshResolution;
        const int maxBlocks =
             std::max( 1, static_cast<int>( std::min<uint32_t>(
                               ( perMeshMax + kDistanceFieldUniqueBrickSize / 2 ) / kDistanceFieldUniqueBrickSize,
                               kDistanceFieldMaxIndirectionDim - 1 ) ) );
        const float voxelsPerUnit = settings.VoxelDensity * settings.ResolutionScale;
        const auto  dimsFor       = [&]( const glm::vec3& size )
        {
            const glm::vec3 desired = size * voxelsPerUnit / static_cast<float>( kUnique );
            return glm::clamp( glm::ivec3( glm::round( desired ) ), glm::ivec3( 1 ), glm::ivec3( maxBlocks ) );
        };
        // UE bMostlyTwoSided (a quarter of the triangles or more): the bounds grow by one texel of the dimensions
        // they would get, so a card's field closes around it. UE's FInt32Vector(2 * .25f) truncates to zero, so
        // the texel is size / (dims * 7) - kept.
        if ( twoSidedCount * 4 >= mesh.Triangles.size() )
        {
            const glm::vec3 texel = ( hi - lo ) / glm::vec3( dimsFor( hi - lo ) * kUnique );
            lo -= texel;
            hi += texel;
        }
        const glm::ivec3 mip0Dims = dimsFor( hi - lo );

        EmbreeScene scene;
        if ( const auto failed = CommitEmbreeScene( mesh, scene ); failed.has_value() )
            return Common::MakeFormattedError<MeshDistanceFieldData>( "{}", *failed );
        auto volume = BuildVolume( mesh, scene.Scene, SignDirections(), lo, hi, mip0Dims, cancel );
        if ( !volume.has_value() )
            return Common::MakeError<MeshDistanceFieldData>( std::string( kMeshDistanceFieldCancelled ) );
        out.Volumes.push_back( std::move( *volume ) );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Assets
