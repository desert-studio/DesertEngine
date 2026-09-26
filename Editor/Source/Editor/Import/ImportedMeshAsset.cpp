#include "ImportedMeshAsset.hpp"

#include "CookPaths.hpp"

#include <Common/Core/AssetHandle.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    std::pair<std::string, int> ParseSourceModelLOD( const std::string& name )
    {
        std::string lower = name;
        std::ranges::transform( lower, lower.begin(),
                                []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        const auto pos = lower.rfind( "_lod" );
        if ( pos == std::string::npos )
            return { name, 0 };
        const std::string digits = name.substr( pos + 4 );
        if ( digits.empty() || digits.size() > 3 ||
             !std::ranges::all_of( digits, []( unsigned char c ) { return std::isdigit( c ) != 0; } ) )
            return { name, 0 };
        return { name.substr( 0, pos ), std::stoi( digits ) };
    }

    Common::ResultStr<Assets::MeshSourceData>
    MeshSourceFromImport( const Ser::MeshAssetData& imported, std::span<const Assets::MeshMaterialSlot> named,
                          const std::string& name )
    {
        using Result = Assets::MeshSourceData;
        if ( imported.IsSkinned )
            return Common::MakeFormattedError<Result>( "'{}' is skinned; its source is AF4f's", name );
        if ( !imported.MorphTargets.empty() )
            return Common::MakeFormattedError<Result>(
                 "'{}' has {} morph target(s), and a static mesh source has no morph layer to keep them in", name,
                 imported.MorphTargets.size() );
        if ( imported.Submeshes.empty() )
            return Common::MakeFormattedError<Result>( "'{}' has no submesh", name );

        std::map<int, std::vector<size_t>> levels;
        for ( size_t s = 0; s < imported.Submeshes.size(); ++s )
            levels[ParseSourceModelLOD( imported.Submeshes[s].Name ).second].push_back( s );
        int expected = 0;
        for ( const auto& [level, submeshes] : levels )
            if ( level != expected++ )
                return Common::MakeFormattedError<Result>( "'{}' has submeshes for LOD{} but none for LOD{}; "
                                                           "authored LODs must be numbered without gaps",
                                                           name, level, expected - 1 );

        Result     out;
        const auto slotOf = [&]( const Ser::SubmeshData& sub )
        {
            for ( size_t i = 0; i < out.MaterialSlots.size(); ++i )
                if ( out.MaterialSlots[i].Material == sub.MaterialGuid )
                    return static_cast<int>( i );
            const auto known = std::ranges::find_if( named, [&]( const auto& slot )
                                                     { return slot.Material == sub.MaterialGuid; } );
            out.MaterialSlots.push_back(
                 { known != named.end() ? known->Name : ParseSourceModelLOD( sub.Name ).first,
                   sub.MaterialGuid } );
            return static_cast<int>( out.MaterialSlots.size() - 1 );
        };

        const bool groupsPerFace = imported.PolyGroups.size() == imported.Indices.size();
        for ( const auto& [level, submeshes] : levels )
        {
            Ser::MeshAssetData part;
            std::vector<int>   slotOfSubmesh;
            for ( const size_t s : submeshes )
            {
                const Ser::SubmeshData& sub       = imported.Submeshes[s];
                const size_t            firstFace = sub.IndexOffset / 3;
                if ( sub.VertexOffset + sub.VertexCount > imported.StaticVertices.size() ||
                     firstFace + sub.IndexCount / 3 > imported.Indices.size() )
                    return Common::MakeFormattedError<Result>( "'{}' submesh '{}' reaches past the mesh's arrays",
                                                               name, sub.Name );
                Ser::SubmeshData copy = sub;
                copy.VertexOffset     = static_cast<uint32_t>( part.StaticVertices.size() );
                copy.IndexOffset      = static_cast<uint32_t>( part.Indices.size() * 3 );
                copy.LODs.clear();
                // Iterator offsets are signed; the bounds were checked above, so these cannot overflow.
                const auto vertexBegin = static_cast<std::ptrdiff_t>( sub.VertexOffset );
                const auto vertexEnd   = static_cast<std::ptrdiff_t>( sub.VertexOffset ) +
                                       static_cast<std::ptrdiff_t>( sub.VertexCount );
                const auto faceBegin = static_cast<std::ptrdiff_t>( firstFace );
                const auto faceEnd   = static_cast<std::ptrdiff_t>( firstFace + sub.IndexCount / 3 );
                part.StaticVertices.insert( part.StaticVertices.end(),
                                            imported.StaticVertices.begin() + vertexBegin,
                                            imported.StaticVertices.begin() + vertexEnd );
                part.Indices.insert( part.Indices.end(), imported.Indices.begin() + faceBegin,
                                     imported.Indices.begin() + faceEnd );
                if ( groupsPerFace )
                    part.PolyGroups.insert( part.PolyGroups.end(), imported.PolyGroups.begin() + faceBegin,
                                            imported.PolyGroups.begin() + faceEnd );
                part.Submeshes.push_back( std::move( copy ) );
                slotOfSubmesh.push_back( slotOf( sub ) );
            }

            auto lifted = Geometry::Bridge::EditMeshFromMeshAssetData( part );
            if ( !lifted.IsSuccess() )
                return Common::MakeFormattedError<Result>( "'{}' LOD{} cannot become a source model: {}", name,
                                                           level, lifted.GetError() );
            Geometry::EditMesh mesh = lifted.ExtractValue();
            // FromMeshAssetData numbers materials by submesh; the source numbers them by the shared slot table.
            for ( int t = 0; t < mesh.MaxTriangleId(); ++t )
                if ( mesh.IsTriangle( t ) )
                    mesh.Attributes().SetMaterialId(
                         t, slotOfSubmesh[static_cast<size_t>( mesh.Attributes().GetMaterialId( t ) )] );
            out.Models.push_back( { Geometry::Bridge::SavedFormFromEditMesh( mesh ) } );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    namespace
    {
        Common::ResultStr<uint64_t> SourceFileHash( const std::filesystem::path& source )
        {
            const auto bytes = Common::Utils::FileSystem::ReadFileContent( source );
            if ( !bytes )
                return Common::MakeFormattedError<uint64_t>( "'{}' cannot be read: {}", source.string(),
                                                             bytes.GetError() );
            return Common::MakeSuccess(
                 Common::Utils::PakContentHash( bytes.GetValue().data(), bytes.GetValue().size() ) );
        }
    } // namespace

    bool ImportedMeshAssetIsFresh( const std::filesystem::path& source )
    {
        std::error_code ec;
        const auto      file = CookPaths::MeshAsset( source );
        if ( !std::filesystem::exists( file, ec ) )
            return false;
        const auto asset = Assets::ReadMeshSourceAssetFile( file );
        const auto hash  = SourceFileHash( source );
        return asset && hash && asset.GetValue().Import.Provenance == Assets::MeshSourceProvenance::Imported &&
               asset.GetValue().Import.SourceFile == Common::AssetHandle::StableKeyForPath( source ) &&
               asset.GetValue().Import.SourceHash == hash.GetValue();
    }

    Common::ResultStr<MeshAssetWrite> WriteImportedMeshAsset( const Ser::MeshAssetData&                 imported,
                                                              std::span<const Assets::MeshMaterialSlot> named,
                                                              const std::filesystem::path&              source )
    {
        const auto file = CookPaths::MeshAsset( source );

        std::optional<Assets::MeshSourceAsset> existing;
        std::error_code                        ec;
        if ( std::filesystem::exists( file, ec ) )
        {
            auto read = Assets::ReadMeshSourceAssetFile( file );
            if ( !read )
                return Common::MakeFormattedError<MeshAssetWrite>(
                     "'{}' exists and is not a mesh asset ({}); it is not overwritten - move it away to re-import "
                     "'{}'",
                     file.string(), read.GetError(), source.string() );
            existing = read.ExtractValue();
        }

        const auto hash = SourceFileHash( source );
        if ( !hash )
            return Common::MakeError<MeshAssetWrite>( hash.GetError() );

        Assets::MeshSourceAsset asset;
        asset.Kind              = Common::Content::ContentKind::StaticMesh;
        asset.Guid              = existing ? existing->Guid : Common::Content::AssetGuid::Generate();
        asset.Name              = source.stem().string();
        asset.Import.SourceFile = Common::AssetHandle::StableKeyForPath( source );
        asset.Import.SourceHash = hash.GetValue();
        if ( existing )
            asset.Import.Settings = existing->Import.Settings;
        auto sourceData = MeshSourceFromImport( imported, named, source.string() );
        if ( !sourceData )
            return Common::MakeError<MeshAssetWrite>( sourceData.GetError() );
        asset.Source = sourceData.ExtractValue();

        if ( existing && *existing == asset )
            return Common::MakeSuccess( MeshAssetWrite::Unchanged );
        std::filesystem::create_directories( file.parent_path(), ec );
        if ( auto wrote = Assets::WriteMeshSourceAssetFile( file, asset ); !wrote )
            return Common::MakeError<MeshAssetWrite>( wrote.GetError() );
        return Common::MakeSuccess( MeshAssetWrite::Written );
    }
} // namespace Desert::Editor
