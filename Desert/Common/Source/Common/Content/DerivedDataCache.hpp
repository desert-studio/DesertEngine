#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Ported from UE 5.8 Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCacheKey.h:20-110 (FCacheBucket,
// FCacheKey) and Private/FileSystemCacheStore.cpp:80-97 (BuildPathForCachePackage, the local file store's
// Buckets/<Bucket>/<b0>/<b1>/<rest> layout), adapted: the key hash is our 64-bit Utils::PakContentHash rather
// than a 160-bit FIoHash (it is the id AF1 stamps into every envelope section, so the two cannot disagree);
// an entry is the raw artifact with its deriver's extension rather than a .udd record/package (each reader
// already validates its own format); the bucket is a string_view over a constexpr literal rather than an
// interned name. Hierarchical/HTTP/Zen/pak stores, cache policies and the async request machinery are not
// ported: one local directory, synchronous Get/Put.
//
// THE DERIVED DATA CACHE (AF5; UE's DDC). Everything the engine can recompute deterministically from an
// asset — SPIR-V, font atlases, icon SDFs, environment cubes, the driver's pipeline blob, editor
// thumbnails — lives here and NOWHERE in git. Deleting the directory is always legal: a miss is a rebuild.
//
// THE KEY. A deriver is named by a bucket (its directory) and a 128-bit VERSION GUID; the key of one entry
// is a hash of { version GUID, bucket, payload hash, settings bytes }. The payload hash is the hash of the
// bytes being derived from (Utils::PakContentHash, the same function AF1's envelope stamps into every
// section TOC — so an envelope asset's section hash can be handed in as-is without re-reading the file).
// The asset's PATH is deliberately not an input: moving or renaming a file must hit the same entry, and a
// key that folded the path would rebuild every derivation of a moved folder. Changing a deriver's
// algorithm means changing its GUID — that is the whole invalidation story, there is no version file.
//
// WHERE IT LIVES. `machine.json` DerivedDataCachePath (UE: [DerivedDataBackendGraph] Path): empty means
// <projectDir>/DerivedDataCache, a relative value is taken against the project directory, an absolute one
// may point anywhere (a fast disk, a share). It is a MACHINE's choice, not the project's: two people on one
// project legitimately keep it in different places, which is why it is not a .deproj field.
//
// THE PACKAGED GAME has no DDC. The packager copies the entries it cooked into Saved/Cooked/<Platform>/
// under the same relative layout (RelativePath) and packs that tree under the archive key
// "Cooked", so a reader that misses the loose DDC file asks the VFS for PackagedPath() of it. The editor
// never reads Saved/Cooked: only the packager names PlatformCookedDir().
namespace Common::DDC
{
    // FCacheBucket::IsValidName: alphanumeric, non-empty, at most 63 code units. It becomes a directory
    // name, so the rule is also what keeps a bucket from escaping the root.
    inline constexpr std::size_t BUCKET_MAX_NAME_LEN = 63;
    constexpr bool               IsValidBucketName( const std::string_view name ) noexcept
    {
        if ( name.empty() || name.size() > BUCKET_MAX_NAME_LEN )
            return false;
        for ( const char c : name )
        {
            const bool alnum = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' );
            if ( !alnum )
                return false;
        }
        return true;
    }

    struct DeriverVersion
    {
        uint64_t Hi = 0;
        uint64_t Lo = 0;
    };

    // One kind of derivation. Constexpr so each deriver declares itself once, beside its algorithm. UE's
    // FCacheKey{Bucket, Hash} is (Deriver::Bucket, the uint64 MakeKey returns): the deriver travels with the
    // hash because it also names the extension, and a hash without its deriver cannot be turned into a path.
    struct Deriver
    {
        std::string_view Bucket;    // directory under the DDC root; also hashed into the key
        std::string_view Extension; // including the dot, e.g. ".spv"
        DeriverVersion   Version;   // change it whenever the algorithm's output for the same inputs changes

        // consteval-checked at every declaration site: a bucket that is not a valid name does not compile.
        consteval Deriver( const std::string_view bucket, const std::string_view extension,
                           const DeriverVersion version )
             : Bucket( bucket ), Extension( extension ), Version( version )
        {
            if ( !IsValidBucketName( bucket ) )
                throw "DDC bucket names are alphanumeric and at most 63 characters (UE FCacheBucket)";
        }
    };

    // The key of one derived entry. `settings` is every parameter that shapes the output (sizes, flags,
    // profile), serialized by the deriver in a fixed order; nullptr/0 when there are none.
    uint64_t MakeKey( const Deriver& deriver, uint64_t payloadHash, const void* settings, size_t settingsSize );

    // The DDC root for a given setting value and project directory — pure, so the rule is testable.
    std::filesystem::path ResolveRoot( std::string_view setting, const std::filesystem::path& projectDir );

    // The live root: machine.json's DerivedDataCachePath resolved against the current project.
    std::filesystem::path Root();

    // "Buckets/<Bucket>/<h0h1>/<h2h3>/<h4..h15><Extension>" over the key's 16 lowercase hex digits — UE's
    // two-level fan-out, so no directory holds more than 1/65536 of a bucket. The one layout shared by the
    // DDC, Saved/Cooked and the archive.
    std::filesystem::path RelativePath( const Deriver& deriver, uint64_t key );

    // Root() / RelativePath(): where the entry is read and written in a development build.
    std::filesystem::path PathFor( const Deriver& deriver, uint64_t key );

    // The local file store's Get/Put (UE FFileSystemCacheStore). Get reads the loose entry, then — in a
    // packaged game — the archive at PackagedPath(); a miss is silent, it is the normal cold start. Put
    // writes atomically (write-then-rename): an entry is the whole artifact or absent.
    std::optional<std::string> Get( const Deriver& deriver, uint64_t key );
    bool                       Put( const Deriver& deriver, uint64_t key, std::string_view bytes );

    // Root() / "Buckets" / bucket — for derivers whose entries are not addressed by MakeKey (thumbnails, see
    // there).
    std::filesystem::path BucketDir( std::string_view bucket );

    // Where a packaged game finds the entry whose loose DDC path is `loosePath`: COOKED_PATH/<relative>.
    // A path outside the DDC root is returned unchanged.
    std::filesystem::path PackagedPath( const std::filesystem::path& loosePath );

    // Name of the platform this binary cooks for ("MacOS", "Windows", "Linux").
    std::string_view CookPlatformName();

    // <projectDir>/Saved/Cooked/<Platform> — the packager's output. Named here once so the census can
    // prove nothing outside Editor/Packaging reaches it.
    std::filesystem::path PlatformCookedDir();
} // namespace Common::DDC
