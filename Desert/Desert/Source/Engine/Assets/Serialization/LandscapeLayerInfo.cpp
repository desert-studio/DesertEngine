#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
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
        for ( size_t i = 0; i < data.Grass.size(); ++i )
        {
            const GrassVariety& v = data.Grass[i];
            if ( const auto guid = Common::Content::AssetGuidFromText( v.Mesh.Guid ); !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<bool>( "layer '{}' grass variety {} names no well-formed Mesh GUID ('{}')",
                                                         data.LayerName, i, v.Mesh.Guid );
            const bool finite = std::isfinite( v.Density ) && std::isfinite( v.PlacementJitter ) &&
                                std::isfinite( v.StartCullDistance ) && std::isfinite( v.EndCullDistance );
            if ( !finite || v.Density <= 0.0f || v.PlacementJitter < 0.0f || v.PlacementJitter > 1.0f ||
                 v.StartCullDistance < 0.0f || v.EndCullDistance <= v.StartCullDistance )
                return Common::MakeFormattedError<bool>(
                     "layer '{}' grass variety {}: Density {} must be > 0, PlacementJitter {} in 0..1, and "
                     "0 <= StartCullDistance {} < EndCullDistance {}",
                     data.LayerName, i, v.Density, v.PlacementJitter, v.StartCullDistance, v.EndCullDistance );
            if ( v.AllowedDensityRange.Min < 0.0f || v.AllowedDensityRange.Max > 1.0f ||
                 v.AllowedDensityRange.Min > v.AllowedDensityRange.Max )
                return Common::MakeFormattedError<bool>( "layer '{}' grass variety {}: AllowedDensityRange [{}, {}] "
                                                         "is not an interval of 0..1",
                                                         data.LayerName, i, v.AllowedDensityRange.Min,
                                                         v.AllowedDensityRange.Max );
            for ( const FoliageFloatInterval& s : { v.ScaleX, v.ScaleY, v.ScaleZ } )
                if ( !( s.Min > 0.0f ) || s.Max < s.Min )
                    return Common::MakeFormattedError<bool>(
                         "layer '{}' grass variety {}: a scale range [{}, {}] must be positive and ordered",
                         data.LayerName, i, s.Min, s.Max );
            if ( v.MaxScaleWeightAttenuation < 0.0f || v.MaxScaleWeightAttenuation > 1.0f )
                return Common::MakeFormattedError<bool>( "layer '{}' grass variety {}: MaxScaleWeightAttenuation {} "
                                                         "is outside 0..1",
                                                         data.LayerName, i, v.MaxScaleWeightAttenuation );
        }
        return BOOLSUCCESS;
    }

    namespace
    {
        // The header's Dependencies of a layer: its varieties' meshes, each once, in variety order.
        std::vector<std::string> GrassDependencies( const LandscapeLayerInfoData& data )
        {
            std::vector<std::string> out;
            for ( const GrassVariety& v : data.Grass )
                if ( std::find( out.begin(), out.end(), v.Mesh.Guid ) == out.end() )
                    out.push_back( v.Mesh.Guid );
            return out;
        }
    } // namespace

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
        if ( data.Header )
        {
            const auto  expected = GrassDependencies( data );
            const auto& stated   = data.Header->Dependencies;
            if ( stated.size() != expected.size() ||
                 !std::is_permutation( stated.begin(), stated.end(), expected.begin() ) )
                return Common::MakeFormattedError<LandscapeLayerInfoData>(
                     "the header states {} Dependencies; layer '{}' references exactly its {} grass mesh(es)",
                     stated.size(), data.LayerName, expected.size() );
        }

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteLandscapeLayerInfo( const LandscapeLayerInfoData& data )
    {
        LandscapeLayerInfoData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::LandscapeLayerInfo,
                                              LandscapeLayerInfoTextSubsystems() );
        out.Header->Dependencies.clear();
        out.Header->Dependencies = GrassDependencies( data );
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
