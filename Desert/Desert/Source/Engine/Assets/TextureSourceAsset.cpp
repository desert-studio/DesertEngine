// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/TextureDerivedData.cpp:419-546
// (GetTextureDerivedDataKeySuffix + SerializeForKey) and :552-571 (TEXTURE_DERIVEDDATA_VER,
// GetTextureDerivedDataKeyFromSuffix), adapted: the key is AF5's 64-bit Common::DDC::MakeKey over
// { deriver GUID, bucket, Source id, settings bytes } instead of a "TEXTURE_<ver>_<suffix>" string; the
// Source id is the SRCE section's PakContentHash (UE: FTextureSource::GetIdString); the settings image is
// our two build inputs (authored intent, block encoder version) in a fixed little-endian order where UE
// writes FTextureBuildSettings through a persistent FMemoryWriter; layers, composite textures, VT and the
// per-platform format version are not ported (we build one layer for one platform format family).
// The asset layout itself follows UE's order "source inside the asset -> platform data derived from it"
// (UTexture::Source -> FTexturePlatformData), adapted to AF1's envelope sections.
#include <cstring>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <fstream>
#include <mutex>
#include <span>
#include <bit>
#include <utility>

namespace Desert::Assets
{
    namespace CC = Common::Content;

    namespace
    {
        // 1 = without ColorSpace, 2 = without AlphaCoverageThresholds, 3 = without ColorDilationMask
        // (UpgradeTextureSourceAsset only)
        constexpr uint32_t kImportInfoVersion = 4;

        void PutU32( std::vector<std::byte>& out, const uint32_t v )
        {
            for ( int i = 0; i < 4; ++i )
                out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
        }
        void PutU64( std::vector<std::byte>& out, const uint64_t v )
        {
            for ( int i = 0; i < 8; ++i )
                out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
        }
        void PutString( std::vector<std::byte>& out, const std::string_view s )
        {
            PutU32( out, static_cast<uint32_t>( s.size() ) );
            for ( const char c : s )
                out.push_back( static_cast<std::byte>( c ) );
        }

        struct TextureSourceReader
        {
            std::span<const std::byte> Bytes;
            size_t                     At = 0;
            bool                       Ok = true;

            uint64_t Get( const int width )
            {
                if ( !Ok || Bytes.size() - At < static_cast<size_t>( width ) )
                {
                    Ok = false;
                    return 0;
                }
                uint64_t v = 0;
                for ( int i = 0; i < width; ++i )
                    v |= static_cast<uint64_t>( std::to_integer<uint8_t>( Bytes[At + i] ) ) << ( 8 * i );
                At += static_cast<size_t>( width );
                return v;
            }
            std::string String()
            {
                const auto n = static_cast<size_t>( Get( 4 ) );
                if ( !Ok || Bytes.size() - At < n )
                {
                    Ok = false;
                    return {};
                }
                std::string s( reinterpret_cast<const char*>( Bytes.data() + At ), n );
                At += n;
                return s;
            }
        };

        std::vector<std::byte> EncodeImportInfo( const TextureImportInfo& info )
        {
            // The intent travels by NAME, as the authored JSON spelled it: reordering the enum must not
            // silently re-intend every asset.
            std::vector<std::byte> out;
            PutU32( out, kImportInfoVersion );
            PutString( out, info.SourceFile );
            PutU64( out, info.SourceHash );
            PutString( out, Core::Formats::TextureIntentName( info.Settings.Intent ) );
            PutString( out, Core::Formats::TextureColorSpaceName( info.Settings.ColorSpace ) );
            for ( const float threshold : info.Settings.AlphaCoverageThresholds )
                PutU32( out, std::bit_cast<uint32_t>( threshold ) );
            PutU64( out, info.Settings.ColorDilationMask.Texture.Hi );
            PutU64( out, info.Settings.ColorDilationMask.Texture.Lo );
            PutU32( out, info.Settings.ColorDilationMask.Channel );
            PutU32( out, std::bit_cast<uint32_t>( info.Settings.ColorDilationMask.Threshold ) );
            return out;
        }

