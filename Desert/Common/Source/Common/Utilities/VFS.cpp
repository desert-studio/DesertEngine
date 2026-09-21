#include "VFS.hpp"

#include "PakFile.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

namespace Common::Utils
{
    namespace
    {
        struct Mount
        {
            std::unique_ptr<PakReader> Pak;
            std::filesystem::path      Root; // absolute, normalized
        };

        // Mount STACK: later mounts override earlier ones for the same key (base.dpak first, then
        // per-type chunks, then patch paks — the patch wins). Lookups walk it newest-first.
        std::vector<Mount> s_Mounts;

        // One absolute, symlink-resolved spelling for an incoming path. weakly_canonical, not
        // lexically_normal alone, because two spellings of ONE directory must compare equal: on macOS
        // the temp tree is reached both as /var/... (a symlink) and /private/var/... (what getcwd
        // returns), and comparing an as-spelled mount root against a resolved current_path made every
        // relative lookup under a symlinked prefix miss the pak. The tail may not exist anywhere but
        // the archive — weakly_canonical resolves the existing prefix and keeps the rest lexical.
        std::filesystem::path CanonicalAbs( const std::filesystem::path& path )
        {
            std::error_code             ec;
            const std::filesystem::path raw =
                 path.is_absolute() ? path.lexically_normal()
                                    : ( std::filesystem::current_path( ec ) / path ).lexically_normal();

            std::error_code             canonEc;
            const std::filesystem::path canon = std::filesystem::weakly_canonical( raw, canonEc );
            return ( canonEc || canon.empty() ) ? raw : canon;
        }

        // Archive key for an incoming (pre-canonicalized) path within ONE mount: relative to that
        // mount's root. nullopt when the path points outside the mounted tree.
        std::optional<std::string> KeyFor( const Mount& mount, const std::filesystem::path& abs )
        {
            const std::filesystem::path rel = abs.lexically_relative( mount.Root );
            if ( rel.empty() || rel.begin()->string() == ".." )
                return std::nullopt;
            return rel.generic_string();
        }

        // Newest-first resolution of a path to the mount that owns it.
        //
        // THE ONE NEW RULE DELETIONS COST, AND IT LIVES HERE ALONE. Before reaching for a mount's
        // content, ask whether that mount MASKS the key (its .dpak-deleted list). A mask ends the walk:
        // everything below it is older, and a patch that removed a file removed it from the base too.
        //
        // Delete-then-re-add falls out of the ordering and is deliberately NOT an error. Patch 1 removes
        // A, patch 2 ships it again: the walk reaches patch 2 first, finds content, and serves it — the
        // sequence a real release history produces (a file dropped in 1.1 and restored in 1.2), so
        // refusing it would refuse a legal update. The analysis document proposed making that case a
        // mount error; it is the one recommendation in it this change does not follow, and this is why.
        template <typename Fn>
        auto Resolve( const std::filesystem::path& path, Fn&& fn )
             -> decltype( fn( *s_Mounts.front().Pak, std::string{} ) )
        {
            if ( s_Mounts.empty() ) // dev: nothing mounted, skip the canonicalization syscalls
                return {};
            const std::filesystem::path abs = CanonicalAbs( path ); // once, not per mount
            for ( auto it = s_Mounts.rbegin(); it != s_Mounts.rend(); ++it )
            {
                const auto key = KeyFor( *it, abs );
                if ( !key )
                    continue;
                if ( it->Pak->IsDeleted( *key ) )
                    return {}; // masked here and in everything older
                if ( !it->Pak->Contains( *key ) )
                    continue;
                return fn( *it->Pak, *key );
            }
            return {};
        }

        // Does anything already mounted SHIP this key? Used to refuse a patch whose deletion list names
        // a file the mounted content does not have — the signature of a patch built against a different
        // base. Content only, masks ignored on purpose: re-deleting an already-deleted key is redundant
        // rather than wrong, and refusing it would break a chain of patches that each carry the full
        // removal set for the version they supersede.
        bool StackShips( const std::filesystem::path& abs )
        {
            for ( const auto& mount : s_Mounts )
                if ( const auto key = KeyFor( mount, abs ); key && mount.Pak->Contains( *key ) )
                    return true;
            return false;
        }
    } // namespace

