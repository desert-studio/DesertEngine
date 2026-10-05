#include "MediaAudioOutput.hpp"

#include <Engine/Audio/AudioEngine.hpp>

#include <miniaudio/miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace Desert::Media
{
    struct MediaAudioOutput::Impl
    {
        ma_data_source_base Base{}; // first member: miniaudio casts the data source back to this struct
        ma_sound            Sound{};
        bool                SoundReady = false;
        uint32_t            SampleRate = 0;
        uint32_t            Channels   = 0;

        mutable std::mutex    Lock;
        std::vector<float>    Ring;     // interleaved, capacity = Ring.size() / Channels frames
        uint64_t              Head = 0; // frames ever read (device side), modulo applied on index
        uint64_t              Tail = 0; // frames ever written
        std::atomic<uint64_t> Played{ 0 };
        std::atomic<bool>     Paused{ true };

        uint64_t CapacityFrames() const
        {
            return Channels ? Ring.size() / Channels : 0;
        }
    };

    namespace
    {
        ma_result OnRead( ma_data_source* source, void* out, ma_uint64 frameCount, ma_uint64* framesRead )
        {
            auto*        impl   = reinterpret_cast<MediaAudioOutput::Impl*>( source );
            float*       dst    = static_cast<float*>( out );
            const size_t stride = impl->Channels;
            ma_uint64    taken  = 0;
            if ( !impl->Paused.load( std::memory_order_acquire ) )
            {
                std::lock_guard lock( impl->Lock );
                const uint64_t  capacity = impl->CapacityFrames();
                taken                    = std::min<uint64_t>( frameCount, impl->Tail - impl->Head );
                for ( ma_uint64 i = 0; i < taken; ++i )
                {
                    const size_t from = static_cast<size_t>( ( impl->Head + i ) % capacity ) * stride;
                    std::memcpy( dst + i * stride, impl->Ring.data() + from, stride * sizeof( float ) );
                }
                impl->Head += taken;
            }
            impl->Played.fetch_add( taken, std::memory_order_release );
            // An underrun (or a pause) is silence, not the end: the sound must keep pulling.
            std::memset( dst + taken * stride, 0,
                         static_cast<size_t>( frameCount - taken ) * stride * sizeof( float ) );
            if ( framesRead )
                *framesRead = frameCount;
            return MA_SUCCESS;
        }

        ma_result OnSeek( ma_data_source*, ma_uint64 )
        {
            return MA_NOT_IMPLEMENTED;
        }

        ma_result OnGetDataFormat( ma_data_source* source, ma_format* format, ma_uint32* channels,
                                   ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap )
        {
            auto* impl = reinterpret_cast<MediaAudioOutput::Impl*>( source );
            if ( format )
                *format = ma_format_f32;
            if ( channels )
                *channels = impl->Channels;
            if ( sampleRate )
                *sampleRate = impl->SampleRate;
            if ( channelMap )
                ma_channel_map_init_standard( ma_standard_channel_map_vorbis, channelMap, channelMapCap,
                                              impl->Channels ); // Opus uses the Vorbis channel order
            return MA_SUCCESS;
        }

        ma_result OnGetCursor( ma_data_source*, ma_uint64* cursor )
        {
            *cursor = 0;
            return MA_NOT_IMPLEMENTED;
        }

        ma_result OnGetLength( ma_data_source*, ma_uint64* length )
        {
            *length = 0;
            return MA_NOT_IMPLEMENTED;
        }

        ma_data_source_vtable g_StreamVTable = { OnRead,  OnSeek, OnGetDataFormat, OnGetCursor, OnGetLength,
                                                 nullptr, 0 };
    } // namespace

    MediaAudioOutput::MediaAudioOutput() : m_Impl( std::make_unique<Impl>() )
    {
    }

    MediaAudioOutput::~MediaAudioOutput()
    {
        if ( m_Impl->SoundReady )
        {
            ma_sound_uninit( &m_Impl->Sound );
            ma_data_source_uninit( &m_Impl->Base );
        }
    }

    std::string MediaAudioOutput::Start( uint32_t sampleRate, uint32_t channels )
    {
        if ( m_Impl->SoundReady )
        {
            if ( sampleRate == m_Impl->SampleRate && channels == m_Impl->Channels )
                return {};
            ma_sound_uninit( &m_Impl->Sound );
            ma_data_source_uninit( &m_Impl->Base );
            m_Impl->SoundReady = false;
        }
        ma_engine* engine = Audio::AudioEngine::Get().GetNativeEngine();
        if ( !engine )
            return "no audio device (the miniaudio engine failed to initialize)";

        m_Impl->SampleRate = sampleRate;
        m_Impl->Channels   = channels;
        m_Impl->Ring.assign( static_cast<size_t>( sampleRate ) * channels, 0.0f ); // one second of room
        Flush();

        ma_data_source_config config = ma_data_source_config_init();
        config.vtable                = &g_StreamVTable;
        if ( ma_data_source_init( &config, &m_Impl->Base ) != MA_SUCCESS )
            return "ma_data_source_init failed";
        if ( ma_sound_init_from_data_source( engine, &m_Impl->Base, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                             &m_Impl->Sound ) != MA_SUCCESS )
        {
            ma_data_source_uninit( &m_Impl->Base );
            return std::format( "ma_sound_init_from_data_source failed ({} Hz, {} channels)", sampleRate,
                                channels );
        }
        m_Impl->SoundReady = true;
        ma_sound_start( &m_Impl->Sound );
        return {};
    }

    void MediaAudioOutput::Push( const float* interleaved, uint64_t frames )
    {
        std::lock_guard lock( m_Impl->Lock );
        const uint64_t  capacity = m_Impl->CapacityFrames();
        const size_t    stride   = m_Impl->Channels;
        // The player decodes a bounded lead (MediaPlayer kAudioLeadFrames, well under the ring's second);
        // a push that would overrun the reader drops its oldest unread frames rather than corrupt them.
        if ( m_Impl->Tail + frames - m_Impl->Head > capacity )
            m_Impl->Head = m_Impl->Tail + frames - capacity;
        for ( uint64_t i = 0; i < frames; ++i )
        {
            const size_t to = static_cast<size_t>( ( m_Impl->Tail + i ) % capacity ) * stride;
            std::memcpy( m_Impl->Ring.data() + to, interleaved + i * stride, stride * sizeof( float ) );
        }
        m_Impl->Tail += frames;
    }

    uint64_t MediaAudioOutput::PlayedFrames() const
    {
        return m_Impl->Played.load( std::memory_order_acquire );
    }

    uint64_t MediaAudioOutput::QueuedFrames() const
    {
        std::lock_guard lock( m_Impl->Lock );
        return m_Impl->Tail - m_Impl->Head;
    }

    void MediaAudioOutput::SetPaused( bool paused )
    {
        m_Impl->Paused.store( paused, std::memory_order_release );
    }

    void MediaAudioOutput::Flush()
    {
        std::lock_guard lock( m_Impl->Lock );
        m_Impl->Head = m_Impl->Tail = 0;
        m_Impl->Played.store( 0, std::memory_order_release );
    }

    void MediaAudioOutput::SetVolume( float volume )
    {
        if ( m_Impl->SoundReady )
            ma_sound_set_volume( &m_Impl->Sound, volume );
    }
} // namespace Desert::Media