        // @p expected is kImportInfoVersion everywhere but the version-1 upgrade, which reads the layout
        // without ColorSpace and leaves the field for the caller to decide.
        Common::ResultStr<TextureImportInfo> DecodeImportInfo( std::span<const std::byte> bytes,
                                                               const uint32_t expected = kImportInfoVersion )
        {
            TextureSourceReader r{ bytes };
            TextureImportInfo info;
            const auto        version = static_cast<uint32_t>( r.Get( 4 ) );
            if ( r.Ok && version != expected )
                return Common::MakeFormattedError<TextureImportInfo>(
                     "texture ImportInfo version {} is not the {} this build reads", version, expected );
            info.SourceFile              = r.String();
            info.SourceHash              = r.Get( 8 );
            const std::string intentName = r.String();
            const std::string spaceName  = expected >= 2 ? r.String() : std::string( "sRGB" );
            if ( expected >= 3 )
            {
                for ( float& threshold : info.Settings.AlphaCoverageThresholds )
                    threshold = std::bit_cast<float>( static_cast<uint32_t>( r.Get( 4 ) ) );
                for ( const float threshold : info.Settings.AlphaCoverageThresholds )
                    if ( !( threshold >= 0.0f && threshold <= 1.0f ) )
                        return Common::MakeFormattedError<TextureImportInfo>(
                             "texture ImportInfo has alpha coverage threshold {}; a threshold is in [0, 1]",
                             threshold );
            }
            if ( expected >= 4 )
            {
                TextureDilationMask& mask = info.Settings.ColorDilationMask;
                mask.Texture.Hi           = r.Get( 8 );
                mask.Texture.Lo           = r.Get( 8 );
                mask.Channel              = static_cast<uint32_t>( r.Get( 4 ) );
                mask.Threshold            = std::bit_cast<float>( static_cast<uint32_t>( r.Get( 4 ) ) );
                if ( r.Ok && ( mask.Channel > 3 || !( mask.Threshold >= 0.0f && mask.Threshold <= 1.0f ) ) )
                    return Common::MakeFormattedError<TextureImportInfo>(
                         "texture ImportInfo has dilation mask channel {} threshold {}; a channel is 0..3 and a "
                         "threshold is in [0, 1]",
                         mask.Channel, mask.Threshold );
            }
            if ( !r.Ok || r.At != bytes.size() )
                return Common::MakeError<TextureImportInfo>(
                     "texture ImportInfo is truncated or has trailing bytes" );
            info.Settings.Intent = Core::Formats::TextureIntentFromName( intentName );
            if ( info.Settings.Intent == Core::Formats::TextureIntent::Count )
                return Common::MakeFormattedError<TextureImportInfo>(
                     "texture ImportInfo names intent '{}', which this build does not know", intentName );
            info.Settings.ColorSpace = Core::Formats::TextureColorSpaceFromName( spaceName );
            if ( info.Settings.ColorSpace == Core::Formats::TextureColorSpace::Count )
                return Common::MakeFormattedError<TextureImportInfo>(
                     "texture ImportInfo names colour space '{}'; this build knows 'Linear' and 'sRGB'",
                     spaceName );
            return Common::MakeSuccess( std::move( info ) );
        }

        // PAYL of a COOKED texture asset: the key record, nothing the editor rebuilds from.
        constexpr uint32_t kCookedKeyVersion = 1;

        std::vector<std::byte> EncodeCookedKey( const TextureAssetKey& key )
        {
            std::vector<std::byte> out;
            PutU32( out, kCookedKeyVersion );
            PutU64( out, key.DerivedDataKey );
            PutU64( out, key.SourceHash );
            PutString( out, key.SourceFile );
            return out;
        }

        Common::BoolResultStr DecodeCookedKey( std::span<const std::byte> bytes, TextureAssetKey& key )
        {
            TextureSourceReader r{ bytes };
            const auto version = static_cast<uint32_t>( r.Get( 4 ) );
            if ( r.Ok && version != kCookedKeyVersion )
                return Common::MakeFormattedError<bool>(
                     "cooked texture key version {} is not the {} this build reads", version, kCookedKeyVersion );
            key.DerivedDataKey = r.Get( 8 );
            key.SourceHash     = r.Get( 8 );
            key.SourceFile     = r.String();
            if ( !r.Ok || r.At != bytes.size() )
                return Common::MakeError<bool>( "cooked texture key is truncated or has trailing bytes" );
            return Common::MakeSuccess( true );
        }

        const CC::SubsystemVersion kKnown[] = { { kTextureAssetSubsystemTag, kTextureAssetSubsystemVersion } };
    } // namespace

    TextureSourceAsset MakeTextureSourceAsset( const CC::ContentKind kind, std::string sourceKey,
                                               std::vector<std::byte>      sourceBytes,
                                               const TextureImportSettings settings )
    {
        TextureSourceAsset asset;
        asset.Kind              = kind;
        asset.Guid              = CC::AssetGuid::Generate();
        asset.Name              = std::filesystem::path( sourceKey ).stem().string();
        asset.Import.SourceFile = std::move( sourceKey );
        asset.Import.SourceHash = Common::Utils::PakContentHash( sourceBytes.data(), sourceBytes.size() );
        asset.Import.Settings   = settings;
        asset.Source            = std::move( sourceBytes );
        return asset;
    }

