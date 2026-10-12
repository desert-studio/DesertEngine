#include <Engine/Core/HLODMeshBuilder.hpp>

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/MeshSimplification.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Core
{
    namespace
    {
        // One source mesh in its own space: render vertices and, per section, its triangles (indices into
        // Vertices, not submesh-local).
        struct SourceMesh
        {
            std::vector<Desert::Vertex>             Vertices;
            std::vector<std::vector<Desert::Index>> Sections;
        };

        // The name a part is reported by: its mesh path, else its GUID, else its primitive.
        std::string PartName( const Assets::InstancedStaticMeshComponentSer& part )
        {
            if ( part.MeshPath.has_value() && !part.MeshPath->empty() )
                return *part.MeshPath;
            if ( part.MeshGuid.has_value() && !part.MeshGuid->empty() )
                return *part.MeshGuid;
            return part.Primitive.has_value() ? std::format( "primitive {}", static_cast<int>( *part.Primitive ) )
                                              : std::string( "a part naming no mesh" );
        }

        Common::ResultStr<SourceMesh> ReadSource( const Assets::InstancedStaticMeshComponentSer& part,
                                                  std::span<const Common::Utils::AssetRegistry>  registries )
        {
            SourceMesh source;
            if ( !part.MeshGuid.has_value() && !part.MeshPath.has_value() )
            {
                if ( !part.Primitive.has_value() )
                    return Common::MakeError<SourceMesh>( "a part names neither a mesh nor a primitive" );
                auto shape = Geometry::MakePrimitive( *part.Primitive );
                if ( !shape.has_value() )
                    return Common::MakeFormattedError<SourceMesh>( "{} has no geometry to merge",
                                                                   PartName( part ) );
                source.Vertices = std::move( shape->Vertices );
                source.Sections.push_back( std::move( shape->Indices ) );
                return Common::MakeSuccess( std::move( source ) );
            }

            const std::string          guidText = part.MeshGuid.value_or( std::string() );
            const std::string          path     = part.MeshPath.value_or( std::string() );
            Common::Content::AssetGuid guid;
            if ( !guidText.empty() )
            {
                auto parsed = Common::Content::AssetGuidFromText( guidText );
                if ( !parsed )
                    return Common::MakeFormattedError<SourceMesh>( "mesh '{}': {}", PartName( part ),
                                                                   parsed.GetError() );
                guid = parsed.GetValue();
            }
            const Common::Utils::AssetRegistryEntry* row = nullptr;
            for ( const auto& registry : registries )
                if ( ( row = registry.FindByGuidReference( guid, path ) ) != nullptr )
                    break;
            if ( row == nullptr )
                return Common::MakeFormattedError<SourceMesh>( "mesh '{}' is in no asset registry",
                                                               PartName( part ) );

            const auto file  = Common::AssetHandle::PathForStableKey( row->Key );
            auto       bytes = Assets::LoadMeshPlatformData( file );
            if ( !bytes )
                return Common::MakeFormattedError<SourceMesh>( "mesh '{}': {}", file.string(), bytes.GetError() );
            auto data = Assets::Serialization::ReadMeshAssetData( bytes.GetValue(), file.string() );
            if ( !data )
                return Common::MakeError<SourceMesh>( data.GetError() );
            auto render = Geometry::RenderFromMeshAssetData( data.GetValue() );
            if ( !render )
                return Common::MakeFormattedError<SourceMesh>( "mesh '{}': {}", file.string(), render.GetError() );

            const Geometry::RenderMeshData& mesh = render.GetValue();
            for ( const Desert::Submesh& submesh : mesh.Submeshes )
            {
                // Submesh-local indices made global, and the submesh's own transform baked, as the draw applies
                // it.
                const auto      base         = static_cast<uint32_t>( source.Vertices.size() );
                const glm::mat3 normalMatrix = glm::inverseTranspose( glm::mat3( submesh.Transform ) );
                for ( uint32_t v = 0; v < submesh.VertexCount; ++v )
                {
                    Desert::Vertex vertex   = mesh.Vertices.at( submesh.VertexOffset + v );
                    vertex.Position         = glm::vec3( submesh.Transform * glm::vec4( vertex.Position, 1.0f ) );
                    vertex.Normal           = glm::normalize( normalMatrix * vertex.Normal );
                    vertex.Tangent          = glm::normalize( glm::mat3( submesh.Transform ) * vertex.Tangent );
                    vertex.Bitangent        = glm::normalize( glm::mat3( submesh.Transform ) * vertex.Bitangent );
                    source.Vertices.push_back( vertex );
                }
                std::vector<Desert::Index>& section = source.Sections.emplace_back();
                for ( uint32_t t = submesh.IndexOffset / 3; t < ( submesh.IndexOffset + submesh.IndexCount ) / 3;
                      ++t )
                {
                    const Desert::Index& local = mesh.Indices.at( t );
                    section.push_back( { base + local.V1, base + local.V2, base + local.V3 } );
                }
            }
            return Common::MakeSuccess( std::move( source ) );
        }
    } // namespace

    Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildHLODMesh( const Rules::HLODMeshRequest&                 request,
                   std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result = Assets::StaticMeshComponentSer;

        // Every material the merged mesh binds, first-seen order: the {GUID, path} text pair a slot states.
        std::vector<std::pair<std::string, std::string>> slots;
        const auto slotOf = [&slots]( const std::pair<std::string, std::string>& key ) -> std::size_t
        {
            const auto found = std::find( slots.begin(), slots.end(), key );
            if ( found != slots.end() )
                return static_cast<std::size_t>( found - slots.begin() );
            slots.push_back( key );
            return slots.size() - 1;
        };
        // Per output section (= slot): its vertices and its section-local triangles.
        std::vector<std::vector<Desert::Vertex>> sectionVertices;
        std::vector<std::vector<Desert::Index>>  sectionTriangles;

        for ( const Assets::InstancedStaticMeshComponentSer& part : request.Parts )
        {
            auto source = ReadSource( part, registries );
            if ( !source )
                return Common::MakeError<Result>( source.GetError() );
            const SourceMesh& mesh = source.GetValue();
            for ( std::size_t section = 0; section < mesh.Sections.size(); ++section )
            {
                std::pair<std::string, std::string> key;
                if ( part.MaterialGuids.has_value() && section < part.MaterialGuids->size() )
                    key.first = ( *part.MaterialGuids )[section];
                if ( part.MaterialPaths.has_value() && section < part.MaterialPaths->size() )
                    key.second = ( *part.MaterialPaths )[section];
                const std::size_t slot = slotOf( key );
                if ( sectionVertices.size() <= slot )
                {
                    sectionVertices.resize( slot + 1 );
                    sectionTriangles.resize( slot + 1 );
                }
                for ( const std::array<float, 16>& flat :
                      part.InstanceTransforms.value_or( std::vector<std::array<float, 16>>{} ) )
                {
                    glm::mat4 world;
                    std::memcpy( &world[0][0], flat.data(), sizeof( float ) * 16 );
                    const glm::mat3 linear       = glm::mat3( world );
                    const glm::mat3 normalMatrix = glm::inverseTranspose( linear );
                    // A mirroring instance flips the winding; swap two corners so the face keeps facing out.
                    const bool                     mirrored = glm::determinant( linear ) < 0.0f;
                    std::vector<Desert::Vertex>&   vertices = sectionVertices[slot];
                    std::map<uint32_t, uint32_t>   local;
                    for ( const Desert::Index& triangle : mesh.Sections[section] )
                    {
                        std::array<uint32_t, 3> corners{ triangle.V1, triangle.V2, triangle.V3 };
                        for ( uint32_t& corner : corners )
                        {
                            const auto [where, fresh] =
                                 local.emplace( corner, static_cast<uint32_t>( vertices.size() ) );
                            if ( fresh )
                            {
                                Desert::Vertex vertex   = mesh.Vertices.at( corner );
                                vertex.Position         = glm::vec3( world * glm::vec4( vertex.Position, 1.0f ) );
                                vertex.Normal           = glm::normalize( normalMatrix * vertex.Normal );
                                vertex.Tangent          = glm::normalize( linear * vertex.Tangent );
                                vertex.Bitangent        = glm::normalize( linear * vertex.Bitangent );
                                vertices.push_back( vertex );
                            }
                            corner = where->second;
                        }
                        if ( mirrored )
                            std::swap( corners[1], corners[2] );
                        sectionTriangles[slot].push_back( { corners[0], corners[1], corners[2] } );
                    }
                }
            }
        }

        // The merged render mesh: one submesh per slot, each with its own contiguous vertex range.
        Geometry::RenderMeshData render;
        for ( std::size_t slot = 0; slot < sectionVertices.size(); ++slot )
        {
            if ( sectionTriangles[slot].empty() )
                continue;
            Desert::Submesh submesh{};
            submesh.Name         = std::format( "Section{}", slot );
            submesh.VertexOffset = static_cast<uint32_t>( render.Vertices.size() );
            submesh.VertexCount  = static_cast<uint32_t>( sectionVertices[slot].size() );
            submesh.IndexOffset  = static_cast<uint32_t>( render.Indices.size() * 3 );
            submesh.IndexCount   = static_cast<uint32_t>( sectionTriangles[slot].size() * 3 );
            submesh.Transform    = glm::mat4( 1.0f );
            render.Vertices.insert( render.Vertices.end(), sectionVertices[slot].begin(),
                                    sectionVertices[slot].end() );
            render.Indices.insert( render.Indices.end(), sectionTriangles[slot].begin(),
                                   sectionTriangles[slot].end() );
            render.Submeshes.push_back( std::move( submesh ) );
            render.SubmeshMaterialIds.push_back( static_cast<int>( slot ) );
        }
        if ( render.Indices.empty() )
            return Common::MakeError<Result>( "the parts hold no triangles to merge" );

        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the merged mesh does not weld: {}", imported.GetError() );
        Geometry::DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );

        if ( request.Type == HLODLayerType::MeshSimplify )
        {
            const int target =
                 std::max( 1, static_cast<int>( std::ceil( static_cast<double>( mesh.TriangleCount() ) *
                                                           request.TrianglePercent ) ) );
            Geometry::QemSimplification simplifier( mesh );
            simplifier.SimplifyToTriangleCount( target );
            // The collapses moved vertices; the normals are the new surface's, not the old corners'.
            if ( mesh.HasAttributes() && mesh.Attributes()->PrimaryNormals() != nullptr )
            {
                Geometry::MeshNormals normals( &mesh );
                normals.RecomputeOverlayNormals( mesh.Attributes()->PrimaryNormals() );
                normals.CopyToOverlay( mesh.Attributes()->PrimaryNormals() );
            }
        }

        Assets::StaticMeshComponentSer block;
        block.EditMesh = Geometry::ToSerialized( mesh );
        // Slots by MaterialID, which is the slot index; written only when some slot names a material.
        const bool named = std::any_of( slots.begin(), slots.end(), []( const auto& slot )
                                        { return !slot.first.empty() || !slot.second.empty(); } );
        if ( named )
        {
            block.MaterialGuids = std::vector<std::string>{};
            block.MaterialPaths = std::vector<std::string>{};
            for ( const auto& [guid, path] : slots )
            {
                block.MaterialGuids->push_back( guid );
                block.MaterialPaths->push_back( path );
            }
        }
        return Common::MakeSuccess( std::move( block ) );
    }

    Rules::HLODMeshBuilder MakeHLODMeshBuilder( std::span<const Common::Utils::AssetRegistry> registries )
    {
        return [registries]( const Rules::HLODMeshRequest& request )
        { return BuildHLODMesh( request, registries ); };
    }
} // namespace Desert::Core
