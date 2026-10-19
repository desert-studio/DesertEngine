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

        bool UnitColour( const std::array<float, 3>& c )
        {
            return std::all_of( c.begin(), c.end(),
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
                    const bool        finite = std::isfinite( b.Height ) && std::isfinite( b.Width ) &&
                                        std::isfinite( b.Taper ) && std::isfinite( b.Curvature ) &&
                                        std::isfinite( b.Stiffness ) && std::isfinite( b.DryTipShare ) &&
                                        std::isfinite( b.DryTipLength ) && std::isfinite( b.Clumping ) &&
                                        std::isfinite( b.ClumpPull ) && std::isfinite( b.ClumpSize ) &&
                                        std::isfinite( b.LodDistance );
                    if ( !finite || !( b.Height > 0.0f ) || !( b.Width > 0.0f ) || !( b.ClumpSize > 0.0f ) ||
                         !( b.LodDistance > 0.0f ) )
                        return Common::MakeFormattedError<bool>( "grass variety {} (Blade): Height {}, Width {}, "
                                                                 "ClumpSize {} and LodDistance {} must be "
                                                                 "finite and > 0",
                                                                 i, b.Height, b.Width, b.ClumpSize,
                                                                 b.LodDistance );
                    for ( const auto& [name, value] :
                          { std::pair{ "Taper", b.Taper }, std::pair{ "Curvature", b.Curvature },
                            std::pair{ "Stiffness", b.Stiffness }, std::pair{ "DryTipShare", b.DryTipShare },
                            std::pair{ "DryTipLength", b.DryTipLength }, std::pair{ "Clumping", b.Clumping },
                            std::pair{ "ClumpPull", b.ClumpPull } } )
                        if ( value < 0.0f || value > 1.0f )
                            return Common::MakeFormattedError<bool>(
                                 "grass variety {} (Blade): {} {} is outside 0..1", i, name, value );
                    if ( b.Curvature > 0.9f )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade): Curvature {} leaves the blade no height (at most 0.9)", i,
                             b.Curvature );
                    if ( b.Segments < 1 || b.Segments > kGrassBladeMaxSegments )
                        return Common::MakeFormattedError<bool>(
                             "grass variety {} (Blade): Segments {} is outside 1..{}", i, b.Segments,
                             kGrassBladeMaxSegments );
                    if ( !UnitColour( b.BaseColor ) || !UnitColour( b.TipColor ) || !UnitColour( b.DryTipColor ) )
                        return Common::MakeFormattedError<bool>( "grass variety {} (Blade): BaseColor, TipColor "
                                                                 "and DryTipColor are linear RGB in 0..1",
                                                                 i );
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
