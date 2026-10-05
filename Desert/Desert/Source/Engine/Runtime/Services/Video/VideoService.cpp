#include "VideoService.hpp"

#include <Engine/Audio/AudioEngine.hpp>
#include <Engine/Media/MediaAudioOutput.hpp>
#include <Engine/Media/MediaPlayer.hpp>
#include <Engine/Media/MediaTexture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>

#include <algorithm>

namespace Desert::Runtime
{
    VideoPlayback::VideoPlayback()                                      = default;
    VideoPlayback::~VideoPlayback()                                     = default;
    VideoPlayback::VideoPlayback( VideoPlayback&& ) noexcept            = default;
    VideoPlayback& VideoPlayback::operator=( VideoPlayback&& ) noexcept = default;

    VideoService::~VideoService()
    {
        Clear();
    }

    VideoPlayback* VideoService::GetOrOpen( const std::string& path )
    {
        if ( auto it = m_Videos.find( path ); it != m_Videos.end() )
            return &it->second;

        VideoPlayback& vp = m_Videos[path];
        vp.Last           = std::chrono::steady_clock::now();

        auto player = std::make_unique<Media::MediaPlayer>();
        if ( const std::string error = player->Open( Media::MediaSource{ path } ); !error.empty() )
        {
            LOG_ERROR( "[Video] '{}' did not open: {}", path, error );
            return &vp; // Valid stays false -> negative cache
        }
        if ( !player->HasVideo() )
        {
            LOG_ERROR( "[Video] '{}' has no video track", path );
            return &vp;
        }
        std::unique_ptr<Media::MediaAudioOutput> sound;
        if ( player->HasAudio() )
        {
            if ( Audio::AudioEngine::Get().GetNativeEngine() != nullptr )
            {
                sound = std::make_unique<Media::MediaAudioOutput>();
                sound->SetVolume( 0.0f ); // silent until a panel that draws it asks for a volume
                player->SetAudioSink( sound.get() );
                if ( player->GetState() == Media::MediaPlayerState::Error )
                {
                    LOG_ERROR( "[Video] '{}': the audio output did not start: {}", path, player->Error() );
                    return &vp;
                }
            }
            else
            {
                LOG_WARN( "[Video] '{}': no audio device on this machine, the clip plays its picture only", path );
            }
        }
        player->SetLooping( true );
        player->Play();

        auto texture = std::make_unique<Media::MediaTexture>();
        // The first frame is converted at open, so the very first Resolve already has a picture to hand out.
        if ( const std::string error = texture->Update( *player ); !error.empty() )
        {
            LOG_ERROR( "[Video] '{}': the first frame did not reach the GPU: {}", path, error );
            return &vp;
        }

        vp.Sound   = std::move( sound );
        vp.Player  = std::move( player );
        vp.Texture = std::move( texture );
        vp.Valid   = true;
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

    Graphic::Image2D* VideoService::Resolve( uint64_t handle, SoundRequest sound )
    {
        if ( handle == 0 )
            return nullptr;
        const std::string path = PathForHandle( handle );
        if ( path.empty() )
            return nullptr;
        VideoPlayback* vp = GetOrOpen( path );
        if ( !vp || !vp->Valid )
            return nullptr;
        if ( !sound.Muted )
            vp->RequestedVolume = std::max( vp->RequestedVolume, std::clamp( sound.Volume, 0.0f, 1.0f ) );
        return vp->Texture->GetImage();
    }

    void VideoService::UpdateAll()
    {
        const auto now = std::chrono::steady_clock::now();
        for ( auto& [path, vp] : m_Videos )
        {
            if ( !vp.Valid )
                continue;

            double dt = std::chrono::duration<double>( now - vp.Last ).count();
            vp.Last   = now;
            dt        = std::clamp( dt, 0.0, 0.25 ); // cap catch-up after a stall / pause / first frame
            if ( dt <= 0.0 )
                continue;

            if ( vp.Sound )
                vp.Sound->SetVolume( vp.RequestedVolume );
            vp.RequestedVolume = 0.0f; // the panels that draw this frame ask again
            vp.Player->Tick( dt );
            // A refused upload keeps the texture's serial behind the player's, so the next tick retries it.
            if ( const std::string error = vp.Texture->Update( *vp.Player ); !error.empty() )
                LOG_ERROR( "[Video] '{}': a decoded frame did not reach the GPU and will be retried: {}", path,
                           error );
        }
    }

    void VideoService::Clear()
    {
        m_Videos.clear();
    }
} // namespace Desert::Runtime
