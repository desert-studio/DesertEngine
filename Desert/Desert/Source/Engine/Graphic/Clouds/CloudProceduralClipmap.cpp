#include "CloudProceduralClipmap.hpp"

#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

namespace Desert::Graphic
{
    static_assert( kCloudClipLevelSlots == Assets::kCloudProceduralClipLevels,
                   "the payload carries one u_CloudLevel per clip level" );
    static_assert( kCloudFarBandSlots == Assets::kCloudFarBands,
                   "the payload carries one u_CloudFarPresence / u_CloudFarProfile per far band" );

    namespace
    {
        constexpr uint32_t kLastLevel = Assets::kCloudProceduralClipLevels - 1u;

        /// [start, start + count) world voxels of one axis as runs of texels: one run, or two where it crosses
        /// the torus' edge; the whole axis when count reaches the side.
        std::vector<std::pair<uint32_t, uint32_t>> TexelRuns( int32_t start, uint32_t count, uint32_t side )
        {
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            if ( count == 0u )
                return runs;
            if ( count >= side )
            {
                runs.emplace_back( 0u, side );
                return runs;
            }
            const uint32_t texel = Assets::CloudProceduralLevelTexel( start, side );
            const uint32_t first = std::min( count, side - texel );
            runs.emplace_back( texel, first );
            if ( first < count )
                runs.emplace_back( 0u, count - first );
            return runs;
        }

        /// The periodic region bake is laid out from its corner (texel 0 = the corner's voxel); the clipmap
        /// keeps every level in torus order (texel = world voxel mod side). Rows of x are rotated, then rows
        /// of z — exact byte moves, nothing resampled.
        std::vector<unsigned char> RotateIntoTorus( const std::vector<unsigned char>& fromCorner,
                                                    const glm::ivec2& originVoxel, uint32_t side )
        {
            const uint32_t height = Assets::kCloudProceduralVolumeHeight;
            const size_t   texel  = Assets::kCloudProceduralBytesPerVoxel;
            const size_t   row    = static_cast<size_t>( side ) * texel;

            std::vector<unsigned char> torus( fromCorner.size() );
            const uint32_t             shiftX = Assets::CloudProceduralLevelTexel( originVoxel.x, side );
            const uint32_t             shiftZ = Assets::CloudProceduralLevelTexel( originVoxel.y, side );
            for ( uint32_t z = 0; z < side; ++z )
            {
                const uint32_t tz = ( z + shiftZ ) % side;
                for ( uint32_t y = 0; y < height; ++y )
                {
                    const unsigned char* src = fromCorner.data() + ( static_cast<size_t>( z ) * height + y ) * row;
                    unsigned char*       dst = torus.data() + ( static_cast<size_t>( tz ) * height + y ) * row;
                    // corner voxel x lands on texel (x + shiftX) mod side: two contiguous moves.
                    const size_t headTexels = side - shiftX;
                    std::memcpy( dst + shiftX * texel, src, headTexels * texel );
                    std::memcpy( dst, src + headTexels * texel, shiftX * texel );
                }
            }
            return torus;
        }
    } // namespace

    CloudProceduralClipmap::~CloudProceduralClipmap()
    {
        // Told to stop and not waited for: every job owns copies of what it reads (parameters, origin,
        // boxes, its flag), so dropping its future is safe — the same arrangement the single bake had.
        CancelAll();
    }

