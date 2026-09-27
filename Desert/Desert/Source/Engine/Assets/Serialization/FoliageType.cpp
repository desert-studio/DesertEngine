#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    namespace
    {
        Common::BoolResultStr CheckInterval( const char* name, const FoliageFloatInterval& range )
        {
            if ( !std::isfinite( range.Min ) || !std::isfinite( range.Max ) || range.Min > range.Max )
                return Common::MakeFormattedError<bool>( "{} [{}, {}] is not a finite range with Min <= Max", name,
                                                         range.Min, range.Max );
            return BOOLSUCCESS;
        }

        std::vector<std::string> DependenciesOf( const FoliageTypeData& data )
        {
            std::vector<std::string> out;
            if ( !data.Mesh.Guid.empty() )
                out.push_back( data.Mesh.Guid );
            if ( !data.Prefab.Guid.empty() )
                out.push_back( data.Prefab.Guid );
            for ( const auto& layer : data.LandscapeLayers )
                out.push_back( layer.Guid );
            return out;
        }
    } // namespace

    // The FOLT generation the header states, read as an untyped tree (TextAssetHeaderCheck.hpp: a struct
    // would impose this build's layout on a file whose problem may be that it is another generation).
    std::optional<uint32_t> StatedFoliageTypeGeneration( const std::string& text )
    {
        const auto members = Common::Json::ObjectMembers( text );
        if ( !members )
            return std::nullopt;
        for ( const auto& [name, value] : members.GetValue() )
            if ( name == Common::Content::kTextHeaderMember )
            {
                const auto header = Common::Json::Read<Common::Content::TextAssetHeaderSerialized>( value );
                if ( !header )
                    return std::nullopt;
                const auto stated = header.GetValue().Versions.find( "FOLT" );
                if ( stated == header.GetValue().Versions.end() )
                    return std::nullopt;
                return stated->second;
            }
        return std::nullopt;
    }

    Common::BoolResultStr ValidateFoliageTypeData( const FoliageTypeData& data )
    {
        if ( !std::isfinite( data.Density ) || data.Density <= 0.0f )
            return Common::MakeFormattedError<bool>( "Density {} must be a positive number", data.Density );
        if ( auto ok = CheckInterval( "ScaleX", data.ScaleX ); !ok )
            return ok;
        if ( data.ScaleX.Min <= 0.0f )
            return Common::MakeFormattedError<bool>( "ScaleX.Min {} must be above zero (a zero scale is an "
                                                     "invisible instance)",
                                                     data.ScaleX.Min );
        if ( auto ok = CheckInterval( "ZOffset", data.ZOffset ); !ok )
            return ok;
        if ( auto ok = CheckInterval( "GroundSlopeAngle", data.GroundSlopeAngle ); !ok )
            return ok;
        if ( data.GroundSlopeAngle.Min < 0.0f || data.GroundSlopeAngle.Max > 90.0f )
            return Common::MakeFormattedError<bool>( "GroundSlopeAngle [{}, {}] must lie within [0, 90] degrees",
                                                     data.GroundSlopeAngle.Min, data.GroundSlopeAngle.Max );
        if ( !std::isfinite( data.RandomPitchAngle ) || data.RandomPitchAngle < 0.0f ||
             data.RandomPitchAngle > 180.0f )
            return Common::MakeFormattedError<bool>( "RandomPitchAngle {} must lie within [0, 180] degrees",
                                                     data.RandomPitchAngle );
        if ( auto ok = CheckInterval( "Height", data.Height ); !ok )
            return ok;
        if ( !std::isfinite( data.MinimumLayerWeight ) || data.MinimumLayerWeight < 0.0f ||
             data.MinimumLayerWeight > 1.0f )
            return Common::MakeFormattedError<bool>( "MinimumLayerWeight {} must lie within [0, 1]",
                                                     data.MinimumLayerWeight );
        if ( auto ok = CheckInterval( "CullDistance", data.CullDistance ); !ok )
            return ok;
        if ( data.CullDistance.Min < 0.0f )
            return Common::MakeFormattedError<bool>( "CullDistance.Min {} must not be negative (it is a distance "
                                                     "from the camera, cm)",
                                                     data.CullDistance.Min );
        if ( !std::isfinite( data.Wind.Strength ) || data.Wind.Strength < 0.0f )
            return Common::MakeFormattedError<bool>( "Wind.Strength {} must be a non-negative distance, cm",
                                                     data.Wind.Strength );
        if ( !std::isfinite( data.Wind.Speed ) || data.Wind.Speed < 0.0f )
            return Common::MakeFormattedError<bool>( "Wind.Speed {} must be a non-negative frequency, Hz",
                                                     data.Wind.Speed );
        if ( !std::isfinite( data.Wind.Height ) || data.Wind.Height <= 0.0f )
            return Common::MakeFormattedError<bool>( "Wind.Height {} must be above zero (the height, cm, at "
                                                     "which the sway is full)",
                                                     data.Wind.Height );
        if ( !std::isfinite( data.Wind.DirectionDegrees ) )
            return Common::MakeFormattedError<bool>( "Wind.DirectionDegrees {} must be a finite angle",
                                                     data.Wind.DirectionDegrees );
        for ( size_t i = 0; i < data.LandscapeLayers.size(); ++i )
        {
            const auto& layer = data.LandscapeLayers[i];
            if ( layer.Guid.empty() || layer.Path.empty() )
                return Common::MakeFormattedError<bool>(
                     "LandscapeLayers[{}] must name a GUID and a path ('{}' / '{}')", i, layer.Guid, layer.Path );
            for ( size_t j = 0; j < i; ++j )
                if ( data.LandscapeLayers[j].Guid == layer.Guid )
                    return Common::MakeFormattedError<bool>(
                         "LandscapeLayers[{}] repeats LandscapeLayers[{}] ('{}')", i, j, layer.Path );
        }
        if ( data.Mesh.Guid.empty() != data.Mesh.Path.empty() )
            return Common::MakeFormattedError<bool>(
                 "Mesh names {} without {} ('{}' / '{}')", data.Mesh.Guid.empty() ? "a path" : "a GUID",
                 data.Mesh.Guid.empty() ? "a GUID" : "a path", data.Mesh.Guid, data.Mesh.Path );
        if ( !data.IsPrefab() )
        {
            if ( !data.Prefab.Guid.empty() || !data.Prefab.Path.empty() )
                return Common::MakeFormattedError<bool>( "a Mesh type names a prefab ('{}' / '{}'); only a Prefab "
                                                         "type places one",
                                                         data.Prefab.Guid, data.Prefab.Path );
            return BOOLSUCCESS;
        }
        // A Prefab type (FO-8): one prefab, no mesh, and none of the instanced-mesh settings — a value there
        // would be a knob that moves nothing.
        if ( data.Prefab.Guid.empty() || data.Prefab.Path.empty() )
            return Common::MakeFormattedError<bool>( "a Prefab type must name its prefab by GUID and path ('{}' / "
                                                     "'{}')",
                                                     data.Prefab.Guid, data.Prefab.Path );
        if ( !data.Mesh.Guid.empty() )
            return Common::MakeFormattedError<bool>( "a Prefab type names mesh '{}' as well; it places prefab "
                                                     "'{}' and draws no mesh of its own",
                                                     data.Mesh.Path, data.Prefab.Path );
        if ( data.CullDistance != FoliageFloatInterval{ 0.0f, 0.0f } || data.Wind.Strength != 0.0f ||
             data.IncludeInHLOD )
            return Common::MakeFormattedError<bool>( "a Prefab type sets CullDistance [{}, {}], Wind.Strength {} or "
                                                     "IncludeInHLOD {}: {}",
                                                     data.CullDistance.Min, data.CullDistance.Max,
                                                     data.Wind.Strength, data.IncludeInHLOD,
                                                     kFoliagePrefabMeshOnlyReason );
        return BOOLSUCCESS;
    }

    Common::ResultStr<FoliageTypeData> ParseFoliageType( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<FoliageTypeData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kFoliageTypeVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<FoliageTypeData>( "foliage type {}", headed.GetError() );

        // THE GENERATION BEFORE THE BODY: a v1 file lacks v2's fields, and the typed read would name those
        // missing fields instead of the one fact that matters — the file is an older generation.
        if ( const auto stated = StatedFoliageTypeGeneration( text );
             stated && *stated != static_cast<uint32_t>( kFoliageTypeVersion ) )
            return Common::MakeFormattedError<FoliageTypeData>(
                 "foliage type states FOLT {}, and this build reads FOLT {} only{}", *stated, kFoliageTypeVersion,
                 *stated < static_cast<uint32_t>( kFoliageTypeVersion )
                      ? " (scripts/Dev/migrate.sh --write raises it)"
                      : "" );

        const auto parsed = Common::Json::Read<FoliageTypeData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<FoliageTypeData>( "{}", parsed.GetError() );
        FoliageTypeData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::FoliageType,
                                                      Assets::kFoliageTypeSchemaTag, kFoliageTypeVersion,
                                                      FoliageTypeTextSubsystems() );
             !header )
            return Common::MakeFormattedError<FoliageTypeData>( "foliage type {}", header.GetError() );

        if ( auto valid = ValidateFoliageTypeData( data ); !valid )
            return Common::MakeFormattedError<FoliageTypeData>( "{}", valid.GetError() );

        // ONE REFERENCE, TWO STATEMENTS OF IT: the registry reads the edge from the header, the loader from
        // Mesh. A file where they disagree would have the two sides load different meshes.
        if ( data.Header->Dependencies != DependenciesOf( data ) )
            return Common::MakeFormattedError<FoliageTypeData>(
                 "the header's Dependencies ({} entries) do not state exactly the mesh's GUID '{}', the prefab's "
                 "GUID '{}' and the {} landscape layer GUID(s), in that order",
                 data.Header->Dependencies.size(), data.Mesh.Guid, data.Prefab.Guid, data.LandscapeLayers.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteFoliageType( const FoliageTypeData& data )
    {
        FoliageTypeData out      = data;
        out.Header               = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::FoliageType,
                                                            FoliageTypeTextSubsystems() );
        out.Header->Dependencies = DependenciesOf( data );
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveFoliageTypeFile( const std::filesystem::path& path, const FoliageTypeData& data )
    {
        if ( auto valid = ValidateFoliageTypeData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write foliage type '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteFoliageType( data ) );
    }

    Common::ResultStr<FoliageTypeFile> FindOrCreateFoliageTypeFile( const std::filesystem::path& dir,
                                                                    const FoliageTypeData&       wanted,
                                                                    const std::string&           stem )
    {
        if ( stem.empty() )
            return Common::MakeFormattedError<FoliageTypeFile>( "a new foliage type under '{}' needs a name",
                                                                dir.string() );
        FoliageTypeData key = wanted;
        key.Header.reset();

        // Through the one content enumeration: a packaged project's types live in a mounted .dpak.
        std::vector<std::filesystem::path> files;
        for ( const std::filesystem::path& file : Common::Utils::FileSystem::ListFilesRecursive( dir ) )
            if ( file.extension() == kFoliageTypeExtension )
                files.push_back( file );
        std::sort( files.begin(), files.end() );

        for ( const auto& file : files )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !text )
                return Common::MakeFormattedError<FoliageTypeFile>( "{}", text.GetError() );
            auto parsed = ParseFoliageType( text.GetValue() );
            if ( !parsed )
                return Common::MakeFormattedError<FoliageTypeFile>(
                     "foliage type '{}' does not parse, so it cannot be ruled out as the one to reuse: {}",
                     file.string(), parsed.GetError() );
            FoliageTypeData   held = parsed.GetValue();
            const std::string guid = held.Header->Guid;
            held.Header.reset();
            if ( held == key )
                return Common::MakeSuccess( FoliageTypeFile{ file, guid, false } );
        }

        std::error_code       ec;
        std::filesystem::path file = dir / ( stem + kFoliageTypeExtension );
        for ( int n = 1; std::filesystem::exists( file, ec ); ++n )
            file = dir / ( stem + "_" + std::to_string( n ) + kFoliageTypeExtension );
        if ( auto saved = SaveFoliageTypeFile( file, key ); !saved )
            return Common::MakeFormattedError<FoliageTypeFile>( "{}", saved.GetError() );
        // Read back: the GUID is minted by the writer, and the caller records it.
        const auto text = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !text )
            return Common::MakeFormattedError<FoliageTypeFile>( "{}", text.GetError() );
        const auto parsed = ParseFoliageType( text.GetValue() );
        if ( !parsed )
            return Common::MakeFormattedError<FoliageTypeFile>( "foliage type '{}' just written does not read "
                                                                "back: {}",
                                                                file.string(), parsed.GetError() );
        return Common::MakeSuccess( FoliageTypeFile{ file, parsed.GetValue().Header->Guid, true } );
    }
} // namespace Desert::Assets::Serialization
