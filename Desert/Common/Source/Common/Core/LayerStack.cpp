#include <Core/LayerStack.hpp>

#include <Core/Logger.hpp>

#include <memory>

namespace Common
{
    Layer* LayerStack::PushLayer( std::unique_ptr<Layer> layer )
    {
        if ( !layer )
            return nullptr;

        LOG_INFO( "[Layer] Adding Layer {}", layer->GetName() );
        m_Layers.push_back( std::move( layer ) );
        return m_Layers.back().get();
    }
} // namespace Common