    std::vector<Assets::CloudProceduralVoxelBox>
    CloudProceduralClipmap::EnteringBoxes( const glm::ivec2& from, const glm::ivec2& to, uint32_t side )
    {
        std::vector<Assets::CloudProceduralVoxelBox> boxes;
        const glm::ivec2                             delta = to - from;
        if ( delta == glm::ivec2( 0 ) )
            return boxes;

        const uint32_t dx = static_cast<uint32_t>( std::abs( delta.x ) );
        const uint32_t dz = static_cast<uint32_t>( std::abs( delta.y ) );
        if ( dx >= side || dz >= side )
        {
            boxes.push_back( { 0u, 0u, side, side } );
            return boxes;
        }

        // THE COLUMNS THAT ENTER ALONG X, over the whole depth of the new window.
        const int32_t enterX = delta.x > 0 ? from.x + static_cast<int32_t>( side ) : to.x;
        for ( const auto& [x, w] : TexelRuns( enterX, dx, side ) )
            for ( const auto& [z, d] : TexelRuns( to.y, side, side ) )
                boxes.push_back( { x, z, w, d } );

        // THE ROWS THAT ENTER ALONG Z, over only the columns both windows share — the rest were just baked.
        const int32_t enterZ = delta.y > 0 ? from.y + static_cast<int32_t>( side ) : to.y;
        const int32_t keptX  = std::max( from.x, to.x );
        for ( const auto& [x, w] : TexelRuns( keptX, side - dx, side ) )
            for ( const auto& [z, d] : TexelRuns( enterZ, dz, side ) )
                boxes.push_back( { x, z, w, d } );
        return boxes;
    }

    glm::ivec2 CloudProceduralClipmap::WantedOrigin( const Assets::CloudProceduralFieldParams& params,
                                                     uint32_t level, const glm::vec2& cameraKm ) const
    {
        return Assets::CloudProceduralLevelOriginVoxel( params, level, cameraKm.x, cameraKm.y );
    }

    CloudProceduralClipmap::Bake
    CloudProceduralClipmap::StartBake( const Assets::CloudProceduralFieldParams& params, uint32_t level,
                                       const glm::ivec2&                            origin,
                                       std::vector<Assets::CloudProceduralVoxelBox> boxes, bool cached ) const
    {
        Bake bake;
        bake.Level  = level;
        bake.Origin = origin;
        bake.Boxes  = boxes;

        bake.Future = Common::JobSystem::Get().Async(
             [params, level, origin, boxes = std::move( boxes ), cached,
              flag = bake.Flag]() -> Common::ResultStr<BakedBytes>
             {
                 DESERT_PROFILE_SCOPE( "Clouds: Modelling clipmap bake" );
                 const auto progress = [&flag]( float fraction )
                 {
                     flag->Fraction.store( fraction, std::memory_order_relaxed );
                     return !flag->Cancelled.load( std::memory_order_relaxed );
                 };

                 BakedBytes out;
                 if ( level == kLastLevel )
                 {
                     // THE FAR PATH: the periodic region, baked whole at this level's corner and turned into
                     // torus order so that every level is addressed the same way.
                     const float voxelKm = Assets::CloudProceduralLevelVoxelKm( params, level );
                     auto baked = Assets::BakeCloudProceduralVolumeCached( params, glm::vec2( origin ) * voxelKm,
                                                                           progress );
                     if ( !baked )
                         return Common::MakeError<BakedBytes>( baked.GetError() );
                     out.FromCache       = baked.GetValue().FromCache;
                     out.CacheWriteError = baked.GetValue().CacheWriteError;
                     out.Boxes.push_back(
                          RotateIntoTorus( baked.GetValue().Voxels, origin, params.VolumeSideVoxels ) );
                     return Common::MakeSuccess( std::move( out ) );
                 }

                 if ( cached )
                 {
                     auto baked = Assets::BakeCloudProceduralLevelCached( params, level, origin, progress );
                     if ( !baked )
                         return Common::MakeError<BakedBytes>( baked.GetError() );
                     out.FromCache       = baked.GetValue().FromCache;
                     out.CacheWriteError = baked.GetValue().CacheWriteError;
                     out.Boxes.push_back( std::move( baked.GetValue().Voxels ) );
                     return Common::MakeSuccess( std::move( out ) );
                 }

                 const float count = static_cast<float>( boxes.size() );
                 for ( size_t i = 0; i < boxes.size(); ++i )
                 {
                     const float done = static_cast<float>( i );
                     auto        baked =
                          Assets::BakeCloudProceduralBox( params, level, origin, boxes[i], [&]( float fraction )
                                                          { return progress( ( done + fraction ) / count ); } );
                     if ( !baked )
                         return Common::MakeError<BakedBytes>( baked.GetError() );
                     out.Boxes.push_back( std::move( baked.GetValue() ) );
                 }
                 return Common::MakeSuccess( std::move( out ) );
             } );
        return bake;
    }

