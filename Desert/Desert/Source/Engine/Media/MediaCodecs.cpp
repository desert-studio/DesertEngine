#include "MediaCodecs.hpp"

#include <dav1d/dav1d.h>
#include <opus.h>
#include <opus_multistream.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

namespace Desert::Media
{
    // ----------------------------------------------------------------------------------------- AV1 (dav1d)

    Av1VideoDecoder::~Av1VideoDecoder()
    {
        if ( m_Context )
            dav1d_close( &m_Context );
    }

    std::string Av1VideoDecoder::Open()
    {
        Dav1dSettings settings;
        dav1d_default_settings( &settings );
        settings.n_threads       = 0; // dav1d's own choice from the core count
        settings.max_frame_delay = 0; // and its own pipelining depth
        if ( const int r = dav1d_open( &m_Context, &settings ); r < 0 )
            return std::format( "dav1d_open failed ({})", r );
        return {};
    }

    bool Av1VideoDecoder::Receive( std::vector<VideoFrame>& out )
    {
        for ( ;; )
        {
            Dav1dPicture picture{};
            const int    r = dav1d_get_picture( m_Context, &picture );
            if ( r == DAV1D_ERR( EAGAIN ) )
                return true;
            if ( r < 0 )
            {
                m_Error = std::format( "dav1d_get_picture failed ({})", r );
                return false;
            }

            VideoFrame frame;
            frame.PtsNs    = picture.m.timestamp;
            frame.Width    = static_cast<uint32_t>( picture.p.w );
            frame.Height   = static_cast<uint32_t>( picture.p.h );
            frame.BitDepth = static_cast<uint32_t>( picture.p.bpc );
            uint32_t ssx = 0, ssy = 0;
            switch ( picture.p.layout )
            {
                case DAV1D_PIXEL_LAYOUT_I400:
                    frame.Chroma = MediaChroma::I400;
                    break;
                case DAV1D_PIXEL_LAYOUT_I420:
                    frame.Chroma = MediaChroma::I420;
                    ssx = ssy = 1;
                    break;
                case DAV1D_PIXEL_LAYOUT_I422:
                    frame.Chroma = MediaChroma::I422;
                    ssx          = 1;
                    break;
                case DAV1D_PIXEL_LAYOUT_I444:
                    frame.Chroma = MediaChroma::I444;
                    break;
            }
            if ( picture.seq_hdr )
            {
                switch ( picture.seq_hdr->mtrx )
                {
                    case DAV1D_MC_BT709:
                        frame.Matrix = MediaColorMatrix::BT709;
                        break;
                    case DAV1D_MC_BT2020_NCL:
                    case DAV1D_MC_BT2020_CL:
                        frame.Matrix = MediaColorMatrix::BT2020;
                        break;
                    default:
                        frame.Matrix = MediaColorMatrix::BT601;
                        break; // BT.601/470BG and "unspecified"
                }
                frame.FullRange = picture.seq_hdr->color_range != 0;
            }

            const uint32_t bytes  = frame.BytesPerSample();
            const int      planes = frame.Chroma == MediaChroma::I400 ? 1 : 3;
            for ( int p = 0; p < planes; ++p )
            {
                const uint32_t  w      = p == 0 ? frame.Width : ( frame.Width + ssx ) >> ssx;
                const uint32_t  h      = p == 0 ? frame.Height : ( frame.Height + ssy ) >> ssy;
                const ptrdiff_t stride = picture.stride[p == 0 ? 0 : 1];
                frame.PlaneWidth[p]    = w;
                frame.PlaneHeight[p]   = h;
                frame.Planes[p].Allocate( static_cast<size_t>( w ) * h * bytes );
                const auto* src = static_cast<const uint8_t*>( picture.data[p] );
                for ( uint32_t y = 0; y < h; ++y )
                    std::memcpy( frame.Planes[p].data() + static_cast<size_t>( y ) * w * bytes,
                                 src + static_cast<ptrdiff_t>( y ) * stride, static_cast<size_t>( w ) * bytes );
            }
            dav1d_picture_unref( &picture );
            out.push_back( std::move( frame ) );
        }
    }

