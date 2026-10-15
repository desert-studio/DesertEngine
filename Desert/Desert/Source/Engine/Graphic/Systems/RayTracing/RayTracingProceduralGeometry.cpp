#include <Engine/Graphic/Systems/RayTracing/RayTracingProceduralGeometry.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanProceduralRayTracingGeometry.hpp>
#include <Engine/Graphic/Renderer.hpp>

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<ProceduralRayTracingGeometry> CreateProceduralRayTracingGeometry()
    {
        return std::make_unique<API::Vulkan::VulkanProceduralRayTracingGeometry>();
    }

    Common::BoolResultStr RayTracingProceduralCache::AddBuilds(
         RDG::Builder& graph, std::span<RayTracingPrimitive> primitives, const uint64_t frame,
         std::vector<RayTracingDynamicGeometryBuild>& buildsOut, std::vector<RDG::BufferRef>& blasOut )
    {
        ++m_Call;
        for ( RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.ProceduralMesh == nullptr || primitive.Geometry != nullptr )
                continue;
            const Mesh& mesh  = *primitive.ProceduralMesh;
            Entry&      entry = m_Entries[Key{ &mesh, primitive.HiddenSubmeshes }];
            if ( entry.LastCall == m_Call )
            {
                // Another primitive of this view (an ISM instance, a second row of the mesh) handled it.
                if ( entry.Ready )
                    primitive.Geometry = static_cast<const RayTracingGeometryResource*>( entry.Geometry.get() );
                continue;
            }
            entry.LastCall = m_Call;
            if ( entry.LastFrame == frame && entry.Geometry )
            {
                // An earlier view of this frame prepared (and, if needed, built) it: instance the same BLAS.
                if ( entry.Ready )
                {
                    primitive.Geometry = static_cast<const RayTracingGeometryResource*>( entry.Geometry.get() );
                    blasOut.push_back( graph.RegisterExternal( entry.Geometry->GetStorage(), "Procedural.BLAS" ) );
                }
                continue;
            }
            entry.LastFrame = frame;
            entry.Ready     = false;
            if ( !entry.Geometry )
                entry.Geometry = CreateProceduralRayTracingGeometry();

            const Common::ResultStr<ProceduralBlasPrepared> prepared =
                 entry.Geometry->Prepare( mesh, primitive.HiddenSubmeshes );
            if ( !prepared )
            {
                if ( !entry.RefusalLogged )
                {
                    LOG_ERROR( "[RayTracing] a procedural mesh (hidden mask {:#x}) is not in the TLAS: {}",
                               primitive.HiddenSubmeshes, prepared.GetError() );
                }
                entry.RefusalLogged = true;
                continue;
            }
            const RDG::BufferRef storage =
                 graph.RegisterExternal( entry.Geometry->GetStorage(), "Procedural.BLAS" );
            if ( prepared.GetValue().NeedsBuild )
            {
                if ( const auto imported =
                          Renderer::ImportVertexBuffer( mesh.GetVertexBuffer(), entry.VerticesImport );
                     !imported )
                {
                    if ( !entry.RefusalLogged )
                    {
                        LOG_ERROR( "[RayTracing] a procedural mesh's vertices could not be imported, it is not in "
                                   "the TLAS: {}",
                                   imported.GetError() );
                    }
                    entry.RefusalLogged = true;
                    continue;
                }
                buildsOut.push_back(
                     { .Target       = entry.Geometry.get(),
                       .Vertices     = graph.RegisterExternal( entry.VerticesImport, "Procedural.Vertices" ),
                       .Storage      = storage,
                       .ScratchBytes = prepared.GetValue().ScratchBytes } );
            }
            else
                blasOut.push_back( storage );
            entry.RefusalLogged = false;
            entry.Ready         = true;
            primitive.Geometry  = static_cast<const RayTracingGeometryResource*>( entry.Geometry.get() );
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
