#include "CollectionFoliageTypes.hpp"

#include <Common/Content/CanonicalText.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <string>

namespace Desert::Editor
{
    namespace
    {
        std::string RelativeTo( const std::filesystem::path& file, const std::filesystem::path& root )
        {
            return file.lexically_normal().lexically_relative( root.lexically_normal() ).generic_string();
        }

        // The recorded type, if its file is still the type the record names.
        Common::ResultStr<Assets::Serialization::FoliageTypeFile>
        OpenRecorded( const CollectionManifestItem& item, const CollectionManifestAssetRef& record,
                      const std::filesystem::path& assetsRoot )
        {
            const std::filesystem::path file = ( assetsRoot / record.Path ).lexically_normal();
            const auto                  text = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !text )
                return Common::MakeFormattedError<Assets::Serialization::FoliageTypeFile>(
                     "collection item '{}' records foliage type '{}' ({}), which cannot be read: {}", item.Name,
                     record.Path, record.Guid, text.GetError() );
            const auto parsed = Assets::Serialization::ParseFoliageType( text.GetValue() );
            if ( !parsed )
                return Common::MakeFormattedError<Assets::Serialization::FoliageTypeFile>(
                     "collection item '{}' records foliage type '{}': {}", item.Name, record.Path,
                     parsed.GetError() );
            const std::string& guid = parsed.GetValue().Header->Guid;
            if ( guid != record.Guid )
                return Common::MakeFormattedError<Assets::Serialization::FoliageTypeFile>(
                     "collection item '{}' records foliage type {} at '{}', but that file is type {}", item.Name,
                     record.Guid, record.Path, guid );
            return Common::MakeSuccess( Assets::Serialization::FoliageTypeFile{ file, guid, false } );
        }
    } // namespace

    Common::ResultStr<CollectionFoliageTypes>
    ResolveCollectionFoliageTypes( CollectionManifest& manifest, const std::filesystem::path& typesDir,
                                   const std::filesystem::path& assetsRoot, const CollectionMeshResolver& meshOf )
    {
        CollectionFoliageTypes out;
        out.Types.reserve( manifest.Items.size() );
        for ( auto& item : manifest.Items )
        {
            if ( item.FoliageType )
            {
                auto recorded = OpenRecorded( item, *item.FoliageType, assetsRoot );
                if ( !recorded )
                    return Common::MakeFormattedError<CollectionFoliageTypes>( "{}", recorded.GetError() );
                out.Types.push_back( recorded.GetValue() );
                continue;
            }

            const auto mesh = meshOf( item );
            if ( !mesh )
                return Common::MakeFormattedError<CollectionFoliageTypes>( "collection item '{}' ('{}'): {}",
                                                                           item.Name, item.Mesh, mesh.GetError() );
            Assets::Serialization::FoliageTypeData wanted;
            wanted.Mesh = mesh.GetValue();

            const std::string stem = std::filesystem::path( item.Mesh ).stem().string();
            auto              type = Assets::Serialization::FindOrCreateFoliageTypeFile( typesDir, wanted,
                                                                            stem.empty() ? item.Name : stem );
            if ( !type )
                return Common::MakeFormattedError<CollectionFoliageTypes>( "collection item '{}': {}", item.Name,
                                                                           type.GetError() );
            item.FoliageType    = CollectionManifestAssetRef{ type.GetValue().Guid,
                                                           RelativeTo( type.GetValue().Path, assetsRoot ) };
            out.ManifestChanged = true;
            out.Types.push_back( type.GetValue() );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr SaveCollectionManifest( const std::filesystem::path& file,
                                                  const CollectionManifest&    manifest )
    {
        if ( auto written =
                  Common::Content::WriteCanonicalJsonFileAtomic( file, WriteCollectionManifest( manifest ) );
             !written )
            return Common::MakeFormattedError<bool>( "cannot write '{}': {}", file.string(), written.GetError() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor
