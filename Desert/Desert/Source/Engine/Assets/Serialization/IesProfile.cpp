#include <Engine/Assets/Serialization/IesProfile.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <string>
#include <system_error>

namespace Desert::Assets::Serialization
{
    namespace
    {
        Common::BoolResultStr RisingWithin( const char* name, const std::vector<float>& angles, float hi )
        {
            if ( angles.empty() )
                return Common::MakeFormattedError<bool>( "{} lists no angle", name );
            for ( std::size_t i = 0; i < angles.size(); ++i )
            {
                if ( !std::isfinite( angles[i] ) || angles[i] < 0.0f || angles[i] > hi )
                    return Common::MakeFormattedError<bool>( "{}[{}] {} is outside [0, {}]", name, i, angles[i],
                                                             hi );
                if ( i > 0 && angles[i] <= angles[i - 1] )
                    return Common::MakeFormattedError<bool>( "{}[{}] {} does not rise above {}", name, i,
                                                             angles[i], angles[i - 1] );
            }
            return BOOLSUCCESS;
        }
    } // namespace

    IesProfileData IesProfileFromPhotometry( const Graphic::IesPhotometricData& photometry )
    {
        IesProfileData data;
        data.VerticalAngles   = photometry.VerticalAngles;
        data.HorizontalAngles = photometry.HorizontalAngles;
        data.Candela          = photometry.Candela;
        return data;
    }

    Graphic::IesPhotometricData PhotometryOf( const IesProfileData& data )
    {
        return Graphic::IesPhotometricData{ .VerticalAngles   = data.VerticalAngles,
                                            .HorizontalAngles = data.HorizontalAngles,
                                            .Candela          = data.Candela };
    }

    Common::BoolResultStr ValidateIesProfileData( const IesProfileData& d )
    {
        if ( auto vertical = RisingWithin( "VerticalAngles", d.VerticalAngles, 180.0f ); !vertical )
            return vertical;
        if ( auto horizontal = RisingWithin( "HorizontalAngles", d.HorizontalAngles, 360.0f ); !horizontal )
            return horizontal;
        const std::size_t expected = d.VerticalAngles.size() * d.HorizontalAngles.size();
        if ( d.Candela.size() != expected )
            return Common::MakeFormattedError<bool>( "Candela holds {} values; {} vertical x {} horizontal angles "
                                                     "need {}",
                                                     d.Candela.size(), d.VerticalAngles.size(),
                                                     d.HorizontalAngles.size(), expected );
        float peak = 0.0f;
        for ( std::size_t i = 0; i < d.Candela.size(); ++i )
        {
            if ( !std::isfinite( d.Candela[i] ) || d.Candela[i] < 0.0f )
                return Common::MakeFormattedError<bool>( "Candela[{}] {} is not a finite non-negative intensity",
                                                         i, d.Candela[i] );
            peak = std::fmax( peak, d.Candela[i] );
        }
        if ( peak <= 0.0f )
            return Common::MakeError<bool>( "every Candela is 0: the profile emits nothing" );
        return BOOLSUCCESS;
    }

    Common::ResultStr<IesProfileData> ParseIesProfileAsset( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<IesProfileData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kIesProfileVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<IesProfileData>( "IES profile {}", headed.GetError() );

        const auto parsed = Common::Json::Read<IesProfileData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<IesProfileData>( "{}", parsed.GetError() );
        IesProfileData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::IesProfile,
                                                      Assets::kIesProfileSchemaTag, kIesProfileVersion,
                                                      IesProfileTextSubsystems() );
             !header )
            return Common::MakeFormattedError<IesProfileData>( "IES profile {}", header.GetError() );

        if ( auto valid = ValidateIesProfileData( data ); !valid )
            return Common::MakeFormattedError<IesProfileData>( "{}", valid.GetError() );

        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<IesProfileData>(
                 "the header states {} Dependencies; an IES profile references no asset",
                 data.Header->Dependencies.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteIesProfileAsset( const IesProfileData& data )
    {
        IesProfileData out = data;
        out.Header         = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::IesProfile,
                                                      IesProfileTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveIesProfileFile( const std::filesystem::path& path, const IesProfileData& data )
    {
        if ( auto valid = ValidateIesProfileData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write IES profile '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for an IES profile: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteIesProfileAsset( data ) );
    }
} // namespace Desert::Assets::Serialization
