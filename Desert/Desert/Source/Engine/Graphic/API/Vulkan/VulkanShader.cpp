#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>

#include <algorithm>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace ReflectionUtils
    {
        static VkShaderStageFlags StageToVkStage( Core::Formats::ShaderStage stage )
        {
            return (VkShaderStageFlags)stage;
        }
        
        static Core::Formats::ShaderStage VkStageToStage( VkShaderStageFlagBits stage )
        {
            return (Core::Formats::ShaderStage)stage;
        }
    }

    VulkanShader::VulkanShader( const Assets::Asset<Assets::ShaderAsset>& asset, const ShaderVariant& variant,
                                const std::string& passName )
         : m_ShaderAsset( asset ), m_PassName( passName ), m_Variant( variant )
    {
        m_ShaderPath = asset->GetMetadata().Filepath;
        m_ShaderName = m_ShaderPath.stem().string();
        if ( !m_PassName.empty() )
            m_ShaderName += "/" + m_PassName;

        Reload();
    }

    VulkanShader::~VulkanShader()
    {
        auto device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        for ( auto module : m_ShaderModules ) vkDestroyShaderModule( device, module, nullptr );
        // The layouts are NOT destroyed here: dropping this shader's references is all this object may
        // do to them. A pipeline that outlives the shader it was built from keeps its own reference and
        // its own valid contract, which is the difference between a stale pipeline and a corrupt one.
        m_DescriptorSetLayouts.clear();
    }

    Common::BoolResultStr VulkanShader::Reload()
    {
        auto asset = m_ShaderAsset.lock();
        if ( !asset ) return Common::MakeError( "Shader asset expired" );

        // THE WARM PATH: the shader map is keyed by the raw text + include files, so a hit is the whole
        // program — metadata and SPIR-V — without parsing the DShader or preprocessing a stage.
        const std::string&    content = asset->GetShaderContent();
        uint64_t              mapKey  = 0;
        Core::ShaderMapLookup lookup;
        {
            const Core::ScopedShaderPhase timer( Core::ShaderPhase::ShaderMap );
            mapKey = Core::ComputeShaderMapKey( content, m_ShaderPath, m_PassName, Core::SpirvDebugInfoThisBuild(),
                                                m_Variant );
            lookup = Core::TryLoadShaderMap( mapKey );
        }
        if ( lookup.Map )
        {
            Core::CountShaderMapHit();
            m_ProgramMeta = std::move( lookup.Map->Meta );
            return BuildFromSpirv( lookup.Map->Stages );
        }
        Core::CountShaderMapMiss();
        if ( !lookup.Rejected.empty() )
            LOG_WARN( "[ShaderMap] '{}': cached entry rejected, rebuilding it ({})", m_ShaderName,
                      lookup.Rejected );

        std::unordered_map<Core::Formats::ShaderStage, std::string> stages;
        {
            const Core::ScopedShaderPhase timer( Core::ShaderPhase::Preprocess );
            m_ProgramMeta = Core::Preprocess::ShaderPreprocess::ParseProgramMetaForPass( asset->GetShaderContent(),
                                                                                         m_PassName );
            stages        = Core::Preprocess::ShaderPreprocess::PreProcessProgramPass( asset->GetShaderContent(),
                                                                                       m_ShaderPath, m_PassName );
        }
        if ( m_ProgramMeta.HasParams() || m_ProgramMeta.State.Topology.has_value() )
        {
            LOG_INFO( "Shader '{}': parsed {} param(s) + render-state from shader metadata", m_ShaderName,
                      m_ProgramMeta.Params.size() );
        }

        Core::ShaderMap built{ m_ProgramMeta, {} };
        for ( const auto& [stage, source] : stages )
        {
            auto spirvResult =
                 Core::ShaderCompiler::CompileGLSLToSPIRV( stage, source, m_ShaderPath.string(), m_Variant );
            if ( !spirvResult.IsSuccess() )
                return Common::MakeError( spirvResult.GetError() );
            built.Stages.push_back( { stage, std::move( spirvResult.GetValue() ) } );
        }
        std::sort( built.Stages.begin(), built.Stages.end(),
                   []( const Core::ShaderMapStage& a, const Core::ShaderMapStage& b )
                   { return static_cast<uint32_t>( a.Stage ) < static_cast<uint32_t>( b.Stage ); } );
        if ( const auto stored = Core::StoreShaderMap( mapKey, built ); !stored )
            LOG_WARN( "[ShaderMap] '{}': could not store the shader map {:016x}: {}", m_ShaderName, mapKey,
                      stored.GetError() );
        return BuildFromSpirv( built.Stages );
    }

    Common::BoolResultStr VulkanShader::BuildFromSpirv( const std::vector<Core::ShaderMapStage>& stages )
    {
        auto device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        // TRANSACTIONAL. Everything is built into locals first, and the shader's own state is only
        // replaced once every stage has compiled and reflected. Tearing the old state down up front
        // meant a hot reload over a shader with one bad line left the object with no modules and no
        // layouts — a "keeping the previous version" that had already thrown the previous version away,
        // and an empty GetPipelineShaderStageCreateInfos() that the next pipeline build indexes [0] of.
        std::vector<VkShaderModule>                  modules;
        std::vector<VkPipelineShaderStageCreateInfo> stageInfos;
        ShaderResource::ReflectionData               reflection;

        const auto discard = [&modules, device]()
        {
            for ( auto module : modules )
                vkDestroyShaderModule( device, module, nullptr );
        };

        for ( const auto& [stage, spirv] : stages )
        {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (uint32_t)(spirv.size() * 4), .pCode = spirv.data() };
            VkShaderModule module  = VK_NULL_HANDLE;
            VkResult       created = VK_SUCCESS;
            {
                const Core::ScopedShaderPhase timer( Core::ShaderPhase::ShaderModule );
                created = vkCreateShaderModule( device, &ci, nullptr, &module );
            }
            if ( created != VK_SUCCESS )
            {
                discard();
                return Common::MakeFormattedError( "Shader '{}': vkCreateShaderModule failed for the {} stage",
                                                   m_ShaderName, GetStringShaderStage( stage ) );
            }
            modules.push_back( module );

            // Debug name so RenderDoc/validation messages identify the module ("Unlit/Shadow [Vertex]").
            VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_SHADER_MODULE,
                                              m_ShaderName + " [" + GetStringShaderStage( stage ) + "]",
                                              module );

            VkShaderStageFlagBits vkStage = (VkShaderStageFlagBits)ReflectionUtils::StageToVkStage( stage );
            stageInfos.push_back( { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                    .stage  = vkStage,
                                    .module = module,
                                    .pName  = "main" } );

            const Core::ScopedShaderPhase reflectTimer( Core::ShaderPhase::Reflect );
            auto                          reflectResult = Reflect( vkStage, spirv, reflection );
            if ( !reflectResult.IsSuccess() )
            {
                discard();
                return Common::MakeError( reflectResult.GetError() );
            }
        }

        // Past this line the compile has succeeded, so the old state may go. The modules are ours alone
        // (a pipeline copies what it needs at creation) and are destroyed; the layouts are only
        // RELEASED, because a pipeline layout, a descriptor pool or an allocated set built from one is
        // still using it and keeps it alive until it is itself rebuilt. Destroying them here is what
        // produced "VkDescriptorSetLayout ... has been destroyed" and a pipeline layout with a different
        // descriptor count from the set bound to it.
        for ( auto module : m_ShaderModules )
            vkDestroyShaderModule( device, module, nullptr );

        m_ShaderModules                  = std::move( modules );
        m_PipelineShaderStageCreateInfos = std::move( stageInfos );
        m_ReflectionData                 = std::move( reflection );
        m_DescriptorSetLayouts.clear();

        const Core::ScopedShaderPhase layoutTimer( Core::ShaderPhase::Reflect );
        return CreateDescriptorsLayout();
    }

    Common::BoolResultStr VulkanShader::Reflect( VkShaderStageFlagBits vkStage, const std::vector<uint32_t>& spirv,
                                                 ShaderResource::ReflectionData& into )
    {
        const Core::Formats::ShaderStage stage = ReflectionUtils::VkStageToStage( vkStage );

        // The reflection proper lives in a device-free translation unit: it is the part that decides
        // which descriptor bucket every resource lands in, and that decision used to be made from the
        // variable's NAME. Keeping it free of VkDevice is what lets a test prove a sampler3D is not
        // filed as a 2D sampler (Desert/Tests/Engine/ShaderReflection, Desert/Tests/Engine/ShaderCacheKey).
        //
        // It reflects into the caller's data, not straight into the shader's: a compile that fails
        // halfway must not leave this object holding half a shader.
        const auto diagnostics = ShaderReflection::ReflectStage( spirv, stage, into );
        if ( diagnostics.empty() )
        {
            return BOOLSUCCESS;
        }

        // Every refused resource is named, with its real type, before the shader is dropped: a missing
        // binding surfaces far from its cause, and a silently mis-filed one never surfaces at all. The
        // same channel carries a descriptor slot claimed twice, which is refused for the same reason —
        // the layout that would come out of it is complete, plausible and missing a resource.
        for ( const auto& message : diagnostics )
        {
            LOG_ERROR( "Shader '{}' [{}]: {}", m_ShaderName, GetStringShaderStage( stage ), message );
        }

        return Common::MakeFormattedError( "Shader '{}' [{}]: {} rejected resource(s); first: {}", m_ShaderName,
                                           GetStringShaderStage( stage ), diagnostics.size(),
                                           diagnostics.front() );
    }

    Common::BoolResultStr VulkanShader::CreateDescriptorsLayout()
    {
        auto device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        for ( const auto& [setIndex, descriptorSet] : m_ReflectionData.ShaderDescriptorSets )
        {
            // The binding list comes from the device-free reflection unit, so the shape a pipeline
            // layout and a descriptor set have to agree on is computed by code a test can run without
            // Vulkan (Tests/Engine/ShaderCacheKey drives it over the real engine shaders).
            const auto bindings = ShaderReflection::BuildLayoutBindings( descriptorSet );

            if ( setIndex >= m_DescriptorSetLayouts.size() )
                m_DescriptorSetLayouts.resize( setIndex + 1 );

            auto layout = std::make_shared<VulkanDescriptorSetLayout>( device, setIndex, bindings, m_ShaderName );
            if ( layout->Handle() == VK_NULL_HANDLE )
                return Common::MakeFormattedError( "Shader '{}': descriptor set layout {} could not be created",
                                                   m_ShaderName, setIndex );

            m_DescriptorSetLayouts[setIndex] = std::move( layout );
        }

        ++m_ReloadGeneration;
        return BOOLSUCCESS;
    }

    const std::vector<ShaderResources::ShaderLayout::UniformBuffer> VulkanShader::GetUniformBufferModels() const 
    { 
        std::vector<ShaderResources::ShaderLayout::UniformBuffer> res;
        for ( const auto& [set, dSet] : m_ReflectionData.ShaderDescriptorSets )
            for ( const auto& [b, ub] : dSet.UniformBuffers ) res.push_back( ub );
        return res;
    }

    const std::vector<ShaderResources::ShaderLayout::StorageBuffer> VulkanShader::GetStorageBufferModels() const 
    { 
        std::vector<ShaderResources::ShaderLayout::StorageBuffer> res;
        for ( const auto& [set, dSet] : m_ReflectionData.ShaderDescriptorSets )
            for ( const auto& [b, sb] : dSet.StorageBuffers ) res.push_back( sb );
        return res;
    }

    const std::vector<ShaderResources::ShaderLayout::ImageCubeSampler> VulkanShader::GetUniformImageCubeModels() const 
    { 
        std::vector<ShaderResources::ShaderLayout::ImageCubeSampler> res;
        for ( const auto& [set, dSet] : m_ReflectionData.ShaderDescriptorSets )
            for ( const auto& [b, sc] : dSet.ImageCubeSamplers ) res.push_back( sc );
        return res;
    }

    const std::vector<ShaderResources::ShaderLayout::Image2DSampler> VulkanShader::GetUniformImage2DModels() const 
    { 
        std::vector<ShaderResources::ShaderLayout::Image2DSampler> res;
        for ( const auto& [set, dSet] : m_ReflectionData.ShaderDescriptorSets )
            for ( const auto& [b, s2] : dSet.Image2DSamplers ) res.push_back( s2 );
        return res;
    }

} // namespace Desert::Graphic::API::Vulkan
