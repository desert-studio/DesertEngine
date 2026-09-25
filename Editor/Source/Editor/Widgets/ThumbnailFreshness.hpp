#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Desert::Editor::ThumbnailFreshness
{
    /**
     * @brief Is the cached PNG the picture to SHOW, or the picture to REPLACE?
     *
     * THE RELATION THIS EXISTS TO KEEP. The readers (the asset browser's grid, its preview pane, the
     * Details material slot, Collections) and the writer (ThumbnailService::ShouldQueue, the background
     * sweep) all ask this ONE rule, so every observable state is either shown or scheduled, and none is
     * both nothing (a permanent flat swatch). Desert/Tests/Editor/ThumbnailFreshness asserts that.
     *
     * THE PICTURE IS KEYED ON THE ASSET'S BYTES, NOT ON ITS CLOCK (TH1). The rule used to compare
     * modtimes: a PNG was stale when its source was more than three seconds newer. A modtime is not a
     * fact about the asset — `git checkout`, a pull, a launcher update that unpacks the content tree, or
     * anything that re-saves a file unchanged moves it — and every such move re-rendered the content tree
     * at the next start (owner's Windows log, 2026-09-25: a `captured` line per material on EVERY launch,
     * and a flat colour swatch in place of each sphere until the queue drained). The input is now a
     * content hash of the source, recorded beside the PNG when the picture is made (Record); the PNG is
     * shown exactly when that hash equals the source's current one. Time is used only so an untouched
     * file is not re-hashed within one process (Detail::Memoised), never to decide.
     *
     * Device-free and header-only on purpose, for the reason ThumbnailKey.hpp gives next door: the
     * decision then belongs to a test instead of to a launched editor.
     */
    struct Observation
    {
        bool PngExists = false;

        /// Hash of the source's bytes recorded when the PNG was written; empty = no record (a PNG made
        /// before TH1, or a writer that died between the PNG and the record).
        std::optional<uint64_t> Recorded;

        /// Hash of the source's bytes now; empty = the source could not be read.
        std::optional<uint64_t> Current;
    };

    enum class Verdict
    {
        Show,   ///< the PNG on disk is the picture; decode and draw it, queue nothing
        Capture ///< there is no usable PNG; draw a placeholder and let the service render one
    };

    /**
     * @brief The one answer. Total by construction: there is no third outcome and no state without one.
     *
     * An unreadable source yields Show rather than Capture, and that direction is deliberate: a PNG that
     * exists is evidence, a source that could not be read is not, and re-rendering on the absence of
     * evidence would be an endless capture loop (the capture could not record a hash either).
     */
    [[nodiscard]] constexpr Verdict Judge( const Observation& seen ) noexcept
    {
        if ( !seen.PngExists )
            return Verdict::Capture;
        if ( !seen.Current )
            return Verdict::Show;
        return seen.Recorded == seen.Current ? Verdict::Show : Verdict::Capture;
    }

    /// What a reader puts on the card. A different question from Judge's, and the difference is the owner's
    /// complaint (2026-09-25: "a flat colour until everything has loaded").
    enum class Picture
    {
        CachedPng,  ///< decode and draw the PNG on disk — fresh, or outdated while its replacement is made
        Placeholder ///< no picture of this asset exists at all; the albedo swatch is the true statement
    };

    /**
     * @brief Draw ANY picture of the asset rather than none. Judge decides whether to capture; this decides
     * what to show meanwhile, and an outdated sphere is closer to the asset than a flat swatch of its albedo:
     * a picture recorded before TH1 (no .src), or one whose source was edited, stays on screen until the
     * service's new capture overwrites it (ThumbnailCache::Get re-decodes the rewritten file). The swatch
     * is left for the one state in which there is no picture to show.
     */
    [[nodiscard]] constexpr Picture Choose( const Observation& seen ) noexcept
    {
        return seen.PngExists ? Picture::CachedPng : Picture::Placeholder;
    }

    /// Where the source hash of a PNG is recorded: beside it, so wiping the bucket wipes both.
    [[nodiscard]] inline std::filesystem::path RecordPath( const std::filesystem::path& png )
    {
        std::filesystem::path record = png;
        record += ".src";
        return record;
    }

    namespace Detail
    {
        /// (size, modtime) of the file a memo entry was computed from; any change re-reads the bytes.
        struct Memo
        {
            uintmax_t                       Size = 0;
            std::filesystem::file_time_type Stamp;
            std::optional<uint64_t>         Value;
        };

        // Readers ask per visible card per frame, so both the hash and the record are memoised per
        // process. Guarded because paint jobs hash and record from JobSystem workers.
        template <typename ReadFn>
        std::optional<uint64_t> Memoised( const std::filesystem::path& file, ReadFn&& read )
        {
            static std::mutex                            mutex;
            static std::unordered_map<std::string, Memo> memo;

            std::error_code sizeEc;
            std::error_code stampEc;
            const uintmax_t size  = std::filesystem::file_size( file, sizeEc );
            const auto      stamp = std::filesystem::last_write_time( file, stampEc );
            if ( sizeEc || stampEc )
                return std::nullopt;

            const std::lock_guard lock( mutex );
            Memo&                 entry = memo[file.string()];
            if ( entry.Value && entry.Size == size && entry.Stamp == stamp )
                return entry.Value;
            entry = { size, stamp, read( file ) };
            return entry.Value;
        }

        inline std::optional<std::string> ReadAll( const std::filesystem::path& file )
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return std::nullopt;
            std::string bytes( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            if ( in.bad() )
                return std::nullopt;
            return bytes;
        }

        inline std::optional<uint64_t> HashFile( const std::filesystem::path& file )
        {
            const std::optional<std::string> bytes = ReadAll( file );
            if ( !bytes )
                return std::nullopt;
            return Common::Utils::PakContentHash( bytes->data(), bytes->size() );
        }

        inline std::optional<uint64_t> ParseRecord( const std::filesystem::path& file )
        {
            const std::optional<std::string> text = ReadAll( file );
            if ( !text || text->empty() )
                return std::nullopt;
            uint64_t   value = 0;
            const auto res   = std::from_chars( text->data(), text->data() + text->size(), value, 16 );
            if ( res.ec != std::errc() )
                return std::nullopt;
            return value;
        }
    } // namespace Detail

    /// Content hash of `source`, re-read only when its size or modtime changed in this process.
    [[nodiscard]] inline std::optional<uint64_t> ContentHash( const std::filesystem::path& source )
    {
        return Detail::Memoised( source, Detail::HashFile );
    }

    /**
     * @brief Record that `png` is a picture of `sourceHash`. Called by the writer once the PNG landed,
     * with the hash taken when the capture was DISPATCHED — an edit made while the picture was being
     * rendered must leave the record stale, not certify the older picture as the newer asset.
     */
    [[nodiscard]] inline Common::BoolResultStr Record( const std::filesystem::path& png, uint64_t sourceHash )
    {
        const std::filesystem::path record = RecordPath( png );
        std::ofstream               out( record, std::ios::binary | std::ios::trunc );
        if ( !out )
            return Common::MakeFormattedError<bool>( "cannot open thumbnail record '{}' for writing",
                                                     record.string() );
        std::array<char, 16> text{};
        const auto           res = std::to_chars( text.data(), text.data() + text.size(), sourceHash, 16 );
        out.write( text.data(), res.ptr - text.data() );
        out.close();
        if ( !out )
            return Common::MakeFormattedError<bool>( "failed writing thumbnail record '{}'", record.string() );
        return Common::MakeSuccess( true );
    }

    /// Ask the filesystem the three questions Judge needs. Separate from Judge so the DECISION stays pure.
    [[nodiscard]] inline Observation Observe( const std::filesystem::path& png,
                                              const std::filesystem::path& source )
    {
        Observation     seen;
        std::error_code existsEc;
        seen.PngExists = std::filesystem::exists( png, existsEc ) && !existsEc;
        if ( !seen.PngExists )
            return seen;
        seen.Current  = ContentHash( source );
        seen.Recorded = Detail::Memoised( RecordPath( png ), Detail::ParseRecord );
        return seen;
    }

    /// The PNG's modification time, absent when there is no file. What "the capture wrote it" is measured by.
    [[nodiscard]] inline std::optional<std::filesystem::file_time_type> Stamp( const std::filesystem::path& png )
    {
        std::error_code ec;
        const auto      stamp = std::filesystem::last_write_time( png, ec );
        if ( ec )
            return std::nullopt;
        return stamp;
    }

    /**
     * @brief One renderer capture, from dispatch until the renderer is idle again — INCLUDING after the
     * service stopped waiting for it.
     *
     * GIVING UP DOES NOT FORGET THE DISPATCH (TH1c). The service stops waiting after a fixed number of
     * frames so the queue is not held hostage, but the renderer's single slot finishes the job anyway and
     * writes the PNG seconds later. That late PNG used to land with no record beside it: a picture the
     * next session could not tell from a stale one, so it was re-rendered on every launch (the two
     * M_SIL_*_Clouds materials in the TH1b measurement). The late PNG is ACCEPTED, not refused: it was
     * rendered from the asset as it stood at dispatch, and the hash taken THEN is what gets recorded, so
     * an edit made in the meantime still reads as stale (Record). Refusing it would discard a finished,
     * correct render only to pay for it again next session.
     *
     * THE PICTURE MUST HAVE MOVED, not merely exist: a stale thumbnail is re-captured in place, so the old
     * file sits at the target path before the renderer starts, and an existence check would certify a
     * capture that wrote nothing.
     */
    class Capture
    {
    public:
        enum class Landed
        {
            Written,   ///< the PNG changed since dispatch; the dispatch-time hash was recorded beside it
            NotWritten ///< the renderer went idle without touching the target
        };

        struct Settled
        {
            std::string                Identity;
            std::filesystem::path      Png;
            Landed                     What = Landed::NotWritten;
            bool                       Late = false;  ///< the service had already given up on it
            std::optional<std::string> RecordError;  ///< Written, but the record could not be saved
        };

        /// The renderer accepted a capture of `source` into `png`. Hash and stamp are taken NOW.
        void Begin( std::string identity, std::filesystem::path png, const std::filesystem::path& source )
        {
            m_Identity   = std::move( identity );
            m_PngBefore  = Stamp( png );
            m_SourceHash = ContentHash( source );
            m_Png        = std::move( png );
            m_GaveUp     = false;
        }

        /// A capture is dispatched and not yet settled, waited for or not.
        [[nodiscard]] bool Outstanding() const { return !m_Identity.empty(); }
        /// Outstanding, and the service is still waiting for it.
        [[nodiscard]] bool Waiting() const { return Outstanding() && !m_GaveUp; }
        [[nodiscard]] const std::string& Identity() const { return m_Identity; }

        /// Stop waiting. The dispatch is KEPT, so the PNG the renderer still writes gets its record.
        void GiveUp() { m_GaveUp = true; }

        /// Call once the renderer is idle. Empty when nothing was outstanding.
        [[nodiscard]] std::optional<Settled> Settle()
        {
            if ( !Outstanding() )
                return std::nullopt;
            Settled settled{ m_Identity, m_Png, Landed::NotWritten, m_GaveUp, std::nullopt };
            const std::optional<std::filesystem::file_time_type> after = Stamp( m_Png );
            if ( after && after != m_PngBefore )
            {
                settled.What = Landed::Written;
                if ( !m_SourceHash )
                    settled.RecordError = "the source could not be hashed at dispatch";
                else if ( const Common::BoolResultStr recorded = Record( m_Png, *m_SourceHash ); !recorded )
                    settled.RecordError = recorded.GetError();
            }
            Reset();
            return settled;
        }

        void Reset()
        {
            m_Identity.clear();
            m_Png.clear();
            m_PngBefore.reset();
            m_SourceHash.reset();
            m_GaveUp = false;
        }

    private:
        std::string                                    m_Identity;
        std::filesystem::path                          m_Png;
        std::optional<std::filesystem::file_time_type> m_PngBefore;
        std::optional<uint64_t>                        m_SourceHash;
        bool                                           m_GaveUp = false;
    };
} // namespace Desert::Editor::ThumbnailFreshness
