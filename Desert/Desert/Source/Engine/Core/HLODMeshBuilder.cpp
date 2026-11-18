#include <Engine/Core/HLODMeshBuilder.hpp>

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/DynamicMeshUVOperations.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>
#include <Engine/Geometry/MeshCore/Parameterization/UVPacking.hpp>
#include <Engine/Geometry/MeshSimplifyOperation.hpp>
#include <Engine/Geometry/MikkTangents.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeSplineMeshes.hpp>
#include <Engine/World/Landscape/LandscapeSplinesJson.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
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
                const Geometry::SurfaceFrameTransform frame( glm::mat3( submesh.Transform ) );
                for ( uint32_t v = 0; v < submesh.VertexCount; ++v )
                {
                    Desert::Vertex vertex = mesh.Vertices.at( submesh.VertexOffset + v );
                    vertex.Position       = glm::vec3( submesh.Transform * glm::vec4( vertex.Position, 1.0f ) );
                    vertex.Normal         = frame.Normal( vertex.Normal );
                    vertex.Tangent        = frame.UnitDirection( vertex.Tangent );
                    vertex.Bitangent      = frame.UnitDirection( vertex.Bitangent );
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

    namespace
    {
        // The parts merged in WORLD space: every distinct material a slot (the {GUID, path} text pair it states,
        // first-seen order), per slot its vertices and its section-local triangles.
        struct MergedParts
        {
            std::vector<std::pair<std::string, std::string>> Slots;
            std::vector<std::vector<Desert::Vertex>>         SectionVertices;
            std::vector<std::vector<Desert::Index>>          SectionTriangles;
        };

        Common::ResultStr<MergedParts> MergeParts( const Rules::HLODMeshRequest&                 request,
                                                   std::span<const Common::Utils::AssetRegistry> registries )
        {
            using Result = MergedParts;
            MergedParts                                       merged;
            std::vector<std::pair<std::string, std::string>>& slots            = merged.Slots;
            std::vector<std::vector<Desert::Vertex>>&         sectionVertices  = merged.SectionVertices;
            std::vector<std::vector<Desert::Index>>&          sectionTriangles = merged.SectionTriangles;
            const auto slotOf = [&slots]( const std::pair<std::string, std::string>& key ) -> std::size_t
            {
                const auto found = std::find( slots.begin(), slots.end(), key );
                if ( found != slots.end() )
                    return static_cast<std::size_t>( found - slots.begin() );
                slots.push_back( key );
                return slots.size() - 1;
            };

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
                        const glm::mat3                       linear = glm::mat3( world );
                        const Geometry::SurfaceFrameTransform frame( linear );
                        // A mirroring instance flips the winding; swap two corners so the face keeps facing out.
                        const bool                   mirrored = glm::determinant( linear ) < 0.0f;
                        std::vector<Desert::Vertex>& vertices = sectionVertices[slot];
                        std::map<uint32_t, uint32_t> local;
                        for ( const Desert::Index& triangle : mesh.Sections[section] )
                        {
                            std::array<uint32_t, 3> corners{ triangle.V1, triangle.V2, triangle.V3 };
                            for ( uint32_t& corner : corners )
                            {
                                const auto [where, fresh] =
                                     local.emplace( corner, static_cast<uint32_t>( vertices.size() ) );
                                if ( fresh )
                                {
                                    Desert::Vertex vertex = mesh.Vertices.at( corner );
                                    vertex.Position  = glm::vec3( world * glm::vec4( vertex.Position, 1.0f ) );
                                    vertex.Normal    = frame.Normal( vertex.Normal );
                                    vertex.Tangent   = frame.UnitDirection( vertex.Tangent );
                                    vertex.Bitangent = frame.UnitDirection( vertex.Bitangent );
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

            return Common::MakeSuccess( std::move( merged ) );
        }

        // The slot list a built block states: written only when some slot names a material.
        std::optional<std::vector<Assets::AssetGuidRef>>
        SlotRefs( const std::vector<std::pair<std::string, std::string>>& slots )
        {
            const bool named = std::any_of( slots.begin(), slots.end(), []( const auto& slot )
                                            { return !slot.first.empty() || !slot.second.empty(); } );
            if ( !named )
                return std::nullopt;
            std::vector<Assets::AssetGuidRef> refs;
            for ( const auto& [guid, path] : slots )
                refs.push_back( Assets::AssetGuidRef{ guid, path } );
            return refs;
        }

        int SimplifyTarget( const Geometry::DynamicMesh3& mesh, float trianglePercent )
        {
            return std::max( 1, static_cast<int>( std::ceil( static_cast<double>( mesh.TriangleCount() ) *
                                                             trianglePercent ) ) );
        }

        // The merged triangles' UV layout as UVPacker reads and writes it: one UV element per render vertex
        // (a render vertex is already split wherever its UV differs), so an island is a set of triangles joined
        // through shared render vertices.
        class RenderUVView final : public Geometry::UVPacker::IUVMeshView
        {
        public:
            RenderUVView( const std::vector<Desert::Vertex>& vertices, const std::vector<Desert::Index>& triangles,
                          std::vector<glm::vec2>& uvs )
                 : m_Vertices( vertices ), m_Triangles( triangles ), m_UVs( uvs )
            {
            }
            [[nodiscard]] Geometry::Index3i GetTriangle( int32_t TID ) const override
            {
                const Desert::Index& t = m_Triangles[static_cast<std::size_t>( TID )];
                return Geometry::Index3i( static_cast<int>( t.V1 ), static_cast<int>( t.V2 ),
                                          static_cast<int>( t.V3 ) );
            }
            [[nodiscard]] Geometry::Index3i GetUVTriangle( int32_t TID ) const override
            {
                return GetTriangle( TID );
            }
            [[nodiscard]] glm::dvec3 GetVertex( int32_t VID ) const override
            {
                return glm::dvec3( m_Vertices[static_cast<std::size_t>( VID )].Position );
            }
            [[nodiscard]] glm::vec2 GetUV( int32_t EID ) const override
            {
                return m_UVs[static_cast<std::size_t>( EID )];
            }
            void SetUV( int32_t EID, glm::vec2 UV ) override
            {
                m_UVs[static_cast<std::size_t>( EID )] = UV;
            }

        private:
            const std::vector<Desert::Vertex>& m_Vertices;
            const std::vector<Desert::Index>&  m_Triangles;
            std::vector<glm::vec2>&            m_UVs;
        };

        // THE ATLAS OF THE MERGED SOURCE (UE MergeComponentsToStaticMesh with bMergeMaterials: the source UV
        // charts packed into one layout): every source UV island - its own UVs, tiling kept - packed into the
        // unit square by UVPacker::StandardPack at the bake's resolution, scaled so a texel covers about the same
        // world length on every island (a far building is sharp evenly, whatever its source tiling). Answers the
        // atlas UV of every render vertex.
        Common::ResultStr<std::vector<glm::vec2>> PackSourceIslands( const std::vector<Desert::Vertex>& vertices,
                                                                     const std::vector<Desert::Index>&  triangles,
                                                                     int textureSize )
        {
            using Result = std::vector<glm::vec2>;
            // Islands: union-find over the render vertices each triangle joins.
            std::vector<uint32_t> parent( vertices.size() );
            for ( std::size_t v = 0; v < parent.size(); ++v )
                parent[v] = static_cast<uint32_t>( v );
            const auto root = [&parent]( uint32_t v )
            {
                while ( parent[v] != v )
                {
                    parent[v] = parent[parent[v]];
                    v         = parent[v];
                }
                return v;
            };
            const auto join = [&]( uint32_t a, uint32_t b )
            {
                const uint32_t ra = root( a );
                const uint32_t rb = root( b );
                if ( ra != rb )
                    parent[rb] = ra;
            };
            for ( const Desert::Index& t : triangles )
            {
                join( t.V1, t.V2 );
                join( t.V2, t.V3 );
            }
            std::map<uint32_t, std::size_t>   islandOfRoot;
            std::vector<std::vector<int32_t>> islands;
            for ( std::size_t t = 0; t < triangles.size(); ++t )
            {
                const uint32_t r          = root( triangles[t].V1 );
                const auto [where, fresh] = islandOfRoot.emplace( r, islands.size() );
                if ( fresh )
                    islands.emplace_back();
                islands[where->second].push_back( static_cast<int32_t>( t ) );
            }

            std::vector<glm::vec2> uvs( vertices.size() );
            for ( std::size_t v = 0; v < vertices.size(); ++v )
                uvs[v] = vertices[v].TexCoord;
            RenderUVView       view( vertices, triangles, uvs );
            Geometry::UVPacker packer;
            packer.TextureResolution                   = textureSize;
            packer.bScaleIslandsByWorldSpaceTexelRatio = true;
            packer.bAllowFlips                         = false;
            packer.bPreserveRotation                   = false;
            const bool packed = packer.StandardPack( &view, static_cast<int32_t>( islands.size() ),
                                                     [&islands]( int32_t island, std::vector<int32_t>& out )
                                                     { out = islands[static_cast<std::size_t>( island )]; } );
            if ( !packed )
                return Common::MakeError<Result>( std::format(
                     "the merged mesh's {} source UV islands do not pack into a {}x{} atlas (no UV area, or more "
                     "islands than texels)",
                     islands.size(), textureSize, textureSize ) );
            return Common::MakeSuccess( std::move( uvs ) );
        }

        // The source surface laid flat on its atlas (HLODMaterialBakeSource::Proxy): every merged triangle with
        // its corners at (u, 0, v) * kHLODBakeProxySideCm of their atlas UV, keeping their source UV and world
        // frame, turned to face +Y (the camera above sees every one front-on), one section per source slot.
        Common::ResultStr<Assets::StaticMeshComponentSer>
        BuildBakeProxy( const MergedParts& merged, const std::vector<std::vector<glm::vec2>>& atlasBySlot )
        {
            using Result = Assets::StaticMeshComponentSer;
            Geometry::RenderMeshData proxy;
            for ( std::size_t slot = 0; slot < merged.SectionVertices.size(); ++slot )
            {
                const std::vector<Desert::Vertex>& vertices = merged.SectionVertices[slot];
                const std::vector<Desert::Index>&  section  = merged.SectionTriangles[slot];
                if ( section.empty() )
                    continue;
                Desert::Submesh submesh{};
                submesh.Name         = std::format( "Source{}", slot );
                submesh.VertexOffset = static_cast<uint32_t>( proxy.Vertices.size() );
                submesh.IndexOffset  = static_cast<uint32_t>( proxy.Indices.size() * 3 );
                submesh.Transform    = glm::mat4( 1.0f );
                uint32_t local       = 0;
                for ( const Desert::Index& triangle : section )
                {
                    std::array<uint32_t, 3>       corners{ triangle.V1, triangle.V2, triangle.V3 };
                    std::array<Desert::Vertex, 3> laid;
                    for ( int c = 0; c < 3; ++c )
                    {
                        laid[c]             = vertices.at( corners[c] );
                        const glm::vec2& uv = atlasBySlot[slot].at( corners[c] );
                        laid[c].Position =
                             glm::vec3( uv.x * kHLODBakeProxySideCm, 0.0f, uv.y * kHLODBakeProxySideCm );
                    }
                    // A render triangle's front is counter-clockwise: (p1 - p0) x (p2 - p0) points at the viewer.
                    const glm::vec3 facing =
                         glm::cross( laid[1].Position - laid[0].Position, laid[2].Position - laid[0].Position );
                    if ( facing.y < 0.0f )
                        std::swap( laid[1], laid[2] );
                    for ( const Desert::Vertex& vertex : laid )
                        proxy.Vertices.push_back( vertex );
                    proxy.Indices.push_back( { local, local + 1, local + 2 } );
                    local += 3;
                }
                submesh.VertexCount = local;
                submesh.IndexCount  = static_cast<uint32_t>( section.size() * 3 );
                proxy.Submeshes.push_back( std::move( submesh ) );
                proxy.SubmeshMaterialIds.push_back( static_cast<int>( slot ) );
            }
            auto imported = Geometry::DynamicMeshFromRenderMesh( proxy );
            if ( !imported )
                return Common::MakeFormattedError<Result>( "the bake proxy does not weld: {}",
                                                           imported.GetError() );
            Assets::StaticMeshComponentSer block;
            block.EditMesh      = Geometry::ToSerialized( imported.GetValue().Mesh );
            block.MaterialSlots = SlotRefs( merged.Slots );
            block.CastShadows   = false;
            return Common::MakeSuccess( std::move( block ) );
        }
    } // namespace

    std::string HLODMeshKey( const Rules::HLODMeshRequest& request )
    {
        // MeshMerge states no TrianglePercent, MeshSimplify its own: the two never share a key.
        std::string identity = request.TrianglePercent.has_value()
                                    ? std::format( "HLODMesh\nSimplify {}\n", *request.TrianglePercent )
                                    : std::string( "HLODMesh\nMerge\n" );
        if ( request.MergeMaterials.has_value() )
            identity += std::format( "Merge {} {} {} {}\n", request.MergeMaterials->TextureSize,
                                     request.MergeMaterials->NormalMap, request.MergeMaterials->ORMMap,
                                     request.MergeMaterials->EmissiveMap );
        for ( const Assets::InstancedStaticMeshComponentSer& part : request.Parts )
        {
            identity += std::format( "Part {}|{}|{}\n", part.MeshGuid.value_or( std::string() ),
                                     part.MeshPath.value_or( std::string() ),
                                     part.Primitive.has_value() ? static_cast<int>( *part.Primitive ) : -1 );
            for ( const std::string& guid : part.MaterialGuids.value_or( std::vector<std::string>{} ) )
                identity += std::format( "G {}\n", guid );
            for ( const std::string& path : part.MaterialPaths.value_or( std::vector<std::string>{} ) )
                identity += std::format( "P {}\n", path );
            for ( const std::array<float, 16>& flat :
                  part.InstanceTransforms.value_or( std::vector<std::array<float, 16>>{} ) )
                for ( const float value : flat )
                    identity += std::format( "{} ", value );
            identity += '\n';
        }
        return std::format( "{:016x}", static_cast<std::uint64_t>(
                                            Common::Utils::PakContentHash( identity.data(), identity.size() ) ) );
    }

    Common::ResultStr<HLODMeshBuild>
    BuildHLODMeshForBake( const Rules::HLODMeshRequest&                 request,
                          std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result     = HLODMeshBuild;
        auto mergedParts = MergeParts( request, registries );
        if ( !mergedParts )
            return Common::MakeError<Result>( mergedParts.GetError() );
        const MergedParts merged = mergedParts.ExtractValue();

        if ( !request.MergeMaterials.has_value() )
        {
            // The merged render mesh: one submesh per slot, each with its own contiguous vertex range.
            Geometry::RenderMeshData render;
            for ( std::size_t slot = 0; slot < merged.SectionVertices.size(); ++slot )
            {
                if ( merged.SectionTriangles[slot].empty() )
                    continue;
                Desert::Submesh submesh{};
                submesh.Name         = std::format( "Section{}", slot );
                submesh.VertexOffset = static_cast<uint32_t>( render.Vertices.size() );
                submesh.VertexCount  = static_cast<uint32_t>( merged.SectionVertices[slot].size() );
                submesh.IndexOffset  = static_cast<uint32_t>( render.Indices.size() * 3 );
                submesh.IndexCount   = static_cast<uint32_t>( merged.SectionTriangles[slot].size() * 3 );
                submesh.Transform    = glm::mat4( 1.0f );
                render.Vertices.insert( render.Vertices.end(), merged.SectionVertices[slot].begin(),
                                        merged.SectionVertices[slot].end() );
                render.Indices.insert( render.Indices.end(), merged.SectionTriangles[slot].begin(),
                                       merged.SectionTriangles[slot].end() );
                render.Submeshes.push_back( std::move( submesh ) );
                render.SubmeshMaterialIds.push_back( static_cast<int>( slot ) );
            }
            if ( render.Indices.empty() )
                return Common::MakeError<Result>( "the parts hold no triangles to merge" );

            auto imported = Geometry::DynamicMeshFromRenderMesh( render );
            if ( !imported )
                return Common::MakeFormattedError<Result>( "the merged mesh does not weld: {}",
                                                           imported.GetError() );
            Geometry::DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
            if ( request.TrianglePercent.has_value() )
                // Recomputes the normals too: the collapses moved vertices.
                Geometry::SimplifyToTriangleCount( mesh, SimplifyTarget( mesh, *request.TrianglePercent ) );

            HLODMeshBuild built;
            built.Mesh.EditMesh = Geometry::ToSerialized( mesh );
            // Slots by MaterialID, which is the slot index.
            built.Mesh.MaterialSlots = SlotRefs( merged.Slots );
            return Common::MakeSuccess( std::move( built ) );
        }

        // MERGED MATERIALS: one atlas, one section; the material is baked by Build HLODs.
        const HLODMaterialMergeSerialized& settings = *request.MergeMaterials;
        // Every slot's vertices and triangles in one array (global indices), the atlas over all of them.
        std::vector<Desert::Vertex> vertices;
        std::vector<Desert::Index>  triangles;
        std::vector<std::size_t>    slotBase;
        for ( std::size_t slot = 0; slot < merged.SectionVertices.size(); ++slot )
        {
            const auto base = static_cast<uint32_t>( vertices.size() );
            slotBase.push_back( base );
            vertices.insert( vertices.end(), merged.SectionVertices[slot].begin(),
                             merged.SectionVertices[slot].end() );
            for ( const Desert::Index& t : merged.SectionTriangles[slot] )
                triangles.push_back( { base + t.V1, base + t.V2, base + t.V3 } );
        }
        if ( triangles.empty() )
            return Common::MakeError<Result>( "the parts hold no triangles to merge" );
        auto packed = PackSourceIslands( vertices, triangles, settings.TextureSize );
        if ( !packed )
            return Common::MakeError<Result>( packed.GetError() );
        const std::vector<glm::vec2>& atlas = packed.GetValue();

        std::vector<std::vector<glm::vec2>> atlasBySlot( merged.SectionVertices.size() );
        for ( std::size_t slot = 0; slot < merged.SectionVertices.size(); ++slot )
            atlasBySlot[slot].assign(
                 atlas.begin() + static_cast<std::ptrdiff_t>( slotBase[slot] ),
                 atlas.begin() +
                      static_cast<std::ptrdiff_t>( slotBase[slot] + merged.SectionVertices[slot].size() ) );
        auto proxy = BuildBakeProxy( merged, atlasBySlot );
        if ( !proxy )
            return Common::MakeError<Result>( proxy.GetError() );

        // The built mesh: the merged triangles in ONE section over the atlas (UV layer 0), their world frames
        // carried (the bake states the normal map in whatever frame the runtime draws, LandscapeHLODBake's rule).
        Geometry::RenderMeshData render;
        render.Vertices = vertices;
        for ( std::size_t v = 0; v < render.Vertices.size(); ++v )
            render.Vertices[v].TexCoord = atlas[v];
        render.Indices = triangles;
        Desert::Submesh submesh{};
        submesh.Name         = "Merged";
        submesh.VertexOffset = 0;
        submesh.VertexCount  = static_cast<uint32_t>( render.Vertices.size() );
        submesh.IndexOffset  = 0;
        submesh.IndexCount   = static_cast<uint32_t>( render.Indices.size() * 3 );
        submesh.Transform    = glm::mat4( 1.0f );
        render.Submeshes.push_back( std::move( submesh ) );
        render.SubmeshMaterialIds.push_back( 0 );
        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the merged mesh does not weld: {}", imported.GetError() );
        Geometry::DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );

        HLODMaterialBakeSource bake;
        bake.Key      = HLODMeshKey( request );
        bake.Settings = settings;
        bake.Proxy    = proxy.ExtractValue();
        if ( request.TrianglePercent.has_value() )
        {
            // UE CreateProxyMesh: the simplified mesh gets its own UVs and the source's flattened material is
            // projected onto them; the source surface it is projected from is the merged mesh on its atlas.
            bake.Source.Positions.reserve( vertices.size() );
            bake.Source.Normals.reserve( vertices.size() );
            bake.Source.UVs.reserve( vertices.size() );
            for ( std::size_t v = 0; v < vertices.size(); ++v )
            {
                bake.Source.Positions.push_back( glm::dvec3( vertices[v].Position ) );
                bake.Source.Normals.push_back( glm::dvec3( vertices[v].Normal ) );
                bake.Source.UVs.push_back( glm::dvec2( atlas[v] ) );
            }
            for ( const Desert::Index& t : triangles )
                bake.Source.Triangles.push_back( glm::uvec3( t.V1, t.V2, t.V3 ) );

            Geometry::SimplifyToTriangleCount( mesh, SimplifyTarget( mesh, *request.TrianglePercent ) );
            // UE FMeshProxySettings' own UV generation: patches flattened and packed at the atlas's resolution;
            // the tangents are rebuilt from the new UVs.
            Geometry::AutoUVSettings uvSettings;
            uvSettings.PackingResolution = settings.TextureSize;
            auto unwrapped               = Geometry::AutoUV( mesh, uvSettings );
            if ( !unwrapped )
                return Common::MakeFormattedError<Result>( "the simplified mesh does not unwrap: {}",
                                                           unwrapped.GetError() );
            mesh = std::move( unwrapped.ExtractValue().Mesh );
        }

        HLODMeshBuild built;
        // No slot yet: Build HLODs binds M_HLOD_<Key> once the bake wrote it (WorldPartitionPanel).
        built.Mesh.EditMesh = Geometry::ToSerialized( mesh );
        built.Bake          = std::move( bake );
        return Common::MakeSuccess( std::move( built ) );
    }

    Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildHLODMesh( const Rules::HLODMeshRequest&                 request,
                   std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result = Assets::StaticMeshComponentSer;
        auto built   = BuildHLODMeshForBake( request, registries );
        if ( !built )
            return Common::MakeError<Result>( built.GetError() );
        return Common::MakeSuccess( std::move( built.ExtractValue().Mesh ) );
    }

    namespace
    {
        // The tile a landscape HLOD request names, read once for both its mesh and its baked material's area:
        // the decoded heights and the frame they are laid out by (the root record's composed world position).
        struct LandscapeHLODTileInput
        {
            World::Landscape::LandscapeTileData Heights;
            World::Landscape::LandscapeFrame    Frame;
        };

        Common::ResultStr<LandscapeHLODTileInput>
        ReadLandscapeHLODTile( const Rules::LandscapeHLODRequest& request )
        {
            using Result        = LandscapeHLODTileInput;
            namespace Landscape = World::Landscape;
            if ( request.Tile >= request.Records.size() || request.World.size() != request.Records.size() )
                return Common::MakeError<Result>( "the landscape tile request names no record of its records" );
            const Assets::EntityData& tile = request.Records[request.Tile];
            Common::Json::Issues      issues;
            const auto tileBlock = Rules::Detail::BlockOf( tile, Rules::kLandscapeTileComponent, issues );
            if ( !tileBlock.has_value() )
                return Common::MakeError<Result>( "the record has no readable LandscapeTile block" );
            Common::UUID rootId;
            if ( !Rules::Detail::ReadReference( *tileBlock, "Landscape", rootId, issues ) || rootId.IsNull() )
                return Common::MakeError<Result>( "the tile names no landscape root" );
            const auto root =
                 std::find_if( request.Records.begin(), request.Records.end(),
                               [&]( const Assets::EntityData& record ) { return record.id == rootId; } );
            if ( root == request.Records.end() )
                return Common::MakeFormattedError<Result>( "the tile's landscape root {} is not in the world",
                                                           static_cast<std::uint64_t>( rootId ) );
            const std::size_t rootIndex = static_cast<std::size_t>( root - request.Records.begin() );
            const auto        rootBlock = Rules::Detail::BlockOf( *root, Rules::kLandscapeRootComponent, issues );
            if ( !rootBlock.has_value() )
                return Common::MakeError<Result>( "the tile's root has no readable Landscape block" );

            Landscape::LandscapeRoot frameRoot;
            frameRoot.Origin = glm::vec3( request.World[rootIndex][3] );
            rootBlock->ReadInto( "QuadsPerTile", frameRoot.QuadsPerTile, issues );
            rootBlock->ReadInto( "SpacingCm", frameRoot.SpacingCm, issues );
            rootBlock->ReadInto( "ZScale", frameRoot.ZScale, issues );
            if ( auto valid = Landscape::ValidateLandscapeRoot( frameRoot ); !valid )
                return Common::MakeError<Result>( "the tile's root: " + valid.GetError() );
            std::int32_t tileX = 0;
            std::int32_t tileZ = 0;
            std::string  heightFile;
            tileBlock->ReadInto( "TileX", tileX, issues );
            tileBlock->ReadInto( "TileZ", tileZ, issues );
            tileBlock->ReadInto( "HeightFile", heightFile, issues );
            if ( !issues.empty() )
                return Common::MakeFormattedError<Result>(
                     "the tile or its root does not read: {} wants {}, holds {}", issues.front().Path,
                     issues.front().Expected, issues.front().Found );

            // The loader's own read (ComponentRegistry's LandscapeTile load): the record's content key, resolved
            // the same way, so the stand-in and the near tile read the same file.
            auto decoded = Landscape::ReadLandscapeTileByKey( heightFile );
            if ( !decoded )
                return Common::MakeFormattedError<Result>( "the tile's height file '{}': {}", heightFile,
                                                           decoded.GetError() );
            Landscape::LandscapeTileData heights = decoded.ExtractValue();
            if ( auto matches = Landscape::CheckTileMatchesRoot( heights, frameRoot ); !matches )
                return Common::MakeError<Result>( matches.GetError() );
            const Landscape::LandscapeFrame frame = Landscape::LandscapeTileFrame( frameRoot, tileX, tileZ );
            return Common::MakeSuccess( Result{ std::move( heights ), frame } );
        }
    } // namespace

    Common::ResultStr<LandscapeHLODTileArea> LandscapeHLODTileAreaOf( const Rules::LandscapeHLODRequest& request )
    {
        auto input = ReadLandscapeHLODTile( request );
        if ( !input )
            return Common::MakeError<LandscapeHLODTileArea>( input.GetError() );
        const auto& tileId = request.Records[request.Tile].id;
        if ( !tileId )
            return Common::MakeError<LandscapeHLODTileArea>( "landscape HLOD tile record has no id" );
        const LandscapeHLODTileInput& tile = input.GetValue();
        return Common::MakeSuccess( LandscapeHLODTileArea{
             .Tile    = *tileId,
             .OriginX = tile.Frame.OriginX,
             .OriginZ = tile.Frame.OriginZ,
             .SizeX   = static_cast<float>( tile.Heights.SamplesX() - 1 ) * tile.Frame.SpacingCm,
             .SizeZ   = static_cast<float>( tile.Heights.SamplesZ() - 1 ) * tile.Frame.SpacingCm,
        } );
    }

    Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildLandscapeHLODMesh( const Rules::LandscapeHLODRequest& request )
    {
        using Result        = Assets::StaticMeshComponentSer;
        namespace Landscape = World::Landscape;
        auto input          = ReadLandscapeHLODTile( request );
        if ( !input )
            return Common::MakeError<Result>( input.GetError() );
        const LandscapeHLODTileInput        tile    = input.ExtractValue();
        const Landscape::LandscapeTileData& heights = tile.Heights;
        const Landscape::LandscapeFrame&    frame   = tile.Frame;

        // The visibility layer, when the tile has one: its holes are not drawn by the terrain either.
        const std::vector<std::uint8_t>* visibility = nullptr;
        for ( const Landscape::LandscapeWeightLayer& layer : heights.WeightLayers() )
            if ( layer.Name == Landscape::kLandscapeVisibilityLayerName )
                visibility = &layer.Weights;

        const std::uint32_t samplesX = heights.SamplesX();
        const std::uint32_t samplesZ = heights.SamplesZ();
        const auto          heightAt = [&]( std::uint32_t x, std::uint32_t z )
        { return frame.BaseY + Landscape::LandscapeHeightCm( heights.Sample( x, z ), frame.ZScale ); };
        const auto isHole = [&]( std::uint32_t x, std::uint32_t z )
        {
            const std::size_t at = ( static_cast<std::size_t>( z ) * samplesX ) + x;
            return visibility != nullptr && at < visibility->size() &&
                   Landscape::LandscapeWeightIsHole( ( *visibility )[at] );
        };

        // THE LANDSCAPE LOD THE HLOD IS SEEN AT (UE ComputeRequiredLandscapeLOD, AutomaticLOD policy): the tile's
        // bounding sphere projected from MinVisibleDistance through UE's fixed view for it (90 degree FOV,
        // 1920x1080: ComputeBoundsScreenSize's ScreenMultiple = 0.5 * 1920 / 1080), against GetLODScreenSizeArray
        // over UE's landscape defaults (LOD0ScreenSize 0.5, LOD0DistributionSetting 1.25, CeilLogTwo(quads + 1)
        // LODs). The first LOD whose screen size the tile does not exceed is kept; LOD n keeps every 2^n-th
        // sample.
        float lowest  = heightAt( 0, 0 );
        float highest = lowest;
        for ( std::uint32_t z = 0; z < samplesZ; ++z )
            for ( std::uint32_t x = 0; x < samplesX; ++x )
            {
                lowest  = std::min( lowest, heightAt( x, z ) );
                highest = std::max( highest, heightAt( x, z ) );
            }
        const float     sizeX           = static_cast<float>( samplesX - 1 ) * frame.SpacingCm;
        const float     sizeZ           = static_cast<float>( samplesZ - 1 ) * frame.SpacingCm;
        const float     radius          = 0.5f * std::sqrt( ( sizeX * sizeX ) + ( sizeZ * sizeZ ) +
                                                            ( ( highest - lowest ) * ( highest - lowest ) ) );
        constexpr float kScreenMultiple = 0.5f * 1920.0f / 1080.0f;
        const float   screenSize = 2.0f * kScreenMultiple * radius / std::max( 1.0f, request.MinVisibleDistance );
        std::uint32_t lodCount   = 0;
        while ( ( 1u << lodCount ) < std::max( samplesX, samplesZ ) )
            ++lodCount;
        std::uint32_t lod       = 0;
        float         lodScreen = 0.5f;
        for ( ; lod + 1 < lodCount; ++lod, lodScreen /= 1.25f )
            if ( screenSize > lodScreen )
                break;
        const std::uint32_t stride = 1u << lod;
        const auto          rows   = [stride]( std::uint32_t samples )
        {
            std::vector<std::uint32_t> kept;
            for ( std::uint32_t at = 0; at + 1 < samples; at += stride )
                kept.push_back( at );
            kept.push_back( samples - 1 );
            return kept;
        };
        const std::vector<std::uint32_t> xs = rows( samplesX );
        const std::vector<std::uint32_t> zs = rows( samplesZ );

        Geometry::RenderMeshData  render;
        std::vector<std::int64_t> vertexOf( static_cast<std::size_t>( samplesX ) * samplesZ, -1 );
        const auto                vertexAt = [&]( std::uint32_t x, std::uint32_t z )
        {
            std::int64_t& slot = vertexOf[( static_cast<std::size_t>( z ) * samplesX ) + x];
            if ( slot >= 0 )
                return static_cast<std::uint32_t>( slot );
            const std::uint32_t x0 = x == 0 ? 0 : x - 1;
            const std::uint32_t x1 = std::min( x + 1, samplesX - 1 );
            const std::uint32_t z0 = z == 0 ? 0 : z - 1;
            const std::uint32_t z1 = std::min( z + 1, samplesZ - 1 );
            const float         dX =
                 ( heightAt( x1, z ) - heightAt( x0, z ) ) / ( static_cast<float>( x1 - x0 ) * frame.SpacingCm );
            const float dZ =
                 ( heightAt( x, z1 ) - heightAt( x, z0 ) ) / ( static_cast<float>( z1 - z0 ) * frame.SpacingCm );
            Desert::Vertex vertex{};
            vertex.Position =
                 glm::vec3( frame.OriginX + ( static_cast<float>( x ) * frame.SpacingCm ), heightAt( x, z ),
                            frame.OriginZ + ( static_cast<float>( z ) * frame.SpacingCm ) );
            // The heightfield's normal; the tangent frame is MikkTSpace's, built once the triangles exist.
            vertex.Normal    = glm::normalize( glm::vec3( -dX, 1.0f, -dZ ) );
            vertex.TexCoord  = glm::vec2( static_cast<float>( x ) / static_cast<float>( samplesX - 1 ),
                                          static_cast<float>( z ) / static_cast<float>( samplesZ - 1 ) );
            slot             = static_cast<std::int64_t>( render.Vertices.size() );
            render.Vertices.push_back( vertex );
            return static_cast<std::uint32_t>( slot );
        };
        using Sample = std::pair<std::uint32_t, std::uint32_t>;
        std::vector<Sample> ring;
        for ( std::size_t j = 0; j + 1 < zs.size(); ++j )
            for ( std::size_t i = 0; i + 1 < xs.size(); ++i )
            {
                const std::uint32_t x0 = xs[i], x1 = xs[i + 1], z0 = zs[j], z1 = zs[j + 1];
                if ( isHole( x0, z0 ) || isHole( x1, z0 ) || isHole( x0, z1 ) || isHole( x1, z1 ) )
                    continue;
                const bool west  = i == 0;
                const bool east  = i + 2 == xs.size();
                const bool south = j == 0;
                const bool north = j + 2 == zs.size();
                if ( !( ( ( west || east ) && z1 - z0 > 1 ) || ( ( south || north ) && x1 - x0 > 1 ) ) )
                {
                    // Facing +Y: (x0,z0) -> (x0,z1) -> (x1,z0) and (x1,z0) -> (x0,z1) -> (x1,z1).
                    render.Indices.push_back( { vertexAt( x0, z0 ), vertexAt( x0, z1 ), vertexAt( x1, z0 ) } );
                    render.Indices.push_back( { vertexAt( x1, z0 ), vertexAt( x0, z1 ), vertexAt( x1, z1 ) } );
                    continue;
                }
                // A cell on the tile's border: its border side keeps every sample (the near tile's edge exactly,
                // no T-junction crack), fanned from the cell's centre sample in the same winding as above - west
                // up, north east, east down, south west.
                ring.clear();
                for ( std::uint32_t z = z0; z < z1; z += west ? 1 : z1 - z0 )
                    ring.emplace_back( x0, z );
                for ( std::uint32_t x = x0; x < x1; x += north ? 1 : x1 - x0 )
                    ring.emplace_back( x, z1 );
                for ( std::uint32_t z = z1; z > z0; z -= east ? 1 : z1 - z0 )
                    ring.emplace_back( x1, z );
                for ( std::uint32_t x = x1; x > x0; x -= south ? 1 : x1 - x0 )
                    ring.emplace_back( x, z0 );
                const Sample centre{ ( x0 + x1 ) / 2, ( z0 + z1 ) / 2 };
                for ( std::size_t k = 0; k < ring.size(); ++k )
                {
                    const Sample& p = ring[k];
                    const Sample& q = ring[( k + 1 ) % ring.size()];
                    const auto    d = []( std::uint32_t a, std::uint32_t b )
                    { return static_cast<std::int64_t>( a ) - static_cast<std::int64_t>( b ); };
                    const std::int64_t cross = ( d( p.first, centre.first ) * d( q.second, centre.second ) ) -
                                               ( d( p.second, centre.second ) * d( q.first, centre.first ) );
                    if ( cross == 0 )
                        continue; // the centre lies on this border segment: no area to cover
                    render.Indices.push_back( { vertexAt( centre.first, centre.second ),
                                                vertexAt( p.first, p.second ), vertexAt( q.first, q.second ) } );
                }
            }
        if ( render.Indices.empty() )
            return Common::MakeError<Result>( "the tile is all hole; it draws nothing and needs no HLOD" );
        if ( !Geometry::ApplyMikkTangents( render.Vertices, 0, std::span<Desert::Index>( render.Indices ) ) )
            return Common::MakeError<Result>( "MikkTSpace could not allocate the tile mesh's tangent frames" );
        Desert::Submesh submesh{};
        submesh.Name        = "Landscape";
        submesh.VertexCount = static_cast<std::uint32_t>( render.Vertices.size() );
        submesh.IndexCount  = static_cast<std::uint32_t>( render.Indices.size() * 3 );
        submesh.Transform   = glm::mat4( 1.0f );
        render.Submeshes.push_back( std::move( submesh ) );
        render.SubmeshMaterialIds.push_back( 0 );

        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the tile mesh does not weld: {}", imported.GetError() );
        Assets::StaticMeshComponentSer block;
        block.EditMesh = Geometry::ToSerialized( imported.ExtractValue().Mesh );
        return Common::MakeSuccess( std::move( block ) );
    }

    Common::ResultStr<std::optional<Assets::StaticMeshComponentSer>>
    BuildLandscapeSplineHLODMesh( const Rules::LandscapeSplineHLODRequest&      request,
                                  std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result = std::optional<Assets::StaticMeshComponentSer>;
        if ( request.Spline >= request.Records.size() )
            return Common::MakeFormattedError<Result>( "record {} is not in the cell", request.Spline );
        Common::Json::Issues issues;
        const auto           block =
             Rules::Detail::BlockOf( request.Records[request.Spline], Rules::kLandscapeSplineComponent, issues );
        if ( !block.has_value() )
            return Common::MakeError<Result>( "the record has no LandscapeSpline block" );
        World::Landscape::LandscapeSplines splines;
        World::Landscape::ReadLandscapeSplinesJson( *block, splines, issues );
        if ( !issues.empty() )
            return Common::MakeFormattedError<Result>( "its LandscapeSpline block does not read ({} issue(s))",
                                                       issues.size() );

        // One source per distinct entry, kept alive while the segments read it (the sources are spans).
        struct Backing
        {
            std::vector<Desert::Vertex>  Vertices;
            std::vector<Desert::Index>   Indices;
            std::vector<Desert::Submesh> Submeshes;
        };
        std::map<std::string, std::unique_ptr<Backing>> backings;
        const auto                                      sourceOf =
             [&]( const World::Landscape::LandscapeSplineMeshEntry& entry ) -> Common::ResultStr<const Backing*>
        {
            const std::string key = entry.Primitive
                                         ? std::format( "primitive {}", static_cast<int>( *entry.Primitive ) )
                                         : std::format( "mesh {}", static_cast<uint64_t>( entry.Mesh ) );
            if ( const auto found = backings.find( key ); found != backings.end() )
                return Common::MakeSuccess( static_cast<const Backing*>( found->second.get() ) );
            auto backing = std::make_unique<Backing>();
            if ( entry.Primitive )
            {
                auto shape = Geometry::MakePrimitive( *entry.Primitive );
                if ( !shape.has_value() )
                    return Common::MakeFormattedError<const Backing*>( "{} has no geometry", key );
                Desert::Submesh whole{};
                whole.Name        = key;
                whole.VertexCount = static_cast<uint32_t>( shape->Vertices.size() );
                whole.IndexCount  = static_cast<uint32_t>( shape->Indices.size() * 3 );
                whole.Transform   = glm::mat4( 1.0f );
                backing->Vertices = std::move( shape->Vertices );
                backing->Indices  = std::move( shape->Indices );
                backing->Submeshes.push_back( std::move( whole ) );
            }
            else
            {
                const Common::Utils::AssetRegistryEntry* row = nullptr;
                for ( const auto& registry : registries )
                    if ( ( row = registry.FindByHandle( static_cast<uint64_t>( entry.Mesh ) ) ) != nullptr )
                        break;
                if ( row == nullptr )
                    return Common::MakeFormattedError<const Backing*>( "{} is in no asset registry", key );
                const auto file  = Common::AssetHandle::PathForStableKey( row->Key );
                auto       bytes = Assets::LoadMeshPlatformData( file );
                if ( !bytes )
                    return Common::MakeFormattedError<const Backing*>( "mesh '{}': {}", file.string(),
                                                                       bytes.GetError() );
                auto data = Assets::Serialization::ReadMeshAssetData( bytes.GetValue(), file.string() );
                if ( !data )
                    return Common::MakeError<const Backing*>( data.GetError() );
                auto render = Geometry::RenderFromMeshAssetData( data.GetValue() );
                if ( !render )
                    return Common::MakeFormattedError<const Backing*>( "mesh '{}': {}", file.string(),
                                                                       render.GetError() );
                Geometry::RenderMeshData mesh = render.ExtractValue();
                backing->Vertices             = std::move( mesh.Vertices );
                backing->Indices              = std::move( mesh.Indices );
                backing->Submeshes            = std::move( mesh.Submeshes );
            }
            const Backing* kept = backing.get();
            backings.emplace( key, std::move( backing ) );
            return Common::MakeSuccess( static_cast<const Backing*>( kept ) );
        };

        // Every segment's bent meshes appended into one render mesh, world cm (the HLOD record's transform is the
        // identity). One section: the spline meshes draw with the default surface in the HLOD.
        Geometry::RenderMeshData render;
        for ( std::size_t s = 0; s < splines.Segments.size(); ++s )
        {
            std::vector<World::Landscape::LandscapeSplineMeshSource> sources;
            for ( const World::Landscape::LandscapeSplineMeshEntry& entry : splines.Segments[s].SplineMeshes )
            {
                auto source = sourceOf( entry );
                if ( !source )
                    return Common::MakeFormattedError<Result>( "segment {}: {}", s, source.GetError() );
                const Backing* backing = source.GetValue();
                sources.push_back( { backing->Vertices, backing->Indices, backing->Submeshes } );
            }
            if ( sources.empty() )
                continue;
            for ( const World::Landscape::LandscapeSplineDeformedMesh& bent :
                  World::Landscape::BuildLandscapeSplineSegmentMeshes( splines, s, sources ) )
            {
                const auto vertexBase = static_cast<uint32_t>( render.Vertices.size() );
                const auto indexBase  = static_cast<uint32_t>( render.Indices.size() * 3 );
                for ( Desert::Submesh part : bent.Submeshes )
                {
                    part.VertexOffset += vertexBase;
                    part.IndexOffset += indexBase;
                    render.Submeshes.push_back( std::move( part ) );
                    render.SubmeshMaterialIds.push_back( 0 );
                }
                render.Vertices.insert( render.Vertices.end(), bent.Vertices.begin(), bent.Vertices.end() );
                render.Indices.insert( render.Indices.end(), bent.Indices.begin(), bent.Indices.end() );
            }
        }
        if ( render.Indices.empty() )
            return Common::MakeSuccess( Result() );
        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the spline meshes do not weld: {}", imported.GetError() );
        Assets::StaticMeshComponentSer out;
        out.EditMesh = Geometry::ToSerialized( imported.ExtractValue().Mesh );
        return Common::MakeSuccess( Result( std::move( out ) ) );
    }

    namespace
    {
        // The geometry a mesh builder builds through: the build's own (HLODBuildContext::Geometry - the editor's
        // Build HLODs, which bakes merged materials) or BuildHLODMesh over the cook's registries.
        [[nodiscard]] Rules::HLODMeshBuilder GeometryOf( const Rules::HLODBuildContext& context )
        {
            if ( context.Geometry )
                return context.Geometry;
            return [&context]( const Rules::HLODMeshRequest& request )
            { return BuildHLODMesh( request, context.Registries ); };
        }

        // UE UHLODBuilderMeshMerge: the gathered batches merged (Rules::MergeHLODBatches over BuildHLODMesh).
        // Reads no settings.
        class HLODBuilderMeshMerge final : public Rules::IHLODBuilder
        {
        public:
            [[nodiscard]] Common::ResultStr<Rules::InstancingHLOD>
            Build( const Rules::HLODBuildContext& context, Rules::InstancingHLOD gathered ) const override
            {
                return Rules::MergeHLODBatches( context, std::move( gathered ), std::nullopt, GeometryOf( context ) );
            }
        };

        // UE UHLODBuilderMeshSimplify: merged, then simplified to its Settings' TrianglePercent
        // (HLODMeshSimplifySettings).
        class HLODBuilderMeshSimplify final : public Rules::IHLODBuilder
        {
        public:
            [[nodiscard]] Common::BoolResultStr
            ValidateSettings( const Common::Json::Value* settings ) const override
            {
                auto read = Rules::ReadHLODBuilderSettings<HLODMeshSimplifySettings>( settings );
                if ( !read )
                    return Common::MakeError<bool>( read.GetError() );
                const std::optional<float> percent = read.GetValue().TrianglePercent;
                if ( !percent.has_value() )
                    return Common::MakeError<bool>( "MeshSimplify's Settings state no TrianglePercent" );
                if ( !( *percent > 0.0f && *percent <= 1.0f ) )
                    return Common::MakeError<bool>(
                         std::format( "MeshSimplify's TrianglePercent {:f} is outside (0, 1]", *percent ) );
                return Common::MakeSuccess( true );
            }

            [[nodiscard]] Common::ResultStr<Rules::InstancingHLOD>
            Build( const Rules::HLODBuildContext& context, Rules::InstancingHLOD gathered ) const override
            {
                if ( auto valid = ValidateSettings( context.Settings ); !valid )
                    return Common::MakeError<Rules::InstancingHLOD>( valid.GetError() );
                auto read = Rules::ReadHLODBuilderSettings<HLODMeshSimplifySettings>( context.Settings );
                if ( !read )
                    return Common::MakeError<Rules::InstancingHLOD>( read.GetError() );
                return Rules::MergeHLODBatches( context, std::move( gathered ), read.GetValue().TrianglePercent,
                                                GeometryOf( context ) );
            }
        };

        // The name the engine registers a built-in type's builder under (Rules::kBuiltInHLODBuilders).
        [[nodiscard]] std::string BuiltInHLODBuilderName( HLODLayerType type )
        {
            HLODLayerSerialized layer;
            layer.Type = type;
            return std::string( Rules::HLODBuilderNameOf( layer ) );
        }
    } // namespace

    Rules::HLODBuilderRegistry& HLODBuilders()
    {
        static Rules::HLODBuilderRegistry registry = []
        {
            Rules::HLODBuilderRegistry builtIn;
            // Three distinct names into an empty registry, none refusable (Register refuses only an empty name, no
            // builder, or a name taken).
            static_cast<void>( builtIn.Register( BuiltInHLODBuilderName( HLODLayerType::Instancing ),
                                                 std::make_shared<Rules::HLODBuilderInstancing>() ) );
            static_cast<void>( builtIn.Register( BuiltInHLODBuilderName( HLODLayerType::MeshMerge ),
                                                 std::make_shared<HLODBuilderMeshMerge>() ) );
            static_cast<void>( builtIn.Register( BuiltInHLODBuilderName( HLODLayerType::MeshSimplify ),
                                                 std::make_shared<HLODBuilderMeshSimplify>() ) );
            builtIn.SetLandscapeBuilder(
                 std::make_shared<Rules::HLODBuilderLandscape>( []( const Rules::LandscapeHLODRequest& request )
                                                                { return BuildLandscapeHLODMesh( request ); } ) );
            builtIn.SetLandscapeSplineBuilder( std::make_shared<Rules::HLODBuilderLandscapeSplines>(
                 []( const Rules::LandscapeSplineHLODRequest&      request,
                     std::span<const Common::Utils::AssetRegistry> registries )
                 { return BuildLandscapeSplineHLODMesh( request, registries ); } ) );
            return builtIn;
        }();
        return registry;
    }
} // namespace Desert::Core
