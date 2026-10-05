#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // Fullscreen resolve material for the Overdraw view: drives OverdrawResolve.shader (heat-maps the overdraw
    // count over the scene). It carries no texture: the additive accumulation (u_Overdraw) is a graph texture the
    // resolve node binds through RDG::PassBindings (MeshRenderer::RecordOverdrawResolve). Header-only.
    class MaterialOverdrawResolve final : public Material
    {
    public:
        MaterialOverdrawResolve() : Material( "MaterialOverdrawResolve", "OverdrawResolve" )
        {
        }
    };
} // namespace Desert::Graphic
