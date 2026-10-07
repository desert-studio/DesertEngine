#pragma once
// WHICH CACHED PICTURES ARE OLDER THAN THEIR FILE, AND WHEN ONE STOPS BEING (ThumbnailCache's bookkeeping, kept
// free of the GPU so its rule is tested on its own).
//
// ThumbnailCache::Get polls each picture through a Common::Utils::WriteWatch, which reports a rewrite ONCE; the
// new pixels take a worker several frames, so the path is remembered here — with the moment the watch reported
// it — until pixels READ AFTER that moment are cached. That is the whole rule, and it is enough: the watch
// hashes the content while the file's stamp is racy, so any later rewrite (same size, same clock tick included)
// is reported again and flags the path anew.
//
// THE RULE IT REPLACED kept the path outdated for as long as the file's stamp was racy (kRacyWriteWindow after
// the write). Every Get in that window took the pixels a worker had just decoded, stayed outdated, and queued
// the same file again: a capture landing on screen was decoded ~25 times in the following second (THM1l live
// log, one material after a hot reload). The racy clause guarded against a same-tick rewrite that the watch
// already catches; the read time is what actually says whether the pixels are the file as it was flagged.
#include <chrono>
#include <string>
#include <unordered_map>

namespace Desert::Editor
{
    class ThumbnailOutdated
    {
    public:
        using Clock = std::chrono::steady_clock;

        /// The watch has just reported @p path rewritten, at @p seen — taken AFTER the watch's observation, so a
        /// read that begins at or after it sees the content the watch now vouches for, or newer.
        void Flag( const std::string& path, Clock::time_point seen )
        {
            m_Since.insert_or_assign( path, seen );
        }

        [[nodiscard]] bool Contains( const std::string& path ) const
        {
            return m_Since.contains( path );
        }

        /// Pixels of @p path, read from the file starting at @p readBegan, were cached. They are the file as it
        /// was flagged (or newer) exactly when the read began at or after the flag; an earlier read — pixels a
        /// worker finished before the rewrite was seen — leaves the path outdated, so the next Get asks for the
        /// file once more. Returns whether the path is now current.
        bool Settle( const std::string& path, Clock::time_point readBegan )
        {
            const auto it = m_Since.find( path );
            if ( it == m_Since.end() )
                return true;
            if ( readBegan < it->second )
                return false;
            m_Since.erase( it );
            return true;
        }

        void Forget( const std::string& path )
        {
            m_Since.erase( path );
        }

        void Clear()
        {
            m_Since.clear();
        }

    private:
        std::unordered_map<std::string, Clock::time_point> m_Since;
    };
} // namespace Desert::Editor
