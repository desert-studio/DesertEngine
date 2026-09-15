#pragma once

#include "Passes/EditorGridPass.hpp"
#include "Passes/EditorColliderPass.hpp"
#include "Passes/EditorUIPass.hpp"

namespace Desert::Editor::Render
{
    // Owns the editor-side render passes injected into the scene render graph through the engine's
    // Editor Pass API (Scene::RegisterExternalPass): the grid now, gizmos/debug draw next. Recreated
    // after every Scene::Init so the pass pipelines rebuild against the fresh scene framebuffers —
    // reset the old registry BEFORE constructing the new one, or the old destructor unregisters the
    // freshly installed passes.
    class RenderRegistry
    {
    public:
        RenderRegistry( const std::shared_ptr<Core::Scene>& scene );

        void Render();

        // Advance the worlds this document's render-texture UI elements show. Forwarded to the UI pass,
        // and called from EditorLayer's pre-update — see EditorUIPass::TickRenderTextures for why it
        // cannot happen inside the pass.
        void TickRenderTextures( Assets::AssetManager& assetManager, const Common::Timestep& ts );

    private:
        std::weak_ptr<Core::Scene> m_Scene;

        std::unique_ptr<EditorGridPass>     m_GridPass;
        std::unique_ptr<EditorColliderPass> m_ColliderPass;
        std::unique_ptr<EditorUIPass>       m_UIPass;
    };
} // namespace Desert::Editor::Render
