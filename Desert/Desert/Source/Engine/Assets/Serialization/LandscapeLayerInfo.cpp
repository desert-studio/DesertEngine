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
    namespace
    {
        std::vector<std::string> LayerInfoDependenciesOf( const LandscapeLayerInfoData& data )
        {
            if ( data.GrassType.Guid.empty() )
                return {};
            return { data.GrassType.Guid };
        }
    } // namespace

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
        if ( data.GrassType.Guid.empty() != data.GrassType.Path.empty() )
            return Common::MakeFormattedError<bool>(
                 "layer '{}': GrassType names {} without {} ('{}' / '{}')", data.LayerName,
                 data.GrassType.Guid.empty() ? "a path" : "a GUID",
                 data.GrassType.Guid.empty() ? "a GUID" : "a path", data.GrassType.Guid, data.GrassType.Path );
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

        // ONE REFERENCE, TWO STATEMENTS OF IT: the registry reads the edge from the header, the grass from
        // GrassType. A file where they disagree would have the two sides load different grass.
        const std::vector<std::string> stated = data.Header ? data.Header->Dependencies : std::vector<std::string>{};
        if ( stated != LayerInfoDependenciesOf( data ) )
            return Common::MakeFormattedError<LandscapeLayerInfoData>(
                 "the header states {} Dependencies; layer '{}' names {} grass type(s)",
                 stated.size(), data.LayerName, LayerInfoDependenciesOf( data ).size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteLandscapeLayerInfo( const LandscapeLayerInfoData& data )
    {
        LandscapeLayerInfoData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::LandscapeLayerInfo,
                                              LandscapeLayerInfoTextSubsystems() );
        out.Header->Dependencies = LayerInfoDependenciesOf( data );
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
