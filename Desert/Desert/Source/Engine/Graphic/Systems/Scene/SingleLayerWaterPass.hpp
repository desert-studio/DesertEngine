#pragma once

// THE SINGLE LAYER WATER PASS AS THE RENDERER SEES IT (UE: the Renderer owns the single layer water passes -
// SingleLayerWaterRendering - and the Water plugin owns the bodies, the quadtree and the water mesh that feed
// them). The renderer schedules the frame's water through this interface and never names the Water module; the
// module registers its pass's factory at StartupModule (Water/Private/WaterModule.cpp). No factory registered (an
// executable without the Water module): the scene has no water pass, and every reader tests the slot.

#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace Desert::Graphic
{
    class Image2D;
} // namespace Desert::Graphic

namespace Desert::Graphic::RDG
{
    class Builder;
} // namespace Desert::Graphic::RDG

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

        // "Deferred: Water": the surface into the scene target, after the scene copy. @p waves: this frame's FFT
        // ocean atlas (RecordedWaves / AddWavePasses), the graph's black without one.
        virtual void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, RDG::TextureRef waterInfo,
                                   RDG::TextureRef waves, RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                                   const SceneViewInputs& view, const SceneFrameBinding& frame )    = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderManual( const RDG::PassContext& context ) = 0;

        // "Translucency: Front Layer G-buffer (Water)": the G-buffer the front layer's reflections trace from.
        [[nodiscard]] virtual bool HasFrontLayerDraws() const = 0;
        virtual void               DeclareFrontLayerDraws( RDG::PassBuilder& pass, RDG::TextureRef waterInfo,
                                                           RDG::TextureRef waves, const SceneViewInputs& view,
                                                           const SceneFrameBinding& frame ) = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderFrontLayerManual( const RDG::PassContext& context ) = 0;

        // THE FFT OCEAN (WATER-RIVERS-FFT; UE Water plugin's FFT wave source): whether this frame's water draws an
        // FFT ocean (after PrepareFrame), its h0 cascades and the persistent two-frame waves atlas the renderer
        // imports, and the passes that advance the ocean to @p timeSeconds into @p waves - recorded once per graph
        // (SceneRenderer::FrameWaterWaves); RecordedWaves answers the ref they returned for @p graph, or invalid.
        [[nodiscard]] virtual bool                           FrameWavesAreFFT() const      = 0;
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetFFTInitialSpectrum() const = 0;
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetFFTWaves() const           = 0;
        [[nodiscard]] virtual RDG::TextureRef                RecordedWaves( const RDG::Builder& graph ) const = 0;
        virtual RDG::TextureRef AddWavePasses( RDG::Builder& graph, RDG::TextureRef initial, RDG::TextureRef waves,
                                               float timeSeconds ) = 0;

        // THE UNDERWATER VOLUME (WATER-UNDERWATER; UE Water: the body's underwater post process): whether the
        // camera is under a body this frame (after PrepareFrame), and "Deferred: Underwater" on the scene target
        // before the water surface, from the opaque scene's copy @p sceneCopy.
        [[nodiscard]] virtual bool HasUnderwater() const = 0;
        // GRAPH BUILD: this frame's UnderwaterUB (the medium, the camera, the sun and sky look of @p frame) as a
        // graph buffer, uploaded by the graph before the node reads it.
        [[nodiscard]] virtual RDG::BufferRef UploadUnderwater( RDG::Builder& graph, const SceneFrameBinding& frame ) = 0;
        virtual void DeclareUnderwater( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, const SceneViewInputs& view,
                                        RDG::BufferRef uniforms )                                         = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderUnderwaterManual( const RDG::PassContext& context ) = 0;

        // THE WATER'S CAUSTICS (WATER-CAUSTICS): the sun refracted through this frame's waves onto the floor, a
        // map and a frame row every sun receiver binds (FrameTransients::WaterCausticsMap / WaterCausticsFrame).
        // The renderer states the view; the module supplies its frame's waves, bodies and lanes.
        struct CausticsInputs
        {
            glm::vec3       SunTravel{ 0.0f, -1.0f, 0.0f }; // the direction the sunlight travels
            glm::vec3       Camera{ 0.0f };                 // the view's world position (cm)
            float           TimeSeconds = 0.0f;             // ViewFrame::TimeSeconds = the water mesh's cameraUB.Time
            RDG::TextureRef Black;     // FrameGraphRefs::System.Black: the splat's ground where no WaterInfo is baked
            RDG::TextureRef WaterInfo; // the imported GetWaterInfo, or invalid when none is baked
            RDG::TextureRef FFTWaves;  // this frame's FFT atlas (the graph's black without one)
        };
        struct CausticsFrame
        {
            RDG::TextureRef Map;
            RDG::BufferRef  Row;
        };
        // Effects.WaterCaustics (SceneRenderer::OnUpdate, with every quality choice of the view).
        virtual void                     SetCausticsEnabled( bool enabled ) noexcept = 0;
        [[nodiscard]] virtual bool       CausticsEnabled() const noexcept            = 0;
        // Whether this frame's water has a waves block for the caustics to refract through (after PrepareFrame).
        [[nodiscard]] virtual bool       FrameHasWaves() const                      = 0;
        // The frame's caustics passes when they draw; nullopt = no pass, the graph keeps its 'off' pair.
        [[nodiscard]] virtual std::optional<CausticsFrame> AddCausticsPasses( RDG::Builder&         graph,
                                                                             const CausticsInputs& inputs ) = 0;

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
