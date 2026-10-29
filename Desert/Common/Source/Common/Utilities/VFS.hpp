#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Common::Utils
{
    // Virtual file system: serves CONTENT reads from a mounted .dpak archive when the file is not on
    // disk. Wiring model (UE-style shipping):
    //
    //   - dev/editor: nothing mounted -> every read is a plain disk read (zero behavior change);
    //   - packaged game: the Runtime mounts <package>/Content.dpak (plus any chunk/patch paks);
    //     FileSystem::ReadFileContent/Exists/... try DISK FIRST (loose-file override for
    //     debugging), then fall back through the mount stack.
    //
    // MOUNT STACK: MountPak may be called repeatedly; LATER mounts override earlier ones for the
    // same key. That is the update mechanism — ship base.dpak (+ per-type chunks), then distribute
    // small patch paks containing only the changed files and mount them last.
    //
    // Lookup: an incoming path (absolute or cwd-relative) is normalized and made relative to each
    // pak's MOUNT ROOT (the directory the .dpak sits in); that relative generic string is the
    // archive key — e.g. "/pkg/Assets/S.desce" with a pak at "/pkg/Content.dpak" -> "Assets/S.desce".
    // WHERE IN THE STACK AN ARCHIVE SITS (WP23). Content (the base and its chunks) always sits BELOW every
    // Patch, whenever it was mounted: a region chunk mounted on demand mid-game must not override the update
    // that was mounted at start (UE: pak order, chunk paks under the _P patch paks). Within a tier a later
    // mount overrides an earlier one, as before.
    enum class MountTier : unsigned char
    {
        Content = 0,
        Patch   = 1,
    };

    class VFS
    {
    public:
        // Mounts one archive on top of the stack.
        //
        // NO_DISCARD AND A NAMED ERROR, BOTH FOR THE SAME REASON. This returned a bare `bool`, and the
        // packaged game's own startup path dropped it at both call sites: a damaged patch pak mounted
        // "successfully", the player kept playing the content the update was supposed to replace, and
        // there was nothing in the log to find. A discarded result is now a compiler warning in a tree
        // that builds at zero warnings — the mechanism, not a convention someone has to remember.
        //
        // The error text is the reader's own account of WHICH step failed with the actual numbers (see
        // PakReader::OpenError), because the caller has to put it in front of a player who has no
        // sources: "missing or corrupt" is not something anyone can act on.
        NO_DISCARD static Common::BoolResultStr MountPak( const std::filesystem::path& pakFile,
                                                          MountTier                    tier = MountTier::Content );

        // ON-DEMAND ARCHIVES (WP23): a region chunk is KNOWN at start and mounted only when a cell of its
        // region is about to load (UE: chunk containers mounted when a streaming level needs them). DeferPak
        // records it; MountDeferred mounts it at the Content tier, once, whichever thread asks first.
        // MountDeferred of a path never deferred and not mounted is a refusal naming it. A patch whose
        // deletions name content of a deferred archive is not refused as orphaned.
        // ReturnToDeferred is the way back (UE: a chunk container unmounted when no streaming level needs it):
        // the archive leaves the stack and is on-demand again, so a later MountDeferred mounts it anew. A path
        // that was never mounted out of the deferred list is a refusal naming it.
        static void                             DeferPak( const std::filesystem::path& pakFile );
        NO_DISCARD static Common::BoolResultStr MountDeferred( const std::filesystem::path& pakFile );
        NO_DISCARD static Common::BoolResultStr ReturnToDeferred( const std::filesystem::path& pakFile );
        static bool                             IsMounted();
        static void                             Unmount();

        static bool                        Exists( const std::filesystem::path& path );

        // WHICH ARCHIVE WOULD ANSWER a read of this path, or nullopt when nothing mounted would.
        //
        // The stack's whole purpose is that a later mount overrides an earlier one for the same key,
        // and until now that was a property nobody could ASK about: ReadFile handed back bytes and the
        // caller had to infer the winner from their content. Inferring it is exactly what a patch test
        // must not do — two archives shipping the same key with the same bytes is a legal and common
        // case (a patch that re-ships a file unchanged), and it makes "which won" invisible to a
        // comparison of the bytes. A precedence that cannot be observed is a precedence that will be
        // silently wrong one day.
        //
        // Answered through the same Resolve walk as Exists and ReadFile, so it cannot disagree with
        // them: whatever this names IS what ReadFile would return, masking included (a key a patch has
        // deleted resolves to nothing here too).
        static std::optional<std::filesystem::path> SourcePak( const std::filesystem::path& path );
        static std::optional<std::string>  ReadFile( const std::filesystem::path& path );
        static std::optional<uint64_t>     FileSize( const std::filesystem::path& path );

        // Files under a directory prefix, returned as FULL paths (mountRoot / key) — callers treat them
        // exactly like disk paths; later reads resolve back through the VFS.
        static std::vector<std::filesystem::path> ListFiles( const std::filesystem::path& directory );
    };
} // namespace Common::Utils
