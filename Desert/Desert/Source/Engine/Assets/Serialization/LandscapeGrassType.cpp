#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    namespace
    {
        bool WellFormedGuid( const std::optional<AssetGuidRef>& ref )
        {
            if ( !ref )
                return false;
            const auto guid = Common::Content::AssetGuidFromText( ref->Guid );
            return guid && !guid.GetValue().IsNull();
        }

        bool UnitRect( const std::array<float, 4>& r )
        {
            return std::all_of( r.begin(), r.end(),
                                []( float x ) { return std::isfinite( x ) && x >= 0.0f && x <= 1.0f; } );
        }

        // A variety states exactly the references and the shape its Kind draws with: a field the kind does not
        // read is refused, not ignored.
        Common::BoolResultStr ValidateVarietyKind( size_t i, const GrassVariety& v )
        {
            switch ( v.Kind )
            {
                case GrassVarietyKind::Mesh:
                    if ( !WellFormedGuid( v.Mesh ) )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Mesh) names no well-formed Mesh GUID ('{}')", i,
                             v.Mesh ? v.Mesh->Guid : std::string() );
                    if ( v.Material || v.Blade )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Mesh) states a {}; only a Blade variety has one", i,
                             v.Material ? "Material" : "Blade" );
                    return BOOLSUCCESS;
                case GrassVarietyKind::Blade:
                {
                    if ( !WellFormedGuid( v.Material ) )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade) names no well-formed Material GUID ('{}')", i,
                             v.Material ? v.Material->Guid : std::string() );
                    if ( v.Mesh )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade) states a Mesh; a blade is built by the vertex stage", i );
                    if ( !v.Blade )
                        return Common::MakeFormattedError<bool>( "grass variety {} (Blade) has no Blade shape",
                                                                 i );
                    const GrassBlade& b      = *v.Blade;
                    const bool        finite = std::isfinite( b.Height ) && std::isfinite( b.Curvature ) &&
                                        std::isfinite( b.CurvatureSpread ) && std::isfinite( b.BendSpread ) &&
                                        std::isfinite( b.Tilt ) && std::isfinite( b.FacingJitter ) &&
                                        std::isfinite( b.Stiffness ) && std::isfinite( b.Clumping ) &&
                                        std::isfinite( b.ClumpPull ) && std::isfinite( b.ClumpSize ) &&
                                        std::isfinite( b.Scatter ) && std::isfinite( b.Patchiness ) &&
                                        std::isfinite( b.PatchSize ) && std::isfinite( b.LodDistance );
                    if ( !finite || !( b.Height > 0.0f ) || !( b.ClumpSize > 0.0f ) ||
                         !( b.PatchSize > 0.0f ) || !( b.LodDistance > 0.0f ) )
                        return Common::MakeFormattedError<bool>( "grass variety {} (Blade): Height {}, "
                                                                 "ClumpSize {}, PatchSize {} and LodDistance {} "
                                                                 "must be finite and > 0",
                                                                 i, b.Height, b.ClumpSize, b.PatchSize,
                                                                 b.LodDistance );
                    for ( const auto& [name, value] :
                          { std::pair{ "Curvature", b.Curvature }, std::pair{ "Stiffness", b.Stiffness },
                            std::pair{ "Clumping", b.Clumping },
                            std::pair{ "ClumpPull", b.ClumpPull }, std::pair{ "Scatter", b.Scatter },
                            std::pair{ "Patchiness", b.Patchiness },
                            std::pair{ "CurvatureSpread", b.CurvatureSpread },
                            std::pair{ "BendSpread", b.BendSpread } } )
                        if ( value < 0.0f || value > 1.0f )
                            return Common::MakeFormattedError<bool>(
                                 "grass variety {} (Blade): {} {} is outside 0..1", i, name, value );
                    if ( b.Tilt < 0.0f || b.Tilt > 60.0f || b.FacingJitter < 0.0f || b.FacingJitter > 180.0f )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade): Tilt {} (0..60 degrees) "
                             "or FacingJitter {} (0..180 degrees) out of range",
                             i, b.Tilt, b.FacingJitter );
                    if ( b.Curvature > 0.9f )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade): Curvature {} leaves the blade no height (at most 0.9)", i,
                             b.Curvature );
                    if ( b.Segments < 1 || b.Segments > kGrassBladeMaxSegments )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade): Segments {} is outside 1..{}", i, b.Segments,
                             kGrassBladeMaxSegments );
                    if ( b.Cards.empty() || b.Cards.size() > kGrassBladeMaxCards )
                        return Common::MakeFormattedError<bool>( "grass variety {} (Blade): {} Cards, 1..{} wanted",
                                                                 i, b.Cards.size(), kGrassBladeMaxCards );
                    for ( size_t c = 0; c < b.Cards.size(); ++c )
                    {
                        const auto& r = b.Cards[c];
                        if ( !UnitRect( r ) || !( r[2] > r[0] ) || !( r[3] > r[1] ) )
                            return Common::MakeFormattedError<bool>(
                                 "grass variety {} (Blade): card {} is not a rectangle (u0 < u1, vTip < vRoot) in "
                                 "0..1",
                                 i, c );
                    }
                    return BOOLSUCCESS;
                }
            }
            return Common::MakeFormattedError<bool>( "grass variety {}: Kind {} is not a known kind", i,
                                                     static_cast<int>( v.Kind ) );
        }
    } // namespace

    Common::BoolResultStr ValidateLandscapeGrassTypeData( const LandscapeGrassTypeData& data )
    {
        if ( data.GrassVarieties.empty() )
            return Common::MakeFormattedError<bool>( "GrassVarieties is empty; a grass type grows nothing" );
        for ( size_t i = 0; i < data.GrassVarieties.size(); ++i )
        {
            const GrassVariety& v = data.GrassVarieties[i];
            if ( auto kind = ValidateVarietyKind( i, v ); !kind )
                return kind;
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
        // The header's Dependencies of a type: its varieties' meshes and blade materials, each once, in variety
        // order.
        std::vector<std::string> GrassTypeDependencies( const LandscapeGrassTypeData& data )
        {
            std::vector<std::string> out;
            for ( const GrassVariety& v : data.GrassVarieties )
                for ( const std::optional<AssetGuidRef>* ref : { &v.Mesh, &v.Material } )
                    if ( *ref && std::find( out.begin(), out.end(), ( *ref )->Guid ) == out.end() )
                        out.push_back( ( *ref )->Guid );
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
                 "the header states {} Dependencies; the type references exactly its {} variety mesh(es) and "
                 "blade material(s)",
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
