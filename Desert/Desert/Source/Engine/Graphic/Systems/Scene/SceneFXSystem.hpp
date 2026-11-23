#pragma once

// THE SCENE'S FX SYSTEM AS THE RENDERER SEES IT (UE FFXSystemInterface: the Renderer calls the effects system at
// fixed points of the frame - PreRender, PostRenderOpaque, the base pass and the translucency - and Niagara
// implements it; the Renderer never names Niagara). The VFX module registers its factory at StartupModule
// (VFX/Private/VFXModule.cpp). No factory registered (an executable without the VFX module): the view has no FX
// system and every reader tests the slot.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Common/Settings/Scalability.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace Desert::Core
{
    class Camera;
    class Scene;
} // namespace Desert::Core

namespace Desert::Graphic
{
    class Framebuffer;
    class SceneRenderer;
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    class MeshRenderer;

    // One emitter of the frame's tick as one view's cull saw it (ISceneFXSystem::ViewEmitterCulls).
    struct ParticleViewEmitterCull
    {
        uint32_t EntityId = 0; // the raw entt entity value, as VFXSystemDebugStats::EntityId
        uint32_t Emitter  = 0; // the emitter's index in its system
        bool     Culled   = false;
        // The sprite / ribbon draws this view resolved for the emitter (UE stat Niagara's draw calls): Forward
        // ones go to ParticlePass, G-buffer ones (Opaque / Masked on a deferred view) to ParticleGBuffer.
        uint32_t ForwardDraws = 0;
        uint32_t GBufferDraws = 0;
    };

    class ISceneFXSystem : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // BeginScene of a view that draws particles (not a capture face): this view's origin, frustum and Effects
        // rows go to the scene's next scalability decision, the frame's emitters are prepared, and the scene's
        // particle point lights (UE Niagara Light Renderer) are returned for this view's local lights.
        [[nodiscard]] virtual std::span<const ShaderProtocols::PointLightPayload>
        PrepareView( const Core::Scene& scene, const Core::Camera* camera,
                     const Common::Scalability::ResolvedQuality& quality ) = 0;

        // Whether this frame's view has a Mesh row (PrepareView): its instances cast into the cascades.
        [[nodiscard]] virtual bool HasMeshRows() const = 0;
        // An emitter this view draws samples @p textureName (the shadow gates' question).
        [[nodiscard]] virtual bool ViewEmittersSample( std::string_view textureName ) const = 0;
        // Every emitter of this frame's tick and whether this view culled it (VFX-PORT-2).
        [[nodiscard]] virtual std::span<const ParticleViewEmitterCull> ViewEmitterCulls() const = 0;

        // UE PreRender: the PreOpaque simulation (the claiming view), this view's sort and its Mesh rows'
        // instances, before the cascades and the base pass build their draws.
        virtual void AddPreOpaque( RDG::Builder& graph, MeshRenderer* meshes ) = 0;
        // UE PostRenderOpaque: the stacks that read the resolved scene depth, then the readbacks.
        virtual void AddPostOpaque( RDG::Builder& graph, RDG::TextureRef sceneDepth ) = 0;

        // The opaque / masked draws into the G-buffer (the base pass) and the forward draws over the lit scene.
        [[nodiscard]] virtual SystemRasterPass GBufferPass() = 0;
        [[nodiscard]] virtual SystemRasterPass DrawPass()    = 0;
        // Whether this frame's view draws a lit sprite (the translucency lighting volume's question).
        [[nodiscard]] virtual bool DrawsLitSprites() const = 0;
    };

    using SceneFXSystemFactory = std::shared_ptr<ISceneFXSystem> ( * )(
         SceneRenderer* sceneRenderer, const std::shared_ptr<Framebuffer>& targetFramebuffer );

    /// The registered FX system factory (module startup, Engine/Core/Modules.cpp); null without the VFX module.
    inline SceneFXSystemFactory& SceneFXSystemFactorySlot()
    {
        static SceneFXSystemFactory factory = nullptr;
        return factory;
    }

    inline void RegisterSceneFXSystem( SceneFXSystemFactory factory )
    {
        SceneFXSystemFactorySlot() = factory;
    }
} // namespace Desert::Graphic::System
