#pragma once

#include <Engine/Media/WebmDemuxer.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct Dav1dContext;
struct OpusDecoder;
struct OpusMSDecoder;

namespace Desert::Media
{
    // The decoded picture, AS THE DECODER PRODUCED IT: three planes of Y'CbCr samples, chroma subsampled
    // or not, 8 bits or 16-bit words for 10/12-bit streams. Turning this into RGB is the GPU's job
    // (MediaTexture) — the colour matrix and range travel with the planes so the shader is told, not told
    // to assume.
    enum class MediaChroma : uint8_t
    {
        I400, // luma only
        I420,
        I422,
        I444,
    };

    enum class MediaColorMatrix : uint8_t
    {
        BT601,
        BT709,
        BT2020,
    };

    // ONE DECODED PLANE'S BYTES, overwritten whole by the decoder. Not std::vector<uint8_t>: a vector
    // value-initialises every byte on resize and walks every byte again on destruction in an unoptimised
    // build -- measured (MEDIA-5, `sample` on the Debug runtime) at ~150 ms per 4K 10-bit frame each way,
    // which is what held a startup movie's frame for seconds while the sound clock ran on.
    class MediaPlane
    {
    public:
        void Allocate( size_t bytes )
        {
            if ( bytes == m_Size )
                return;
            m_Bytes =
                 std::make_unique_for_overwrite<uint8_t[]>( bytes ); // default-initialised: no pass over the bytes
            m_Size = bytes;
        }
        uint8_t* data()
        {
            return m_Bytes.get();
        }
        [[nodiscard]] const uint8_t* data() const
        {
            return m_Bytes.get();
        }
        // The GPU upload API (Core::Formats::ImagePixelData) takes a mutable byte pointer; it only reads it,
        // copying into staging once.
        [[nodiscard]] std::byte* UploadBytes() const
        {
            return std::bit_cast<std::byte*>( m_Bytes.get() );
        }
        [[nodiscard]] size_t size() const
        {
            return m_Size;
        }
        [[nodiscard]] bool empty() const
        {
            return m_Size == 0;
        }
        uint8_t operator[]( size_t i ) const
        {
            return m_Bytes[i];
        }
        bool operator==( const MediaPlane& other ) const
        {
            return m_Size == other.m_Size && ( m_Size == 0 || std::memcmp( data(), other.data(), m_Size ) == 0 );
        }

    private:
        std::unique_ptr<uint8_t[]> m_Bytes;
        size_t                     m_Size = 0;
    };

    struct VideoFrame
    {
        int64_t                   PtsNs     = 0;
        uint32_t                  Width     = 0;
        uint32_t                  Height    = 0;
        uint32_t                  BitDepth  = 8; // 8, 10 or 12; above 8 each sample is a uint16
        MediaChroma               Chroma    = MediaChroma::I420;
        MediaColorMatrix          Matrix    = MediaColorMatrix::BT601;
        bool                      FullRange = false;
        std::array<MediaPlane, 3> Planes;        // Y, U, V; rows tightly packed (stride = width × bytes)
        std::array<uint32_t, 3>   PlaneWidth{};  // in samples
        std::array<uint32_t, 3>   PlaneHeight{}; // in rows

        [[nodiscard]] uint32_t BytesPerSample() const
        {
            return BitDepth > 8 ? 2u : 1u;
        }
    };

    // AV1 → VideoFrame, on dav1d. Frames come out in presentation order, each tagged with its packet's pts.
    class Av1VideoDecoder
    {
    public:
        Av1VideoDecoder() = default;
        ~Av1VideoDecoder();
        Av1VideoDecoder( const Av1VideoDecoder& )            = delete;
        Av1VideoDecoder& operator=( const Av1VideoDecoder& ) = delete;

        std::string Open();                                                            // empty on success
        bool        Decode( const MediaPacket& packet, std::vector<VideoFrame>& out ); // false: corrupt stream
        void        Drain( std::vector<VideoFrame>& out ); // end of stream: frames still queued
        void        Flush();                               // seek: drop everything in flight
        [[nodiscard]] const std::string& Error() const
        {
            return m_Error;
        }

    private:
        bool Receive( std::vector<VideoFrame>& out );

        Dav1dContext* m_Context = nullptr;
        std::string   m_Error;
    };

    // Opus → interleaved float PCM at 48 kHz (Opus's own rate, whatever the source was), with the stream's
    // pre-skip dropped at the start and each packet's DiscardPadding dropped at the end — so a one-second
    // clip decodes to exactly 48 000 frames.
    class OpusAudioDecoder
    {
    public:
        static constexpr uint32_t kSampleRate = 48000;

        OpusAudioDecoder() = default;
        ~OpusAudioDecoder();
        OpusAudioDecoder( const OpusAudioDecoder& )            = delete;
        OpusAudioDecoder& operator=( const OpusAudioDecoder& ) = delete;

        std::string Open( const WebmAudioTrack& track ); // empty on success
        // Appends the packet's samples to `interleaved`; returns the number of frames appended, -1 on error.
        int64_t Decode( const MediaPacket& packet, std::vector<float>& interleaved );
        void    Reset( bool fromStart ); // seek: decoder state cleared; pre-skip again when fromStart
        [[nodiscard]] uint32_t Channels() const
        {
            return m_Channels;
        }
        [[nodiscard]] const std::string& Error() const
        {
            return m_Error;
        }

    private:
        OpusDecoder*       m_Decoder     = nullptr;
        OpusMSDecoder*     m_MultiStream = nullptr;
        uint32_t           m_Channels    = 0;
        uint32_t           m_PreSkip     = 0;
        uint32_t           m_SkipLeft    = 0;
        std::vector<float> m_Scratch;
        std::string        m_Error;
    };
} // namespace Desert::Media
