#include <Engine/Assets/LevelSequenceAsset.hpp>

#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>

namespace Desert::Assets
{
    namespace
    {
        // The header, and every other member of the TMLN document carried untouched: the one place this
        // file looks inside the block is the identity the block leaves empty.
        struct LevelSequenceEnvelope
        {
            Common::Content::TextAssetHeaderSerialized Header;
            Common::Json::CarriedKeys                  Body;
        };
    } // namespace

    LevelSequenceAsset::LevelSequenceAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::LevelSequence )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::ResultStr<std::string> LevelSequenceAsset::Write( const Animation::Timeline::Sequence& sequence,
                                                              const Common::Content::AssetGuid&    guid )
    {
        if ( sequence.Host != Animation::Timeline::SequenceHost::LevelSequence )
            return Common::MakeFormattedError<std::string>( "a .dseq holds a LevelSequence-host sequence, not {}",
                                                            Animation::Timeline::ToString( sequence.Host ) );
        if ( guid.IsNull() )
            return Common::MakeError<std::string>( "a .dseq needs a GUID; the null GUID names no asset" );

        auto written = Animation::Timeline::WriteSequence( sequence );
        if ( !written )
            return Common::MakeFormattedError<std::string>( "the sequence did not write: {}", written.GetError() );
        const std::vector<uint8_t> block    = written.ExtractValue();
        auto                       envelope = Common::Json::Read<LevelSequenceEnvelope>(
             std::string_view( reinterpret_cast<const char*>( block.data() ), block.size() ) );
        if ( !envelope )
            return Common::MakeFormattedError<std::string>( "the TMLN block did not re-read: {}",
                                                            envelope.GetError() );
        LevelSequenceEnvelope data = envelope.ExtractValue();
        data.Header.Kind           = std::string( Animation::Timeline::kLevelSequenceKind );
        data.Header.Guid           = Common::Content::AssetGuidToText( guid );
        return Common::Content::CanonicalJsonTextOfWriterOutput( Common::Json::Write( data ) );
    }

    Common::ResultStr<LevelSequenceAsset::Parsed> LevelSequenceAsset::Parse( std::string_view text )
    {
        const auto envelope = Common::Json::Read<LevelSequenceEnvelope>( text );
        if ( !envelope )
            return Common::MakeFormattedError<Parsed>( "not a .dseq document: {}", envelope.GetError() );
        const auto& header = envelope.GetValue().Header;
        if ( header.Kind != Animation::Timeline::kLevelSequenceKind )
            return Common::MakeFormattedError<Parsed>( "header states Kind '{}', a .dseq states '{}'", header.Kind,
                                                       Animation::Timeline::kLevelSequenceKind );
        const auto guid = Common::Content::AssetGuidFromText( header.Guid );
        if ( !guid )
            return Common::MakeFormattedError<Parsed>( "header GUID: {}", guid.GetError() );

        auto sequence = Animation::Timeline::ReadSequence(
             std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( text.data() ), text.size() ) );
        if ( !sequence )
            return Common::MakeFormattedError<Parsed>( "{}", sequence.GetError() );
        if ( sequence.GetValue().Host != Animation::Timeline::SequenceHost::LevelSequence )
            return Common::MakeFormattedError<Parsed>( "the block's host is {}, a .dseq holds LevelSequence",
                                                       Animation::Timeline::ToString( sequence.GetValue().Host ) );
        return Common::MakeSuccess( Parsed{ guid.GetValue(), sequence.ExtractValue() } );
    }

    Common::BoolResultStr LevelSequenceAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           text = ReadTextForIdentity( file );
        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "level sequence '{}' is empty or could not be opened",
                                                     file.string() );
        }
        auto parsed = Parse( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "level sequence '{}': {}", file.string(), parsed.GetError() );
        }
        Parsed value = parsed.ExtractValue();
        m_Guid       = value.Guid;
        m_Sequence   = std::move( value.Sequence );
        m_Ready      = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr LevelSequenceAsset::Unload()
    {
        m_Sequence = Animation::Timeline::Sequence{};
        m_Ready    = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr LevelSequenceAsset::Save( const Common::Filepath&              filepath,
                                                    const Animation::Timeline::Sequence& sequence,
                                                    const Common::Content::AssetGuid&    guid )
    {
        const auto text = Write( sequence, guid );
        if ( !text )
            return Common::MakeFormattedError<bool>( "level sequence '{}' was not written: {}", filepath.string(),
                                                     text.GetError() );
        const auto written = Common::Content::WriteCanonicalJsonFileAtomic( filepath, text.GetValue() );
        if ( !written )
            return Common::MakeFormattedError<bool>( "level sequence '{}' was not written: {}", filepath.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
