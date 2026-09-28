#include "WriteWatch.hpp"

#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <fstream>
#include <vector>

namespace Common::Utils
{
    namespace
    {
        // Straight off the disk, not through FileSystem::ReadFileContent: a file served from a mounted pak has
        // no write time, so it never reaches here, and a watched file is by definition a loose one.
        //
        // ONE READ OF THE WHOLE FILE, NOT A BYTE AT A TIME. This runs on the editor's thread for every racy
        // observation, and the decoded-thumbnail memo observes each picture on screen every frame: a capture
        // landing keeps its PNG racy for kRacyWriteWindow, and a folder of materials being captured keeps a
        // handful of ~300 KB PNGs racy at all times. Read through istreambuf_iterator (a push_back per byte),
        // that was 36 % of the editor thread in a folder of 240 materials (THUMB3, sampled), which is most
        // of why opening such a folder held every frame at 60-100 ms for minutes.
        std::optional<uint32_t> HashContent( const std::filesystem::path& path )
        {
            std::ifstream in( path, std::ios::binary | std::ios::ate );
            if ( !in )
                return std::nullopt;
            const std::streamoff end = in.tellg();
            if ( end < 0 )
                return std::nullopt;
            std::vector<char> bytes( static_cast<std::size_t>( end ) );
            in.seekg( 0 );
            if ( !bytes.empty() && !in.read( bytes.data(), static_cast<std::streamsize>( bytes.size() ) ) )
                return std::nullopt;
            return Crc32c( bytes.data(), bytes.size() );
        }
    } // namespace

    WriteWatch::Seen WriteWatch::Observe( const std::string& key, const std::filesystem::path& path )
    {
        // Sampled BEFORE the stat: a write that lands after this instant carries a write time at or past it,
        // so the observation it produces is racy by construction.
        const auto      readBegan = std::filesystem::file_time_type::clock::now();
        std::error_code timeError;
        std::error_code sizeError;
        const auto      writeTime = std::filesystem::last_write_time( path, timeError );
        const auto      size      = std::filesystem::file_size( path, sizeError );
        if ( timeError || sizeError )
            return Seen::Missing;

        const bool racy = IsRacyWriteTime( writeTime, readBegan );
        Stamp      next{ .WriteTime = writeTime, .Size = size, .ContentHash = std::nullopt, .Racy = racy };

        const auto it = m_Stamps.find( key );
        if ( it == m_Stamps.end() )
        {
            if ( racy )
                next.ContentHash = HashContent( path );
            m_Stamps.emplace( key, next );
            return Seen::First;
        }

        Stamp&     seen        = it->second;
        const bool stampEquals = seen.WriteTime == writeTime && seen.Size == size;
        if ( stampEquals && !seen.Racy )
            return Seen::Unchanged;

        // Either the stamp moved, or it cannot witness: the last observation was racy, so compare the content.
        // A hash that could not be taken on either side is a doubt, and a doubt reports a change.
        std::optional<uint32_t> hash;
        if ( racy || stampEquals )
            hash = HashContent( path );
        const bool changed = !stampEquals || !hash || !seen.ContentHash || *hash != *seen.ContentHash;

        if ( racy )
            next.ContentHash = hash;
        seen = next;
        return changed ? Seen::Changed : Seen::Unchanged;
    }

    void WriteWatch::Forget( const std::string& key )
    {
        m_Stamps.erase( key );
    }

    void WriteWatch::Clear()
    {
        m_Stamps.clear();
    }
} // namespace Common::Utils
