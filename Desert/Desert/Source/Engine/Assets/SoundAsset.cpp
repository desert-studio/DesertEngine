#include <Engine/Assets/SoundAsset.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

namespace Desert::Assets
{
    namespace
    {
        struct SoundDocument
        {
            Common::Content::TextAssetHeaderSerialized Header;
            std::string                                Source;
        };
    } // namespace

    SoundAsset::SoundAsset( const Common::Filepath& filepath ) : AssetBase( filepath, AssetTypeID::Sound )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::ResultStr<SoundAsset::Parsed> SoundAsset::Parse( std::string_view text )
    {
        const auto document = Common::Json::Read<SoundDocument>( text );
        if ( !document )
            return Common::MakeFormattedError<Parsed>( "not a .desound document: {}", document.GetError() );
        const SoundDocument& data = document.GetValue();
        if ( data.Header.Kind != kSoundKind )
            return Common::MakeFormattedError<Parsed>( "header states Kind '{}', a .desound states '{}'",
                                                       data.Header.Kind, kSoundKind );
        const auto guid = Common::Content::AssetGuidFromText( data.Header.Guid );
        if ( !guid )
            return Common::MakeFormattedError<Parsed>( "header GUID: {}", guid.GetError() );
        if ( guid.GetValue().IsNull() )
            return Common::MakeError<Parsed>( "header GUID is null; the null GUID names no sound" );
        if ( data.Source.empty() )
            return Common::MakeError<Parsed>( "Source is empty; a sound names the audio file it was imported from" );
        return Common::MakeSuccess( Parsed{ guid.GetValue(), data.Source } );
    }

    Common::ResultStr<std::string> SoundAsset::Write( const Common::Content::AssetGuid& guid, std::string_view source )
    {
        if ( guid.IsNull() )
            return Common::MakeError<std::string>( "a .desound needs a GUID; the null GUID names no sound" );
        if ( source.empty() )
            return Common::MakeError<std::string>( "a .desound names its source audio file; none was given" );
        SoundDocument data;
        data.Header = Common::Content::MakeTextHeader( Common::Content::ContentKind::Sound, guid, {} );
        data.Source = std::string( source );
        return Common::Content::CanonicalJsonTextOfWriterOutput( Common::Json::Write( data ) );
    }

    Common::ResultStr<std::filesystem::path> SoundAsset::ResolveSourceFile( const Common::Content::AssetGuid& guid )
    {
        using Result = std::filesystem::path;
        if ( guid.IsNull() )
            return Common::MakeError<Result>( "no sound: the null GUID names none" );
        const Common::Utils::AssetRegistry&      registry = ContentRegistry::Get();
        const Common::Utils::AssetRegistryEntry* row =
             registry.FindByHandle( Common::Content::HandleForGuid( guid ) );
        if ( row == nullptr )
            return Common::MakeFormattedError<Result>( "sound {} is in no registry row",
                                                       Common::Content::AssetGuidToText( guid ) );
        auto followed = registry.FollowRedirectors( *row );
        if ( !followed )
            return Common::MakeFormattedError<Result>( "sound {}: {}", Common::Content::AssetGuidToText( guid ),
                                                       followed.GetError() );
        const std::filesystem::path desound = Common::AssetHandle::PathForStableKey( followed.GetValue()->Key );
        const std::string           text    = ReadTextForIdentity( ContentRegistry::FileToOpen( desound ) );
        if ( text.empty() )
            return Common::MakeFormattedError<Result>( "sound '{}' is empty or could not be opened",
                                                       desound.string() );
        const auto parsed = Parse( text );
        if ( !parsed )
            return Common::MakeFormattedError<Result>( "sound '{}': {}", desound.string(), parsed.GetError() );
        return Common::MakeSuccess( desound.parent_path() / parsed.GetValue().Source );
    }

    Common::BoolResultStr SoundAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           text = ReadTextForIdentity( file );
        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "sound '{}' is empty or could not be opened", file.string() );
        }
        auto parsed = Parse( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "sound '{}': {}", file.string(), parsed.GetError() );
        }
        m_Guid       = parsed.GetValue().Guid;
        m_SourceFile = m_Metadata.Filepath.parent_path() / parsed.GetValue().Source;
        m_Ready      = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SoundAsset::Unload()
    {
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SoundAsset::Save( const Common::Filepath& filepath, const Common::Content::AssetGuid& guid,
                                            std::string_view source )
    {
        const auto text = Write( guid, source );
        if ( !text )
            return Common::MakeFormattedError<bool>( "sound '{}' was not written: {}", filepath.string(),
                                                     text.GetError() );
        const auto written = Common::Content::WriteCanonicalJsonFileAtomic( filepath, text.GetValue() );
        if ( !written )
            return Common::MakeFormattedError<bool>( "sound '{}' was not written: {}", filepath.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
