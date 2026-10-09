#pragma once

#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/Framebuffer.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    struct RenderPassSpecification
    {
        struct
        {
            glm::vec4 Color = { 0.1000000015, 0.1000000015, 0.1000000015, 1.00 };
            // X = Depth, Y = Stencil. Depth clears to 0 because the engine renders REVERSED-Z and 0 IS
            // THE FAR PLANE (Core/Projection.hpp) — an empty pixel must read as "infinitely far", and
            // under this convention that is 0, not 1. The one pass that still wants 1 is the shadow
            // cascade, which is deliberately standard-Z and overrides this via SystemRasterPass::ClearDepth.
            glm::vec2 DepthStencil = { Core::kDepthClear, 0.0f };
        } ClearColor;

        std::shared_ptr<Framebuffer> TargetFramebuffer;
        std::string                  DebugName;
    };

    class RenderPass final
    {
    public:
        virtual ~RenderPass() = default;

        RenderPass( const RenderPassSpecification& spec );

        virtual RenderPassSpecification& GetSpecification()
        {
            return m_RenderPassSpecification;
        }
        virtual const RenderPassSpecification& GetSpecification() const
        {
            return m_RenderPassSpecification;
        }

        static std::shared_ptr<RenderPass> Create( const RenderPassSpecification& spec );

    private:
        RenderPassSpecification m_RenderPassSpecification;
    };
} // namespace Desert::Graphic