#include <variant>
#include "TextureImporter.hpp"

#include <mutex>
#include "CookPaths.hpp"
#include "CookedJsonWrite.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstring>

#include <stb_image/stb_image.h>

namespace Desert::Editor
{
    static std::filesystem::path BuildCookedPath( const std::filesystem::path& sourcePath,
                                                  const std::string&           extension )
    {
        // Path formula is shared (CookPaths::CookedTexture); this wrapper ensures the dir exists for writing.
        const auto result = Editor::CookPaths::CookedTexture( sourcePath, extension );
        std::filesystem::create_directories( result.parent_path() );
        return result;
    }

    std::filesystem::path TextureImporter::CookedMetaPath( const std::filesystem::path& source )
    {
        return BuildCookedPath( source, ".tex" );
    }

    Common::UUID TextureImporter::Import( const std::filesystem::path& path )
    {
        // Bulk cooking runs mesh imports in PARALLEL; two meshes often share textures, and two threads
        // writing the same cooked .tex would corrupt it. Texture cooking is cheap next to the Assimp
        // parse, so one global lock here is the simplest safe answer.
        static std::mutex           s_CookMutex;
        std::lock_guard<std::mutex> cookLock( s_CookMutex );

        auto abs = std::filesystem::weakly_canonical( path ).string();

        if ( m_Cache.contains( abs ) )
        {
            return m_Cache[abs];
        }

        const auto meta = BuildCookedPath( path, ".tex" );

        // Deterministic, always: the handle is a function of the source path, so wiping Cooked/ and
        // re-cooking yields the SAME id and every material/scene reference keyed by it still resolves.
        //
        // This used to read the handle back out of an existing `.tex` instead, "back-compat with older
        // random-handle cooks". That branch did not preserve compatibility, it preserved the DEFECT: a
        // texture cooked in the random era kept its per-launch id frozen on disk for ever, and the moment
        // anyone deleted Cooked/ the id changed and every reference to it died. One such handle was still
        // in the repository (T_Checker.tex, referenced by M_CheckerFloor.demat); both files were re-stamped
        // with the derived id by the change that deleted this branch.
        //
        // THROUGH FromCookedPath, and `path` rather than `abs`. Hashing the canonical ABSOLUTE string was
        // the last producer of asset identity that keyed on where the project sits, and it is the one that
        // reached the repository: T_Checker.tex carried 16135626166276358966, which is FNV-1a of
        // `/Users/<a developer>/…/Textures/T_Checker.png`, and M_CheckerFloor.demat named the texture by
        // that number. It resolved only because both files were shipped together with the number already
        // frozen; re-cooking that texture on any other machine minted a different id and emptied the
        // material's slot. FromCookedPath keys on the source's place INSIDE the project, so the re-cook
        // now agrees between machines. The same two files are re-stamped by this change, exactly as the
        // paragraph above records being done last time.
        const Common::UUID handle = Common::AssetHandle::FromCookedPath( path );

        // The SAME key the handle is hashed from is what SourcePath stores: the source's place inside the
        // project behind its root's tag (`assets:Textures/T.png`), never the spelling of this machine's
        // checkout. The paragraph above celebrates curing the HANDLE of machine-dependence; SourcePath used
        // to be written one line below it as the weakly-canonical ABSOLUTE path — the identical defect, and
        // the one the runtime actually loads pixels through (TextureFactory reads it back verbatim). A
        // naive relative(source, ASSETS_PATH) is not an option for the same reason TextureSlot.cpp records:
        // COOKED_PATH is a SIBLING of the assets root, so some legitimate sources relativize to `../` and
        // fall back to the absolute spelling anyway. StableKeyForPath owns the root table and the fallback.
        const std::string sourceKey = Common::AssetHandle::StableKeyForPath( path );

        // THE SOURCE'S BYTES, READ ONCE AND USED FOR BOTH QUESTIONS: is the cook stale, and what goes
        // into the container. A decode needs them anyway, so the CRC is not a second pass over the file.
        const auto sourceBytes = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !sourceBytes.IsSuccess() )
        {
            LOG_ERROR( "[TextureImporter] '{0}' could not be read ({1}); no cooked texture was written and "
                       "the null handle is returned.",
                       abs, sourceBytes.GetError() );
            return Common::AssetHandle::Null();
        }
        const uint32_t sourceHash =
             Common::Utils::Crc32c( sourceBytes.GetValue().data(), sourceBytes.GetValue().size() );

