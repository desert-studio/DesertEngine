#pragma once

// THE SINGLE LAYER WATER PASS AS THE RENDERER SEES IT (UE: the Renderer owns the single layer water passes -
// SingleLayerWaterRendering - and the Water plugin owns the bodies, the quadtree and the water mesh that feed
// them). The renderer schedules the frame's water through this interface and never names the Water module; the
// module registers its pass's factory at StartupModule (Water/Private/WaterModule.cpp). No factory registered (an
// executable without the Water module): the scene has no water pass, and every reader tests the slot.

#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string_view>

namespace Desert::Graphic
{
    class Image2D;
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    class ISingleLayerWaterPass : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // The frame in three steps before the graph records (FPS-ARCH-SETUP3): PrepareFrame on the render
        // thread, PrepareTiles on a setup task, FinishPrepare after the join.
        virtual void PrepareFrame()          = 0;
        virtual void PrepareTiles() noexcept = 0;
        virtual void FinishPrepare()         = 0;

        [[nodiscard]] virtual bool HasDraws() const = 0;
        // Whether this frame's water program samples @p textureName (the shadow gates' question).
        [[nodiscard]] virtual bool FrameSamples( std::string_view textureName ) const = 0;
        // The persistent WaterInfo ground the frame graph imports and the water passes sample.
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetWaterInfo() const = 0;

        // "Deferred: Water": the surface into the scene target, after the scene copy.
        virtual void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, RDG::TextureRef waterInfo,
                                   RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                                   const SceneViewInputs& view, const SceneFrameBinding& frame )    = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderManual( const RDG::PassContext& context ) = 0;

        // "Translucency: Front Layer G-buffer (Water)": the G-buffer the front layer's reflections trace from.
        [[nodiscard]] virtual bool HasFrontLayerDraws() const = 0;
        virtual void               DeclareFrontLayerDraws( RDG::PassBuilder& pass, RDG::TextureRef waterInfo,
                                                           const SceneViewInputs& view, const SceneFrameBinding& frame ) = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderFrontLayerManual( const RDG::PassContext& context ) = 0;

        // At EndScene: the frame's submitted zones and draws dropped.
        virtual void ClearQueue() = 0;
    };

    using SingleLayerWaterPassFactory = std::shared_ptr<ISingleLayerWaterPass> ( * )(
         SceneRenderer* sceneRenderer, const std::shared_ptr<Framebuffer>& targetFramebuffer );

    /// The registered pass factory (module startup, Engine/Core/Modules.cpp); null without the Water module.
    inline SingleLayerWaterPassFactory& SingleLayerWaterPassFactorySlot()
    {
        static SingleLayerWaterPassFactory factory = nullptr;
        return factory;
    }

    inline void RegisterSingleLayerWaterPass( SingleLayerWaterPassFactory factory )
    {
        SingleLayerWaterPassFactorySlot() = factory;
    }
} // namespace Desert::Graphic::System
