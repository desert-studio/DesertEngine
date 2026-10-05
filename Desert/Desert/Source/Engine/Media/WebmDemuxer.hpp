#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Desert::Media
{
    // ONE ELEMENTARY-STREAM PACKET, as the container carries it: one AV1 temporal unit or one Opus packet.
    // Times are nanoseconds on the media timeline (Matroska's own unit once TimecodeScale is applied).
    enum class MediaTrackKind : uint8_t
    {
        Video,
        Audio,
    };

    struct MediaPacket
    {
        MediaTrackKind       Kind             = MediaTrackKind::Video;
        int64_t              PtsNs            = 0;
        bool                 Keyframe         = false;
        int64_t              DiscardPaddingNs = 0; // Matroska DiscardPadding: trailing audio to drop (end trim)
        std::vector<uint8_t> Data;
    };

    struct WebmVideoTrack
    {
        bool                 Present = false;
        uint64_t             Number  = 0;
        std::string          CodecId; // "V_AV1"
        uint32_t             Width  = 0;
        uint32_t             Height = 0;
        std::vector<uint8_t> CodecPrivate; // av1C
    };

    struct WebmAudioTrack
    {
        bool                 Present = false;
        uint64_t             Number  = 0;
        std::string          CodecId; // "A_OPUS"
        uint32_t             SampleRate    = 0;
        uint32_t             Channels      = 0;
        int64_t              CodecDelayNs  = 0; // Opus pre-skip, in time
        int64_t              SeekPreRollNs = 0;
        std::vector<uint8_t> CodecPrivate; // OpusHead
    };

    // WebM (the Matroska subset: EBML header, Segment → Info / Tracks / Cues / Cluster) read as a STREAM
    // from the file: Open() reads the headers and indexes the clusters (offset + timecode, bodies skipped),
    // NextPacket() then loads one cluster at a time. Seek() goes through Cues — the container's own
    // keyframe index — to the cluster that holds the last video keyframe at or before the target.
    //
    // WHY A PARSER OF OUR OWN AND NOT libwebm: what playback needs from Matroska is ~15 element ids, the
    // three lacing modes and the vint grammar. libwebm's mkvparser is a 10k-line C++ parser of the whole
    // format (chapters, tags, content encodings, encryption) with its own reader interface and error model;
    // the part used would be this file, and the rest would be a second dependency to pin and build.
    class WebmDemuxer
    {
    public:
        // Empty string on success, otherwise what is wrong with the file and where.
        std::string Open( const std::filesystem::path& path );

        bool NextPacket( MediaPacket& out ); // false at the end of the stream (or on a read error: Error())
        bool Seek( int64_t targetNs );       // false when the file has no Cues for a non-zero target

        const WebmVideoTrack& Video() const
        {
            return m_Video;
        }
        const WebmAudioTrack& Audio() const
        {
            return m_Audio;
        }
        int64_t DurationNs() const
        {
            return m_DurationNs;
        }
        const std::string& Error() const
        {
            return m_Error;
        }

    private:
        struct ClusterEntry
        {
            uint64_t BodyOffset = 0; // absolute file offset of the cluster's first child
            uint64_t BodySize   = 0;
        };
        struct CueEntry
        {
            int64_t  TimeNs        = 0;
            uint64_t ClusterOffset = 0; // absolute file offset of the Cluster element
        };

        bool        LoadCluster( size_t index );
        bool        ParseBlock( const uint8_t* data, size_t size, int64_t clusterTimeNs, bool simple,
                                int64_t discardPaddingNs );
        std::string ParseInfo( const std::vector<uint8_t>& body );
        std::string ParseTracks( const std::vector<uint8_t>& body );
        void        ParseCues( const std::vector<uint8_t>& body );

        std::ifstream             m_File;
        std::filesystem::path     m_Path;
        uint64_t                  m_SegmentDataOffset = 0;
        uint64_t                  m_TimecodeScaleNs   = 1000000;
        int64_t                   m_DurationNs        = 0;
        WebmVideoTrack            m_Video;
        WebmAudioTrack            m_Audio;
        std::vector<ClusterEntry> m_Clusters;
        std::vector<uint64_t>     m_ClusterElementOffsets; // parallel to m_Clusters: offset of the element id
        std::vector<CueEntry>     m_Cues;
        size_t                    m_NextCluster = 0;
        std::deque<MediaPacket>   m_Pending;
        std::string               m_Error;
    };
} // namespace Desert::Media