    NO_DISCARD Common::BoolResultStr VFS::MountPak( const std::filesystem::path& pakFile )
    {
        auto reader = std::make_unique<PakReader>( pakFile );
        if ( !reader->IsOpen() )
        {
            // LOG_ERROR, not LOG_WARN: nothing downstream recovers from an archive that did not mount
            // — its content is simply absent — so this is a failure, not a caution. The message is
            // only half the answer, though; the RESULT is the half the caller cannot ignore.
            LOG_ERROR( "[VFS] Could not mount {}: {}", pakFile.string(), reader->OpenError() );
            return Common::MakeFormattedError( "{}: {}", pakFile.string(), reader->OpenError() );
        }

        Mount mount;
        // The root must live in the same canonical spelling KeyFor produces for lookups, or a pak
        // mounted through a symlink (macOS /var -> /private/var) can never resolve anything.
        mount.Root = CanonicalAbs( pakFile ).parent_path();
        mount.Pak  = std::move( reader );

        // A DELETION THAT HAS NOTHING TO DELETE IS EVIDENCE, NOT A NO-OP. The list names keys relative
        // to this pak's own root; if the mounted stack does not ship one of them, this patch was built
        // against a base that is not the base it landed on — and the file it was published to remove is
        // some OTHER file, or none. Applying the rest of such a patch would produce a content set that
        // has never been tested anywhere, so the mount is refused and the caller gets the count and an
        // example. The refusal is worth having precisely because the failure it replaces is invisible:
        // the game would start, look updated, and be wrong.
        std::vector<std::string> orphans;
        for ( const auto& key : mount.Pak->DeletedKeys() )
            if ( !StackShips( CanonicalAbs( mount.Root / std::filesystem::path( key ) ) ) )
                orphans.push_back( key );
        if ( !orphans.empty() )
        {
            LOG_ERROR( "[VFS] Refusing {}: {} of its {} deletion(s) name content that is not mounted "
                       "(first: '{}')",
                       pakFile.string(), orphans.size(), mount.Pak->DeletedKeys().size(), orphans.front() );
            return Common::MakeFormattedError(
                 "{}: {} of its {} deletion(s) name content that is not mounted (first: '{}') — this patch "
                 "was built against a different base",
                 pakFile.string(), orphans.size(), mount.Pak->DeletedKeys().size(), orphans.front() );
        }

        LOG_INFO( "[VFS] Mounted {} ({} entries, {} deletion(s), root {}, priority {})", pakFile.string(),
                  mount.Pak->EntryCount(), mount.Pak->DeletedKeys().size(), mount.Root.string(), s_Mounts.size() );
        s_Mounts.push_back( std::move( mount ) );
        return Common::MakeSuccess( true );
    }

    bool VFS::IsMounted()
    {
        return !s_Mounts.empty();
    }

    void VFS::Unmount()
    {
        s_Mounts.clear();
    }

    bool VFS::Exists( const std::filesystem::path& path )
    {
        // THROUGH Resolve, not a second copy of the walk. This used to carry its own loop, and the
        // moment masking arrived that loop was a file that a patch had deleted still answering "yes" to
        // Exists while ReadFile said no — the recurring defect where both ends of a chain look right and
        // one link in between drops a property. `bool{}` is false, which is exactly "unresolved".
        return Resolve( path, []( const PakReader&, const std::string& ) { return true; } );
    }

    std::optional<std::filesystem::path> VFS::SourcePak( const std::filesystem::path& path )
    {
        return Resolve( path, []( const PakReader& pak, const std::string& )
                        { return std::optional<std::filesystem::path>( pak.ArchivePath() ); } );
    }

    std::optional<std::string> VFS::ReadFile( const std::filesystem::path& path )
    {
        return Resolve( path, []( const PakReader& pak, const std::string& key ) { return pak.Read( key ); } );
    }

    std::optional<uint64_t> VFS::FileSize( const std::filesystem::path& path )
    {
        return Resolve( path,
                        []( const PakReader& pak, const std::string& key ) { return pak.EntrySize( key ); } );
    }

    std::vector<std::filesystem::path> VFS::ListFiles( const std::filesystem::path& directory )
    {
        // Union across the stack, newest mount first, deduped by full path — a patched file lists once.
        //
        // Masking is applied in the SAME walk, and it works because the walk is newest-first: a mount's
        // deletions are recorded before any older mount is visited, so by the time the base's copy of a
        // deleted key comes up the mask is already standing. Keyed on the FULL path rather than the
        // archive key, because two mounts with different roots spell the same file differently.
        std::vector<std::filesystem::path>  result;
        std::unordered_set<std::string>     seen;
        std::unordered_set<std::string>     masked;

        if ( s_Mounts.empty() )
            return result;
        const std::filesystem::path abs = CanonicalAbs( directory );
        for ( auto it = s_Mounts.rbegin(); it != s_Mounts.rend(); ++it )
        {
            for ( const auto& deleted : it->Pak->DeletedKeys() )
                masked.insert( ( it->Root / std::filesystem::path( deleted ) ).generic_string() );

            auto prefix = KeyFor( *it, abs );
            if ( !prefix )
                continue;
            std::string p = *prefix;
            if ( p == "." )
                p.clear(); // the mount root itself
            else if ( !p.empty() && p.back() != '/' )
                p += '/';

            for ( const auto& key : it->Pak->KeysWithPrefix( p ) )
            {
                const std::filesystem::path full  = it->Root / std::filesystem::path( key );
                const std::string           spelt = full.generic_string();
                if ( masked.contains( spelt ) )
                    continue;
                if ( seen.insert( spelt ).second )
                    result.push_back( full );
            }
        }
        return result;
    }
} // namespace Common::Utils
