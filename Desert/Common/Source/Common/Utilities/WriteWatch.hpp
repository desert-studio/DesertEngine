#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace Common::Utils
{
    // THE ONE ANSWER TO "HAS THIS FILE CHANGED SINCE I LAST LOOKED?" for everything that polls a file and acts on
    // the answer: the asset and script hot reloaders and the editor's decoded-thumbnail memo.
    //
    // Each of them used to keep its own table of write times and call a file changed when the time moved. A
    // write time is only as fine as the file system's clock (~15.6 ms on NTFS), so a second same-size write
    // inside one tick left the stamp unchanged: the reloader kept the first edit until the file's next write,
    // the memo kept drawing the old picture. This applies the racy rule (IsRacyWriteTime) instead: a stamp that
    // was still inside kRacyWriteWindow when it was observed is not trusted — the file's CONTENT is hashed at
    // that observation and compared at the next one. Once an observation lands after the window, the stamp
    // settles and a poll costs one stat again.
    //
    // THE HASH IS TAKEN BEFORE THE CALLER READS. Observe() runs ahead of the caller's own load, so a write that
    // lands between the two can only make the next poll report a change the caller already has — a spare
    // reload, never a missed one. For a hot reloader that is the right direction to be wrong in.
    class WriteWatch
    {
    public:
        enum class Seen
        {
            Missing,   // no write time or size could be read; the previous observation is kept
            First,     // no previous observation under this key: a baseline, not an edit
            Unchanged, // the stamp is settled and equal, or it was racy and the content hashes equal
            Changed,   // the stamp moved, or it was racy and the content differs (or could not be hashed)
        };

        // @p key names the observation (callers key by asset key or path spelling); @p path is the file probed.
        [[nodiscard]] Seen Observe( const std::string& key, const std::filesystem::path& path );

        void Forget( const std::string& key );
        void Clear();

    private:
        struct Stamp
        {
            std::filesystem::file_time_type WriteTime{};
            uintmax_t                       Size = 0;
            // Set only while the stamp is racy: the content it stood for when it was observed.
            std::optional<uint32_t> ContentHash;
            bool                    Racy = false;
        };

        std::unordered_map<std::string, Stamp> m_Stamps;
    };
} // namespace Common::Utils
