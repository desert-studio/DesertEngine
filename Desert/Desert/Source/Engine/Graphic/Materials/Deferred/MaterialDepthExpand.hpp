#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // "Deferred: DepthExpand": DepthExpand.shader writes the single-sample G-buffer depth into every sample of the
    // multisampled scene depth. The material holds nothing of its own: u_Depth is a pass parameter (not in the
    // shader's Properties), bound by DepthExpandRenderer::Record through RDG::PassBindings from the node's
    // SampledGraphics read. Header-only.
    class MaterialDepthExpand final : public Material
    {
    public:
        MaterialDepthExpand() : Material( "MaterialDepthExpand", "DepthExpand" )
        {
        }
    };
} // namespace Desert::Graphic
