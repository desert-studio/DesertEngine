#pragma once

#include <Engine/Graphic/RDG/RDGLayoutCache.hpp>

#include <memory>

namespace Desert::Graphic
{
    class Shader;

    // The binding layout a graph node declares its block against (RDG::PassBuilder::Bindings), kept with the
    // renderer that owns the shader. Derived from the shader's reflection (Renderer::GetBindingLayout, a walk of
    // every slot) the first time it is asked for and again only when the shader is another object or recompiled
    // (a hot reload bumps Shader::GetCodeGeneration) - RDG::LayoutCache - so a node's per-frame setup hands the
    // graph a kept layout by pointer instead of re-walking the reflection. One cache per shader slot of a
    // renderer.
    //
    // Get is defined in ShaderBindingLayoutCache.cpp so this header - included by every renderer header that keeps
    // a layout, UIMaterialCache.hpp among them - does not pull Renderer.hpp into them (RenderGraphCompile
    // ShaderBindingLayoutCacheHeaderStaysLight).
    class ShaderBindingLayoutCache
    {
    public:
        [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
        Get( const std::shared_ptr<Shader>& shader );

    private:
        RDG::LayoutCache m_Cache;
    };

    // The layouts of a node that declares one block PER SHADER its draws record with (a mesh draw list: every
    // material executor + recording pipeline pair is a block): one kept layout per shader object
    // (RDG::LayoutCacheSet), each re-derived only on that shader's reload, never per frame. Get keys on the
    // RECORDING pipeline's shader (`pipeline->GetSpecification().Shader`). DropExpired once per frame.
    class ShaderBindingLayoutSet
    {
    public:
        [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
        Get( const std::shared_ptr<Shader>& shader );

        void DropExpired()
        {
            m_Caches.DropExpired();
        }

    private:
        RDG::LayoutCacheSet m_Caches;
    };
} // namespace Desert::Graphic
