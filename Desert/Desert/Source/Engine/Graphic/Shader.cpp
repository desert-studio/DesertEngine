#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{

    std::shared_ptr<Shader> Shader::Create( const Assets::Asset<Assets::ShaderAsset>& asset,
                                            const ShaderVariant& variant, const std::string& passName )
    {
        auto shader = Renderer::GetInstance().Rhi().CreateShader( asset, variant, passName );
        DESERT_VERIFY( shader );
        return shader;
    }

    std::shared_ptr<Shader> Shader::CreateFromShaderMap( const Assets::Asset<Assets::ShaderAsset>& asset,
                                                         const ShaderVariant& variant, const std::string& passName,
                                                         const Core::ShaderMap& built )
    {
        auto shader = Renderer::GetInstance().Rhi().CreateShaderFromMap( asset, variant, passName, built );
        DESERT_VERIFY( shader );
        return shader;
    }

} // namespace Desert::Graphic
