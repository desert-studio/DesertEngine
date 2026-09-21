#pragma once

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/UUID.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Common
{
    // A first-class asset handle. Derives from UUID so it stays a standard-layout 8-byte value that is
    // implicitly interconvertible with the raw uint64 (serialization, service maps, comparisons all keep
    // working unchanged), while OWNING the stable-id derivation that used to live in the free
    // Desert::Assets::StableAssetId() helper.
    //
    // What it adds over UUID:
    //   * FromCookedPath()/FromKey() give deterministic, path-derived handles that survive re-cooks and
    //     restarts and are computable WITHOUT reading the asset payload.
    // (The null default ctor is no longer a difference: UUID's default is null too, for the reasons in
    // UUID.hpp. Both are spelled out here anyway because this is the type asset code reads.)
    //
    // The 64-bit value and its serialized form are still a plain uint64.
    //
    // THE SENTENCE THAT USED TO BE HERE IS NO LONGER TRUE, AND SAYING SO IS THE POINT. It read
    // "nothing in the repository referenced a path-derived handle by number", and it was the licence
    // under which FromCookedPath's derivation was re-stamped project-relative without a migration.
    // Counted over `Editor/Resources/Assets` on 2026-09-21: 95 `TextureHandle`, 22 `MeshGuid` and 113
    // `MaterialId`/`ParentMaterialId` occurrences are path-derived handles written down AS NUMBERS in
    // committed content. TextureAsset::Load already knows this — it logs that a stale stored number
    // makes "every `.demat` naming the old number resolve to nothing" — so the two statements had been
    // contradicting each other in one tree.
    //
    // WHAT THAT MEANS FOR ANYONE CHANGING THE DERIVATION BELOW: it is a corpus migration now, not an
    // edit. `AssetHandleInverse::EveryNumericReferenceInShippedContentStillNamesItsFile` is the census
    // that turns that from a warning into a red build.
    class AssetHandle : public UUID
    {
    public:
        AssetHandle() noexcept : UUID( static_cast<uint64_t>( 0 ) ) // Null / unset — NOT random
        {
        }

        AssetHandle( uint64_t value ) noexcept : UUID( value )
        {
        }

        AssetHandle( const UUID& uuid ) noexcept : UUID( uuid )
        {
        }

        // Deterministic FNV-1a (64) over an arbitrary stable key. Same key -> same handle.
        static AssetHandle FromKey( std::string_view key ) noexcept
        {
            uint64_t h = 1469598103934665603ull;
            for ( unsigned char c : key )
            {
                h ^= c;
                h *= 1099511628211ull;
            }
            return AssetHandle( h ? h : 1ull ); // never collide with the null handle
        }

        // The roots an asset can live under, each with the TAG its relative paths are hashed behind.
        //
        // Why a TAG and not the bare relative path: `Cooked/Textures/T.tex` and
        // `Resources/Assets/Textures/T.tex` both reduce to `Textures/T.tex`, so without a tag a cooked
        // asset and a content asset that happen to sit at mirrored offsets would share one handle. The
        // tag is part of the hashed key, never of any path on disk.
        //
        // Why THESE three: they are exactly the roots AssetPreloader scans. Content and cooked assets
        // move with the project (Constants::Path::SetProjectRoot rewrites them); engine resources never
        // do. Longest match wins, which is what makes the default sandbox layout — where ASSETS_PATH
        // (`Resources/Assets/`) is NESTED INSIDE RESOURCE_PATH (`Resources/`) — resolve to `assets`
        // rather than to `engine`, without the answer depending on the order of this array.
        struct PathRoot
        {
            const std::filesystem::path* Root;
            std::string_view             Tag;
        };

        // ONE table, read by both directions. It used to be a local array inside StableKeyForPath, and
        // the moment the inverse below appeared that would have made two lists of roots that must agree
        // with nothing checking that they do — the defect shape this file's own comments are about. A
        // root added here reaches the writer and the reader in the same edit.
        static const std::array<PathRoot, 3>& ContentRoots() noexcept
        {
            static const std::array<PathRoot, 3> roots = {
                 PathRoot{ &Constants::Path::ASSETS_PATH, "assets" },
                 PathRoot{ &Constants::Path::COOKED_PATH, "cooked" },
                 PathRoot{ &Constants::Path::RESOURCE_PATH, "engine" },
            };
            return roots;
        }

        // The tag a given content root's keys are written behind, looked up by that root's own ADDRESS so
        // there is no second spelling of any tag anywhere in the repository. Empty if the root is not in
        // the table, which is a broken invariant of the table rather than a case a caller handles.
        static std::string_view TagForRoot( const std::filesystem::path& root ) noexcept
        {
            for ( const PathRoot& candidate : ContentRoots() )
            {
                if ( candidate.Root == &root )
                    return candidate.Tag;
            }
            return {};
        }

        // The tag PROJECT CONTENT is keyed behind, read out of the table above instead of spelled a
        // second time. It exists for callers that must compose a key WITHOUT touching the filesystem or
        // the live project root: a scene migration is pure by contract (DC §4.4), so it cannot call
        // StableKeyForPath — that one calls fs::absolute and compares against the roots as they stand
        // right now — yet the key it writes has to be the key StableKeyForPath would have produced.
        //
        // The value is a compile-time string literal and cannot vary with the project root; only the
        // ROW is looked up, and it is looked up by the assets root's own address so there is no second
        // spelling of "assets" anywhere in the repository.
        static std::string_view AssetsTag() noexcept
        {
            return TagForRoot( Constants::Path::ASSETS_PATH );
        }

        // The tag ENGINE RESOURCES are keyed behind, for the same callers and the same reason. A font or
        // a vector icon may legitimately live under either root — both are scan roots for their services
        // (Runtime/Services/ServiceScanRoots.hpp) — so a migration that can only name one of the two
        // would have to guess about the other.
        static std::string_view EngineTag() noexcept
        {
            return TagForRoot( Constants::Path::RESOURCE_PATH );
        }

        // Builds the stable key a path-derived handle is hashed from: the path RELATIVE to whichever
        // root contains it, behind that root's tag.
        //
        // WHY RELATIVE. This used to hash whatever spelling the caller happened to hold. Two measured
        // consequences: `Assets/Clouds/X.dcnv` hashed to 122788169303960361 while the same file spelled
        // absolutely hashed to 868888776058461864 — one file, two identities — and because every content
        // root turns absolute the moment a `.deproj` is opened (Constants::Path::SetProjectRoot), the
        // absolute form is the one that won in practice. That form encodes the developer's home
        // directory, so a handle written down on one machine named nothing on any other. The three cloud
        // branches in ComponentRegistry::MakeAssetResolver already store their paths relative to
        // ASSETS_PATH for exactly this reason, and the reasoning is written out there; this is that same
        // rule applied where identity is actually minted, so it holds for every asset class instead of
        // the three somebody remembered.
        //
        // A path under NO root keeps its normalized spelling, unchanged from before. That is deliberate
        // on two counts: a file genuinely outside the project has no project-relative identity to give
        // it (ComponentRegistry says the same thing in the same situation — "outside the project, say so
        // plainly"), and the synthetic keys that are not filesystem paths at all — `procedural://` clips,
        // `memory://` sequencer clips — must keep hashing to what they always did rather than acquire a
        // dependency on the process's working directory.
        // NOT noexcept, AND IT NEVER COULD BE. The three functions below build std::strings and
        // std::filesystem::paths; every one of those allocates, so each was a signature promising a
        // guarantee its body does not honour — the same "a comment is not the code" shape, written into the
        // type system where it is worse, because an allocation failure inside a noexcept function is
        // std::terminate rather than an exception a caller could see. Nothing depended on the promise:
        // none of the three is used where a non-throwing operation is required.
        static std::string StableKeyForPath( const std::filesystem::path& path )
        {
            namespace fs = std::filesystem;

            const fs::path normalized = path.lexically_normal();

            // Absolute forms are used ONLY to decide which root contains the path. Comparing the two
            // spellings directly cannot work: with a project open the roots are absolute while callers
            // still pass working-directory-relative strings (shaders always do — SHADERDIR_PATH is const
            // and is never remapped), and with no project open it is the other way round.
            //
            // A path that is ALREADY absolute skips fs::absolute, which consults the working directory.
            // Worth having and not worth much: measured over a 2000-asset dedup scan it took 56.9 s to
            // 53.6 s, because the cost here is the path algebra and its allocations rather than the
            // syscall. What actually made this function cheap enough to sit in the registry was calling
            // it once per asset instead of once per comparison — see AssetManager::CreateAsset.
            std::error_code ec;
            const fs::path  absolutePath =
                 normalized.is_absolute() ? normalized : fs::absolute( normalized, ec ).lexically_normal();
            if ( ec )
                return normalized.generic_string();

            std::string_view bestTag;
            std::string      bestRelative;
            std::size_t      bestRootLength = 0;

            for ( const PathRoot& candidate : ContentRoots() )
            {
                std::error_code rootEc;
                const fs::path  absoluteRoot = candidate.Root->is_absolute()
                                                    ? *candidate.Root
                                                    : fs::absolute( *candidate.Root, rootEc ).lexically_normal();
                if ( rootEc )
                    continue;

                const std::string relative = absolutePath.lexically_relative( absoluteRoot ).generic_string();

                // Empty means the two paths have no relation at all; "." means the path IS the root; a
                // leading ".." means the path escapes upwards, i.e. it is not under this root.
                if ( relative.empty() || relative == "." || relative.rfind( "..", 0 ) == 0 )
                    continue;

                const std::size_t rootLength = absoluteRoot.generic_string().size();
                if ( rootLength > bestRootLength )
                {
                    bestRootLength = rootLength;
                    bestTag        = candidate.Tag;
                    bestRelative   = relative;
                }
            }

            if ( bestRootLength == 0 )
                return normalized.generic_string();

            return std::string( bestTag ) + ':' + bestRelative;
        }

        // Does a key produced by StableKeyForPath actually name a place INSIDE the project?
        //
        // The two callers ask it for opposite reasons and both need the same answer: TextureImporter warns
        // at cook time that an untagged key binds the cooked file to one machine, and TextureAsset::Load
        // uses it to decide whether a stored handle can be compared against the derivation at all (a key
        // with no tag has no machine-independent identity to compare with). It is one sentence of the root
        // table's meaning, so it lives beside the table — the importer used to spell the loop inline, which
        // is how the second asker would have got a second, drifting copy.
        static bool IsProjectRelativeKey( std::string_view key )
        {
            for ( const PathRoot& candidate : ContentRoots() )
            {
                const std::string prefix = std::string( candidate.Tag ) + ':';
                if ( key.rfind( prefix, 0 ) == 0 )
                    return true;
            }
            return false;
        }

        // THE EXACT INVERSE of StableKeyForPath: turns `assets:Textures/T.png` back into the path that
        // root spells today. Written here, beside the forward direction, because the two are the classic
        // pair that must agree and the agreement is asserted (AssetHandleStability) rather than hoped for.
        //
        // WHY IT EXISTS. A reference stored in a scene has to name an asset in a form that survives being
        // sent to another machine, and StableKeyForPath is already the engine's one answer to "where is
        // this asset in the project". Storing that answer is only useful if it can be read back, and no
        // plain relative path can do the job for every asset class: a material lives under ASSETS_PATH
        // while a cooked texture lives under COOKED_PATH, which is a SIBLING of it, so a path made
        // relative to the assets root comes out as `../Cooked/...` for a texture and falls back to the
        // absolute spelling — i.e. to a developer's home directory in a committed file. The tag is what
        // carries the missing bit, and it is a bit no path can carry.
        //
        // A string with no known tag is returned unchanged. That covers three real cases and is not a
        // fallback: a plain relative path written before this form existed, an absolute path to a file
        // genuinely outside the project (StableKeyForPath returns those verbatim too), and the synthetic
        // `procedural://` / `memory://` keys that are identities rather than locations.
        static std::filesystem::path PathForStableKey( std::string_view key )
        {
            for ( const PathRoot& candidate : ContentRoots() )
            {
                const std::string prefix = std::string( candidate.Tag ) + ':';
                if ( key.rfind( prefix, 0 ) != 0 )
                    continue;

                const std::string_view relative = key.substr( prefix.size() );
                // A tag with nothing after it names the root itself, which is not an asset; returning the
                // root would silently hand back a directory, so the key is treated as untagged text.
                if ( relative.empty() )
                    break;

                return ( *candidate.Root / std::filesystem::path( relative ) ).lexically_normal();
            }

            return std::filesystem::path( key );
        }

        // Deterministic handle from an asset's path, keyed on its location RELATIVE to the project (see
        // StableKeyForPath) so the same file carries the same handle on every machine and under every
        // spelling. Computable without parsing the (large) payload.
        //
        // AND IT RECORDS THE INVERSE. The hash is one-way, so the only moment at which the number and
        // the key it came from are both in hand is right here. Every other way of building a
        // handle->path table — a directory walk, a per-type resolver branch, a service registry — is a
        // SECOND list that has to be kept in step with this one, and this repository has already paid
        // for that twice (the packager's hand-written tree list that forgot fonts and icons; the
        // twelve-branch ToPath that returns "" for a type nobody added). Recording at the mint makes
        // the inverse total by construction: there is no middle link to drop it.
        //
        // The record's answer is DISCARDED, and that is deliberate rather than sloppy. Its only failure
        // is a collision — two different keys reaching one number — and there is nothing this function
        // could do about one: both callers asked for the identity of their own file and both are
        // entitled to an answer. AssetPathIndex::Record has already logged the number and both keys,
        // which is the report; the census over shipped content asserts the case never arises.
        //
        // NOT noexcept, AND IT NEVER WAS ABLE TO BE. StableKeyForPath allocates — its own comment says
        // so at length — so the promise this signature used to carry was one its body could not keep,
        // and an allocation failure under it would have been std::terminate rather than an exception.
        // Nothing depended on the promise: no caller uses this where a non-throwing operation is
        // required.
        static AssetHandle FromCookedPath( const std::filesystem::path& cookedPath )
        {
            const std::string key    = StableKeyForPath( cookedPath );
            const AssetHandle handle = FromKey( key );
            static_cast<void>( AssetPathIndex::Record( static_cast<uint64_t>( handle ), key ) );
            return handle;
        }

        // Fresh, random runtime id — for assets with no stable source path (procedural / builtin meshes).
        static AssetHandle Generate() noexcept
        {
            return AssetHandle( UUID::Generate() );
        }

        static AssetHandle Null() noexcept
        {
            return AssetHandle();
        }
    };
} // namespace Common

namespace std
{
    template <>
    struct hash<Common::AssetHandle>
    {
        std::size_t operator()( const Common::AssetHandle& handle ) const noexcept
        {
            return static_cast<std::size_t>( static_cast<uint64_t>( handle ) );
        }
    };
} // namespace std
