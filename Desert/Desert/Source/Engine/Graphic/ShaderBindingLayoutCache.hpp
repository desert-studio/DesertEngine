#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Graphic
{
    // The binding layout a graph node declares its block against (RDG::PassBuilder::Bindings), kept with the
    // renderer that owns the shader. Derived from the shader's reflection (Renderer::GetBindingLayout, a walk of
    // every slot) the first time it is asked for and again only after the shader recompiled - a hot reload bumps
    // Shader::GetReloadGeneration - so a node's per-frame setup reads a kept value instead of re-walking the
    // reflection. One cache per shader object: a renderer with two shaders keeps two.
    class ShaderBindingLayoutCache
    {
    public:
        [[nodiscard]] const RDG::ShaderBindingLayout& Get( const Shader& shader )
        {
            const uint32_t generation = shader.GetReloadGeneration();
            if ( m_Generation != generation )
            {
                m_Layout     = Renderer::GetInstance().GetBindingLayout( shader );
                m_Generation = generation;
            }
            return m_Layout;
        }

    private:
        RDG::ShaderBindingLayout m_Layout;
        std::optional<uint32_t>  m_Generation; // the shader generation m_Layout was derived at; none yet
    };
} // namespace Desert::Graphic
