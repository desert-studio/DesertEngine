#include "VideoService.hpp"

#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>

#include <pl_mpeg/pl_mpeg.h>

#include <algorithm>

namespace Desert::Runtime
{
    namespace
    {
        // pl_mpeg calls this for every frame decoded inside plm_decode(). We keep only the latest frame's
        // RGBA (pl_mpeg guarantees the plm_frame_t is valid for the duration of the callback) and flag it
        // for upload; the actual GPU upload happens once per UpdateAll, after decoding catches up.
        void PlmOnFrame( plm_t*, plm_frame_t* frame, void* user )
        {
            auto* vp = static_cast<VideoPlayback*>( user );
            if ( !vp || vp->Rgba.empty() || !frame )
                return;
            plm_frame_to_rgba( frame, vp->Rgba.data(), vp->Width * 4 );
            vp->Dirty = true;
        }

        Graphic::Image2D* ResolveTextureImage( const std::shared_ptr<Graphic::Texture2D>& tex )
        {
            if ( !tex )
                return nullptr;
            auto* imgService = ResourceRegistry::GetImageService();
            if ( !imgService )
                return nullptr;
            return static_cast<Graphic::Image2D*>( imgService->Resolve( tex->GetImageHandle() ) );
        }
    } // namespace

    VideoService::~VideoService()
    {
        Clear();
    }

    VideoPlayback* VideoService::GetOrOpen( const std::string& path )
    {
        if ( auto it = m_Videos.find( path ); it != m_Videos.end() )
            return &it->second;

        // Insert first so the callback user-pointer is the STABLE map element (unordered_map keeps node
        // addresses fixed across rehash), not a local temporary.
        VideoPlayback& vp = m_Videos[path];
        vp.Last           = std::chrono::steady_clock::now();

        auto raw = Common::Utils::FileSystem::ReadByteFileContent( path );
        if ( !raw || raw.GetValue().empty() )
            return &vp; // Valid stays false -> negative cache

        vp.Bytes   = raw.ExtractValue();
        plm_t* plm = plm_create_with_memory( vp.Bytes.data(), vp.Bytes.size(), 0 /*don't free our buffer*/ );
        if ( !plm )
            return &vp;

        plm_set_audio_enabled( plm, 0 ); // video-only for now
        plm_set_loop( plm, 1 );

        const int w = plm_get_width( plm );
        const int h = plm_get_height( plm );
        if ( w <= 0 || h <= 0 )
        {
            plm_destroy( plm );
            return &vp;
        }

        vp.Width  = w;
        vp.Height = h;
        vp.Rgba.assign( static_cast<size_t>( w ) * h * 4, 0 ); // black until the first frame is decoded

        // A STABLE texture we re-upload into every frame (its Image2D* never changes, so Render2D's
        // per-texture executor and the UI walk keep sampling the same handle).
        auto texResult = Graphic::Texture2D::Create(
             "video", static_cast<uint32_t>( w ), static_cast<uint32_t>( h ), Core::Formats::ImageFormat::RGBA8F,
             Core::Formats::ImagePixelData( vp.Rgba ) );
        if ( !texResult.IsSuccess() )
        {
            plm_destroy( plm );
            return &vp;
        }

        vp.Texture = texResult.ExtractValue();
        vp.Plm     = plm;
        vp.Valid   = true;
        plm_set_video_decode_callback( plm, PlmOnFrame, &vp );
        return &vp;
    }

    uint64_t VideoService::RegisterVideo( const std::string& path )
    {
        if ( path.empty() )
            return 0;
        // FromCookedPath, not FromKey -- one file is one handle whatever spelling registered it. See the
        // note in FontService::RegisterFont; the two services key their registries the same way.
        // The record of which file this number names is made by FromCookedPath itself, into
        // `Common::AssetPathIndex`; this service kept a private copy of that table and no longer does.
        return static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( path ) );
    }

    std::string VideoService::PathForHandle( uint64_t handle ) const
    {
        return Common::AssetPathIndex::PathFor( handle ).generic_string();
    }

    Graphic::Image2D* VideoService::Resolve( uint64_t handle )
    {
        if ( handle == 0 )
            return nullptr;
        const std::string path = PathForHandle( handle );
        if ( path.empty() )
            return nullptr;
        VideoPlayback* vp = GetOrOpen( path );
        if ( !vp || !vp->Valid )
            return nullptr;
        return ResolveTextureImage( vp->Texture );
    }

    void VideoService::UpdateAll()
    {
        const auto now = std::chrono::steady_clock::now();
        for ( auto& [path, vp] : m_Videos )
        {
            if ( !vp.Valid || !vp.Plm )
                continue;

            float dt = std::chrono::duration<float>( now - vp.Last ).count();
            vp.Last  = now;
            dt       = std::clamp( dt, 0.0f, 0.25f ); // cap catch-up after a stall / pause / first frame
            if ( dt <= 0.0f )
                continue;

            plm_decode( vp.Plm, dt ); // fires PlmOnFrame for each frame that fell due in dt

            if ( vp.Dirty )
            {
                if ( auto* img = ResolveTextureImage( vp.Texture ) )
                {
                    // THE DIRTY FLAG SURVIVES A REFUSED UPLOAD, and that is the whole content of this
                    // change. Clearing it regardless meant a frame that never reached the GPU was
                    // recorded as delivered, so the video froze on the previous picture and the decoder
                    // walked on past it — the one thing that could have corrected it was the flag that
                    // had just been thrown away. It stays set, so the next tick uploads vp.Rgba again.
                    const auto uploaded = img->SetData( Core::Formats::ImagePixelData( vp.Rgba ) );
                    if ( uploaded.IsSuccess() )
                        vp.Dirty = false;
                    else
                        LOG_ERROR( "[Video] a decoded frame did not reach the GPU and will be retried: {}",
                                   uploaded.GetError() );
                }
                else
                {
                    vp.Dirty = false; // no texture to write into; the decode still advanced
                }
            }
        }
    }

    void VideoService::Clear()
    {
        for ( auto& [path, vp] : m_Videos )
            if ( vp.Plm )
                plm_destroy( vp.Plm );
        m_Videos.clear();
    }
} // namespace Desert::Runtime
