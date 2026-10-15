#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <Common/Core/Core.hpp>

#include <format>
#include <optional>

namespace Desert::Graphic::RayTracing
{
    namespace
    {
        struct ScheduledBuild
        {
            RayTracingDynamicGeometryTarget* Target = nullptr;
            RDG::BufferRef                   Vertices;
            RDG::BufferRef                   Indices;
            RDG::BufferRef                   Storage;
            uint64_t                         ScratchOffset = 0;
        };
    } // namespace

    Common::BoolResultStr AddRayTracingDynamicGeometryPass( RDG::Builder&                                   graph,
                                                            std::span<const RayTracingDynamicGeometryBuild> builds,
                                                            std::vector<RDG::BufferRef>& blasOut )
    {
        if ( builds.empty() )
            return BOOLSUCCESS;

        std::vector<ScheduledBuild> scheduled;
        scheduled.reserve( builds.size() );
        uint64_t scratchBytes = 0;
        for ( std::size_t index = 0; index < builds.size(); ++index )
        {
            const RayTracingDynamicGeometryBuild& build = builds[index];
            if ( build.Target == nullptr || !build.Storage.IsValid() || !build.Vertices.IsValid() )
                return Common::MakeError(
                     std::format( "{}: build request {} of {} has no target, vertices or storage",
                                  kDynamicGeometryPassName, index, builds.size() ) );
            scheduled.push_back( { build.Target, build.Vertices, build.Indices, build.Storage, scratchBytes } );
            scratchBytes += build.ScratchBytes;
        }

        const RDG::BufferRef scratch =
             graph.CreateBuffer( RDG::BufferDesc{ scratchBytes }, kDynamicGeometryScratchName );
        graph.AddPass(
             kDynamicGeometryPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Write( scratch, RDG::Access::AccelStructBuildWrite );
                 for ( const ScheduledBuild& build : scheduled )
                 {
                     pass.Read( build.Vertices, RDG::Access::AccelStructBuildInput );
                     if ( build.Indices.IsValid() )
                         pass.Read( build.Indices, RDG::Access::AccelStructBuildInput );
                     pass.Write( build.Storage, RDG::Access::AccelStructBuildWrite );
                 }
             },
             [scheduled, scratch]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto scratchBinding = context.GetBuffer( scratch, RDG::Access::AccelStructBuildWrite );
                 if ( !scratchBinding )
                     return Common::MakeError( scratchBinding.GetError() );
                 for ( const ScheduledBuild& build : scheduled )
                 {
                     const auto vertices = context.GetBuffer( build.Vertices, RDG::Access::AccelStructBuildInput );
                     if ( !vertices )
                         return Common::MakeError( vertices.GetError() );
                     std::optional<RDG::BufferBinding> indices;
                     if ( build.Indices.IsValid() )
                     {
                         const auto indexBinding =
                              context.GetBuffer( build.Indices, RDG::Access::AccelStructBuildInput );
                         if ( !indexBinding )
                             return Common::MakeError( indexBinding.GetError() );
                         indices = indexBinding.GetValue();
                     }
                     if ( const auto storage =
                               context.GetBuffer( build.Storage, RDG::Access::AccelStructBuildWrite );
                          !storage )
                         return Common::MakeError( storage.GetError() );
                     if ( const auto recorded = build.Target->RecordBuild(
                               context, vertices.GetValue(), indices ? &*indices : nullptr,
                               scratchBinding.GetValue(), build.ScratchOffset );
                          !recorded )
                         return recorded;
                 }
                 return BOOLSUCCESS;
             } );
        for ( const ScheduledBuild& build : scheduled )
            blasOut.push_back( build.Storage );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
