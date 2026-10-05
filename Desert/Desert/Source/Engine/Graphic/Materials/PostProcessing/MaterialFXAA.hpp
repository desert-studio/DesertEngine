#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // The FXAA program's material: it carries no values and no textures -- the input (tonemapped) image is a
    // graph texture the FXAA exec binds through RDG::PassBindings (FXAARenderer::Record).
    class MaterialFXAA final : public Material
    {
    public:
        MaterialFXAA();
    };
} // namespace Desert::Graphic
