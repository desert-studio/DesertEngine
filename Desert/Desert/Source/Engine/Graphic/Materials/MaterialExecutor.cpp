#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/ShaderResources/ShaderResourcesManager.hpp>

static constexpr uint32_t kMaxPushConstantsSize = 128U;

namespace Desert::Graphic
{
    MaterialExecutor::MaterialExecutor( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                                        std::unique_ptr<MaterialBackend>&& materialBackend )
         : m_DebugName( std::move( debugName ) ), m_MaterialBackend( std::move( materialBackend ) ),
           m_Shader( shader )
    {
        m_PushConstantBuffer.Allocate( kMaxPushConstantsSize );
        // Zero so any push-constant bytes the shader declares but a draw doesn't explicitly write
        // (we push the full reflected range size) are defined rather than garbage.
        m_PushConstantBuffer.ZeroInitialize();
        InitializeProperties();
    }

    void MaterialExecutor::InitializeProperties()
    {
        auto uniformManager =
             ShaderResources::ShaderResourcesManager::Create( "Material_" + m_Shader->GetName(), m_Shader );

        // THE NAME AND THE ANSWER COME FROM THE SAME MANAGER, so a miss here is a broken invariant
        // rather than an ordinary failure — and that is precisely why these four unwraps went
        // unchecked for so long. They stopped being safe the day the registrars learned to REFUSE:
        // a resource that could not be created is no longer registered under its name, so the name
        // list and the lookup can now disagree by design. Skipping the entry keeps the two property
        // vectors and their lookup maps consistent with each other, which is what the draw path
        // indexes; building a property around nothing would have put a null in the vector and moved
        // the crash to the first frame that drew with this material.
        const auto addProperties =
             [&]( const auto& names, auto&& fetch, auto&& make, auto& storage, auto& lookup, const char* kind )
        {
            for ( auto [name, index] : names )
            {
                auto resource = fetch( name );
                if ( !resource )
                {
                    LOG_ERROR( "[MaterialExecutor] shader '{}' declares {} '{}', but it is not "
                               "registered: {}. The material is built WITHOUT it.",
                               m_Shader->GetName(), kind, name, resource.GetError() );
                    continue;
                }
                storage.push_back( make( resource.GetValue() ) );
                lookup[name] = storage.size() - 1;
            }
        };

        addProperties(
             uniformManager->GetUniformBufferTotal().Names,
             [&]( const std::string& n ) { return uniformManager->GetUniformBuffer( n ); },
             []( const auto& r ) { return std::make_shared<UniformBufferProperty>( r ); },
             m_UniformBufferPropertiesStorage, m_UniformBufferPropertiesLookup, "uniform buffer" );

        addProperties(
             uniformManager->GetStorageBufferTotal().Names,
             [&]( const std::string& n ) { return uniformManager->GetStorageBuffer( n ); },
             []( const auto& r ) { return std::make_shared<StorageBufferProperty>( r ); },
             m_StorageBufferPropertiesStorage, m_StorageBufferPropertiesLookup, "storage buffer" );

        addProperties(
             uniformManager->GetUniformImageCubeTotal().Names,
             [&]( const std::string& n ) { return uniformManager->GetUniformImageCube( n ); },
             []( const auto& r ) { return std::make_shared<TextureCubeProperty>( r ); },
             m_TextureCubePropertiesStorage, m_TextureCubePropertiesLookup, "image cube" );

        addProperties(
             uniformManager->GetUniformImage2DTotal().Names,
             [&]( const std::string& n ) { return uniformManager->GetUniformImage2D( n ); },
             []( const auto& r ) { return std::make_shared<Texture2DProperty>( r ); },
             m_Texture2DPropertiesStorage, m_Texture2DPropertiesLookup, "image2D" );
    }

    void MaterialExecutor::Apply() const
    {
        auto backend = m_MaterialBackend.get();
        if ( !backend )
        {
            LOG_ERROR( "MaterialExecutor::Apply: MaterialBackend is null for: {}", m_DebugName );
            return;
        }

        for ( auto& prop : m_UniformBufferPropertiesStorage )
        {
            prop->Apply( backend );
        }

        for ( auto& prop : m_Texture2DPropertiesStorage )
        {
            prop->Apply( backend );
        }

        for ( auto& prop : m_TextureCubePropertiesStorage )
        {
            prop->Apply( backend );
        }

        for ( auto& prop : m_StorageBufferPropertiesStorage )
        {
            prop->Apply( backend );
        }

        backend->FlushUpdates();
    }

    std::unique_ptr<MaterialExecutor> MaterialExecutor::Create( std::string&& debugName, std::string&& shaderName )
    {
        const auto& resolvedShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
        if ( !resolvedShader )
        {
            LOG_ERROR( "Could not find the shader: {}", shaderName );
            DESERT_VERIFY( false );
        }

        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                return std::make_unique<MaterialExecutor>(
                     std::move( debugName ), resolvedShader,
                     std::make_unique<API::Vulkan::VulkanMaterialBackend>( resolvedShader ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

    std::unique_ptr<MaterialExecutor> MaterialExecutor::Create( std::string&&                  debugName,
                                                                const std::shared_ptr<Shader>& shader )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                return std::make_unique<MaterialExecutor>(
                     std::move( debugName ), shader,
                     std::make_unique<API::Vulkan::VulkanMaterialBackend>( shader ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

    std::shared_ptr<UniformBufferProperty>
    MaterialExecutor::GetUniformBufferProperty( const std::string& name ) const
    {
        auto it = m_UniformBufferPropertiesLookup.find( name );
        if ( it != m_UniformBufferPropertiesLookup.end() )
        {
            return m_UniformBufferPropertiesStorage[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<Desert::Graphic::StorageBufferProperty>
    MaterialExecutor::GetStorageBufferProperty( const std::string& name ) const
    {
        auto it = m_StorageBufferPropertiesLookup.find( name );
        if ( it != m_StorageBufferPropertiesLookup.end() )
        {
            return m_StorageBufferPropertiesStorage[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<Texture2DProperty> MaterialExecutor::GetTexture2DProperty( const std::string& name ) const
    {
        auto it = m_Texture2DPropertiesLookup.find( name );
        if ( it != m_Texture2DPropertiesLookup.end() )
        {
            return m_Texture2DPropertiesStorage[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<TextureCubeProperty> MaterialExecutor::GetTextureCubeProperty( const std::string& name ) const
    {
        auto it = m_TextureCubePropertiesLookup.find( name );
        if ( it != m_TextureCubePropertiesLookup.end() )
        {
            return m_TextureCubePropertiesStorage[it->second];
        }
        return nullptr;
    }
} // namespace Desert::Graphic