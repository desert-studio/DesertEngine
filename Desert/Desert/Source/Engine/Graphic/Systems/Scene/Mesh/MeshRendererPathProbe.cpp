// NET-PAWN-TELEPORT-FRAME measurement (wip, removed by the fix it points at): per skinned record, what the draw
// command cache, the G-buffer's CPU walk and the sun cascades' CPU walk held for it in a frame (PathProbeFacts).

#include "MeshRendererInternal.hpp"

#include <algorithm>

namespace Desert::Graphic::System
{
    // What the published cache holds for each skinned record this frame (FeedDrawCommandCache, after Rebuild).
    void MeshRenderer::RecordPathProbe( std::span<const GpuDriven::PrimitiveIndex> fed )
    {
        // After the publish a slot IsHeld exactly when this feed claimed its primitive.
        for ( const auto& data : m_SkinnedQueue )
        {
            PathProbeFacts& facts = m_PathProbe[data.Entity];
            facts                 = PathProbeFacts{};
            facts.Frame           = m_PathProbeFrame;
            facts.Primitive       = data.GpuPrimitive;
            facts.Baked           = data.VatAddress != 0;
            facts.NoBones         = data.BoneMatrices.empty();
            facts.Claimed         = data.GpuPrimitive < m_BinnedFeed.size() && m_BinnedFeed[data.GpuPrimitive].IsHeld;
            facts.Fed = std::find( fed.begin(), fed.end(), data.GpuPrimitive ) != fed.end();
            if ( !data.Sections.empty() )
            {
                const uint64_t hidden = data.Sections.begin()->HiddenSubmeshes;
                facts.BinnedGBuffer =
                     m_DrawCommandCache->SubmeshesBinned( GpuDriven::MeshPass::GBuffer, data.GpuPrimitive, hidden );
                facts.BinnedCascade = m_DrawCommandCache->SubmeshesBinned( GpuDriven::MeshPass::ShadowCascade,
                                                                           data.GpuPrimitive, hidden );
                facts.BinnedVsm = m_DrawCommandCache->SubmeshesBinned( GpuDriven::MeshPass::VirtualShadow,
                                                                       data.GpuPrimitive, hidden );
            }
        }
    }

    void MeshRenderer::PrintPathProbe()
    {
        for ( auto watch = m_PathProbeWatch.begin(); watch != m_PathProbeWatch.end(); )
        {
            if ( const auto facts = m_PathProbe.find( watch->first ); facts != m_PathProbe.end() )
            {
                const PathProbeFacts& f = facts->second;
                LOG_INFO( "[PathProbe] frame {} entity {} primitive {}: {}, bones {}, feed claimed {} fed {}, cache "
                          "binned GBuffer {} ShadowCascade {} VirtualShadow {}, GBuffer CPU walk {}, cascade CPU "
                          "walk {}",
                          f.Frame, watch->first, f.Primitive, f.Baked ? "baked" : "skeletal",
                          f.NoBones ? "none" : "posed", f.Claimed, f.Fed, f.BinnedGBuffer, f.BinnedCascade,
                          f.BinnedVsm, int{ f.CpuGBuffer }, int{ f.CpuCascade } );
            }
            else
                LOG_INFO( "[PathProbe] frame {} entity {}: not in the skinned queue", m_PathProbeFrame,
                          watch->first );
            if ( --watch->second <= 0 )
                watch = m_PathProbeWatch.erase( watch );
            else
                ++watch;
        }
        m_PathProbe.clear();
    }
} // namespace Desert::Graphic::System
