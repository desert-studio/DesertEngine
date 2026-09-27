#include "WriteWatch.hpp"

#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <fstream>
#include <iterator>
#include <vector>

namespace Common::Utils
{
    namespace
    {
        // Straight off the disk, not through FileSystem::ReadFileContent: a file served from a mounted pak has
        // no write time, so it never reaches here, and a watched file is by definition a loose one.
        std::optional<uint32_t> HashContent( const std::filesystem::path& path )
        {
            std::ifstream in( path, std::ios::binary );
            if ( !in )
                return std::nullopt;
            const std::vector<char> bytes( ( std::istreambuf_iterator<char>( in ) ),
                                           std::istreambuf_iterator<char>() );
            if ( in.bad() )
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
