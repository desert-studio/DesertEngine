#pragma once
// THE TEXTURE ASSET (AF3) — a `.detex` is an AF1 envelope that CARRIES its source image.
//
// UE keeps a texture's source inside the .uasset (UTexture::Source, an FTextureSource whose bytes are
// FEditorBulkData — Engine/Classes/Engine/Texture.h:1268) and never re-reads the file it was imported
// from; the imported file's name and hash are provenance (FAssetImportInfo). The same here:
//
//   header   Kind = Texture (or Skybox for a sky panorama), Guid.Hi = the texture's 64-bit AssetHandle
//            — FROZEN at import, the number every `.demat` names; Guid.Lo is minted once and only copied
//   Meta     the asset's display name
//   IMPT     ImportInfo: the stable key of the file it was imported from, that file's content hash at
//            import, and the import settings (the authored intent)
//   SRCE     the imported file's bytes, verbatim (png, jpg, hdr, ...)
//
// Everything derived from it — mip chain, BC7/BC5/BC4 levels — is PLATFORM DATA and lives in the
// DerivedDataCache (AF5) under `TextureDerivedDataKey`, never beside the asset and never in git.
//
// WHY THE HANDLE IS IN THE HEADER AND NOT DERIVED. Before AF3 the handle was `FromCookedPath(<source
// image>)`, a function of a PATH; the migration froze that number into the asset, so the 116 `.demat`
// files keep resolving without being rewritten, and a later rename/move of the asset changes nothing.
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
        Common::Content::AssetGuid   Guid;                                         // Hi = AssetHandle
        std::string                  Name;
        TextureImportInfo            Import;
        std::vector<std::byte>       Source;

        Common::UUID Handle() const
        {
            return Common::UUID( Guid.Hi );
        }
        bool operator==( const TextureSourceAsset& ) const = default;
    };

    // A new asset around `sourceBytes`: the handle is given (the importer passes the path-derived number
    // the texture has always had), Guid.Lo is minted, SourceHash is computed.
    TextureSourceAsset MakeTextureSourceAsset( Common::Content::ContentKind kind, Common::UUID handle,
                                               std::string sourceKey, std::vector<std::byte> sourceBytes,
                                               TextureImportSettings settings );

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
} // namespace Desert::Assets
