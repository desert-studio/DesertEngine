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
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <fstream>
#include <mutex>
#include <utility>

namespace Desert::Assets
{
    namespace CC = Common::Content;

    namespace
    {
        constexpr uint32_t kImportInfoVersion = 1;

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

        struct Reader
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
            return out;
        }

        Common::ResultStr<TextureImportInfo> DecodeImportInfo( std::span<const std::byte> bytes )
        {
            Reader            r{ bytes };
            TextureImportInfo info;
            const auto        version = static_cast<uint32_t>( r.Get( 4 ) );
            if ( r.Ok && version != kImportInfoVersion )
                return Common::MakeFormattedError<TextureImportInfo>(
                     "texture ImportInfo version {} is not the {} this build reads", version, kImportInfoVersion );
            info.SourceFile              = r.String();
            info.SourceHash              = r.Get( 8 );
            const std::string intentName = r.String();
            if ( !r.Ok || r.At != bytes.size() )
                return Common::MakeError<TextureImportInfo>(
                     "texture ImportInfo is truncated or has trailing bytes" );
            info.Settings.Intent = Core::Formats::TextureIntentFromName( intentName );
            if ( info.Settings.Intent == Core::Formats::TextureIntent::Count )
                return Common::MakeFormattedError<TextureImportInfo>(
                     "texture ImportInfo names intent '{}', which this build does not know", intentName );
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
            Reader     r{ bytes };
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

    std::vector<std::byte> SerializeTextureSettingsForKey( const TextureBuildSettings& settings )
    {
        // UE SerializeForKey: every field that shapes the output, in a fixed order, endian-stable.
        std::vector<std::byte> out;
        PutU32( out, static_cast<uint32_t>( settings.Import.Intent ) );
        PutU32( out, settings.EncoderVersion );
        return out;
    }

    uint64_t TextureDerivedDataKey( const uint64_t sourceHash, const TextureBuildSettings& settings,
                                    const Common::DDC::Deriver& deriver )
    {
        const std::vector<std::byte> image = SerializeTextureSettingsForKey( settings );
        return Common::DDC::MakeKey( deriver, sourceHash, image.data(), image.size() );
    }

    Common::ResultStr<TextureAssetKey> ReadTextureAssetKey( const std::filesystem::path& asset )
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
            key.DerivedDataKey = TextureDerivedDataKey(
                 key.SourceHash, TextureBuildSettings{ info.GetValue().Settings, kTextureBlockEncoderVersion } );
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
        key.DerivedDataKey = TextureDerivedDataKey(
             asset.Import.SourceHash, TextureBuildSettings{ asset.Import.Settings, kTextureBlockEncoderVersion } );

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
        if ( auto hit = Common::DDC::Get( kTextureDeriver, key.GetValue().DerivedDataKey ); hit.has_value() )
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
} // namespace Desert::Assets
