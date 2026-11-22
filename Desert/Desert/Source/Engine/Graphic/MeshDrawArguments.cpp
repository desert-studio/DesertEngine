#include <Engine/Graphic/MeshDrawArguments.hpp>

#include <Engine/Geometry/Mesh.hpp>

#include <format>

namespace Desert::Graphic
{
    Common::ResultStr<uint32_t> AppendMeshDrawArguments( const Mesh& mesh, const SubmeshMask hiddenSubmeshMask,
                                                         const uint32_t lodLevel, const uint32_t firstInstance,
                                                         std::vector<uint32_t>& words )
    {
        // Mesh-relative records: the per-mesh bind puts the geometry at the mesh's own offsets.
        const auto&    geometry   = mesh.GetGeometry();
        const uint32_t indexCount = geometry.Indices != nullptr ? geometry.IndexCount : 0u;
        const auto&    submeshes  = mesh.GetSubmeshes();
        uint32_t       records    = 0;
        for ( size_t si = 0; si < submeshes.size(); ++si )
        {
            if ( hiddenSubmeshMask.Hides( si ) )
                continue;
            const Submesh& submesh = submeshes[si];
            if ( indexCount == 0u )
            {
                words.insert( words.end(), { submesh.VertexCount, 0u, submesh.VertexOffset, firstInstance, 0u } );
                ++records;
                continue;
            }
            const SubmeshIndexRange range = SubmeshIndices( submesh, lodLevel );
            if ( range.First + range.Count > indexCount )
                return Common::MakeError<uint32_t>(
                     std::format( "submesh {} reads indices [{}, {}) of a {}-index mesh", si, range.First,
                                  range.First + range.Count, indexCount ) );
            words.insert( words.end(), { range.Count, 0u, range.First, submesh.VertexOffset, firstInstance } );
            ++records;
        }
        return Common::MakeSuccess( records );
    }
} // namespace Desert::Graphic
