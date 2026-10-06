#pragma once

#include <Engine/Graphic/RDG/RDGLayoutCache.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <memory>

namespace Desert::Graphic
{
    // The binding layout a graph node declares its block against (RDG::PassBuilder::Bindings), kept with the
    // renderer that owns the shader. Derived from the shader's reflection (Renderer::GetBindingLayout, a walk of
    // every slot) the first time it is asked for and again only when the shader is another object or recompiled
    // (a hot reload bumps Shader::GetReloadGeneration) - RDG::LayoutCache - so a node's per-frame setup hands the
    // graph a kept layout by pointer instead of re-walking the reflection. One cache per shader slot of a
    // renderer.
    class ShaderBindingLayoutCache
    {
    public:
        [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
        Get( const std::shared_ptr<Shader>& shader )
        {
            return m_Cache.Get( shader, shader->GetReloadGeneration(),
                                []( const Shader& s ) { return Renderer::GetInstance().GetBindingLayout( s ); } );
        }

    private:
        RDG::LayoutCache m_Cache;
    };
} // namespace Desert::Graphic