        // FRESHNESS IS A FACT ABOUT BYTES NOW, NOT ABOUT TIMESTAMPS. The `.tex` records the CRC-32C of
        // the source it was cooked from; the cook is up to date exactly when that number still matches,
        // and when the two identity fields agree with their derivations.
        //
        // WHAT THE mtime COMPARISON THIS REPLACED COULD NOT DO, in this repository's own words: "git
        // sets mtimes to checkout time, so a committed stale .tex was up to date for ever by
        // construction". It also could not see the case an artist actually produces — a PNG re-exported
        // with the same content, or edited and then restored — and it re-cooked on every clone. Reading
        // and hashing the source costs one pass at 8.17 GB/s (Common/Utilities/Crc32c.hpp), against a
        // decode plus a full mip chain for the answer "nothing changed".
        if ( const auto existing = Common::Utils::FileSystem::ReadFileContentPrefix(
                  meta, Assets::Serialization::kTextureBinaryPrefixBytes );
             existing.IsSuccess() )
        {
            const auto stored =
                 Assets::Serialization::DecodeTextureHeader( existing.GetValue(), meta.string() );
            if ( stored.IsSuccess() && stored.GetValue().SourceContentHash == sourceHash &&
                 static_cast<uint64_t>( stored.GetValue().Handle ) == static_cast<uint64_t>( handle ) &&
                 stored.GetValue().SourcePath == sourceKey )
            {
                m_Cache[abs] = handle; // up to date AND consistent — keep the cooked container as it is
                return handle;
            }

            if ( !stored.IsSuccess() )
            {
                LOG_INFO( "[TextureImporter] Re-cooking '{0}': {1}", meta.string(), stored.GetError() );
            }
            else
            {
                LOG_INFO( "[TextureImporter] Re-cooking '{0}': it stores Handle={1} SourcePath='{2}' "
                          "source CRC {3:#010x}; the source now derives Handle={4} SourcePath='{5}' CRC "
                          "{6:#010x}.",
                          meta.string(), static_cast<uint64_t>( stored.GetValue().Handle ),
                          stored.GetValue().SourcePath, stored.GetValue().SourceContentHash,
                          static_cast<uint64_t>( handle ), sourceKey, sourceHash );
            }
        }

        // A SOURCE FORMAT IS AN INPUT, NOT A STORAGE FORMAT — `Docs/Textures/T2_CONTAINER_DECISION.md`.
        // This is the ONE place in the project that decodes one, and everything downstream reads the
        // container this function writes.
        //
        // HDR SOURCES KEEP THEIR RANGE. The cooked file used to record `Format: RGBA8F` for every
        // source including `.hdr` — a field that was simultaneously wrong and unread, because the
        // loader sniffed the source file itself and decided again. The container's format field is the
        // answer now, so it has to be the true one.
        const bool isHDR = stbi_is_hdr_from_memory(
             reinterpret_cast<const stbi_uc*>( sourceBytes.GetValue().data() ),
             static_cast<int>( sourceBytes.GetValue().size() ) ) != 0;

        int                          w = 0, h = 0, ch = 0;
        std::vector<std::byte>       base;
        Desert::Core::Formats::ImageFormat format = Desert::Core::Formats::ImageFormat::RGBA8F;

