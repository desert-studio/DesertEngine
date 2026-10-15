#include <Engine/Graphic/Systems/RayTracing/RayTracingGather.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>

#include <algorithm>
#include <format>
#include <unordered_map>

namespace Desert::Graphic::RayTracing
{
    namespace
    {
        // One GPU-scene row of the static / generic queues before its BLAS is resolved.
        struct RowRecord
        {
            Mesh*                   MeshPtr = nullptr;
            glm::mat4               World{ 1.0f };
            uint32_t                Row         = 0;
            uint64_t                Visible     = 0; // union over the row's records
            bool                    CastShadows = false;
            RayTracingPrimitiveKind Kind        = RayTracingPrimitiveKind::Static;
        };

        void Count( RayTracingGatherResult& out, const RayTracingSkip reason )
        {
            ++out.Skipped[static_cast<std::size_t>( reason )];
        }

        // Resolves @p mesh's BLAS for @p hidden; false: the row is skipped (counted, the error kept).
        bool ResolveGeometry( RayTracingGeometryVariants& variants, Mesh& mesh, const uint64_t hidden,
                              RayTracingGatherResult& out, const void*& geometry )
        {
            const Common::ResultStr<const RayTracingGeometryResource*> resolved = variants.Resolve( mesh, hidden );
            if ( !resolved )
            {
                out.Errors.push_back( resolved.GetError() );
                Count( out, RayTracingSkip::NoGeometry );
                return false;
            }
            geometry = resolved.GetValue();
            return true;
        }
    } // namespace

    void GatherRayTracingPrimitives( const RayTracingGatherInputs& inputs, RayTracingGeometryVariants& variants,
                                     RayTracingGatherResult& out )
    {
        out.Clear();

        // Static + generic: merge the records of one row (same row AND same mesh; a row is one object's world).
        std::vector<RowRecord>                    rows;
        std::unordered_map<uint32_t, std::size_t> rowIndex;
        const auto add = [&]( Mesh* mesh, const glm::mat4& world, const uint32_t row, const uint64_t visible,
                              const bool castShadows, const RayTracingPrimitiveKind kind )
        {
            if ( mesh == nullptr )
                return;
            if ( const auto found = rowIndex.find( row );
                 found != rowIndex.end() && rows[found->second].MeshPtr == mesh )
            {
                RowRecord& merged = rows[found->second];
                merged.Visible |= visible;
                merged.CastShadows = merged.CastShadows || castShadows;
                return;
            }
            rowIndex[row] = rows.size();
            rows.push_back( { mesh, world, row, visible, castShadows, kind } );
        };
        for ( const System::MeshRenderer::StaticMeshRenderData& data : inputs.Static )
            add( data.Mesh, data.Transform, data.MotionRow, ~data.HiddenSubmeshes, data.CastShadows,
                 RayTracingPrimitiveKind::Static );
        for ( const System::MeshRenderer::GenericMeshRenderData& data : inputs.Generic )
            if ( data.VisibleInRayTracing )
                add( data.Mesh, data.Transform, data.MotionRow, data.VisibleSubmeshMask, data.CastShadows,
                     RayTracingPrimitiveKind::Generic );

        for ( const RowRecord& record : rows )
        {
            const std::size_t submeshes = std::min<std::size_t>( record.MeshPtr->GetSubmeshes().size(), 64 );
            const uint64_t    all       = submeshes >= 64 ? ~0ull : ( ( 1ull << submeshes ) - 1ull );
            const uint64_t    hidden    = all & ~record.Visible;
            if ( submeshes > 0 && hidden == all )
            {
                Count( out, RayTracingSkip::NothingVisible );
                continue;
            }
            const void* geometry = nullptr;
            if ( !ResolveGeometry( variants, *record.MeshPtr, hidden, out, geometry ) )
                continue;
            RayTracingPrimitive& primitive = out.Primitives.emplace_back();
            primitive.Geometry             = geometry;
            primitive.World                = record.World;
            primitive.GpuSceneRow          = record.Row;
            primitive.CastShadows          = record.CastShadows;
            primitive.HiddenSubmeshes      = hidden;
            primitive.Kind                 = record.Kind;
        }

        // ISM / foliage: every instance its own row and TLAS instance, the mesh's one BLAS shared.
        for ( const System::MeshRenderer::InstancedMeshRenderData& data : inputs.Instanced )
        {
            if ( data.Mesh == nullptr || !data.Transforms )
                continue;
            const void* geometry = nullptr;
            if ( !ResolveGeometry( variants, *data.Mesh, 0, out, geometry ) )
                continue;
            const InstanceCullDistance    cull       = ScaledByLens( data.CullDistance, inputs.DrawDistanceScale );
            const std::vector<glm::mat4>& transforms = *data.Transforms;
            for ( std::size_t instance = 0; instance < transforms.size(); ++instance )
            {
                if ( !KeepsInstanceAtDistance( cull, static_cast<uint32_t>( instance ),
                                               glm::vec3( transforms[instance][3] ), inputs.ViewOrigin ) )
                    continue;
                RayTracingPrimitive& primitive = out.Primitives.emplace_back();
                primitive.Geometry             = geometry;
                primitive.World                = transforms[instance];
                primitive.GpuSceneRow          = inputs.InstancedMotionRows[data.FirstMotionRow + instance];
                primitive.CastShadows          = data.CastShadows;
                primitive.Kind                 = RayTracingPrimitiveKind::Instanced;
            }
        }

        // Skinned: the per-frame dynamic BLAS fills Geometry (SelectDynamicGeometry); until then counted Skinned.
        for ( const System::MeshRenderer::SkinnedMeshRenderData& data : inputs.Skinned )
        {
            RayTracingPrimitive& primitive = out.Primitives.emplace_back();
            primitive.World                = data.Transform;
            primitive.GpuSceneRow          = data.MotionRow;
            primitive.Kind                 = RayTracingPrimitiveKind::Skinned;
            primitive.DeformedMesh         = data.Mesh;
        }
    }
} // namespace Desert::Graphic::RayTracing
