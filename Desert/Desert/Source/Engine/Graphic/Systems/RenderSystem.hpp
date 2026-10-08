#pragma once

#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>
#include <Engine/Graphic/IRenderSystem.hpp>

#include <Engine/Core/EngineContext.hpp>

namespace Desert::Graphic
{
    class SceneRenderer;
}

namespace Desert::Graphic::System
{
    class RenderSystem : public IRenderSystem
    {
    public:
        explicit RenderSystem( SceneRenderer* sceneRenderer, const std::shared_ptr<Framebuffer>& targetFramebuffer )
             : m_SceneRenderer( sceneRenderer ), m_TargetFramebuffer( targetFramebuffer )
        {
        }
        virtual ~RenderSystem() = default;

        virtual Common::BoolResultStr Initialize() = 0;

        virtual const std::shared_ptr<Framebuffer>& GetSystemFramebuffer() const final
        {
            return m_Framebuffer;
        }

    protected:
        SceneRenderer*             m_SceneRenderer;
        std::weak_ptr<Framebuffer> m_TargetFramebuffer;

        std::shared_ptr<Framebuffer> m_Framebuffer;
    };
} // namespace Desert::Graphic::System