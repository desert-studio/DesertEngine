#pragma once
// THE TEXTURE ASSET (AF3) — a `.detex` is an AF1 envelope that CARRIES its source image.
//
// UE keeps a texture's source inside the .uasset (UTexture::Source, an FTextureSource whose bytes are
// FEditorBulkData — Engine/Classes/Engine/Texture.h:1268) and never re-reads the file it was imported
// from; the imported file's name and hash are provenance (FAssetImportInfo). The same here:
//
//   header   Kind = Texture (or Skybox for a sky panorama), Guid = the texture's identity, minted once at
//            import and only copied; the runtime handle is HandleForGuid(Guid), the one fold (AssetEnvelope.hpp)
//   Meta     the asset's display name
//   IMPT     ImportInfo: the stable key of the file it was imported from, that file's content hash at
//            import, and the import settings (the authored intent)
//   SRCE     the imported file's bytes, verbatim (png, jpg, hdr, ...)
//
// Everything derived from it — mip chain, BC7/BC5/BC4 levels — is PLATFORM DATA and lives in the
// DerivedDataCache (AF5) under `TextureDerivedDataKey`, never beside the asset and never in git.
//
// THE COOKED FORM (AM0: IMPT and SRCE are the editor's and are cut at cook). A package carries the same
// header and Meta and, instead of IMPT + SRCE, one PAYL section holding exactly what the runtime reads:
// the DDC key, the source's content hash (the environment bake's signature) and its stable key.
//
// WHY THE IDENTITY IS IN THE HEADER AND NOT DERIVED. Before AF3 the handle was `FromCookedPath(<source
// image>)`, a function of a PATH, so a rename changed it. The header GUID survives any move; the handle is
// its fold, like every other asset's (SCNE 28 step 6). Assets imported before step 6 carry the old path
// number in Guid.Hi -- that is now just 64 random-looking bits of their GUID, not a handle.
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets
{
    inline constexpr const char* kTextureAssetExtension = ".detex";

    // The envelope's subsystem stamp for the IMPT layout below (UE custom version).
    inline constexpr uint32_t kTextureAssetSubsystemTag     = Common::Content::FourCC( "TXAS" );
    inline constexpr uint32_t kTextureAssetSubsystemVersion = 1;

    // Import settings: what, besides the source bytes, shapes the platform data. Serialized into IMPT and,
    // separately and in a fixed order, into the DDC key (SerializeTextureSettingsForKey).
    struct TextureImportSettings
    {
        Core::Formats::TextureIntent Intent = Core::Formats::TextureIntent::Unspecified;
        bool                         operator==( const TextureImportSettings& ) const = default;
    };

    struct TextureImportInfo
    {
        // `AssetHandle::StableKeyForPath` of the file the source was imported from (`assets:Textures/T.png`)
        // — provenance only: nothing opens it after import.
        std::string SourceFile;
        // Utils::PakContentHash of that file's bytes at import == the SRCE section's TOC hash. A reimport
        // whose file hashes the same is a no-op (freshness is content, never mtime).
        uint64_t              SourceHash = 0;
        TextureImportSettings Settings;
        bool                  operator==( const TextureImportInfo& ) const = default;
    };

    struct TextureSourceAsset
    {
        Common::Content::ContentKind Kind = Common::Content::ContentKind::Texture; // Texture or Skybox
        Common::Content::AssetGuid   Guid;
        std::string                  Name;
        TextureImportInfo            Import;
        std::vector<std::byte>       Source;

        Common::UUID Handle() const
        {
            return Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( Guid ) ) );
        }
        bool operator==( const TextureSourceAsset& ) const = default;
    };

    // A new asset around `sourceBytes`: a fresh GUID is minted (its fold is the handle), SourceHash is computed.
    TextureSourceAsset MakeTextureSourceAsset( Common::Content::ContentKind kind, std::string sourceKey,
                                               std::vector<std::byte> sourceBytes,
                                               TextureImportSettings  settings );

    Common::ResultStr<std::vector<std::byte>> EncodeTextureSourceAsset( const TextureSourceAsset& asset );
    Common::ResultStr<TextureSourceAsset>     DecodeTextureSourceAsset( std::span<const std::byte> file );
    Common::ResultStr<TextureSourceAsset>     ReadTextureSourceAssetFile( const std::filesystem::path& file );
    Common::BoolResultStr                     WriteTextureSourceAssetFile( const std::filesystem::path& file,
                                                                           const TextureSourceAsset&    asset );

    // True when the file starts with the envelope magic — a legacy `.detex` (JSON `{"Intent": ...}`) is not.
    bool IsTextureSourceAssetFile( const std::filesystem::path& file );

    // ── THE DERIVED DATA KEY ─────────────────────────────────────────────────────────────────────────
    // UE: TEXTURE_DERIVEDDATA_VER (TextureDerivedData.cpp:68). Change it whenever the platform data the
    // texture deriver produces for the same source and settings changes — nothing else invalidates.
    inline constexpr Common::DDC::Deriver kTextureDeriver{
         "Texture", ".tex", { 0x95BCE5A0BFB94953ULL, 0x9A18684748C633C9ULL } };

    // The settings image the key hashes (UE SerializeForKey): fixed order, little-endian, one u32 per field.
    struct TextureBuildSettings
    {
        TextureImportSettings Import;
        uint32_t              EncoderVersion = 0; // the block encoder's own version (TextureImporter)
    };
    std::vector<std::byte> SerializeTextureSettingsForKey( const TextureBuildSettings& settings );

    // Source payload id + settings + deriver GUID -> DDC key. The asset's path and handle are NOT inputs:
    // renaming or moving a `.detex` hits the same entry (UE GetTextureDerivedDataKeySuffix is keyed on
    // Source.GetIdString(), not the package name).
    uint64_t TextureDerivedDataKey( uint64_t sourceHash, const TextureBuildSettings& settings,
                                    const Common::DDC::Deriver& deriver = kTextureDeriver );

    // The block encoder's own version -- an input of every texture key, so it lives beside the deriver and
    // not in the editor that runs the encoder: the runtime computes the same key to find the entry.
    inline constexpr uint32_t kTextureBlockEncoderVersion = 1;

    // What the runtime needs from a `.detex` to reach its platform data (UE: the package summary + the
    // FTextureSource id, not the bulk data). Read from IMPT in an editor asset, from PAYL in a cooked one;
    // import SETTINGS are not here because the cooked form has none -- they are already in the key.
    struct TextureAssetKey
    {
        Common::Content::ContentKind Kind = Common::Content::ContentKind::Texture;
        Common::Content::AssetGuid   Guid;               // the header's; the handle is HandleForGuid(Guid)
        std::string                  SourceFile;         // TextureImportInfo::SourceFile
        uint64_t                     SourceHash     = 0; // TextureImportInfo::SourceHash
        uint64_t                     DerivedDataKey = 0;
    };
    // Reads a prefix of the file (header + TOC + IMPT or PAYL), never the source an editor asset carries.
    Common::ResultStr<TextureAssetKey> ReadTextureAssetKey( const std::filesystem::path& asset );

    // The cooked form of an editor `.detex` (see the top of this file): header and Meta kept, IMPT and
    // SRCE replaced by the PAYL key record. Refuses anything that is not an editor texture asset, so a
    // package cannot carry a half-cooked or foreign file under a texture's name.
    Common::ResultStr<std::vector<std::byte>> CookTextureAssetForRuntime( std::span<const std::byte> editorAsset );

    // -- ASSET -> DDC -> GPU ------------------------------------------------------------------------
    // UE FTexturePlatformData::Cache: the runtime asks the DDC for the key the asset describes. A miss is
    // built only where a builder exists -- the editor registers one (its importer decodes and encodes, then
    // Puts); a packaged game registers none, so a miss there is an error naming the asset, because the
    // package was supposed to carry the entry (the packager stages the Texture bucket).
    using TexturePlatformDataBuilder =
         std::function<Common::ResultStr<std::string>( const std::filesystem::path& asset )>;
    void SetTexturePlatformDataBuilder( TexturePlatformDataBuilder builder );

    // The `.tex` container bytes (TextureBinary) of the asset's platform data.
    Common::ResultStr<std::string> LoadTexturePlatformData( const std::filesystem::path& asset );
} // namespace Desert::Assets