    bool Av1VideoDecoder::Decode( const MediaPacket& packet, std::vector<VideoFrame>& out )
    {
        Dav1dData data{};
        uint8_t*  buffer = dav1d_data_create( &data, packet.Data.size() );
        if ( !buffer )
        {
            m_Error = "dav1d_data_create failed (out of memory)";
            return false;
        }
        std::memcpy( buffer, packet.Data.data(), packet.Data.size() );
        data.m.timestamp = packet.PtsNs;

        // dav1d takes the packet in pieces when its queue is full (EAGAIN): drain pictures, then resend.
        while ( data.sz > 0 )
        {
            const int r = dav1d_send_data( m_Context, &data );
            if ( r < 0 && r != DAV1D_ERR( EAGAIN ) )
            {
                dav1d_data_unref( &data );
                m_Error = std::format( "dav1d_send_data failed ({}) on the packet at {} ns", r, packet.PtsNs );
                return false;
            }
            if ( !Receive( out ) )
            {
                dav1d_data_unref( &data );
                return false;
            }
        }
        return true;
    }

    void Av1VideoDecoder::Drain( std::vector<VideoFrame>& out )
    {
        // With no more data sent, dav1d hands out what its frame threads still hold, then says EAGAIN.
        Receive( out );
    }

    void Av1VideoDecoder::Flush()
    {
        if ( m_Context )
            dav1d_flush( m_Context );
    }

    // ----------------------------------------------------------------------------------------- Opus

    OpusAudioDecoder::~OpusAudioDecoder()
    {
        if ( m_Decoder )
            opus_decoder_destroy( m_Decoder );
        if ( m_MultiStream )
            opus_multistream_decoder_destroy( m_MultiStream );
    }

    std::string OpusAudioDecoder::Open( const WebmAudioTrack& track )
    {
        // OpusHead (RFC 7845 §5.1): "OpusHead", version, channels, pre-skip, input rate, gain, mapping family.
        const std::vector<uint8_t>& head = track.CodecPrivate;
        if ( head.size() < 19 || std::memcmp( head.data(), "OpusHead", 8 ) != 0 )
            return "the Opus track's CodecPrivate is not an OpusHead";
        m_Channels           = head[9];
        m_PreSkip            = static_cast<uint32_t>( head[10] | ( head[11] << 8 ) );
        m_SkipLeft           = m_PreSkip;
        const uint8_t family = head[18];
        int           error  = OPUS_OK;
        if ( family == 0 )
            m_Decoder = opus_decoder_create( kSampleRate, static_cast<int>( m_Channels ), &error );
        else
        {
            if ( head.size() < 21u + m_Channels )
                return std::format( "OpusHead mapping family {} has no channel mapping table", family );
            m_MultiStream = opus_multistream_decoder_create( kSampleRate, static_cast<int>( m_Channels ), head[19],
                                                             head[20], head.data() + 21, &error );
        }
        if ( error != OPUS_OK || m_Channels == 0 )
            return std::format( "opus decoder for {} channels failed: {}", m_Channels, opus_strerror( error ) );
        m_Scratch.resize( static_cast<size_t>( 5760 ) * m_Channels ); // 120 ms, Opus's longest packet
        return {};
    }

    int64_t OpusAudioDecoder::Decode( const MediaPacket& packet, std::vector<float>& interleaved )
    {
        const auto* data = packet.Data.data();
        const auto  size = static_cast<opus_int32>( packet.Data.size() );
        const int   frames =
             m_Decoder ? opus_decode_float( m_Decoder, data, size, m_Scratch.data(), 5760, 0 )
                         : opus_multistream_decode_float( m_MultiStream, data, size, m_Scratch.data(), 5760, 0 );
        if ( frames < 0 )
        {
            m_Error = std::format( "opus decode failed at {} ns: {}", packet.PtsNs, opus_strerror( frames ) );
            return -1;
        }
        uint32_t first = std::min<uint32_t>( m_SkipLeft, static_cast<uint32_t>( frames ) );
        m_SkipLeft -= first;
        const int64_t discard =
             packet.DiscardPaddingNs > 0 ? ( packet.DiscardPaddingNs * kSampleRate + 500000000 ) / 1000000000 : 0;
        const int64_t last = std::max<int64_t>( first, frames - discard );
        interleaved.insert( interleaved.end(), m_Scratch.begin() + static_cast<ptrdiff_t>( first ) * m_Channels,
                            m_Scratch.begin() + static_cast<ptrdiff_t>( last ) * m_Channels );
        return last - first;
    }

    void OpusAudioDecoder::Reset( bool fromStart )
    {
        if ( m_Decoder )
            opus_decoder_ctl( m_Decoder, OPUS_RESET_STATE );
        if ( m_MultiStream )
            opus_multistream_decoder_ctl( m_MultiStream, OPUS_RESET_STATE );
        m_SkipLeft = fromStart ? m_PreSkip : 0;
    }
} // namespace Desert::Media
