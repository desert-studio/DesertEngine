#include <variant>
#include "TextureImporter.hpp"

#include <mutex>
#include "CookPaths.hpp"
#include "CookedJsonWrite.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Assets/Serialization/Texture.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <fstream>
#include <sstream>

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

        std::error_code ec;
        if ( std::filesystem::exists( meta, ec ) )
        {
            std::error_code ec2;
            const auto      metaT = std::filesystem::last_write_time( meta, ec2 );
            const auto      srcT  = std::filesystem::last_write_time( path, ec2 );
            if ( !ec2 && metaT >= srcT )
            {
                // mtime alone used to decide, and the derived handle was returned without anything checking
                // that the FILE stores the same one — while the runtime takes its handle from the file
                // (TextureAsset::Load), not from this return value. Nobody owned the relation
                // "returned == stored", and git sets mtimes to checkout time, so a committed stale .tex was
                // "up to date" for ever by construction. The file is now read back and kept only if it
                // agrees with the derivation on BOTH identity fields; anything else is re-cooked, loudly.
                std::ifstream     in( meta, std::ios::binary );
                std::stringstream buffer;
                buffer << in.rdbuf();
                const auto stored = rfl::json::read<Assets::Serialization::TextureAssetData>( buffer.str() );
                if ( stored.has_value() &&
                     static_cast<uint64_t>( stored->Handle ) == static_cast<uint64_t>( handle ) &&
                     stored->SourcePath == sourceKey )
                {
                    m_Cache[abs] = handle; // up-to-date AND consistent — keep the cooked metadata as-is
                    return handle;
                }

                if ( !stored.has_value() )
                {
                    LOG_WARN( "[TextureImporter] '{0}' is up to date by mtime but does not parse ({1}); "
                              "re-cooking it.",
                              meta.string(), stored.error().what() );
                }
                else
                {
                    LOG_INFO( "[TextureImporter] Re-stamping '{0}': it stores Handle={1} SourcePath='{2}', "
                              "the derivation says Handle={3} SourcePath='{4}'.",
                              meta.string(), static_cast<uint64_t>( stored->Handle ), stored->SourcePath,
                              static_cast<uint64_t>( handle ), sourceKey );
                }
            }
        }

        int      w = 0, h = 0, ch = 0;
        stbi_uc* pixels = stbi_load( abs.c_str(), &w, &h, &ch, 4 );
        if ( !pixels )
        {
            // No .tex is written and the null handle is returned: a failed decode used to fall through and
            // freeze the UNINITIALIZED w/h into the cooked file, silently — garbage dimensions in a file
            // that mtime then declares up to date for ever. The failure is not cached either, so fixing the
            // image and importing again works without restarting the editor.
            const char* reason = stbi_failure_reason();
            LOG_ERROR( "[TextureImporter] stbi_load failed for '{0}' ({1}); no cooked metadata was written "
                       "and the null handle is returned.",
                       abs, reason ? reason : "no reason reported" );
            return Common::AssetHandle::Null();
        }

        // Only the dimensions are needed here; the pixels are loaded again at draw time from SourcePath.
        stbi_image_free( pixels );

        // A source outside every content root has no project-relative name to store, so the key IS the
        // absolute spelling (StableKeyForPath's documented behaviour) and the cooked file is bound to this
        // machine. Say so once, at cook time, instead of letting the artist discover it on a colleague's
        // machine as an empty material slot. The predicate is AssetHandle's, not a loop over the root
        // table spelled here: TextureAsset::Load asks the same question for the opposite reason, and two
        // readings of one table is the drift this file's own history is made of.
        if ( !Common::AssetHandle::IsProjectRelativeKey( sourceKey ) )
        {
            LOG_WARN( "[TextureImporter] '{0}' lies outside every content root, so its cooked metadata "
                      "stores the absolute path and will not resolve on another machine.",
                      abs );
        }

        Assets::Serialization::TextureAssetData data;
        data.Handle     = handle;
        data.SourcePath = sourceKey;
        data.Width      = static_cast<uint32_t>( w );
        data.Height     = static_cast<uint32_t>( h );
        data.Channels   = 4;
        data.Format     = Desert::Core::Formats::ImageFormat::RGBA8F;

        // A HANDLE IS ONLY RETURNED FOR METADATA THAT IS ON THE DISK. The write used to be unchecked
        // (Д31-D), and the handle plus the cache entry went back regardless — so the very next lookup
        // was served out of memory, the missing `.tex` was not noticed until the next session, and
        // mtime then declared the absent file up to date. The null handle is this function's existing
        // "it did not happen" (see the stbi_load branch above) and it is the right answer here too.
        if ( const auto written = WriteCookedJson( data, meta ); !written )
        {
            LOG_ERROR( "[TextureImporter] '{0}' was decoded but its cooked metadata was not written: {1}. "
                       "The null handle is returned and nothing is cached, so importing again after fixing "
                       "the cause works without restarting the editor.",
                       abs, written.GetError() );
            return Common::AssetHandle::Null();
        }

        m_Cache[abs] = handle;

        return handle;
    }

} // namespace Desert::Editor