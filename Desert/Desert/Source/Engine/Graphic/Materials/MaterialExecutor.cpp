#include <algorithm>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/ShaderResources/ShaderResourcesManager.hpp>

#include <cstring>

namespace Desert::Graphic
{
    namespace
    {
        // The push block of a Vulkan program, by the one function its pipeline's layout range uses.
        uint32_t VulkanPushBlockSize( const std::shared_ptr<Shader>& shader )
        {
            if ( !shader )
            {
                return 0u;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): Vulkan branch, Vulkan shader
            auto* vulkanShader = static_cast<API::Vulkan::VulkanShader*>( shader.get() );
            return ShaderResources::ShaderLayout::PushBlockSize( vulkanShader->GetShaderPushConstant() );
        }
    } // namespace

    MaterialExecutor::MaterialExecutor( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                                        const Core::Formats::ShaderProgramMeta& parameterSchema,
                                        std::unique_ptr<MaterialBackend>&&      materialBackend,
                                        uint32_t                                pushBlockSize )
         : m_DebugName( std::move( debugName ) ), m_MaterialBackend( std::move( materialBackend ) ),
           m_Shader( shader )
    {
        // Exactly the shader's push block — the pipeline's layout range — so what the material pushes is
        // never longer than the range it is pushed through. Zeroed so the bytes a draw does not write are
        // defined.
        m_PushConstantBuffer.Allocate( pushBlockSize );
        m_PushConstantBuffer.ZeroInitialize();
        InitializeProperties( parameterSchema );
    }

    void MaterialExecutor::PushConstant( const void* buffer, uint32_t bufferSize, uint32_t offset )
    {
        // Not Buffer::Write: that sets Size to the LAST write's length, and the block is the shader's whole
        // range whatever was written last.
        if ( static_cast<std::size_t>( offset ) + bufferSize > m_PushConstantBuffer.AllocatedSize )
        {
            if ( !m_ReportedPushOverflow )
            {
                m_ReportedPushOverflow = true;
                LOG_ERROR(
                     "Material '{}': a push write of {} bytes at offset {} is past shader '{}''s {}-byte push "
                     "block; refused (reported once per material)",
                     m_DebugName, bufferSize, offset, m_Shader ? m_Shader->GetName() : std::string( "<none>" ),
                     m_PushConstantBuffer.AllocatedSize );
            }
            return;
        }
        std::memcpy( static_cast<std::byte*>( m_PushConstantBuffer.Data ) + offset, buffer, bufferSize );
    }

    void MaterialExecutor::InitializeProperties( const Core::Formats::ShaderProgramMeta& parameterSchema )
    {
        // THE MATERIAL OWNS EXACTLY ITS PROPERTIES. A sampler the shader declares and the schema's
        // Properties block does not is a pass parameter (shadow cascades, environment cubes, BRDF LUT, cloud
        // shadow map, scene textures): the pass binds it through RDG::PassBindings, and no property exists
        // here for anything to write into -- "filled both" cannot be built.
        const auto parameterFor = [&]( const std::string& name, bool cube )
        { return Core::Formats::FindMaterialTextureParameter( parameterSchema, name, cube ); };

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
        const auto addProperties = [&]( const auto& names, auto&& admit, auto&& fetch, auto&& make, auto& storage,
                                        auto& lookup, const char* kind )
        {
            for ( auto [name, index] : names )
            {
                if ( !admit( name ) )
                    continue;
                auto resource = fetch( name );
                if ( !resource )
                {
                    LOG_ERROR( "[MaterialExecutor] shader '{}' declares {} '{}', but it is not "
                               "registered: {}. The material is built WITHOUT it.",
                               m_Shader->GetName(), kind, name, resource.GetError() );
                    continue;
                }
                storage.push_back( make( name, resource.GetValue() ) );
                lookup[name] = storage.size() - 1;
            }
        };

        addProperties(
             uniformManager->GetUniformBufferTotal().Names, []( const std::string& ) { return true; },
             [&]( const std::string& n ) { return uniformManager->GetUniformBuffer( n ); },
             []( const std::string&, const auto& r ) { return std::make_shared<UniformBufferProperty>( r ); },
             m_UniformBufferPropertiesStorage, m_UniformBufferPropertiesLookup, "uniform buffer" );

        addProperties(
             uniformManager->GetStorageBufferTotal().Names, []( const std::string& ) { return true; },
             [&]( const std::string& n ) { return uniformManager->GetStorageBuffer( n ); },
             []( const std::string&, const auto& r ) { return std::make_shared<StorageBufferProperty>( r ); },
             m_StorageBufferPropertiesStorage, m_StorageBufferPropertiesLookup, "storage buffer" );

        addProperties(
             uniformManager->GetUniformImageCubeTotal().Names,
             [&]( const std::string& n ) { return parameterFor( n, true ) != nullptr; },
             [&]( const std::string& n ) { return uniformManager->GetUniformImageCube( n ); },
             []( const std::string&, const auto& r ) { return std::make_shared<TextureCubeProperty>( r ); },
             m_TextureCubePropertiesStorage, m_TextureCubePropertiesLookup, "image cube" );

        addProperties(
             uniformManager->GetUniformImage2DTotal().Names, [&]( const std::string& n )
             { return parameterFor( n, false ) != nullptr; }, [&]( const std::string& n )
             { return uniformManager->GetUniformImage2D( n ); }, [&]( const std::string& n, const auto& r )
             { return std::make_shared<Texture2DProperty>( r, parameterFor( n, false )->DefaultTexture ); },
             m_Texture2DPropertiesStorage, m_Texture2DPropertiesLookup, "image2D" );
    }

    RDG::OtherRouteFill MaterialExecutor::GetRouteFill() const
    {
        RDG::OtherRouteFill fill;
        fill.PushConstants = m_PushConstantBuffer.Size != 0u;
        const auto add     = [&fill]( const auto& lookup )
        {
            for ( const auto& [name, index] : lookup )
                fill.Slots.push_back( name );
        };
        add( m_UniformBufferPropertiesLookup );
        // A storage buffer nothing ever wrote is NOT filled: its slot is then "filled by neither the pass nor the
        // material" in the pass's setup validation, before any draw is recorded.
        for ( const auto& [name, index] : m_StorageBufferPropertiesLookup )
            if ( m_StorageBufferPropertiesStorage[index]->IsWritten() )
                fill.Slots.push_back( name );
        add( m_Texture2DPropertiesLookup );
        add( m_TextureCubePropertiesLookup );
        // Deterministic, so a validation message does not depend on hash-map order.
        std::sort( fill.Slots.begin(), fill.Slots.end() );
        return fill;
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

    std::unique_ptr<MaterialExecutor>
    MaterialExecutor::Create( std::string&& debugName, const std::string& shaderName,
                              const Core::Formats::ShaderProgramMeta* parameterSchema )
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
                     parameterSchema != nullptr ? *parameterSchema : resolvedShader->GetProgramMeta(),
                     std::make_unique<API::Vulkan::VulkanMaterialBackend>( resolvedShader ),
                     VulkanPushBlockSize( resolvedShader ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

    std::unique_ptr<MaterialExecutor>
    MaterialExecutor::Create( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                              const Core::Formats::ShaderProgramMeta* parameterSchema )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                return std::make_unique<MaterialExecutor>(
                     std::move( debugName ), shader,
                     parameterSchema != nullptr ? *parameterSchema : shader->GetProgramMeta(),
                     std::make_unique<API::Vulkan::VulkanMaterialBackend>( shader ),
                     VulkanPushBlockSize( shader ) );
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