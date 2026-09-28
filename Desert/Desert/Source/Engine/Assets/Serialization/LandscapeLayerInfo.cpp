#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    Common::BoolResultStr ValidateLandscapeLayerInfoData( const LandscapeLayerInfoData& data )
    {
        if ( data.LayerName.empty() )
            return Common::MakeFormattedError<bool>(
                 "LayerName is empty; it is the key a weight plane is found by" );
        if ( data.LayerName.size() > World::Landscape::kLandscapeMaxWeightLayerName )
            return Common::MakeFormattedError<bool>( "LayerName '{}' is {} bytes long, the limit is {}",
                                                     data.LayerName, data.LayerName.size(),
                                                     World::Landscape::kLandscapeMaxWeightLayerName );
        if ( !std::isfinite( data.Hardness ) || data.Hardness < 0.0f || data.Hardness > 1.0f )
            return Common::MakeFormattedError<bool>( "layer '{}' has Hardness {}, outside 0..1", data.LayerName,
                                                     data.Hardness );
        const glm::vec3& c = data.LayerUsageDebugColor;
        if ( !std::isfinite( c.r ) || !std::isfinite( c.g ) || !std::isfinite( c.b ) )
            return Common::MakeFormattedError<bool>(
                 "layer '{}' has LayerUsageDebugColor ({}, {}, {}), not finite", data.LayerName, c.r, c.g, c.b );
        return BOOLSUCCESS;
    }

    Common::ResultStr<LandscapeLayerInfoData> ParseLandscapeLayerInfo( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<LandscapeLayerInfoData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kLandscapeLayerInfoVersion, std::nullopt );
             !headed )
            return Common::MakeFormattedError<LandscapeLayerInfoData>( "landscape layer info {}",
                                                                       headed.GetError() );

        const auto parsed = Common::Json::Read<LandscapeLayerInfoData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<LandscapeLayerInfoData>( "{}", parsed.GetError() );
        LandscapeLayerInfoData data = parsed.GetValue();

        if ( auto header =
                  Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::LandscapeLayerInfo,
                                             Assets::kLandscapeLayerInfoSchemaTag, kLandscapeLayerInfoVersion,
                                             LandscapeLayerInfoTextSubsystems() );
             !header )
            return Common::MakeFormattedError<LandscapeLayerInfoData>( "landscape layer info {}",
                                                                       header.GetError() );

        if ( auto valid = ValidateLandscapeLayerInfoData( data ); !valid )
            return Common::MakeFormattedError<LandscapeLayerInfoData>( "{}", valid.GetError() );

        // A layer info references no other asset, so a stated Dependency is an edge the registry would
        // follow to something this file never names.
        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<LandscapeLayerInfoData>(
                 "the header states {} Dependencies; layer '{}' references no asset",
                 data.Header->Dependencies.size(), data.LayerName );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteLandscapeLayerInfo( const LandscapeLayerInfoData& data )
    {
        LandscapeLayerInfoData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::LandscapeLayerInfo,
                                              LandscapeLayerInfoTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveLandscapeLayerInfoFile( const std::filesystem::path&  path,
                                                      const LandscapeLayerInfoData& data )
    {
        if ( auto valid = ValidateLandscapeLayerInfoData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write landscape layer info '{}': {}",
                                                     path.string(), valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for landscape layer info: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteLandscapeLayerInfo( data ) );
    }
} // namespace Desert::Assets::Serialization
