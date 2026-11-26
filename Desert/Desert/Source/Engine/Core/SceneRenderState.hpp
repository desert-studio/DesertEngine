#pragma once

// THE SCENE'S RENDERER STATE, AS THE ENGINE SEES IT (UE: FSceneInterface - the world holds its render scene by an
// interface the renderer module implements). The renderer's objects cached on a scene - reflection captures,
// ray-tracing cache, GPU scene, sky light, atmosphere, local shadows, distance fields, surface cache - are
// Graphic::SceneRenderState (Renderer module); a Scene owns one through this interface, made by the maker the
// Renderer's StartupModule registers (ModuleHookLists::SceneRenderState), and calls only what follows. Renderer
// code reaches the objects through Graphic::RenderStateOf( scene ).

#include <Common/Core/Timestep.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <Common/Core/Entt.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Graphic
{
    class SceneRenderer;
    // `struct`: the definition in Graphic/Environment/SceneEnvironment.hpp uses struct (MSVC decorates the key).
    struct Environment;
} // namespace Desert::Graphic

namespace Desert::Graphic::ShaderProtocols
{
    struct DirectionLight;
} // namespace Desert::Graphic::ShaderProtocols

namespace Desert::Core
{
    class Scene;

    class ISceneRenderState
    {
    public:
        virtual ~ISceneRenderState() = default;

        /// Once per scene update, on the game thread, before any view opens: the objects Clear parked die at the
        /// fence once @p framesInFlight frames after @p absoluteFrame retired, and the surface cache is handed
        /// the frame's @p viewOrigins with the scene's unbound post settings read from @p registry here.
        virtual void BeginFrame( const entt::registry& registry, bool deferred, std::uint64_t absoluteFrame,
                                 std::uint32_t framesInFlight, std::vector<glm::vec3> viewOrigins ) = 0;

        /// At the fence, before the first view of the update: the reflection captures' face views, fed the
        /// recorded draws by @p record (ReflectionCaptureRenderer::RenderCaptureViews).
        virtual void RenderReflectionCaptures( const Scene&                                          scene,
                                               const std::function<void( Graphic::SceneRenderer& )>& record,
                                               Common::Timestep timestep, Common::Timestep realTimestep,
                                               const Graphic::ShaderProtocols::DirectionLight& dirLights,
                                               const std::optional<Graphic::Environment>&      environment,
                                               bool skyRecapturesForSun ) = 0;

        /// The scene's world was emptied (Scene::Clear): every object starts over for the next world; what frames
        /// in flight still read is parked from @p clearFrame.
        virtual void Clear( std::uint64_t clearFrame ) = 0;

        /// The device is idle and the render task joined (Scene::ReleaseParkedRenderers): the parked objects die
        /// now.
        virtual void ReleaseParked() = 0;
    };

    /// Makes a scene's renderer state, its device objects rows of @p ledger (the scene's ledger instance).
    using SceneRenderStateMaker =
         std::unique_ptr<ISceneRenderState> ( * )( Graphic::ResourceOwnerInstance ledger );
} // namespace Desert::Core
