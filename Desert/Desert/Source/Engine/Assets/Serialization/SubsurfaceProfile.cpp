#include <Engine/Assets/Serialization/SubsurfaceProfile.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <string>
#include <system_error>

namespace Desert::Assets::Serialization
{
    namespace
    {
        Common::BoolResultStr InRange( const char* name, float value, float lo, float hi )
        {
            if ( !std::isfinite( value ) || value < lo || value > hi )
                return Common::MakeFormattedError<bool>( "{} {} is outside [{}, {}]", name, value, lo, hi );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr InRange( const char* name, const glm::vec3& value, float lo, float hi )
        {
            for ( int c = 0; c < 3; ++c )
                if ( !std::isfinite( value[c] ) || value[c] < lo || value[c] > hi )
                    return Common::MakeFormattedError<bool>( "{}[{}] {} is outside [{}, {}]", name, c, value[c], lo,
                                                             hi );
            return BOOLSUCCESS;
        }
    } // namespace

    Common::BoolResultStr ValidateSubsurfaceProfileData( const SubsurfaceProfileData& d )
    {
        // UE's ClampMin/ClampMax (SubsurfaceProfile.h); colours that UE leaves unclamped are held to [0, 1].
        for ( const auto& check :
              { InRange( "SurfaceAlbedo", d.SurfaceAlbedo, 0.01f, 1.0f ),
                InRange( "MeanFreePathColor", d.MeanFreePathColor, 0.001f, 1.0f ),
                InRange( "MeanFreePathDistance", d.MeanFreePathDistance, 0.1f, 50.0f ),
                InRange( "Tint", d.Tint, 0.0f, 1.0f ), InRange( "BoundaryColorBleed", d.BoundaryColorBleed, 0.0f, 1.0f ),
                InRange( "ExtinctionScale", d.ExtinctionScale, 0.01f, 100.0f ),
                InRange( "NormalScale", d.NormalScale, 0.01f, 1.0f ),
                InRange( "DistanceScale", d.DistanceScale, 0.1f, 10.0f ),
                InRange( "ScatteringDistribution", d.ScatteringDistribution, 0.01f, 1.0f ),
                InRange( "IOR", d.IOR, 1.0f, 3.0f ),
                InRange( "TransmissionTintColor", d.TransmissionTintColor, 0.0f, 1.0f ),
                InRange( "Roughness0", d.Roughness0, 0.5f, 2.0f ), InRange( "Roughness1", d.Roughness1, 0.5f, 2.0f ),
                InRange( "LobeMix", d.LobeMix, 0.1f, 0.9f ) } )
            if ( !check )
                return check;
        return BOOLSUCCESS;
    }

    Common::ResultStr<SubsurfaceProfileData> ParseSubsurfaceProfile( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<SubsurfaceProfileData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kSubsurfaceProfileVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<SubsurfaceProfileData>( "subsurface profile {}", headed.GetError() );

        const auto parsed = Common::Json::Read<SubsurfaceProfileData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<SubsurfaceProfileData>( "{}", parsed.GetError() );
        SubsurfaceProfileData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::SubsurfaceProfile,
                                                      Assets::kSubsurfaceProfileSchemaTag, kSubsurfaceProfileVersion,
                                                      SubsurfaceProfileTextSubsystems() );
             !header )
            return Common::MakeFormattedError<SubsurfaceProfileData>( "subsurface profile {}", header.GetError() );

        if ( auto valid = ValidateSubsurfaceProfileData( data ); !valid )
            return Common::MakeFormattedError<SubsurfaceProfileData>( "{}", valid.GetError() );

        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<SubsurfaceProfileData>(
                 "the header states {} Dependencies; a subsurface profile references no asset",
                 data.Header->Dependencies.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteSubsurfaceProfile( const SubsurfaceProfileData& data )
    {
        SubsurfaceProfileData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::SubsurfaceProfile,
                                              SubsurfaceProfileTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveSubsurfaceProfileFile( const std::filesystem::path& path,
                                                     const SubsurfaceProfileData& data )
    {
        if ( auto valid = ValidateSubsurfaceProfileData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write subsurface profile '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for a subsurface profile: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteSubsurfaceProfile( data ) );
    }
} // namespace Desert::Assets::Serialization
