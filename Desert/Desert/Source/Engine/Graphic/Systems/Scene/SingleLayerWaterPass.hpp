#pragma once

#include <Engine/Graphic/RainOcclusion.hpp>

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
    // The frame's water in one graph: what every reader of the surface binds.
    struct WaterFrameRefs
    {
        RDG::TextureRef Waves; // the FFT ocean's array, or one still texel
        RDG::TextureRef Shore; // the floor and its shore
        RDG::TextureRef River; // the rivers' flow and level
        RDG::TextureRef Foam;  // the foam atlas this frame wrote, the graph's black without one

        [[nodiscard]] bool IsValid() const
        {
            return Waves.IsValid() && Shore.IsValid() && River.IsValid() && Foam.IsValid();
        }
    };
    // What the graph imports for it.
    struct WaterFrameImports
    {
        RDG::TextureRef River;
        RDG::TextureRef Spectrum; // invalid: no FFT ocean
        RDG::TextureRef FoamHistory;
        RDG::TextureRef FoamTarget; // both invalid: no foam
        RDG::TextureRef Black;
    };

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
        // THE FRAME'S WATER (SceneRenderer::FrameWater): the persistent images the renderer imports (null: none),
        // and the passes that advance the waves and the foam to @p timeSeconds, recorded once per graph;
        // RecordedFrame answers the refs they returned for @p graph.
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetRiverFlow() const          = 0;
        [[nodiscard]] virtual bool                            FrameWavesAreFFT() const      = 0;
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetFFTInitialSpectrum() const = 0;
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetFoamHistory() const        = 0;
        [[nodiscard]] virtual const std::shared_ptr<Image2D>& GetFoamTarget() const         = 0;
        // Where the ground finds the foam written last (RainOcclusionUB's shore half).
        [[nodiscard]] virtual ShoreFoamUniforms GetShoreFoamUniforms() const = 0;
        [[nodiscard]] virtual WaterFrameRefs                  RecordedFrame( const RDG::Builder& graph ) const = 0;
        virtual WaterFrameRefs AddFramePasses( RDG::Builder& graph, const WaterFrameImports& imports,
                                               float timeSeconds ) = 0;

        // "Deferred: Water": the surface into the scene target, after the scene copy.
        virtual void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, const WaterFrameRefs& water,
                                   RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                                   const SceneViewInputs& view, const SceneFrameBinding& frame )    = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderManual( const RDG::PassContext& context ) = 0;

        // "Translucency: Front Layer G-buffer (Water)": the G-buffer the front layer's reflections trace from.
        [[nodiscard]] virtual bool HasFrontLayerDraws() const = 0;
        virtual void               DeclareFrontLayerDraws( RDG::PassBuilder& pass, const WaterFrameRefs& water,
                                                           const SceneViewInputs& view,
                                                           const SceneFrameBinding& frame ) = 0;
        [[nodiscard]] virtual Common::BoolResultStr RenderFrontLayerManual( const RDG::PassContext& context ) = 0;

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
            WaterFrameRefs  Water;                          // invalid when no water of the frame has waves
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
