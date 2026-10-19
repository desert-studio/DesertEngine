#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    Common::BoolResultStr ValidateLandscapeGrassTypeData( const LandscapeGrassTypeData& data )
    {
        if ( data.GrassVarieties.empty() )
            return Common::MakeFormattedError<bool>( "GrassVarieties is empty; a grass type grows nothing" );
        for ( size_t i = 0; i < data.GrassVarieties.size(); ++i )
        {
            const GrassVariety& v = data.GrassVarieties[i];
            if ( v.Kind != GrassVarietyKind::Mesh )
                return Common::MakeFormattedError<bool>( "grass variety {}: Kind {} is not a known kind", i,
                                                         static_cast<int>( v.Kind ) );
            if ( const auto guid = Common::Content::AssetGuidFromText( v.Mesh.Guid );
                 !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<bool>( "grass variety {} names no well-formed Mesh GUID ('{}')",
                                                         i, v.Mesh.Guid );
            const bool finite = std::isfinite( v.Density ) && std::isfinite( v.PlacementJitter ) &&
                                std::isfinite( v.StartCullDistance ) && std::isfinite( v.EndCullDistance );
            if ( !finite || v.Density <= 0.0f || v.PlacementJitter < 0.0f || v.PlacementJitter > 1.0f ||
                 v.StartCullDistance < 0.0f || v.EndCullDistance <= v.StartCullDistance )
                return Common::MakeFormattedError<bool>(
                     "grass variety {}: Density {} must be > 0, PlacementJitter {} in 0..1, and "
                     "0 <= StartCullDistance {} < EndCullDistance {}",
                     i, v.Density, v.PlacementJitter, v.StartCullDistance, v.EndCullDistance );
            if ( v.AllowedDensityRange.Min < 0.0f || v.AllowedDensityRange.Max > 1.0f ||
                 v.AllowedDensityRange.Min > v.AllowedDensityRange.Max )
                return Common::MakeFormattedError<bool>(
                     "grass variety {}: AllowedDensityRange [{}, {}] is not an interval of 0..1", i,
                     v.AllowedDensityRange.Min, v.AllowedDensityRange.Max );
            for ( const FoliageFloatInterval& s : { v.ScaleX, v.ScaleY, v.ScaleZ } )
                if ( !( s.Min > 0.0f ) || s.Max < s.Min )
                    return Common::MakeFormattedError<bool>(
                         "grass variety {}: a scale range [{}, {}] must be positive and ordered", i, s.Min,
                         s.Max );
            if ( v.MaxScaleWeightAttenuation < 0.0f || v.MaxScaleWeightAttenuation > 1.0f )
                return Common::MakeFormattedError<bool>(
                     "grass variety {}: MaxScaleWeightAttenuation {} is outside 0..1", i,
                     v.MaxScaleWeightAttenuation );
        }
        return BOOLSUCCESS;
    }

    namespace
    {
        // The header's Dependencies of a type: its varieties' meshes, each once, in variety order.
        std::vector<std::string> GrassTypeDependencies( const LandscapeGrassTypeData& data )
        {
            std::vector<std::string> out;
            for ( const GrassVariety& v : data.GrassVarieties )
                if ( std::find( out.begin(), out.end(), v.Mesh.Guid ) == out.end() )
                    out.push_back( v.Mesh.Guid );
            return out;
        }
    } // namespace

    Common::ResultStr<LandscapeGrassTypeData> ParseLandscapeGrassType( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<LandscapeGrassTypeData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kLandscapeGrassTypeVersion, std::nullopt );
             !headed )
            return Common::MakeFormattedError<LandscapeGrassTypeData>( "landscape grass type {}",
                                                                       headed.GetError() );

        const auto parsed = Common::Json::Read<LandscapeGrassTypeData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<LandscapeGrassTypeData>( "{}", parsed.GetError() );
        LandscapeGrassTypeData data = parsed.GetValue();

        if ( auto header =
                  Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::LandscapeGrassType,
                                             Assets::kLandscapeGrassTypeSchemaTag, kLandscapeGrassTypeVersion,
                                             LandscapeGrassTypeTextSubsystems() );
             !header )
            return Common::MakeFormattedError<LandscapeGrassTypeData>( "landscape grass type {}",
                                                                       header.GetError() );

        if ( auto valid = ValidateLandscapeGrassTypeData( data ); !valid )
            return Common::MakeFormattedError<LandscapeGrassTypeData>( "{}", valid.GetError() );

        // ONE REFERENCE, TWO STATEMENTS OF IT: the registry reads the edges from the header, the renderer from
        // the varieties. A file where they disagree would have the two sides load different meshes.
        const auto  expected = GrassTypeDependencies( data );
        const auto& stated   = data.Header ? data.Header->Dependencies : std::vector<std::string>{};
        if ( stated.size() != expected.size() ||
             !std::is_permutation( stated.begin(), stated.end(), expected.begin() ) )
            return Common::MakeFormattedError<LandscapeGrassTypeData>(
                 "the header states {} Dependencies; the type references exactly its {} variety mesh(es)",
                 stated.size(), expected.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteLandscapeGrassType( const LandscapeGrassTypeData& data )
    {
        LandscapeGrassTypeData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::LandscapeGrassType,
                                              LandscapeGrassTypeTextSubsystems() );
        out.Header->Dependencies = GrassTypeDependencies( data );
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveLandscapeGrassTypeFile( const std::filesystem::path&  path,
                                                      const LandscapeGrassTypeData& data )
    {
        if ( auto valid = ValidateLandscapeGrassTypeData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write landscape grass type '{}': {}",
                                                     path.string(), valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for landscape grass type: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteLandscapeGrassType( data ) );
    }
} // namespace Desert::Assets::Serialization