        if ( isHDR )
        {
            float* pixels = stbi_loadf_from_memory(
                 reinterpret_cast<const stbi_uc*>( sourceBytes.GetValue().data() ),
                 static_cast<int>( sourceBytes.GetValue().size() ), &w, &h, &ch, 4 );
            if ( !pixels )
            {
                const char* reason = stbi_failure_reason();
                LOG_ERROR( "[TextureImporter] stbi_loadf failed for '{0}' ({1}); no cooked texture was "
                           "written and the null handle is returned.",
                           abs, reason ? reason : "no reason reported" );
                return Common::AssetHandle::Null();
            }
            format = Desert::Core::Formats::ImageFormat::RGBA32F;
            base.resize( static_cast<size_t>( w ) * h * 4u * sizeof( float ) );
            std::memcpy( base.data(), pixels, base.size() );
            stbi_image_free( pixels );
        }
        else
        {
            stbi_uc* pixels = stbi_load_from_memory(
                 reinterpret_cast<const stbi_uc*>( sourceBytes.GetValue().data() ),
                 static_cast<int>( sourceBytes.GetValue().size() ), &w, &h, &ch, 4 );
            if ( !pixels )
            {
                // No `.tex` is written and the null handle is returned: a failed decode used to fall
                // through and freeze the UNINITIALIZED w/h into the cooked file, silently. The failure
                // is not cached either, so fixing the image and importing again works without
                // restarting the editor.
                const char* reason = stbi_failure_reason();
                LOG_ERROR( "[TextureImporter] stbi_load failed for '{0}' ({1}); no cooked texture was "
                           "written and the null handle is returned.",
                           abs, reason ? reason : "no reason reported" );
                return Common::AssetHandle::Null();
            }
            base.resize( static_cast<size_t>( w ) * h * 4u );
            std::memcpy( base.data(), pixels, base.size() );
            stbi_image_free( pixels );
        }

        // A source outside every content root has no project-relative name to store, so the key IS the
        // absolute spelling (StableKeyForPath's documented behaviour) and the cooked file is bound to this
        // machine. Say so once, at cook time, instead of letting the artist discover it on a colleague's
        // machine as an empty material slot.
        if ( !Common::AssetHandle::IsProjectRelativeKey( sourceKey ) )
        {
            LOG_WARN( "[TextureImporter] '{0}' lies outside every content root, so its cooked container "
                      "stores the absolute path and will not resolve on another machine.",
                      abs );
        }

        Assets::Serialization::TextureAssetData data;
        data.Handle            = handle;
        data.SourcePath        = sourceKey;
        data.SourceContentHash = sourceHash;
        data.Width             = static_cast<uint32_t>( w );
        data.Height            = static_cast<uint32_t>( h );
        data.Format            = format;

        // THE MIP CHAIN IS BUILT HERE, ON THE CPU, ONCE PER COOK. It used to be built on the GPU on
        // every load with `vkCmdBlitImage`, which is impossible for the block-compressed formats this
        // container exists to carry (`blitDst=0`) — so the chain has to be in the file before the
        // format can change, and that ordering is `Docs/World/PROGRAMME.md` §5.
        auto chain = Assets::Serialization::BuildMipChain( data.Width, data.Height, data.Format, base,
                                                           data.Pixels );
        if ( !chain.IsSuccess() )
        {
            LOG_ERROR( "[TextureImporter] '{0}' was decoded but its mip chain could not be built: {1}. "
                       "The null handle is returned and nothing is cached.",
                       abs, chain.GetError() );
            return Common::AssetHandle::Null();
        }
        data.Levels = chain.ExtractValue();

        // A HANDLE IS ONLY RETURNED FOR A CONTAINER THAT IS ON THE DISK. The write used to be unchecked
        // (Д31-D), and the handle plus the cache entry went back regardless — so the very next lookup
        // was served out of memory and the missing `.tex` was not noticed until the next session.
        if ( const auto written =
                  WriteCookedBytes( Assets::Serialization::EncodeTextureBinary( data ), meta );
             !written )
        {
            LOG_ERROR( "[TextureImporter] '{0}' was decoded but its cooked container was not written: {1}. "
                       "The null handle is returned and nothing is cached, so importing again after fixing "
                       "the cause works without restarting the editor.",
                       abs, written.GetError() );
            return Common::AssetHandle::Null();
        }

        m_Cache[abs] = handle;

        return handle;
    }

} // namespace Desert::Editor