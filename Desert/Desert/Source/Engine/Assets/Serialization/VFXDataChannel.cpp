#include <Engine/Assets/Serialization/VFXDataChannel.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace Desert::Assets::Serialization
{
    namespace
    {
        bool IsIdentifier( const std::string& name )
        {
            if ( name.empty() )
                return false;
            for ( std::size_t i = 0; i < name.size(); ++i )
            {
                const char c     = name[i];
                const bool alpha = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c == '_';
                const bool digit = c >= '0' && c <= '9';
                if ( !alpha && !( digit && i > 0 ) )
                    return false;
            }
            return true;
        }
    } // namespace

    uint32_t FloatCount( const VFXDataChannelFieldType type )
    {
        switch ( type )
        {
            case VFXDataChannelFieldType::Position:
            case VFXDataChannelFieldType::Direction:
                return 3;
            case VFXDataChannelFieldType::Color:
                return 4;
            case VFXDataChannelFieldType::Float:
            case VFXDataChannelFieldType::Int:
                return 1;
        }
        return 0;
    }

    Common::BoolResultStr ValidateVFXDataChannelData( const VFXDataChannelData& data )
    {
        if ( data.Fields.empty() )
            return Common::MakeFormattedError<bool>( "a VFX data channel carries no field" );
        if ( data.Fields.size() > kVFXDataChannelMaxFields )
            return Common::MakeFormattedError<bool>( "a VFX data channel carries {} fields, at most {}",
                                                     data.Fields.size(), kVFXDataChannelMaxFields );
        std::set<std::string> names;
        for ( std::size_t i = 0; i < data.Fields.size(); ++i )
        {
            const VFXDataChannelField& f = data.Fields[i];
            if ( !IsIdentifier( f.Name ) )
                return Common::MakeFormattedError<bool>( "field {} name '{}' is not an identifier", i, f.Name );
            if ( !names.insert( f.Name ).second )
                return Common::MakeFormattedError<bool>( "field {} name '{}' is repeated", i, f.Name );
            if ( FloatCount( f.Type ) == 0 )
                return Common::MakeFormattedError<bool>( "field {} '{}' has an unknown type", i, f.Name );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<VFXDataChannelData> ParseVFXDataChannel( const std::string& text )
    {
        using Result = VFXDataChannelData;
        if ( text.empty() )
            return Common::MakeFormattedError<Result>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kVFXDataChannelVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<Result>( "VFX data channel {}", headed.GetError() );

        const auto parsed = Common::Json::Read<VFXDataChannelData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<Result>( "{}", parsed.GetError() );
        VFXDataChannelData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::VFXDataChannel,
                                                      Assets::kVFXDataChannelSchemaTag, kVFXDataChannelVersion,
                                                      VFXDataChannelTextSubsystems() );
             !header )
            return Common::MakeFormattedError<Result>( "VFX data channel {}", header.GetError() );

        if ( auto valid = ValidateVFXDataChannelData( data ); !valid )
            return Common::MakeFormattedError<Result>( "{}", valid.GetError() );

        // A channel references no other asset, so a stated Dependency is an edge to nothing it names.
        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<Result>(
                 "the header states {} Dependencies; a VFX data channel references no asset",
                 data.Header->Dependencies.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteVFXDataChannel( const VFXDataChannelData& data )
    {
        VFXDataChannelData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::VFXDataChannel,
                                              VFXDataChannelTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveVFXDataChannelFile( const std::filesystem::path& path,
                                                  const VFXDataChannelData&    data )
    {
        if ( auto valid = ValidateVFXDataChannelData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write VFX data channel '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for VFX data channel: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteVFXDataChannel( data ) );
    }

    Common::ResultStr<VFXDataChannelData> LoadVFXDataChannelFile( const std::filesystem::path& path )
    {
        std::ifstream file( path, std::ios::binary );
        if ( !file )
            return Common::MakeFormattedError<VFXDataChannelData>( "cannot open VFX data channel '{}'",
                                                                   path.string() );
        std::ostringstream text;
        text << file.rdbuf();
        auto parsed = ParseVFXDataChannel( text.str() );
        if ( !parsed.IsSuccess() )
            return Common::MakeFormattedError<VFXDataChannelData>( "VFX data channel '{}': {}", path.string(),
                                                                   parsed.GetError() );
        return parsed;
    }
} // namespace Desert::Assets::Serialization
