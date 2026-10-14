#include "SceneDependencies.hpp"

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Document.hpp>

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
        // Only text that IS an asset GUID is a reference: a "Guid" key also names identities that are not assets
        // (a landscape edit layer's id is a decimal 64-bit number), and the header reader refuses anything else.
        void AddAssetGuid( const Common::Json::Node& node, std::vector<std::string>& out )
        {
            if ( const auto text = node.AsString(); text && !text.GetValue().empty() )
                if ( const auto guid = Common::Content::AssetGuidFromText( text.GetValue() );
                     guid.IsSuccess() && !guid.GetValue().IsNull() )
                    out.push_back( text.GetValue() );
        }

        void Collect( const Common::Json::Node& node, const std::filesystem::path& assetsRoot,
                      std::vector<std::string>& out )
        {
            switch ( node.GetKind() )
            {
                case Common::Json::Kind::Object:
                {
                    const bool statesGuid = node.Find( "Guid" ).has_value();
                    node.ForEachMember(
                         [&]( std::string_view key, const Common::Json::Node& field )
                         {
                             if ( key == "Header" )
                             {
                                 // A hosted block's identity is not a reference; what IT depends on is the
                                 // scene's too.
                                 if ( const auto list = field.Find( "Dependencies" ) )
                                     list->ForEachElement( [&]( std::size_t, const Common::Json::Node& element )
                                                           { AddAssetGuid( element, out ); } );
                                 return;
                             }
                             if ( key == "Guid" || key == "MeshGuid" )
                             {
                                 AddAssetGuid( field, out );
                                 return;
                             }
                             if ( key == "MaterialGuids" )
                             {
                                 field.ForEachElement( [&]( std::size_t, const Common::Json::Node& element )
                                                       { AddAssetGuid( element, out ); } );
                                 return;
                             }
                             if ( statesGuid && key == "Path" )
                                 return; // the Guid beside it is the reference
                             Collect( field, assetsRoot, out );
                         } );
                    break;
                }
                case Common::Json::Kind::Array:
                    node.ForEachElement( [&]( std::size_t, const Common::Json::Node& element )
                                         { Collect( element, assetsRoot, out ); } );
                    break;
                case Common::Json::Kind::String:
                    if ( auto guid = StatedPathGuid( node.AsString().GetValue(), assetsRoot ) )
                        out.push_back( std::move( *guid ) );
                    break;
                default:
                    break;
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
                Collect( Common::Json::Root( block ), assetsRoot, gather.Guids );
            if ( !record.PrefabPath.has_value() )
                continue;
            if ( auto guid = StatedPathGuid( *record.PrefabPath, assetsRoot ) )
                gather.Guids.push_back( std::move( *guid ) );
            else
                gather.Refused.push_back( "prefab '" + *record.PrefabPath + "': no file under an ancestor of " +
                                          assetsRoot.generic_string() + " states its GUID" );
        }
        if ( settings != nullptr )
            Collect( Common::Json::Root( *settings ), assetsRoot, gather.Guids );
        std::sort( gather.Guids.begin(), gather.Guids.end() );
        gather.Guids.erase( std::unique( gather.Guids.begin(), gather.Guids.end() ), gather.Guids.end() );
        return gather;
    }
} // namespace Desert::Core
