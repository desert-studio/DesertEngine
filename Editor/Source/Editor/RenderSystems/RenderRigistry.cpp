#include "RenderRigistry.hpp"

namespace Desert::Editor::Render
{
    RenderRegistry::RenderRegistry( const std::shared_ptr<::Desert::Core::Scene>& scene ) : m_Scene( scene )
    {
        m_GridPass = std::make_unique<EditorGridPass>();
        if ( const auto result = m_GridPass->Install( scene ); !result )
        {
            LOG_WARN( "[RenderRegistry] {}", result.GetError() );
            m_GridPass.reset();
        }

        m_ColliderPass = std::make_unique<EditorColliderPass>();
        if ( const auto result = m_ColliderPass->Install( scene ); !result )
        {
            LOG_WARN( "[RenderRegistry] {}", result.GetError() );
            m_ColliderPass.reset();
        }

        m_UIPass = std::make_unique<EditorUIPass>();
        if ( const auto result = m_UIPass->Install( scene ); !result )
        {
            LOG_WARN( "[RenderRegistry] {}", result.GetError() );
            m_UIPass.reset();
        }
    }

    void RenderRegistry::TickRenderTextures( Assets::AssetManager& assetManager, const Common::Timestep& ts )
    {
        // A document whose UI pass failed to install has no render-texture cache and therefore no
        // captures and no slots — nothing to advance, and nothing to say about it that Install did not
        // already say.
        if ( m_UIPass )
        {
            m_UIPass->TickRenderTextures( assetManager, ts );
        }
    }

    void RenderRegistry::Render()
    {
        // Per-frame editor draws that DON'T go through the render graph would go here. The graph-injected
        // passes (grid) execute inside SceneRenderer::OnUpdate on their own.
    }

} // namespace Desert::Editor::Render