    Common::ResultStr<std::vector<std::byte>> EncodeTextureSourceAsset( const TextureSourceAsset& asset )
    {
        if ( asset.Kind != CC::ContentKind::Texture && asset.Kind != CC::ContentKind::Skybox )
            return Common::MakeError<std::vector<std::byte>>( "a texture asset is kind Texture or Skybox" );
        if ( asset.Import.SourceHash != Common::Utils::PakContentHash( asset.Source.data(), asset.Source.size() ) )
            return Common::MakeError<std::vector<std::byte>>(
                 "a texture asset's ImportInfo hash must be the hash of the source it carries" );
        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = asset.Kind;
        envelope.Asset.Guid       = asset.Guid;
        envelope.Asset.Subsystems = { kKnown[0] };
        CC::EnvelopeMeta meta;
        meta.Name = asset.Name;
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Meta, CC::EnvelopeCodec::Stored, CC::EncodeEnvelopeMeta( meta ) } );
        envelope.Sections.push_back(
             { CC::EnvelopeSection::ImportInfo, CC::EnvelopeCodec::Stored, EncodeImportInfo( asset.Import ) } );
        envelope.Sections.push_back( { CC::EnvelopeSection::Source, CC::EnvelopeCodec::Stored, asset.Source } );
        return CC::WriteAssetEnvelope( envelope );
    }

    Common::ResultStr<TextureSourceAsset> DecodeTextureSourceAsset( std::span<const std::byte> file )
    {
        auto envelope = CC::ReadAssetEnvelope( file, CC::AssetHeaderReadContext{ kKnown } );
        if ( !envelope.IsSuccess() )
            return Common::MakeError<TextureSourceAsset>( envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        TextureSourceAsset       asset;
        asset.Kind = e.Asset.Kind;
        asset.Guid = e.Asset.Guid;
        if ( asset.Kind != CC::ContentKind::Texture && asset.Kind != CC::ContentKind::Skybox )
            return Common::MakeFormattedError<TextureSourceAsset>( "the envelope is kind '{}', not a texture",
                                                                   CC::KindName( asset.Kind ) );
        bool haveMeta = false, haveImport = false, haveSource = false;
        for ( const CC::EnvelopeSectionData& s : e.Sections )
        {
            if ( s.Tag == CC::EnvelopeSection::Meta )
            {
                auto meta = CC::DecodeEnvelopeMeta( s.Bytes );
                if ( !meta.IsSuccess() )
                    return Common::MakeError<TextureSourceAsset>( meta.GetError() );
                asset.Name = meta.GetValue().Name;
                haveMeta   = true;
            }
            else if ( s.Tag == CC::EnvelopeSection::ImportInfo )
            {
                auto info = DecodeImportInfo( s.Bytes );
                if ( !info.IsSuccess() )
                    return Common::MakeError<TextureSourceAsset>( info.GetError() );
                asset.Import = info.GetValue();
                haveImport   = true;
            }
            else if ( s.Tag == CC::EnvelopeSection::Source )
            {
                asset.Source = s.Bytes;
                haveSource   = true;
            }
        }
        if ( !haveMeta || !haveImport || !haveSource )
            return Common::MakeError<TextureSourceAsset>(
                 "a texture asset needs Meta, ImportInfo and Source sections; one is missing" );
        if ( asset.Import.SourceHash != Common::Utils::PakContentHash( asset.Source.data(), asset.Source.size() ) )
            return Common::MakeError<TextureSourceAsset>(
                 "the texture asset's ImportInfo hash does not describe the source it carries" );
        return Common::MakeSuccess( std::move( asset ) );
    }

    Common::ResultStr<TextureSourceAsset> ReadTextureSourceAssetFile( const std::filesystem::path& file )
    {
        const auto bytes = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !bytes.IsSuccess() )
            return Common::MakeError<TextureSourceAsset>( bytes.GetError() );
        const std::string& s      = bytes.GetValue();
        auto               result = DecodeTextureSourceAsset(
             std::span<const std::byte>( reinterpret_cast<const std::byte*>( s.data() ), s.size() ) );
        if ( !result.IsSuccess() )
            return Common::MakeFormattedError<TextureSourceAsset>( "'{}': {}", file.string(), result.GetError() );
        return result;
    }

    Common::BoolResultStr WriteTextureSourceAssetFile( const std::filesystem::path& file,
                                                       const TextureSourceAsset&    asset )
    {
        const auto bytes = EncodeTextureSourceAsset( asset );
        if ( !bytes.IsSuccess() )
            return Common::MakeError<bool>( bytes.GetError() );
        // Atomic: an asset is the whole file or the previous one, never half of the new one.
        return Common::Utils::FileSystem::WriteBytesToFileAtomic(
             file, std::span<const std::byte>( reinterpret_cast<const std::byte*>( bytes.GetValue().data() ),
                                               bytes.GetValue().size() ) );
    }

    Common::ResultStr<TextureSourceWrite> WriteTextureSource( const std::filesystem::path& asset,
                                                              const CC::ContentKind kind, std::string sourceKey,
                                                              std::vector<std::byte>       sourceBytes,
                                                              const TextureImportSettings& settingsIfCreated )
    {
        if ( IsTextureSourceAssetFile( asset ) )
        {
            auto existing = ReadTextureSourceAssetFile( asset );
            if ( !existing.IsSuccess() )
                return Common::MakeError<TextureSourceWrite>( existing.GetError() );
            const uint64_t hash = Common::Utils::PakContentHash( sourceBytes.data(), sourceBytes.size() );
            if ( existing.GetValue().Import.SourceHash == hash )
                return Common::MakeSuccess( TextureSourceWrite::Unchanged );
            TextureSourceAsset updated = existing.ExtractValue();
            updated.Import.SourceFile  = std::move( sourceKey );
            updated.Import.SourceHash  = hash;
            updated.Source             = std::move( sourceBytes );
            if ( auto w = WriteTextureSourceAssetFile( asset, updated ); !w.IsSuccess() )
                return Common::MakeError<TextureSourceWrite>( w.GetError() );
            return Common::MakeSuccess( TextureSourceWrite::Updated );
        }
        const TextureSourceAsset created =
             MakeTextureSourceAsset( kind, std::move( sourceKey ), std::move( sourceBytes ), settingsIfCreated );
        if ( auto w = WriteTextureSourceAssetFile( asset, created ); !w.IsSuccess() )
            return Common::MakeError<TextureSourceWrite>( w.GetError() );
        return Common::MakeSuccess( TextureSourceWrite::Created );
    }

    Common::ResultStr<std::vector<std::byte>> ReadTextureSourceImage( const std::filesystem::path& file )
    {
        if ( IsTextureSourceAssetFile( file ) )
        {
            auto asset = ReadTextureSourceAssetFile( file );
            if ( !asset.IsSuccess() )
                return Common::MakeError<std::vector<std::byte>>( asset.GetError() );
            return Common::MakeSuccess( std::move( asset.ExtractValue().Source ) );
        }
        const auto raw = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw.IsSuccess() )
            return Common::MakeError<std::vector<std::byte>>( raw.GetError() );
        const auto bytes = std::as_bytes( std::span( raw.GetValue() ) );
        return Common::MakeSuccess( std::vector<std::byte>( bytes.begin(), bytes.end() ) );
    }

    bool IsTextureSourceAssetFile( const std::filesystem::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        char          magic[4] = {};
        if ( !in.read( magic, 4 ) )
            return false;
        if ( !( magic[0] == 'D' && magic[1] == 'A' && magic[2] == 'S' && magic[3] == 'T' ) )
            return false;
        // The magic is the ENVELOPE's, shared by every binary asset (a `.dclayout` since container 2), so
        // the kind decides. Recorded, not judged: a texture whose TXAS version this build does not read is
        // still a texture, and ReadTextureSourceAssetFile refuses it by name.
        in.clear();
        in.seekg( 0, std::ios::beg );
        const auto header = CC::ReadEnvelopeHeader( in, CC::AssetHeaderReadContext{ {}, true } );
        return header.IsSuccess() && ( header.GetValue().Asset.Kind == CC::ContentKind::Texture ||
                                       header.GetValue().Asset.Kind == CC::ContentKind::Skybox );
    }

    bool IsLegacyTextureIntentSidecar( const std::span<const std::byte> file )
    {
        for ( const std::byte b : file )
        {
            const char c = static_cast<char>( b );
            if ( c == ' ' || c == '\t' || c == '\r' || c == '\n' )
                continue;
            return c == '{';
        }
        return false;
    }

    std::vector<std::byte> SerializeTextureSettingsForKey( const TextureBuildSettings& settings )
    {
        // UE SerializeForKey: every field that shapes the output, in a fixed order, endian-stable.
        std::vector<std::byte> out;
        PutU32( out, static_cast<uint32_t>( settings.Import.Intent ) );
        PutU32( out, settings.EncoderVersion );
        PutU32( out, static_cast<uint32_t>( settings.Import.ColorSpace ) );
        for ( const float threshold : settings.Import.AlphaCoverageThresholds )
            PutU32( out, std::bit_cast<uint32_t>( threshold ) );
        // The mask is another asset, keyed by identity (UE keys CompositeTexture by its source id). Appended only
        // when set, as UE appends its optional settings: a texture without one keeps the key (and DDC entry) it had.
        if ( const TextureDilationMask& mask = settings.Import.ColorDilationMask; mask.Enabled() )
        {
            PutU32( out, Common::Content::FourCC( "DILM" ) );
            PutU64( out, mask.Texture.Hi );
            PutU64( out, mask.Texture.Lo );
            PutU32( out, mask.Channel );
            PutU32( out, std::bit_cast<uint32_t>( mask.Threshold ) );
            // The block gate grades a masked colour only where the mask shows it (GRASS-ASSET p7); a revision of
            // that rule re-derives masked textures alone, the way EncoderVersion re-derives all of them.
            PutU32( out, kTextureMaskedGateVersion );
        }
        if ( settings.Bc7Effort != 1.0f )
        {
            PutU32( out, Common::Content::FourCC( "ENCE" ) );
            PutU32( out, std::bit_cast<uint32_t>( settings.Bc7Effort ) );
        }
        return out;
    }

    namespace
    {
        std::mutex            s_EncodingMutex;
        TextureEncodingSource s_EncodingSource;

        Project::TextureEncodingSettings CurrentTextureEncoding()
        {
            TextureEncodingSource source;
            {
                const std::lock_guard<std::mutex> lock( s_EncodingMutex );
                source = s_EncodingSource;
            }
            return source ? source() : Project::TextureEncodingSettings{};
        }

        // The keys of one plan for one editor asset's import record.
        void KeyEditorAsset( TextureAssetKey& key, const TextureImportSettings& settings, const TextureEncodeRole role )
        {
            const TextureEncodePlan plan = PlanTextureEncode( role );
            key.DerivedDataKey = TextureDerivedDataKey(
                 key.SourceHash, TextureBuildSettings{ settings, kTextureBlockEncoderVersion, plan.BuildEffort } );
            key.FetchFirstKey =
                 plan.FetchFirstEffort && *plan.FetchFirstEffort != plan.BuildEffort
                      ? TextureDerivedDataKey( key.SourceHash, TextureBuildSettings{ settings, kTextureBlockEncoderVersion,
                                                                                     *plan.FetchFirstEffort } )
                      : 0;
        }
    } // namespace

    void SetTextureEncodingSource( TextureEncodingSource source )
    {
        const std::lock_guard<std::mutex> lock( s_EncodingMutex );
        s_EncodingSource = std::move( source );
    }

    TextureEncodePlan PlanTextureEncode( const TextureEncodeRole role )
    {
        const Project::TextureEncodingSettings settings = CurrentTextureEncoding();
        const Project::TextureEncodeSpeed      speed =
             role == TextureEncodeRole::Cook ? settings.CookUses : settings.EditorUses;
        switch ( speed )
        {
            case Project::TextureEncodeSpeed::Final:
                return { settings.FinalEffort, std::nullopt };
            case Project::TextureEncodeSpeed::FinalIfAvailable:
                return { settings.FastEffort, settings.FinalEffort };
            case Project::TextureEncodeSpeed::Fast:
                break;
        }
        return { settings.FastEffort, std::nullopt };
    }

    std::optional<std::string> FetchTexturePlatformData( const TextureAssetKey& key )
    {
        if ( key.FetchFirstKey != 0 )
            if ( auto hit = Common::DDC::Get( kTextureDeriver, key.FetchFirstKey ); hit.has_value() )
                return hit;
        return Common::DDC::Get( kTextureDeriver, key.DerivedDataKey );
    }

    uint64_t TextureDerivedDataKey( const uint64_t sourceHash, const TextureBuildSettings& settings,
                                    const Common::DDC::Deriver& deriver )
    {
        const std::vector<std::byte> image = SerializeTextureSettingsForKey( settings );
        return Common::DDC::MakeKey( deriver, sourceHash, image.data(), image.size() );
    }

    Common::ResultStr<TextureAssetKey> ReadTextureAssetKey( const std::filesystem::path& asset,
                                                            const TextureEncodeRole      role )
    {
        // THE PREFIX, NOT THE FILE: an editor `.detex` is mostly its SRCE section (megabytes of png), and the
        // key needs the header, the TOC and the ~100-byte IMPT that precedes the source (or, cooked, PAYL).
        constexpr std::size_t kPrefixBytes = 4096;
        auto                  prefix = Common::Utils::FileSystem::ReadFileContentPrefix( asset, kPrefixBytes );
        if ( !prefix.IsSuccess() )
            return Common::MakeError<TextureAssetKey>( prefix.GetError() );
        const auto asBytes = []( const std::string& s )
        { return std::span<const std::byte>( reinterpret_cast<const std::byte*>( s.data() ), s.size() ); };
        auto header = CC::ReadEnvelopeHeader( asBytes( prefix.GetValue() ), CC::AssetHeaderReadContext{ kKnown } );
        if ( !header.IsSuccess() )
            return Common::MakeFormattedError<TextureAssetKey>( "'{}': {}", asset.string(), header.GetError() );
        const CC::EnvelopeHeader& h = header.GetValue();
        if ( h.Asset.Kind != CC::ContentKind::Texture && h.Asset.Kind != CC::ContentKind::Skybox )
            return Common::MakeFormattedError<TextureAssetKey>( "'{}' is kind '{}', not a texture", asset.string(),
                                                                CC::KindName( h.Asset.Kind ) );
        const auto impt   = h.Find( CC::EnvelopeSection::ImportInfo );
        const auto cooked = h.Find( CC::EnvelopeSection::Payload );
        const auto keyed  = impt ? impt : cooked;
        if ( !keyed || keyed->Codec != CC::EnvelopeCodec::Stored )
            return Common::MakeFormattedError<TextureAssetKey>(
                 "'{}' has neither a stored ImportInfo nor a cooked key section, so its platform data has no key",
                 asset.string() );
        const uint64_t end = keyed->Offset + keyed->Size;
        if ( end > prefix.GetValue().size() )
        {
            prefix = Common::Utils::FileSystem::ReadFileContentPrefix( asset, static_cast<std::size_t>( end ) );
            if ( !prefix.IsSuccess() )
                return Common::MakeError<TextureAssetKey>( prefix.GetError() );
            if ( end > prefix.GetValue().size() )
                return Common::MakeFormattedError<TextureAssetKey>(
                     "'{}' is truncated: its key section ends at byte {} of {}", asset.string(), end,
                     prefix.GetValue().size() );
        }
        const auto bytes =
             asBytes( prefix.GetValue() )
                  .subspan( static_cast<std::size_t>( keyed->Offset ), static_cast<std::size_t>( keyed->Size ) );
        if ( Common::Utils::PakContentHash( bytes.data(), bytes.size() ) != keyed->Hash )
            return Common::MakeFormattedError<TextureAssetKey>( "'{}': the key section fails its hash",
                                                                asset.string() );

        TextureAssetKey key;
        key.Kind   = h.Asset.Kind;
        key.Guid   = h.Asset.Guid;
        if ( impt )
        {
            auto info = DecodeImportInfo( bytes );
            if ( !info.IsSuccess() )
                return Common::MakeFormattedError<TextureAssetKey>( "'{}': {}", asset.string(), info.GetError() );
            key.SourceFile     = info.GetValue().SourceFile;
            key.SourceHash     = info.GetValue().SourceHash;
            KeyEditorAsset( key, info.GetValue().Settings, role );
        }
        else if ( auto decoded = DecodeCookedKey( bytes, key ); !decoded.IsSuccess() )
        {
            return Common::MakeFormattedError<TextureAssetKey>( "'{}': {}", asset.string(), decoded.GetError() );
        }
        return Common::MakeSuccess( std::move( key ) );
    }

    Common::ResultStr<std::vector<std::byte>> CookTextureAssetForRuntime( std::span<const std::byte> editorAsset )
    {
        auto source = DecodeTextureSourceAsset( editorAsset );
        if ( !source.IsSuccess() )
            return Common::MakeError<std::vector<std::byte>>( source.GetError() );
        const TextureSourceAsset& asset = source.GetValue();

        TextureAssetKey key;
        key.SourceFile     = asset.Import.SourceFile;
        key.SourceHash     = asset.Import.SourceHash;
        // The package names ONE key: the cook role's final entry when the cook put one there (FinalIfAvailable),
        // else the entry its plan builds -- the one the packager's TextureImporter (Cook role) just stored.
        KeyEditorAsset( key, asset.Import.Settings, TextureEncodeRole::Cook );
        if ( key.FetchFirstKey != 0 && Common::DDC::Get( kTextureDeriver, key.FetchFirstKey ).has_value() )
            key.DerivedDataKey = key.FetchFirstKey;
        key.FetchFirstKey = 0;

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = asset.Kind;
        envelope.Asset.Guid       = asset.Guid;
        envelope.Asset.Subsystems = { kKnown[0] };
        CC::EnvelopeMeta meta;
        meta.Name = asset.Name;
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Meta, CC::EnvelopeCodec::Stored, CC::EncodeEnvelopeMeta( meta ) } );
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored, EncodeCookedKey( key ) } );
        return CC::WriteAssetEnvelope( envelope );
    }

    namespace
    {
        std::mutex                 s_BuilderMutex;
        TexturePlatformDataBuilder s_Builder;
    } // namespace

    void SetTexturePlatformDataBuilder( TexturePlatformDataBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_BuilderMutex );
        s_Builder = std::move( builder );
    }

    Common::ResultStr<std::string> LoadTexturePlatformData( const std::filesystem::path& asset )
    {
        const auto key = ReadTextureAssetKey( asset );
        if ( !key.IsSuccess() )
            return Common::MakeError<std::string>( key.GetError() );
        if ( auto hit = FetchTexturePlatformData( key.GetValue() ); hit.has_value() )
            return Common::MakeSuccess( std::move( *hit ) );

        TexturePlatformDataBuilder builder;
        {
            const std::lock_guard<std::mutex> lock( s_BuilderMutex );
            builder = s_Builder;
        }
        if ( !builder )
            return Common::MakeFormattedError<std::string>(
                 "texture asset '{}' has no platform data under DDC key {:016x} ({}), and this build cannot "
                 "derive it: the package was cooked without it",
                 asset.string(), key.GetValue().DerivedDataKey,
                 Common::DDC::RelativePath( kTextureDeriver, key.GetValue().DerivedDataKey ).generic_string() );
        return builder( asset );
    }
    Core::Formats::TextureColorSpace DefaultTextureColorSpace( const Core::Formats::TextureIntent intent,
                                                               std::span<const std::byte>         sourceBytes )
    {
        const auto startsWith = [&]( const std::string_view magic )
        {
            return sourceBytes.size() >= magic.size() &&
                   std::memcmp( sourceBytes.data(), magic.data(), magic.size() ) == 0;
        };
        // OpenEXR's magic is 0x76 0x2f 0x31 0x01; its values are scene-linear floats (UE forces SRGB off for
        // them).
        if ( startsWith( "#?RADIANCE" ) || startsWith( "#?RGBE" ) ||
             startsWith( std::string_view( "v/1\x01", 4 ) ) )
            return Core::Formats::TextureColorSpace::Linear;
        return intent == Core::Formats::TextureIntent::Colour ||
                         intent == Core::Formats::TextureIntent::Unspecified
                    ? Core::Formats::TextureColorSpace::SRGB
                    : Core::Formats::TextureColorSpace::Linear;
    }

    Common::ResultStr<std::optional<std::vector<std::byte>>>
    UpgradeTextureSourceAsset( std::span<const std::byte> file )
    {
        using Out = std::optional<std::vector<std::byte>>;
        if ( DecodeTextureSourceAsset( file ).IsSuccess() )
            return Common::MakeSuccess( Out{} );
        if ( IsLegacyTextureIntentSidecar( file ) )
            return Common::MakeError<Out>( std::string( kLegacyTextureIntentSidecarRefusal ) );
        // Version 3 (no ColorDilationMask) first, then 2 (no AlphaCoverageThresholds either), then 1 (no ColorSpace).
        uint32_t                   from     = 3;
        const CC::SubsystemVersion v3[]     = { { kTextureAssetSubsystemTag, 3 } };
        const CC::SubsystemVersion v2[]     = { { kTextureAssetSubsystemTag, 2 } };
        const CC::SubsystemVersion v1[]     = { { kTextureAssetSubsystemTag, 1 } };
        auto                       envelope = CC::ReadAssetEnvelope( file, CC::AssetHeaderReadContext{ v3 } );
        if ( !envelope.IsSuccess() )
        {
            from     = 2;
            envelope = CC::ReadAssetEnvelope( file, CC::AssetHeaderReadContext{ v2 } );
        }
        if ( !envelope.IsSuccess() )
        {
            from     = 1;
            envelope = CC::ReadAssetEnvelope( file, CC::AssetHeaderReadContext{ v1 } );
        }
        if ( !envelope.IsSuccess() )
            return Common::MakeError<Out>( envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( e.Asset.Kind != CC::ContentKind::Texture && e.Asset.Kind != CC::ContentKind::Skybox )
            return Common::MakeFormattedError<Out>( "the envelope is kind '{}', not a texture",
                                                    CC::KindName( e.Asset.Kind ) );
        TextureSourceAsset asset;
        asset.Kind      = e.Asset.Kind;
        asset.Guid      = e.Asset.Guid;
        bool     haveImport = false, haveSource = false;
        uint32_t importVersion = 0;
        for ( const CC::EnvelopeSectionData& s : e.Sections )
        {
            if ( s.Tag == CC::EnvelopeSection::Meta )
            {
                auto meta = CC::DecodeEnvelopeMeta( s.Bytes );
                if ( !meta.IsSuccess() )
                    return Common::MakeError<Out>( meta.GetError() );
                asset.Name = meta.GetValue().Name;
            }
            else if ( s.Tag == CC::EnvelopeSection::ImportInfo )
            {
                // The IMPT section states its own layout version; an envelope raised earlier may carry an
                // older IMPT (WP_Protocol_HLOD: envelope 3, IMPT 2), so the section is read at the version it states.
                const uint32_t stated = TextureSourceReader{ s.Bytes }.Get( 4 );
                if ( stated == 0 || stated > kImportInfoVersion )
                    return Common::MakeFormattedError<Out>( "texture ImportInfo version {} is not one this build "
                                                            "can raise (1..{})",
                                                            stated, kImportInfoVersion );
                auto info = DecodeImportInfo( s.Bytes, stated );
                if ( !info.IsSuccess() )
                    return Common::MakeError<Out>( info.GetError() );
                asset.Import  = info.GetValue();
                haveImport    = true;
                importVersion = stated;
            }
            else if ( s.Tag == CC::EnvelopeSection::Source )
            {
                asset.Source = s.Bytes;
                haveSource   = true;
            }
        }
        if ( !haveImport || !haveSource )
            return Common::MakeFormattedError<Out>(
                 "a version-{} texture asset without IMPT or SRCE cannot be raised", from );
        if ( importVersion == 1 )
            asset.Import.Settings.ColorSpace = DefaultTextureColorSpace( asset.Import.Settings.Intent, asset.Source );
        auto encoded                     = EncodeTextureSourceAsset( asset );
        if ( !encoded.IsSuccess() )
            return Common::MakeError<Out>( encoded.GetError() );
        return Common::MakeSuccess( Out{ encoded.ExtractValue() } );
    }

    Common::ResultStr<bool> SetTextureColorSpace( const std::filesystem::path&           path,
                                                  const Core::Formats::TextureColorSpace space )
    {
        auto read = ReadTextureSourceAssetFile( path );
        if ( !read.IsSuccess() )
            return Common::MakeError<bool>( read.GetError() );
        TextureSourceAsset asset = read.ExtractValue();
        if ( asset.Import.Settings.ColorSpace == space )
            return Common::MakeSuccess( false );
        asset.Import.Settings.ColorSpace = space;
        if ( const auto written = WriteTextureSourceAssetFile( path, asset ); !written.IsSuccess() )
            return Common::MakeError<bool>( written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool> SetTextureAlphaCoverage( const std::filesystem::path& path,
                                                     const std::array<float, 4>&  thresholds )
    {
        for ( const float threshold : thresholds )
            if ( !( threshold >= 0.0f && threshold <= 1.0f ) )
                return Common::MakeFormattedError<bool>(
                     "'{}': alpha coverage threshold {} is outside [0, 1]", path.generic_string(), threshold );
        auto read = ReadTextureSourceAssetFile( path );
        if ( !read.IsSuccess() )
            return Common::MakeError<bool>( read.GetError() );
        TextureSourceAsset asset = read.ExtractValue();
        if ( asset.Import.Settings.AlphaCoverageThresholds == thresholds )
            return Common::MakeSuccess( false );
        asset.Import.Settings.AlphaCoverageThresholds = thresholds;
        if ( const auto written = WriteTextureSourceAssetFile( path, asset ); !written.IsSuccess() )
            return Common::MakeError<bool>( written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool> SetTextureColorDilationMask( const std::filesystem::path& path,
                                                         const TextureDilationMask&   mask )
    {
        if ( mask.Channel > 3 || !( mask.Threshold >= 0.0f && mask.Threshold <= 1.0f ) )
            return Common::MakeFormattedError<bool>( "'{}': dilation mask channel {} threshold {} (channel 0..3, "
                                                     "threshold in [0, 1])",
                                                     path.generic_string(), mask.Channel, mask.Threshold );
        auto read = ReadTextureSourceAssetFile( path );
        if ( !read.IsSuccess() )
            return Common::MakeError<bool>( read.GetError() );
        TextureSourceAsset asset = read.ExtractValue();
        if ( asset.Guid == mask.Texture )
            return Common::MakeFormattedError<bool>( "'{}': a texture cannot be its own dilation mask",
                                                     path.generic_string() );
        if ( asset.Import.Settings.ColorDilationMask == mask )
            return Common::MakeSuccess( false );
        asset.Import.Settings.ColorDilationMask = mask;
        if ( const auto written = WriteTextureSourceAssetFile( path, asset ); !written.IsSuccess() )
            return Common::MakeError<bool>( written.GetError() );
        return Common::MakeSuccess( true );
    }

} // namespace Desert::Assets
