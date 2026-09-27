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
            if ( data.Mesh.Guid.empty() )
                return {};
            return { data.Mesh.Guid };
        }
    } // namespace

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
        if ( data.Mesh.Guid.empty() != data.Mesh.Path.empty() )
            return Common::MakeFormattedError<bool>(
                 "Mesh names {} without {} ('{}' / '{}')", data.Mesh.Guid.empty() ? "a path" : "a GUID",
                 data.Mesh.Guid.empty() ? "a GUID" : "a path", data.Mesh.Guid, data.Mesh.Path );
        return BOOLSUCCESS;
    }

    Common::ResultStr<FoliageTypeData> ParseFoliageType( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<FoliageTypeData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kFoliageTypeVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<FoliageTypeData>( "foliage type {}", headed.GetError() );

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
                 "the header's Dependencies ({} entries) do not state exactly the mesh's GUID '{}'",
                 data.Header->Dependencies.size(), data.Mesh.Guid );

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

        std::error_code                    ec;
        std::vector<std::filesystem::path> files;
        if ( std::filesystem::exists( dir, ec ) )
        {
            for ( auto it = std::filesystem::recursive_directory_iterator( dir, ec );
                  !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
                if ( it->is_regular_file() && it->path().extension() == kFoliageTypeExtension )
                    files.push_back( it->path() );
            if ( ec )
                return Common::MakeFormattedError<FoliageTypeFile>( "cannot list foliage types under '{}': {}",
                                                                    dir.string(), ec.message() );
        }
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
