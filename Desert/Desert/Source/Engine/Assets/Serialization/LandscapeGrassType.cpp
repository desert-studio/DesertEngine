#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    namespace
    {
        Common::BoolResultStr CheckGrassInterval( size_t variety, const char* name, const GrassFloatInterval& range,
                                             float lowest, float highest )
        {
            if ( !std::isfinite( range.Min ) || !std::isfinite( range.Max ) || range.Min > range.Max ||
                 range.Min < lowest || range.Max > highest )
                return Common::MakeFormattedError<bool>(
                     "variety {}: {} [{}, {}] is not a finite range with {} <= Min <= Max <= {}", variety, name,
                     range.Min, range.Max, lowest, highest );
            return BOOLSUCCESS;
        }

        // One edge per distinct mesh, in the order the varieties first name it: two varieties drawing the
        // same mesh are one dependency, not two.
        std::vector<std::string> GrassTypeDependenciesOf( const LandscapeGrassTypeData& data )
        {
            std::vector<std::string> out;
            for ( const GrassVariety& v : data.GrassVarieties )
                if ( std::find( out.begin(), out.end(), v.GrassMesh.Guid ) == out.end() )
                    out.push_back( v.GrassMesh.Guid );
            return out;
        }
    } // namespace

    Common::BoolResultStr ValidateLandscapeGrassTypeData( const LandscapeGrassTypeData& data )
    {
        if ( data.GrassVarieties.empty() )
            return Common::MakeFormattedError<bool>( "GrassVarieties is empty; a grass type grows nothing" );
        if ( data.GrassVarieties.size() > kLandscapeGrassMaxVarieties )
            return Common::MakeFormattedError<bool>( "GrassVarieties has {} entries, the limit is {}",
                                                     data.GrassVarieties.size(), kLandscapeGrassMaxVarieties );
        for ( size_t i = 0; i < data.GrassVarieties.size(); ++i )
        {
            const GrassVariety& v = data.GrassVarieties[i];
            if ( v.GrassMesh.Guid.empty() || v.GrassMesh.Path.empty() )
                return Common::MakeFormattedError<bool>(
                     "variety {}: GrassMesh must name both a GUID and a path ('{}' / '{}')", i, v.GrassMesh.Guid,
                     v.GrassMesh.Path );
            if ( !std::isfinite( v.GrassDensity ) || v.GrassDensity <= 0.0f ||
                 v.GrassDensity > kLandscapeGrassMaxDensity )
                return Common::MakeFormattedError<bool>( "variety {}: GrassDensity {} must lie in (0, {}]", i,
                                                         v.GrassDensity, kLandscapeGrassMaxDensity );
            if ( !std::isfinite( v.EndCullDistance ) || v.EndCullDistance <= 0.0f ||
                 v.EndCullDistance > kLandscapeGrassMaxCullDistance )
                return Common::MakeFormattedError<bool>( "variety {}: EndCullDistance {} cm must lie in (0, {}]",
                                                         i, v.EndCullDistance, kLandscapeGrassMaxCullDistance );
            if ( !std::isfinite( v.StartCullDistance ) || v.StartCullDistance < 0.0f ||
                 v.StartCullDistance > v.EndCullDistance )
                return Common::MakeFormattedError<bool>(
                     "variety {}: StartCullDistance {} cm must lie in [0, EndCullDistance {}]", i,
                     v.StartCullDistance, v.EndCullDistance );
            if ( auto ok = CheckGrassInterval( i, "AllowedDensityRange", v.AllowedDensityRange, 0.0f, 1.0f ); !ok )
                return ok;
            // A zero scale is an invisible instance; the UI clamps nothing, so the file is where it is refused.
            for ( const auto& [name, range] : { std::pair{ "ScaleX", &v.ScaleX }, std::pair{ "ScaleY", &v.ScaleY },
                                                std::pair{ "ScaleZ", &v.ScaleZ } } )
            {
                if ( auto ok = CheckGrassInterval( i, name, *range, 0.0f, 1.0e6f ); !ok )
                    return ok;
                if ( range->Min <= 0.0f )
                    return Common::MakeFormattedError<bool>( "variety {}: {}.Min {} must be above zero", i, name,
                                                             range->Min );
            }
        }
        return BOOLSUCCESS;
    }

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

        // ONE REFERENCE, TWO STATEMENTS OF IT: the registry reads the edges from the header, the loader from
        // the varieties. A file where they disagree would have the two sides load different meshes.
        if ( data.Header->Dependencies != GrassTypeDependenciesOf( data ) )
            return Common::MakeFormattedError<LandscapeGrassTypeData>(
                 "the header's Dependencies ({} entries) do not state exactly the varieties' mesh GUIDs ({})",
                 data.Header->Dependencies.size(), GrassTypeDependenciesOf( data ).size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteLandscapeGrassType( const LandscapeGrassTypeData& data )
    {
        LandscapeGrassTypeData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::LandscapeGrassType,
                                              LandscapeGrassTypeTextSubsystems() );
        out.Header->Dependencies = GrassTypeDependenciesOf( data );
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
