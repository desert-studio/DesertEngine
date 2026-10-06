#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>

namespace Desert::Graphic
{
    const std::shared_ptr<const RDG::ShaderBindingLayout>&
    ShaderBindingLayoutCache::Get( const std::shared_ptr<Shader>& shader )
    {
        return m_Cache.Get( shader, shader->GetReloadGeneration(),
                            []( const Shader& s ) { return Renderer::GetInstance().GetBindingLayout( s ); } );
    }
} // namespace Desert::Graphic
