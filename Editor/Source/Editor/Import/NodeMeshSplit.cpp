#include "NodeMeshSplit.hpp"
#include "SourceToEngine.hpp"

#include "CookPaths.hpp"
#include "ImportedMeshAsset.hpp"
#include "StaticMeshOutput.hpp"

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <span>
#include <utility>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    namespace
    {
        // FNV-1a over the bytes, from @p basis: two different bases give the GUID's two halves.
        uint64_t Fnv1a( std::string_view bytes, uint64_t basis )
        {
            uint64_t hash = basis;
            for ( const char c : bytes )
            {
                hash ^= static_cast<unsigned char>( c );
                hash *= 0x100000001B3ULL;
            }
            return hash;
        }

        std::string GroupName( const std::string& node )
        {
            const std::string base = ParseSourceModelLOD( node ).first;
            return base.empty() ? std::string( "Mesh" ) : base;
        }
    } // namespace

    Common::ResultStr<std::vector<NodeMesh>> SplitStaticMeshByNode( const Ser::MeshAssetData&    combined,
                                                                    std::span<const std::string> submeshNodes )
    {
        using Result = std::vector<NodeMesh>;
        if ( combined.IsSkinned )
            return Common::MakeFormattedError<Result>( "a skinned mesh is not split by node" );
        if ( !combined.MorphTargets.empty() )
            return Common::MakeFormattedError<Result>( "a mesh with {} morph target(s) is not split by node",
                                                       combined.MorphTargets.size() );
        if ( submeshNodes.size() != combined.Submeshes.size() )
            return Common::MakeFormattedError<Result>( "{} node name(s) for {} submesh(es)", submeshNodes.size(),
                                                       combined.Submeshes.size() );

        const bool groupsPerFace = combined.PolyGroups.size() == combined.Indices.size();
        const bool hasColors     = !combined.Colors.empty();
        const bool hasUV1        = !combined.UV1.empty();

        Result out;
        for ( std::size_t s = 0; s < combined.Submeshes.size(); ++s )
        {
            const std::string name = GroupName( submeshNodes[s] );
            auto it = std::ranges::find_if( out, [&]( const NodeMesh& n ) { return n.Node == name; } );
            if ( it == out.end() )
            {
                out.push_back( { name, {} } );
                it = std::prev( out.end() );
            }
            Ser::MeshAssetData&     mesh = it->Mesh;
            const Ser::SubmeshData& from = combined.Submeshes[s];
            if ( static_cast<std::size_t>( from.VertexOffset ) + from.VertexCount >
                      combined.StaticVertices.size() ||
                 ( static_cast<std::size_t>( from.IndexOffset ) + from.IndexCount ) / 3 > combined.Indices.size() )
                return Common::MakeFormattedError<Result>(
                     "submesh '{}' of node '{}' reaches past the mesh's arrays", from.Name, submeshNodes[s] );

            Ser::SubmeshData sub = from;
            sub.VertexOffset     = static_cast<uint32_t>( mesh.StaticVertices.size() );
            sub.IndexOffset      = static_cast<uint32_t>( mesh.Indices.size() * 3 );
            // The submesh's range of one of the combined mesh's arrays, appended to the node's.
            const auto append = []( auto& into, const auto& all, std::size_t first, std::size_t count )
            {
                const auto range = std::span( all ).subspan( first, count );
                into.insert( into.end(), range.begin(), range.end() );
            };
            append( mesh.StaticVertices, combined.StaticVertices, from.VertexOffset, from.VertexCount );
            if ( hasColors )
                append( mesh.Colors, combined.Colors, from.VertexOffset, from.VertexCount );
            if ( hasUV1 )
                append( mesh.UV1, combined.UV1, from.VertexOffset, from.VertexCount );
            const std::size_t firstFace = from.IndexOffset / 3;
            const std::size_t faces     = from.IndexCount / 3;
            // Indices are submesh-local (the importer's convention), so they are copied unchanged.
            append( mesh.Indices, combined.Indices, firstFace, faces );
            if ( groupsPerFace )
                append( mesh.PolyGroups, combined.PolyGroups, firstFace, faces );
            mesh.Submeshes.push_back( std::move( sub ) );
        }

        for ( NodeMesh& node : out )
        {
            const auto box = Ser::MeshDataBounds( node.Mesh );
            if ( !box )
                return Common::MakeFormattedError<Result>( "node '{}' has no submesh", node.Node );
            const glm::vec3 pivot{ ( box->Min.x + box->Max.x ) * 0.5f, box->Min.y,
                                   ( box->Min.z + box->Max.z ) * 0.5f };
            for ( auto& v : node.Mesh.StaticVertices )
                v.Position = { v.Position.x - pivot.x, v.Position.y - pivot.y, v.Position.z - pivot.z };
            for ( auto& sub : node.Mesh.Submeshes )
                sub.BoundingBox = { sub.BoundingBox.Min - pivot, sub.BoundingBox.Max - pivot };
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<std::vector<NodeMesh>> NodeMeshesOfImport( const Ser::MeshAssetData&    combined,
                                                                 std::span<const std::string> submeshNodes,
                                                                 const std::filesystem::path& source )
    {
        using Result        = std::vector<NodeMesh>;
        const auto settings = Ser::ReadImportRecordSettings( source );
        if ( !settings )
            return Common::MakeError<Result>( settings.GetError() );
        if ( settings.GetValue().CombineMeshes )
            return Common::MakeSuccess( Result{} );
        auto split = SplitStaticMeshByNode( combined, submeshNodes );
        if ( !split )
            return Common::MakeFormattedError<Result>( "'{}' was not split by node: {}", source.string(),
                                                       split.GetError() );
        if ( split.GetValue().size() < 2 )
            return Common::MakeSuccess( Result{} );
        return split;
    }

    Common::Content::AssetGuid NodeMeshGuid( const std::filesystem::path& source, std::string_view node )
    {
        const std::string key =
             std::format( "{}\n{}", CookPaths::MeshRelativeId( source ).generic_string(), node );
        Common::Content::AssetGuid guid{ Fnv1a( key, 0xCBF29CE484222325ULL ),
                                         Fnv1a( key, 0x84222325CBF29CE4ULL ) };
        if ( guid.IsNull() )
            guid.Lo = 1; // the null GUID means "none"; this key never names nothing
        return guid;
    }

    std::filesystem::path NodeMeshAssetPath( const std::filesystem::path& source, std::string_view node )
    {
        return source.parent_path() /
               std::format( "{}{}", StaticMeshAssetName( std::format( "{}_{}", source.stem().string(), node ) ),
                            Common::Constants::Extensions::STATIC_MESH );
    }

    Common::ResultStr<std::filesystem::path> WriteNodeMeshAsset( const NodeMesh&                           node,
                                                                 std::span<const Assets::MeshMaterialSlot> named,
                                                                 const std::filesystem::path&              source )
    {
        using Result                     = std::filesystem::path;
        const std::filesystem::path path = NodeMeshAssetPath( source, node.Node );
        const auto                  hash = Assets::HashMeshSourceFile( source );
        if ( !hash )
            return Common::MakeError<Result>( hash.GetError() );
        const auto settings = Ser::ReadImportRecordSettings( source );
        if ( !settings )
            return Common::MakeError<Result>( settings.GetError() );
        auto sourceData = MeshSourceFromImport( node.Mesh, named, path.string() );
        if ( !sourceData )
            return Common::MakeError<Result>( sourceData.GetError() );
        if ( const std::string warning = SkippedFacesWarning( sourceData.GetValue(), path.string() );
             !warning.empty() )
            LOG_WARN( "{}", warning );

        Assets::MeshSourceAsset asset;
        asset.Kind              = Common::Content::ContentKind::StaticMesh;
        asset.Guid              = NodeMeshGuid( source, node.Node );
        asset.Name              = path.stem().string();
        asset.Import.SourceFile = Common::AssetHandle::StableKeyForPath( source );
        asset.Import.SourceHash = hash.GetValue();
        asset.Import.Settings   = settings.GetValue().Mesh;
        asset.Source            = std::move( sourceData.ExtractValue().Source );
        if ( auto written = Assets::WriteMeshSourceAssetFile( path, asset ); !written )
            return Common::MakeFormattedError<Result>( "'{}' could not be written: {}", path.generic_string(),
                                                       written.GetError() );
        return Common::MakeSuccess( path );
    }

    Common::BoolResultStr RecordImport( const std::filesystem::path&        source,
                                        const Common::Content::ContentKind  kind,
                                        const Ser::MeshAssetData*           imported,
                                        const Assets::SourceImportSettings& settings )
    {
        std::optional<Common::Math::AABB> box;
        if ( imported != nullptr )
            if ( const auto sourceBox = Ser::MeshDataBounds( *imported ) )
                box = SourceToEngineBounds( *sourceBox, settings.Mesh );
        if ( auto identity = Ser::EnsureImportRecord( source, kind, box, settings ); !identity )
            return Common::MakeError<bool>( identity.GetError() );
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::vector<std::pair<NodeMesh, std::filesystem::path>>>
    WriteStaticMeshImport( const Ser::MeshAssetData& imported, std::span<const std::string> submeshNodes,
                           std::span<const Assets::MeshMaterialSlot> named, const std::filesystem::path& source,
                           const Assets::SourceImportSettings& settings )
    {
        using Result = std::vector<std::pair<NodeMesh, std::filesystem::path>>;
        if ( !Ser::MeshDataBounds( imported ) )
            return Common::MakeFormattedError<Result>( "'{}': the import has no submesh, so no box",
                                                       source.string() );
        if ( auto recorded = RecordImport( source, Common::Content::ContentKind::StaticMesh, &imported, settings );
             !recorded )
            return Common::MakeError<Result>( recorded.GetError() );

        auto split = NodeMeshesOfImport( imported, submeshNodes, source );
        if ( !split )
            return Common::MakeError<Result>( split.GetError() );
        if ( split.GetValue().empty() )
        {
            if ( auto written = WriteImportedMeshAsset( imported, named, source ); !written )
                return Common::MakeError<Result>( written.GetError() );
            if ( auto cleared = Ser::SetImportRecordNodes( source, std::nullopt ); !cleared )
                return Common::MakeError<Result>( cleared.GetError() );
            return Common::MakeSuccess( Result{} );
        }

        Result                   out;
        std::vector<std::string> names;
        std::string              firstFailure;
        for ( NodeMesh& node : split.ExtractValue() )
        {
            auto written = WriteNodeMeshAsset( node, named, source );
            if ( !written )
            {
                if ( firstFailure.empty() )
                    firstFailure = written.GetError();
                continue;
            }
            names.push_back( node.Node );
            out.emplace_back( std::move( node ), written.ExtractValue() );
        }
        if ( auto recorded = Ser::SetImportRecordNodes( source, names ); !recorded && firstFailure.empty() )
            firstFailure = recorded.GetError();
        if ( !firstFailure.empty() )
            return Common::MakeError<Result>( firstFailure );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Editor
