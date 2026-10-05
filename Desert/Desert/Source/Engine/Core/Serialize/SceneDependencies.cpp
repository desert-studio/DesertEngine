#include "SceneDependencies.hpp"

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <algorithm>
#include <fstream>

namespace Desert::Core
{
    std::optional<std::string> StatedPathGuid( const std::string&           statedPath,
                                               const std::filesystem::path& assetsRoot )
    {
        namespace fs = std::filesystem;
        if ( statedPath.empty() )
            return std::nullopt;
        std::error_code ec;
        const fs::path  relative( statedPath );
        if ( relative.is_absolute() || !relative.has_extension() )
            return std::nullopt;
        fs::path file;
        for ( fs::path at = fs::absolute( assetsRoot, ec ).lexically_normal(); !at.empty(); at = at.parent_path() )
        {
            if ( fs::is_regular_file( at / relative, ec ) )
            {
                file = at / relative;
                break;
            }
            if ( at == at.parent_path() )
                break;
        }
        if ( file.empty() )
            return std::nullopt;

        // A cooked mesh binary states its GUID in its own prefix; every other kind in the asset header.
        {
            std::ifstream in( file, std::ios::binary );
            std::string   prefix( Common::Content::kMeshBinaryPrefixSize, '\0' );
            in.read( prefix.data(), static_cast<std::streamsize>( prefix.size() ) );
            prefix.resize( static_cast<std::size_t>( in.gcount() ) );
            if ( const auto guid = Common::Content::ReadMeshHeaderGuid( prefix ); guid && !guid->IsNull() )
                return Common::Content::AssetGuidToText( Common::Content::AssetGuid( *guid ) );
        }
        const auto header =
             Common::Content::ReadAssetHeaderIfStated( file, Common::Content::AssetHeaderReadContext{ {}, true } );
        if ( !header || !header.GetValue().has_value() || header.GetValue()->Guid.IsNull() )
            return std::nullopt;
        return Common::Content::AssetGuidToText( header.GetValue()->Guid );
    }

    namespace
    {
        void AddText( const Common::Json::Value& value, std::vector<std::string>& out )
        {
            if ( const auto text = value.to_string(); text.has_value() && !text.value().empty() )
                out.push_back( text.value() );
        }

        void Collect( const Common::Json::Value& value, const std::filesystem::path& assetsRoot,
                      std::vector<std::string>& out )
        {
            if ( const auto object = value.to_object(); object.has_value() )
            {
                bool statesGuid = false;
                for ( const auto& [key, field] : object.value() )
                    statesGuid = statesGuid || key == "Guid";
                for ( const auto& [key, field] : object.value() )
                {
                    if ( key == "Header" )
                    {
                        // A hosted block's identity is not a reference; what IT depends on is the scene's too.
                        if ( const auto header = field.to_object(); header.has_value() )
                            for ( const auto& [hk, hv] : header.value() )
                                if ( hk == "Dependencies" )
                                    if ( const auto list = hv.to_array(); list.has_value() )
                                        for ( const auto& element : list.value() )
                                            AddText( element, out );
                        continue;
                    }
                    if ( key == "Guid" || key == "MeshGuid" )
                    {
                        AddText( field, out );
                        continue;
                    }
                    if ( key == "MaterialGuids" )
                    {
                        if ( const auto list = field.to_array(); list.has_value() )
                            for ( const auto& element : list.value() )
                                AddText( element, out );
                        continue;
                    }
                    if ( statesGuid && key == "Path" )
                        continue; // the Guid beside it is the reference
                    Collect( field, assetsRoot, out );
                }
            }
            else if ( const auto list = value.to_array(); list.has_value() )
            {
                for ( const auto& element : list.value() )
                    Collect( element, assetsRoot, out );
            }
            else if ( const auto text = value.to_string(); text.has_value() )
            {
                if ( auto guid = StatedPathGuid( text.value(), assetsRoot ) )
                    out.push_back( std::move( *guid ) );
            }
        }
    } // namespace

    SceneDependencyGather GatherSceneDependencies( const std::vector<Assets::EntityData>& entities,
                                                   const Common::Json::Value*             settings,
                                                   const std::filesystem::path&           assetsRoot )
    {
        SceneDependencyGather gather;
        for ( const auto& record : entities )
        {
            for ( const auto& [key, block] : record.Components )
                Collect( block, assetsRoot, gather.Guids );
            if ( !record.PrefabPath.has_value() )
                continue;
            if ( auto guid = StatedPathGuid( *record.PrefabPath, assetsRoot ) )
                gather.Guids.push_back( std::move( *guid ) );
            else
                gather.Refused.push_back( "prefab '" + *record.PrefabPath + "': no file under an ancestor of " +
                                          assetsRoot.generic_string() + " states its GUID" );
        }
        if ( settings != nullptr )
            Collect( *settings, assetsRoot, gather.Guids );
        std::sort( gather.Guids.begin(), gather.Guids.end() );
        gather.Guids.erase( std::unique( gather.Guids.begin(), gather.Guids.end() ), gather.Guids.end() );
        return gather;
    }
} // namespace Desert::Core
