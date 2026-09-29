#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RenderPhaseRegistry.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <mutex>
#include <Common/Core/Units.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <format>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <Engine/Graphic/SceneRendererFrame.hpp>

namespace Desert::Graphic
{
    void SceneRenderer::AddFrameGBuffer( RDG::Builder& graph, const std::vector<RDG::TextureRef>& gbuffer,
                                         System::MeshRenderer* meshRenderer )
    {
        AddLegacy( graph, "Deferred: GBuffer", {}, gbuffer,
                   [meshRenderer]() { meshRenderer->RenderGBufferManual(); } );
    }

    void SceneRenderer::AddFrameTerrainGBuffer( RDG::Builder& graph, const std::vector<RDG::TextureRef>& gbuffer )
    {
        // Its own row, so the ground's G-buffer cost reads as a pass line (the forward path's is the
        // graph's "TerrainPass").
        // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        AddLegacy(
             graph, "TerrainGBuffer", {}, gbuffer,
             [this]() {
                 UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->RenderGBufferManual();
             } );
        // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
    }

    void SceneRenderer::AddFrameRSM( RDG::Builder& graph, const std::vector<RDG::TextureRef>& rsm,
                                     System::MeshRenderer* meshRenderer, const glm::vec3& sunDir )
    {
        if ( glm::distance( sunDir, m_RSMLastSunDir ) > 1e-4f || m_RSMFrameCounter == 0 )
        {
            AddLegacy( graph, "Deferred: RSM", {}, rsm, [meshRenderer]() { meshRenderer->RenderRSMManual(); } );
            m_RSMLastSunDir = sunDir;
        }
    }

    void SceneRenderer::AddFrameGeneric( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor,
                                         System::MeshRenderer* meshRenderer )
    {
        AddLegacy( graph, "Deferred: Generic", {}, sceneColor,
                   [meshRenderer]() { meshRenderer->RenderGenericManual(); } );
    }

    void SceneRenderer::AddFrameSkinned( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor,
                                         System::MeshRenderer* meshRenderer )
    {
        AddLegacy( graph, "Deferred: Skinned", {}, sceneColor,
                   [meshRenderer]() { meshRenderer->RenderSkinnedManual(); } );
    }

    void SceneRenderer::AddFrameGlass( RDG::Builder& graph, const std::vector<RDG::TextureRef>& copyReads,
                                       const std::vector<RDG::TextureRef>&       sceneColor,
                                       System::MeshRenderer*                     meshRenderer,
                                       const std::shared_ptr<LegacyFrameValues>& values )
    {
        AddLegacy( graph, "Deferred: Glass", copyReads, sceneColor,
                   [meshRenderer, values]() { meshRenderer->RenderGlassManual( values->SceneCopy ); } );
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameOverdraw( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor )
    {
        AddLegacy(
             graph, "Debug: Overdraw", {}, sceneColor, [this]()
             { UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->RenderOverdrawManual(); } );
    }
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Graphic
