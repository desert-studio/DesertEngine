#include "ShaderResourcesManager.hpp"

#include <Common/Core/Logger.hpp>

namespace Desert::ShaderResources
{
    // A NULL BUFFER IS NOT REGISTERED UNDER ITS NAME. Since Г13 the two factories refuse to hand back a
    // buffer that could not allocate or map its device memory, so `Create` can answer nullptr — and a
    // registry entry pointing at nothing is worse than a missing one: every lookup by name would then
    // succeed and hand a null shared_ptr to material code that dereferences it. A MISSING name is a
    // question the material layer already knows how to answer.
    void ShaderResourcesManager::AddUniformBuffer( std::shared_ptr<UniformBuffer>&& buffer,
                                                   const std::string&               name )
    {
        if ( !buffer )
        {
            LOG_ERROR( "[ShaderResources] uniform buffer '{}' was not created (see the error above) and is "
                       "not registered; every material that declares it will report it missing.",
                       name );
            return;
        }
        const auto index = m_UniformBuffersData.Data.size();
        m_UniformBuffersData.Data.push_back( std::move( buffer ) );
        m_UniformBuffersData.Names[name] = index;
    }

    void ShaderResourcesManager::AddStorageBuffer( std::shared_ptr<StorageBuffer>&& buffer,
                                                   const std::string&               name )
    {
        if ( !buffer )
        {
            LOG_ERROR( "[ShaderResources] storage buffer '{}' was not created (see the error above) and is "
                       "not registered; every material that declares it will report it missing.",
                       name );
            return;
        }
        const auto index = m_SrorageBuffersData.Data.size();
        m_SrorageBuffersData.Data.push_back( std::move( buffer ) );
        m_SrorageBuffersData.Names[name] = index;
    }

    // THE SAME RULE, AND IT APPLIES TO ALL FOUR REGISTRARS OR TO NONE. The comment above stated the
    // rule for buffers and these two did the opposite: they registered whatever they were handed,
    // null included, so `GetUniformImage2D(name)` answered SUCCESS holding nullptr — a successful
    // answer carrying nothing, which is the one shape the delivery contract §1.4 names outright.
    // `UniformImage2D::Create`/`UniformImageCube::Create` do return nullptr (the `RendererAPIType::None`
    // arm), so the null is reachable, and MaterialExecutor unwraps these lookups without a check.
    void ShaderResourcesManager::AddImageCube( std::shared_ptr<UniformImageCube>&& buffer,
                                               const std::string&                  name )
    {
        if ( !buffer )
        {
            LOG_ERROR( "[ShaderResources] image cube '{}' was not created (see the error above) and is "
                       "not registered; every material that declares it will report it missing.",
                       name );
            return;
        }
        const auto index = m_ImageCubeData.Data.size();
        m_ImageCubeData.Data.push_back( std::move( buffer ) );
        m_ImageCubeData.Names[name] = index;
    }

    void ShaderResourcesManager::AddImage2D( std::shared_ptr<UniformImage2D>&& buffer, const std::string& name )
    {
        if ( !buffer )
        {
            LOG_ERROR( "[ShaderResources] image2D '{}' was not created (see the error above) and is "
                       "not registered; every material that declares it will report it missing.",
                       name );
            return;
        }
        const auto index = m_Image2DData.Data.size();
        m_Image2DData.Data.push_back( std::move( buffer ) );
        m_Image2DData.Names[name] = index;
    }

    Common::ResultStr<std::shared_ptr<UniformBuffer>>
    ShaderResourcesManager::GetUniformBuffer( const std::string& name ) const
    {
        auto it = m_UniformBuffersData.Names.find( name );
        if ( it == m_UniformBuffersData.Names.end() )
        {
            return Common::MakeFormattedError<std::shared_ptr<UniformBuffer>>(
                 "Uniform '{}' not found in material", name );
        }

        return Common::MakeSuccess( m_UniformBuffersData.Data[it->second] );
    }

    Common::ResultStr<std::shared_ptr<Desert::ShaderResources::StorageBuffer>>
    ShaderResourcesManager::GetStorageBuffer( const std::string& name ) const
    {
        auto it = m_SrorageBuffersData.Names.find( name );
        if ( it == m_SrorageBuffersData.Names.end() )
        {
            return Common::MakeFormattedError<std::shared_ptr<StorageBuffer>>(
                 "Storage buffer '{}' not found in material", name );
        }

        return Common::MakeSuccess( m_SrorageBuffersData.Data[it->second] );
    }

    Common::ResultStr<std::shared_ptr<UniformImageCube>>
    ShaderResourcesManager::GetUniformImageCube( const std::string& name ) const
    {
        auto it = m_ImageCubeData.Names.find( name );
        if ( it == m_ImageCubeData.Names.end() )
        {

            return Common::MakeFormattedError<std::shared_ptr<UniformImageCube>>(
                 "Uniform '{}' not found in material", name );
        }

        return Common::MakeSuccess( m_ImageCubeData.Data[it->second] );
    }

    Common::ResultStr<std::shared_ptr<Desert::ShaderResources::UniformImage2D>>
    ShaderResourcesManager::GetUniformImage2D( const std::string& name ) const
    {
        auto it = m_Image2DData.Names.find( name );
        if ( it == m_Image2DData.Names.end() )
        {

            return Common::MakeFormattedError<std::shared_ptr<UniformImage2D>>(
                 "Uniform '{}' not found in material", name );
        }

        return Common::MakeSuccess( m_Image2DData.Data[it->second] );
    }

    std::shared_ptr<ShaderResourcesManager>
    ShaderResourcesManager::Create( const std::string_view                  debugName,
                                    const std::shared_ptr<Graphic::Shader>& shader )
    {
        return std::make_shared<ShaderResourcesManager>( debugName, shader );
    }

    ShaderResourcesManager::ShaderResourcesManager( const std::string_view                  debugName,
                                                    const std::shared_ptr<Graphic::Shader>& shader )
         : m_DebugName( debugName )
    {

        // Buffers
        {
            const auto& models        = shader->GetUniformBufferModels();
            const auto& storageModels = shader->GetStorageBufferModels();
            m_UniformBuffersData.Data.reserve( models.size() + storageModels.size() );
            m_SrorageBuffersData.Names.reserve( models.size() + storageModels.size() );

            for ( const auto& model : models )
            {
                AddUniformBuffer( UniformBuffer::Create( model ), model.Name );
            }

            // Maybe it should be separated?
            for ( const auto& model : storageModels )
            {
                AddStorageBuffer( StorageBuffer::Create( model.Name, 36, model.BindingPoint ), model.Name );
            }
        }

        // ImageCube
        {
            const auto& models = shader->GetUniformImageCubeModels();
            m_ImageCubeData.Data.reserve( models.size() );
            m_ImageCubeData.Names.reserve( models.size() );

            for ( const auto& model : models )
            {
                auto uniform              = UniformImageCube::Create( model.Name, model.BindingPoint );
                uniform->m_SamplerBinding = model.SamplerBindingPoint;
                AddImageCube( std::move( uniform ), model.Name );
            }
        }

        // Image2D
        {
            const auto& models = shader->GetUniformImage2DModels();
            m_Image2DData.Data.reserve( models.size() );
            m_Image2DData.Names.reserve( models.size() );

            for ( const auto& model : models )
            {
                auto uniform              = UniformImage2D::Create( model.Name, model.BindingPoint );
                uniform->m_SamplerBinding = model.SamplerBindingPoint;
                AddImage2D( std::move( uniform ), model.Name );
            }
        }
    }

} // namespace Desert::ShaderResources