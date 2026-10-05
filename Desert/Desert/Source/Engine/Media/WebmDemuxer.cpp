#include "WebmDemuxer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace Desert::Media
{
    namespace
    {
        // Matroska element ids, with their length-marker bits kept (the spec's notation).
        constexpr uint32_t kEbml           = 0x1A45DFA3;
        constexpr uint32_t kDocType        = 0x4282;
        constexpr uint32_t kSegment        = 0x18538067;
        constexpr uint32_t kInfo           = 0x1549A966;
        constexpr uint32_t kTimecodeScale  = 0x2AD7B1;
        constexpr uint32_t kDuration       = 0x4489;
        constexpr uint32_t kTracks         = 0x1654AE6B;
        constexpr uint32_t kTrackEntry     = 0xAE;
        constexpr uint32_t kTrackNumber    = 0xD7;
        constexpr uint32_t kTrackType      = 0x83;
        constexpr uint32_t kCodecId        = 0x86;
        constexpr uint32_t kCodecPrivate   = 0x63A2;
        constexpr uint32_t kCodecDelay     = 0x56AA;
        constexpr uint32_t kSeekPreRoll    = 0x56BB;
        constexpr uint32_t kVideo          = 0xE0;
        constexpr uint32_t kPixelWidth     = 0xB0;
        constexpr uint32_t kPixelHeight    = 0xBA;
        constexpr uint32_t kAudio          = 0xE1;
        constexpr uint32_t kSamplingFreq   = 0xB5;
        constexpr uint32_t kChannels       = 0x9F;
        constexpr uint32_t kCues           = 0x1C53BB6B;
        constexpr uint32_t kCuePoint       = 0xBB;
        constexpr uint32_t kCueTime        = 0xB3;
        constexpr uint32_t kCueTrackPos    = 0xB7;
        constexpr uint32_t kCueTrack       = 0xF7;
        constexpr uint32_t kCueClusterPos  = 0xF1;
        constexpr uint32_t kCluster        = 0x1F43B675;
        constexpr uint32_t kTimecode       = 0xE7;
        constexpr uint32_t kSimpleBlock    = 0xA3;
        constexpr uint32_t kBlockGroup     = 0xA0;
        constexpr uint32_t kBlock          = 0xA1;
        constexpr uint32_t kDiscardPadding = 0x75A2;

        constexpr uint64_t kUnknownSize = ~0ull;

        // A bounded cursor over an in-memory element body.
        struct Cursor
        {
            const uint8_t* Data = nullptr;
            size_t         Size = 0;
            size_t         Pos  = 0;

            bool AtEnd() const
            {
                return Pos >= Size;
            }

            bool ReadVint( uint64_t& value, bool keepMarker, size_t* length = nullptr )
            {
                if ( Pos >= Size )
                    return false;
                const uint8_t first = Data[Pos];
                if ( first == 0 )
                    return false;
                const size_t len = static_cast<size_t>( std::countl_zero( first ) ) + 1;
                if ( len > 8 || Pos + len > Size )
                    return false;
                uint64_t v       = keepMarker ? first : static_cast<uint64_t>( first & ( 0xFF >> len ) );
                bool     allOnes = ( first & ( 0xFF >> len ) ) == ( 0xFF >> len );
                for ( size_t i = 1; i < len; ++i )
                {
                    v = ( v << 8 ) | Data[Pos + i];
                    allOnes &= Data[Pos + i] == 0xFF;
                }
                Pos += len;
                if ( length )
                    *length = len;
                value = ( !keepMarker && allOnes ) ? kUnknownSize : v;
                return true;
            }

            // One child: its id and a cursor over its body. False at the end or on a malformed child.
            bool Next( uint32_t& id, Cursor& body )
            {
                uint64_t rawId = 0, size = 0;
                if ( !ReadVint( rawId, true ) || !ReadVint( size, false ) || size == kUnknownSize ||
                     size > Size - Pos )
                    return false;
                id   = static_cast<uint32_t>( rawId );
                body = Cursor{ Data + Pos, static_cast<size_t>( size ), 0 };
                Pos += static_cast<size_t>( size );
                return true;
            }

            uint64_t Uint() const
            {
                uint64_t v = 0;
                for ( size_t i = 0; i < Size && i < 8; ++i )
                    v = ( v << 8 ) | Data[i];
                return v;
            }
            int64_t Int() const
            {
                if ( Size == 0 )
                    return 0;
                int64_t v = static_cast<int8_t>( Data[0] );
                for ( size_t i = 1; i < Size && i < 8; ++i )
                    v = static_cast<int64_t>( static_cast<uint64_t>( v ) << 8 ) | Data[i];
                return v;
            }
            double Float() const
            {
                if ( Size == 4 )
                    return static_cast<double>( std::bit_cast<float>( static_cast<uint32_t>( Uint() ) ) );
                if ( Size == 8 )
                    return std::bit_cast<double>( Uint() );
                return 0.0;
            }
            std::string String() const
            {
                std::string s( reinterpret_cast<const char*>( Data ), Size );
                s.erase( std::find( s.begin(), s.end(), '\0' ), s.end() );
                return s;
            }
            std::vector<uint8_t> Bytes() const
            {
                return std::vector<uint8_t>( Data, Data + Size );
            }
        };

        // Element header straight from the file (the Segment walk: bodies are skipped, not read).
        bool ReadFileVint( std::ifstream& f, uint64_t& value, bool keepMarker )
        {
            uint8_t bytes[8];
            if ( !f.read( reinterpret_cast<char*>( bytes ), 1 ) || bytes[0] == 0 )
                return false;
            const size_t len = static_cast<size_t>( std::countl_zero( bytes[0] ) ) + 1;
            if ( len > 8 || ( len > 1 && !f.read( reinterpret_cast<char*>( bytes + 1 ),
                                                  static_cast<std::streamsize>( len - 1 ) ) ) )
                return false;
            Cursor c{ bytes, len, 0 };
            return c.ReadVint( value, keepMarker );
        }

        bool ReadFileBody( std::ifstream& f, uint64_t size, std::vector<uint8_t>& out )
        {
            out.resize( static_cast<size_t>( size ) );
            return size == 0 || static_cast<bool>( f.read( reinterpret_cast<char*>( out.data() ),
                                                           static_cast<std::streamsize>( size ) ) );
        }
    } // namespace

    std::string WebmDemuxer::Open( const std::filesystem::path& path )
    {
        m_Path = path;
        m_File.open( path, std::ios::binary );
        if ( !m_File )
            return std::format( "{}: cannot open the file", path.string() );

        uint64_t             id = 0, size = 0;
        std::vector<uint8_t> body;
        if ( !ReadFileVint( m_File, id, true ) || id != kEbml || !ReadFileVint( m_File, size, false ) ||
             size == kUnknownSize || !ReadFileBody( m_File, size, body ) )
            return std::format( "{}: not an EBML file (no EBML header)", path.string() );
        {
            Cursor      header{ body.data(), body.size(), 0 }, child;
            uint32_t    childId = 0;
            std::string docType;
            while ( header.Next( childId, child ) )
                if ( childId == kDocType )
                    docType = child.String();
            if ( docType != "webm" && docType != "matroska" )
                return std::format( "{}: DocType '{}' is neither webm nor matroska", path.string(), docType );
        }

        if ( !ReadFileVint( m_File, id, true ) || id != kSegment || !ReadFileVint( m_File, size, false ) )
            return std::format( "{}: no Segment after the EBML header", path.string() );
        m_SegmentDataOffset   = static_cast<uint64_t>( m_File.tellg() );
        const uint64_t segEnd = size == kUnknownSize ? ~0ull : m_SegmentDataOffset + size;

        while ( static_cast<uint64_t>( m_File.tellg() ) < segEnd )
        {
            const uint64_t elementOffset = static_cast<uint64_t>( m_File.tellg() );
            if ( !ReadFileVint( m_File, id, true ) || !ReadFileVint( m_File, size, false ) )
                break; // end of file
            const uint64_t bodyOffset = static_cast<uint64_t>( m_File.tellg() );
            if ( size == kUnknownSize )
                return std::format( "{}: element 0x{:X} at byte {} has an unknown size (a live-stream "
                                    "WebM); playback needs a finalised file",
                                    path.string(), id, elementOffset );
            if ( id == kInfo || id == kTracks || id == kCues )
            {
                if ( !ReadFileBody( m_File, size, body ) )
                    return std::format( "{}: element 0x{:X} at byte {} is truncated", path.string(), id,
                                        elementOffset );
                std::string error = id == kInfo ? ParseInfo( body ) : id == kTracks ? ParseTracks( body ) : "";
                if ( id == kCues )
                    ParseCues( body );
                if ( !error.empty() )
                    return std::format( "{}: {}", path.string(), error );
                continue;
            }
            if ( id == kCluster )
            {
                m_Clusters.push_back( { bodyOffset, size } );
                m_ClusterElementOffsets.push_back( elementOffset );
            }
            m_File.seekg( static_cast<std::streamoff>( bodyOffset + size ) );
        }
        m_File.clear();

        if ( !m_Video.Present && !m_Audio.Present )
            return std::format( "{}: no AV1 video or Opus audio track", path.string() );
        if ( m_Clusters.empty() )
            return std::format( "{}: the Segment holds no Cluster (no media data)", path.string() );
        m_NextCluster = 0;
        return {};
    }

    std::string WebmDemuxer::ParseInfo( const std::vector<uint8_t>& bytes )
    {
        Cursor   info{ bytes.data(), bytes.size(), 0 }, child;
        uint32_t id       = 0;
        double   duration = 0.0;
        while ( info.Next( id, child ) )
        {
            if ( id == kTimecodeScale )
                m_TimecodeScaleNs = child.Uint();
            else if ( id == kDuration )
                duration = child.Float();
        }
        if ( m_TimecodeScaleNs == 0 )
            return "Info has TimecodeScale 0";
        m_DurationNs = static_cast<int64_t>( duration * static_cast<double>( m_TimecodeScaleNs ) );
        return {};
    }

    std::string WebmDemuxer::ParseTracks( const std::vector<uint8_t>& bytes )
    {
        Cursor   tracks{ bytes.data(), bytes.size(), 0 }, entry, child, sub;
        uint32_t id = 0, childId = 0, subId = 0;
        while ( tracks.Next( id, entry ) )
        {
            if ( id != kTrackEntry )
                continue;
            uint64_t             number = 0, type = 0, codecDelay = 0, seekPreRoll = 0;
            uint32_t             width = 0, height = 0, channels = 0;
            double               rate = 0.0;
            std::string          codec;
            std::vector<uint8_t> priv;
            while ( entry.Next( childId, child ) )
            {
                switch ( childId )
                {
                    case kTrackNumber:
                        number = child.Uint();
                        break;
                    case kTrackType:
                        type = child.Uint();
                        break;
                    case kCodecId:
                        codec = child.String();
                        break;
                    case kCodecPrivate:
                        priv = child.Bytes();
                        break;
                    case kCodecDelay:
                        codecDelay = child.Uint();
                        break;
                    case kSeekPreRoll:
                        seekPreRoll = child.Uint();
                        break;
                    case kVideo:
                        while ( child.Next( subId, sub ) )
                        {
                            if ( subId == kPixelWidth )
                                width = static_cast<uint32_t>( sub.Uint() );
                            else if ( subId == kPixelHeight )
                                height = static_cast<uint32_t>( sub.Uint() );
                        }
                        break;
                    case kAudio:
                        while ( child.Next( subId, sub ) )
                        {
                            if ( subId == kSamplingFreq )
                                rate = sub.Float();
                            else if ( subId == kChannels )
                                channels = static_cast<uint32_t>( sub.Uint() );
                        }
                        break;
                    default:
                        break;
                }
            }
            if ( type == 1 && !m_Video.Present )
            {
                if ( codec != "V_AV1" )
                    return std::format( "video track {} is '{}'; the engine plays AV1 (V_AV1) only", number,
                                        codec );
                m_Video = { true, number, codec, width, height, std::move( priv ) };
            }
            else if ( type == 2 && !m_Audio.Present )
            {
                if ( codec != "A_OPUS" )
                    return std::format( "audio track {} is '{}'; the engine plays Opus (A_OPUS) only", number,
                                        codec );
                m_Audio = { true,
                            number,
                            codec,
                            static_cast<uint32_t>( rate ),
                            channels,
                            static_cast<int64_t>( codecDelay ),
                            static_cast<int64_t>( seekPreRoll ),
                            std::move( priv ) };
            }
        }
        return {};
    }

    void WebmDemuxer::ParseCues( const std::vector<uint8_t>& bytes )
    {
        Cursor   cues{ bytes.data(), bytes.size(), 0 }, point, child, sub;
        uint32_t id = 0, childId = 0, subId = 0;
        while ( cues.Next( id, point ) )
        {
            if ( id != kCuePoint )
                continue;
            uint64_t time = 0;
            while ( point.Next( childId, child ) )
            {
                if ( childId == kCueTime )
                    time = child.Uint();
                else if ( childId == kCueTrackPos )
                {
                    uint64_t track = 0, position = 0;
                    while ( child.Next( subId, sub ) )
                    {
                        if ( subId == kCueTrack )
                            track = sub.Uint();
                        else if ( subId == kCueClusterPos )
                            position = sub.Uint();
                    }
                    // Cues index the VIDEO keyframes: that is where decoding may restart.
                    if ( track == m_Video.Number || !m_Video.Present )
                        m_Cues.push_back( { static_cast<int64_t>( time * m_TimecodeScaleNs ),
                                            m_SegmentDataOffset + position } );
                }
            }
        }
        std::sort( m_Cues.begin(), m_Cues.end(),
                   []( const CueEntry& a, const CueEntry& b ) { return a.TimeNs < b.TimeNs; } );
    }

    bool WebmDemuxer::LoadCluster( size_t index )
    {
        const ClusterEntry&  cluster = m_Clusters[index];
        std::vector<uint8_t> body;
        m_File.clear();
        m_File.seekg( static_cast<std::streamoff>( cluster.BodyOffset ) );
        if ( !ReadFileBody( m_File, cluster.BodySize, body ) )
        {
            m_Error = std::format( "{}: cluster at byte {} is truncated", m_Path.string(), cluster.BodyOffset );
            return false;
        }
        Cursor   c{ body.data(), body.size(), 0 }, child, sub;
        uint32_t id = 0, subId = 0;
        int64_t  clusterTimeNs = 0;
        while ( c.Next( id, child ) )
        {
            if ( id == kTimecode )
                clusterTimeNs = static_cast<int64_t>( child.Uint() * m_TimecodeScaleNs );
            else if ( id == kSimpleBlock )
            {
                if ( !ParseBlock( child.Data, child.Size, clusterTimeNs, true, 0 ) )
                    return false;
            }
            else if ( id == kBlockGroup )
            {
                const uint8_t* block     = nullptr;
                size_t         blockSize = 0;
                int64_t        discard   = 0;
                while ( child.Next( subId, sub ) )
                {
                    if ( subId == kBlock )
                    {
                        block     = sub.Data;
                        blockSize = sub.Size;
                    }
                    else if ( subId == kDiscardPadding )
                        discard = sub.Int();
                }
                if ( block && !ParseBlock( block, blockSize, clusterTimeNs, false, discard ) )
                    return false;
            }
        }
        return true;
    }

    bool WebmDemuxer::ParseBlock( const uint8_t* data, size_t size, int64_t clusterTimeNs, bool simple,
                                  int64_t discardPaddingNs )
    {
        Cursor   c{ data, size, 0 };
        uint64_t track = 0;
        if ( !c.ReadVint( track, false ) || c.Pos + 3 > size )
        {
            m_Error = std::format( "{}: malformed block header", m_Path.string() );
            return false;
        }
        const int16_t relative = static_cast<int16_t>( ( data[c.Pos] << 8 ) | data[c.Pos + 1] );
        const uint8_t flags    = data[c.Pos + 2];
        c.Pos += 3;

        MediaTrackKind kind;
        if ( m_Video.Present && track == m_Video.Number )
            kind = MediaTrackKind::Video;
        else if ( m_Audio.Present && track == m_Audio.Number )
            kind = MediaTrackKind::Audio;
        else
            return true; // a track the player does not decode (subtitles, a second audio track)

        // Lacing: several frames in one block. 0 none, 1 Xiph, 2 fixed-size, 3 EBML.
        const int           lacing = ( flags >> 1 ) & 3;
        std::vector<size_t> sizes;
        if ( lacing == 0 )
            sizes.push_back( size - c.Pos );
        else
        {
            if ( c.Pos >= size )
                return false;
            const size_t count = static_cast<size_t>( data[c.Pos++] ) + 1;
            size_t       known = 0;
            if ( lacing == 1 )
            {
                for ( size_t i = 0; i + 1 < count; ++i )
                {
                    size_t s = 0;
                    while ( c.Pos < size && data[c.Pos] == 0xFF )
                        s += data[c.Pos++];
                    if ( c.Pos >= size )
                        return false;
                    s += data[c.Pos++];
                    sizes.push_back( s );
                    known += s;
                }
            }
            else if ( lacing == 3 )
            {
                uint64_t first = 0;
                if ( !c.ReadVint( first, false ) )
                    return false;
                sizes.push_back( static_cast<size_t>( first ) );
                known = static_cast<size_t>( first );
                for ( size_t i = 1; i + 1 < count; ++i )
                {
                    uint64_t raw = 0;
                    size_t   len = 0;
                    if ( !c.ReadVint( raw, false, &len ) )
                        return false;
                    const int64_t bias = ( int64_t( 1 ) << ( 7 * len - 1 ) ) - 1; // signed vint
                    const int64_t s    = static_cast<int64_t>( sizes.back() ) + static_cast<int64_t>( raw ) - bias;
                    if ( s < 0 )
                        return false;
                    sizes.push_back( static_cast<size_t>( s ) );
                    known += static_cast<size_t>( s );
                }
            }
            if ( lacing == 2 )
                sizes.assign( count, ( size - c.Pos ) / count );
            else
            {
                if ( known > size - c.Pos )
                    return false;
                sizes.push_back( size - c.Pos - known );
            }
        }

        const int64_t ptsNs =
             clusterTimeNs + static_cast<int64_t>( relative ) * static_cast<int64_t>( m_TimecodeScaleNs );
        for ( size_t i = 0; i < sizes.size(); ++i )
        {
            if ( c.Pos + sizes[i] > size )
            {
                m_Error = std::format( "{}: block lace overruns its block", m_Path.string() );
                return false;
            }
            MediaPacket p;
            p.Kind             = kind;
            p.PtsNs            = ptsNs; // laced frames share the block time; Opus/AV1 carry their own durations
            p.Keyframe         = simple ? ( flags & 0x80 ) != 0 : false;
            p.DiscardPaddingNs = i + 1 == sizes.size() ? discardPaddingNs : 0;
            p.Data.assign( data + c.Pos, data + c.Pos + sizes[i] );
            c.Pos += sizes[i];
            m_Pending.push_back( std::move( p ) );
        }
        return true;
    }

    bool WebmDemuxer::NextPacket( MediaPacket& out )
    {
        while ( m_Pending.empty() )
        {
            if ( m_NextCluster >= m_Clusters.size() || !m_Error.empty() )
                return false;
            if ( !LoadCluster( m_NextCluster++ ) )
                return false;
        }
        out = std::move( m_Pending.front() );
        m_Pending.pop_front();
        return true;
    }

    bool WebmDemuxer::Seek( int64_t targetNs )
    {
        m_Pending.clear();
        m_Error.clear();
        if ( targetNs <= 0 )
        {
            m_NextCluster = 0;
            return true;
        }
        if ( m_Cues.empty() )
        {
            m_Error = std::format( "{}: no Cues, so no keyframe index to seek by", m_Path.string() );
            return false;
        }
        uint64_t clusterOffset = m_Cues.front().ClusterOffset;
        for ( const CueEntry& cue : m_Cues )
            if ( cue.TimeNs <= targetNs )
                clusterOffset = cue.ClusterOffset;
        const auto it = std::find( m_ClusterElementOffsets.begin(), m_ClusterElementOffsets.end(), clusterOffset );
        if ( it == m_ClusterElementOffsets.end() )
        {
            m_Error = std::format( "{}: a Cue points at byte {}, which starts no Cluster", m_Path.string(),
                                   clusterOffset );
            return false;
        }
        m_NextCluster = static_cast<size_t>( it - m_ClusterElementOffsets.begin() );
        return true;
    }
} // namespace Desert::Media