    Common::BoolResultStr CloudProceduralClipmap::EnsureImage( uint32_t side )
    {
        if ( m_Image && m_Image->GetWidth() == side )
            return BOOLSUCCESS;

        // A DIFFERENT GRID IS A DIFFERENT IMAGE, and only then: frames in flight may still sample the old
        // one, so the device is idled once, as the single volume did on every rebake. Scrolling never comes
        // here.
        if ( m_Image )
            Renderer::GetInstance().WaitDeviceIdle();

        const Core::Formats::Image3DSpecification spec{
             .Tag        = "CloudModellingClipmap",
             .Width      = side,
             .Height     = Assets::kCloudProceduralVolumeHeight * kLevels,
             .Depth      = side,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Data       = Core::Formats::EmptyPixelData{},
             .Properties = Core::Formats::Sample,
        };
        m_Image = Image3D::Create( spec );
        if ( !m_Image )
            return Common::MakeFormattedError<bool>(
                 "the {}x{}x{} RGBA8 cloud modelling clipmap could not be created on the device", side,
                 Assets::kCloudProceduralVolumeHeight * kLevels, side );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    CloudProceduralClipmap::Write( const std::vector<std::pair<const Bake*, const BakedBytes*>>& landed )
    {
        std::vector<Image3D::RegionWrite> writes;
        for ( const auto& [bake, bytes] : landed )
        {
            if ( bytes->Boxes.size() != bake->Boxes.size() )
                return Common::MakeFormattedError<bool>( "level {} bake returned {} boxes for {}", bake->Level,
                                                         bytes->Boxes.size(), bake->Boxes.size() );
            for ( size_t i = 0; i < bake->Boxes.size(); ++i )
            {
                const Assets::CloudProceduralVoxelBox& box = bake->Boxes[i];
                writes.push_back( Image3D::RegionWrite{
                     .X      = box.X,
                     .Y      = bake->Level * Assets::kCloudProceduralVolumeHeight,
                     .Z      = box.Z,
                     .Width  = box.Width,
                     .Height = Assets::kCloudProceduralVolumeHeight,
                     .Depth  = box.Depth,
                     .Bytes  = bytes->Boxes[i].data(),
                     .Size   = bytes->Boxes[i].size(),
                } );
            }
        }
        return m_Image->WriteRegions( writes );
    }

    void CloudProceduralClipmap::BeginShape( const Assets::CloudProceduralFieldParams& params,
                                             const glm::vec2&                          cameraKm )
    {
        CancelAll();

        m_ShapeParams       = params;
        m_ShapeInFlight     = true;
        m_ShapeStarted      = std::chrono::steady_clock::now();
        const uint32_t side = params.VolumeSideVoxels;
        for ( uint32_t level = 0; level < kLevels; ++level )
        {
            m_ShapeOrigin[level] = WantedOrigin( params, level, cameraKm );
            m_ShapeBakes[level]  = StartBake( params, level, m_ShapeOrigin[level], { { 0u, 0u, side, side } },
                                              /*cached=*/true );
        }
    }

    void CloudProceduralClipmap::CancelAll()
    {
        const auto cancel = [this]( std::optional<Bake>& bake )
        {
            if ( !bake )
                return;
            bake->Flag->Cancelled.store( true, std::memory_order_relaxed );
            bake.reset();
            ++m_Cancelled;
        };
        for ( auto& bake : m_ShapeBakes )
            cancel( bake );
        for ( auto& bake : m_Scroll )
            cancel( bake );
        for ( auto& landed : m_ShapeLanded )
            landed.reset();
        m_ShapeInFlight = false;
    }

    float CloudProceduralClipmap::ShapeProgress() const
    {
        float slowest = 1.0f;
        for ( const auto& bake : m_ShapeBakes )
            if ( bake )
                slowest = std::min( slowest, bake->Flag->Fraction.load( std::memory_order_relaxed ) );
        return slowest;
    }

    Common::ResultStr<std::optional<CloudProceduralClipmap::ShapeLanded>>
    CloudProceduralClipmap::Update( const glm::vec2& cameraKm, bool block )
    {
        using Result     = std::optional<ShapeLanded>;
        const auto ready = []( const std::optional<Bake>& bake )
        { return bake && bake->Future.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready; };

        // THE FIRST SHAPE OF A VIEW BLOCKS: with nothing on the device there is no sky to show meanwhile, and
        // a headless shot would otherwise finish its frames before the bake.
        if ( block && m_ShapeInFlight && !m_Valid )
            for ( auto& bake : m_ShapeBakes )
                if ( bake )
                    bake->Future.wait();

        Result landedShape;
        if ( m_ShapeInFlight )
        {
            for ( uint32_t level = 0; level < kLevels; ++level )
            {
                if ( !ready( m_ShapeBakes[level] ) )
                    continue;
                auto baked = m_ShapeBakes[level]->Future.get();
                m_ShapeBakes[level].reset();
                if ( !baked )
                {
                    CancelAll();
                    return Common::MakeFormattedError<Result>( "clip level {}: {}", level, baked.GetError() );
                }
                m_ShapeLanded[level] = std::move( baked.GetValue() );
            }

            const bool all = std::all_of( m_ShapeLanded.begin(), m_ShapeLanded.end(),
                                          []( const std::optional<BakedBytes>& one ) { return one.has_value(); } );
            if ( all )
            {
                // ALL LEVELS AT ONCE, so the sky never shows one shape near and another far.
                if ( auto image = EnsureImage( m_ShapeParams.VolumeSideVoxels ); !image )
                {
                    CancelAll();
                    m_Valid = false;
                    return Common::MakeError<Result>( image.GetError() );
                }
                const uint32_t                                         side = m_ShapeParams.VolumeSideVoxels;
                std::array<Bake, kLevels>                              whole;
                std::vector<std::pair<const Bake*, const BakedBytes*>> writes;
                ShapeLanded                                            report;
                for ( uint32_t level = 0; level < kLevels; ++level )
                {
                    whole[level].Level = level;
                    whole[level].Boxes = { { 0u, 0u, side, side } };
                    writes.emplace_back( &whole[level], &*m_ShapeLanded[level] );
                    report.LevelsFromCache += m_ShapeLanded[level]->FromCache ? 1u : 0u;
                    if ( report.CacheWriteError.empty() )
                        report.CacheWriteError = m_ShapeLanded[level]->CacheWriteError;
                }
                if ( auto wrote = Write( writes ); !wrote )
                {
                    CancelAll();
                    m_Valid = false;
                    return Common::MakeError<Result>( wrote.GetError() );
                }
                m_FarStatistics =
                     Assets::CloudProceduralFarStatisticsOf( m_ShapeLanded[kLastLevel]->Boxes.front(), side );
                for ( auto& landed : m_ShapeLanded )
                    landed.reset();

                m_DeviceParams  = m_ShapeParams;
                m_DeviceOrigin  = m_ShapeOrigin;
                m_Valid         = true;
                m_ShapeInFlight = false;
                report.Ms =
                     std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_ShapeStarted )
                          .count();
                landedShape = std::move( report );
            }
        }

        // SCROLLING, only for a shape that is on the device and not being replaced: a slab of the old shape
        // would be written beside the new shape's levels the moment those land.
        if ( !m_Valid || m_ShapeInFlight )
            return Common::MakeSuccess( std::move( landedShape ) );

        std::vector<std::pair<const Bake*, const BakedBytes*>> writes;
        std::array<std::optional<BakedBytes>, kLevels>         bytes;
        for ( uint32_t level = 0; level < kLevels; ++level )
        {
            if ( !ready( m_Scroll[level] ) )
                continue;
            auto baked = m_Scroll[level]->Future.get();
            if ( !baked )
            {
                m_Scroll[level].reset();
                return Common::MakeFormattedError<Result>( "clip level {} slab: {}", level, baked.GetError() );
            }
            bytes[level] = std::move( baked.GetValue() );
            writes.emplace_back( &*m_Scroll[level], &*bytes[level] );
        }
        if ( !writes.empty() )
        {
            // ONE SUBMISSION FOR EVERY SLAB THAT LANDED THIS FRAME, and the corner moves in the same frame
            // the bytes do: the uniform and the texels describe one window.
            if ( auto wrote = Write( writes ); !wrote )
            {
                for ( auto& bake : m_Scroll )
                    bake.reset();
                return Common::MakeError<Result>( wrote.GetError() );
            }
            for ( uint32_t level = 0; level < kLevels; ++level )
                if ( bytes[level] )
                {
                    if ( level == kLastLevel )
                        m_FarStatistics = Assets::CloudProceduralFarStatisticsOf( bytes[level]->Boxes.front(),
                                                                                  m_DeviceParams.VolumeSideVoxels );
                    m_DeviceOrigin[level] = m_Scroll[level]->Origin;
                    m_Scroll[level].reset();
                }
        }

        const uint32_t side = m_DeviceParams.VolumeSideVoxels;
        for ( uint32_t level = 0; level < kLevels; ++level )
        {
            if ( m_Scroll[level] )
                continue; // one bake per level in flight; the next frame starts from where it lands
            const glm::ivec2 wanted = WantedOrigin( m_DeviceParams, level, cameraKm );
            if ( wanted == m_DeviceOrigin[level] )
                continue;

            if ( level == kLastLevel )
            {
                m_Scroll[level] = StartBake( m_DeviceParams, level, wanted, { { 0u, 0u, side, side } }, true );
                continue;
            }
            auto       boxes = EnteringBoxes( m_DeviceOrigin[level], wanted, side );
            const bool whole = boxes.size() == 1u && boxes[0].Width == side && boxes[0].Depth == side;
            m_Scroll[level]  = StartBake( m_DeviceParams, level, wanted, std::move( boxes ), whole );
        }
        return Common::MakeSuccess( std::move( landedShape ) );
    }

    std::array<glm::vec4, CloudProceduralClipmap::kLevels> CloudProceduralClipmap::LevelUniforms() const
    {
        std::array<glm::vec4, kLevels> levels;
        for ( uint32_t level = 0; level < kLevels; ++level )
        {
            if ( !m_Valid )
            {
                // Finite and harmless before the first shape lands: the species count is what stops the
                // volume being read on those frames.
                levels[level] = glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f );
                continue;
            }
            const float     voxelKm = Assets::CloudProceduralLevelVoxelKm( m_DeviceParams, level );
            const float     sideKm  = Assets::CloudProceduralLevelSideKm( m_DeviceParams, level );
            const glm::vec2 corner  = glm::vec2( m_DeviceOrigin[level] ) * voxelKm;
            levels[level]           = glm::vec4( corner.x, corner.y, 1.0f / sideKm, voxelKm );
        }
        return levels;
    }

    glm::vec2 CloudProceduralClipmap::RegionOriginKm() const
    {
        if ( !m_Valid )
            return glm::vec2( 0.0f );
        return glm::vec2( m_DeviceOrigin[kLastLevel] ) *
               Assets::CloudProceduralLevelVoxelKm( m_DeviceParams, kLastLevel );
    }
} // namespace Desert::Graphic
