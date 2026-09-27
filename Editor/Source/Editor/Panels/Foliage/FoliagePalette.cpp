#include "FoliagePalette.hpp"

#include <Editor/Panels/Collections/CollectionFoliageTypes.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>

#include <algorithm>
#include <cctype>

namespace Desert::Editor::Foliage
{
    FoliageTypeCost MeasureFoliageTypeCost( std::span<const glm::mat4>                         instances,
                                            const Assets::Serialization::FoliageFloatInterval& cullDistance,
                                            const glm::vec3& view, uint64_t trianglesPerInstance, bool hidden )
    {
        FoliageTypeCost cost;
        cost.Instances            = instances.size();
        cost.TrianglesPerInstance = trianglesPerInstance;
        if ( hidden )
            return cost;
        const Graphic::InstanceCullDistance cull{ cullDistance.Min, cullDistance.Max };
        for ( size_t i = 0; i < instances.size(); ++i )
            if ( Graphic::KeepsInstanceAtDistance( cull, static_cast<uint32_t>( i ), glm::vec3( instances[i][3] ),
                                                   view ) )
                ++cost.InCullRange;
        cost.Triangles = static_cast<uint64_t>( cost.InCullRange ) * trianglesPerInstance;
        return cost;
    }

    FoliageFootprint PreviewFoliageFootprint( float density, float radius, float paintDensity,
                                              std::span<const glm::mat4> instances, const glm::vec3& centre )
    {
        FoliageFootprint out;
        out.Desired    = Tools::FoliageBrushDesiredCount( density, radius, paintDensity );
        const float r2 = radius * radius;
        for ( const auto& m : instances )
        {
            const glm::vec3 d = glm::vec3( m[3] ) - centre;
            if ( glm::dot( d, d ) <= r2 )
                ++out.Existing;
        }
        out.Expected = std::max( 0.0f, out.Desired - static_cast<float>( out.Existing ) );
        return out;
    }

    bool PaletteNameMatches( std::string_view name, std::string_view filter )
    {
        const auto  lower = []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); };
        std::string hay( name.size(), '\0' );
        std::ranges::transform( name, hay.begin(), lower );
        size_t at = 0;
        while ( at < filter.size() )
        {
            while ( at < filter.size() && std::isspace( static_cast<unsigned char>( filter[at] ) ) )
                ++at;
            size_t end = at;
            while ( end < filter.size() && !std::isspace( static_cast<unsigned char>( filter[end] ) ) )
                ++end;
            if ( end > at )
            {
                std::string word( end - at, '\0' );
                std::transform( filter.begin() + static_cast<std::ptrdiff_t>( at ),
                                filter.begin() + static_cast<std::ptrdiff_t>( end ), word.begin(), lower );
                if ( hay.find( word ) == std::string::npos )
                    return false;
            }
            at = end;
        }
        return true;
    }

    Common::ResultStr<std::filesystem::path>
    SaveFoliageTypeCopy( const std::filesystem::path& source, const Assets::Serialization::FoliageTypeData& data )
    {
        if ( source.extension() != Assets::Serialization::kFoliageTypeExtension )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "'{}' is not a foliage type ({})", source.string(),
                 Assets::Serialization::kFoliageTypeExtension );
        std::error_code             ec;
        const std::filesystem::path dir  = source.parent_path();
        const std::string           stem = source.stem().string() + "_Copy";
        std::filesystem::path       file = dir / ( stem + Assets::Serialization::kFoliageTypeExtension );
        for ( int n = 1; std::filesystem::exists( file, ec ); ++n )
            file = dir / ( stem + "_" + std::to_string( n ) + Assets::Serialization::kFoliageTypeExtension );
        Assets::Serialization::FoliageTypeData copy = data;
        copy.Header.reset(); // the writer mints the copy's own GUID
        if ( auto saved = Assets::Serialization::SaveFoliageTypeFile( file, copy ); !saved )
            return Common::MakeFormattedError<std::filesystem::path>( "{}", saved.GetError() );
        return Common::MakeSuccess( file );
    }

    Common::ResultStr<std::filesystem::path> SavePalettePreset( const std::filesystem::path& collectionsDir,
                                                                const std::string&           name,
                                                                const std::vector<PalettePresetEntry>& entries )
    {
        if ( entries.empty() )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "preset '{}': the palette lists no foliage type to save", name );
        const bool plain =
             !name.empty() &&
             std::ranges::all_of( name, []( unsigned char c )
                                  { return std::isalnum( c ) || c == '_' || c == '-' || c == ' '; } ) &&
             name.front() != ' ' && name.back() != ' ';
        if ( !plain )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "preset name '{}' is not a plain folder name (letters, digits, '_', '-', inner spaces)", name );
        std::error_code             ec;
        const std::filesystem::path dir = collectionsDir / name;
        if ( std::filesystem::exists( dir, ec ) )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "preset '{}': '{}' already exists; a preset does not overwrite a collection", name,
                 dir.string() );

        CollectionManifest manifest;
        manifest.Name = name;
        for ( const auto& entry : entries )
        {
            CollectionManifestItem item;
            item.Name        = entry.Name;
            item.Mesh        = entry.MeshPath;
            item.FoliageType = CollectionManifestAssetRef{ entry.Type.Guid, entry.Type.Path };
            manifest.Items.push_back( std::move( item ) );
        }
        std::filesystem::create_directories( dir, ec );
        if ( ec )
            return Common::MakeFormattedError<std::filesystem::path>( "preset '{}': '{}' cannot be created: {}",
                                                                      name, dir.string(), ec.message() );
        const std::filesystem::path file = dir / "collection.json";
        if ( auto saved = SaveCollectionManifest( file, manifest ); !saved )
            return Common::MakeFormattedError<std::filesystem::path>( "preset '{}': {}", name, saved.GetError() );
        return Common::MakeSuccess( file );
    }
} // namespace Desert::Editor::Foliage
