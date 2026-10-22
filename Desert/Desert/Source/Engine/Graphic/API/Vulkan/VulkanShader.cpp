#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>

#include <algorithm>
#include <format>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderMapBuild.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // SPIR-V OpCapability (opcode 17) and the capability a ray-query stage declares (SPIR-V spec,
        // Capability RayQueryKHR = 4472; glslang emits it for GL_EXT_ray_query).
        constexpr uint32_t kSpirvOpCapability       = 17;
        constexpr uint32_t kSpirvCapabilityRayQuery = 4472;
        constexpr size_t   kSpirvHeaderWords        = 5;

        // Whether @p spirv declares @p capability. OpCapability instructions open the module, right after
        // the header; the walk stops at the first other instruction.
        bool SpirvDeclaresCapability( const std::vector<uint32_t>& spirv, const uint32_t capability )
        {
            size_t word = kSpirvHeaderWords;
            while ( word < spirv.size() )
            {
                const uint32_t count  = spirv[word] >> 16;
                const uint32_t opcode = spirv[word] & 0xFFFFu;
                if ( opcode != kSpirvOpCapability || count < 2 || word + 1 >= spirv.size() )
                    return false;
                if ( spirv[word + 1] == capability )
                    return true;
                word += count;
            }
            return false;
        }
    } // namespace

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

        // The CPU half (shader map cache, preprocess, compile) is Core::BuildShaderMap, shared with the cold-start
        // build that runs it on the job system (SHC1); only the device half stays here.
        auto built = Core::BuildShaderMap(
             { asset->GetShaderContent(), m_ShaderPath, m_PassName, m_Variant, m_ShaderName } );
        if ( !built.IsSuccess() )
        {
            m_CompileError = built.GetError();
            return Common::MakeError( built.GetError() );
        }
        // A program the build answered with no stage at all is not "compiled with nothing to do": nothing would
        // ever draw or dispatch through it. Refused by name here, so the reason is not an empty stage list.
        if ( built.GetValue().Stages.empty() )
        {
            m_CompileError = std::format( "Shader '{}': the build produced no stage", m_ShaderName );
            return Common::MakeError( m_CompileError );
        }
        m_ProgramMeta = built.GetValue().Meta;
        auto rebuilt  = BuildFromSpirv( built.GetValue().Stages );
        if ( rebuilt )
        {
            m_CompileError.clear();
            BumpCodeGeneration(); // the pipelines built from the previous modules are now behind
        }
        else
            m_CompileError = rebuilt.GetError();
        return rebuilt;
    }

    Common::BoolResultStr VulkanShader::BuildFromSpirv( const std::vector<Core::ShaderMapStage>& stages )
    {
        const auto logical = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        auto       device  = logical->GetVulkanLogicalDevice();

        // A program that needs inline ray tracing is not built on a device without it (UE: a ray-tracing shader
        // is not compiled for a platform/device that lacks RHI ray tracing): vkCreateShaderModule would be handed
        // the RayQueryKHR capability (VUID-VkShaderModuleCreateInfo-pCode-08740) and the set layout an
        // ACCELERATION_STRUCTURE_KHR binding the device has no enumerator for. The refusal names the device; the
        // methods that use these programs are never resolved on it (NeedsRayQuery, Graphic::ResolveRenderMethods).
        const DeviceCaps& caps = logical->GetPhysicalDevice()->GetDeviceCaps();
        if ( !caps.Has( Capability::RayQuery ) || !caps.Has( Capability::AccelerationStructure ) )
        {
            for ( const auto& [stage, spirv] : stages )
            {
                if ( !SpirvDeclaresCapability( spirv, kSpirvCapabilityRayQuery ) )
                    continue;
                m_DeviceRefusal =
                     std::format( "its {} stage uses inline ray tracing (SPIR-V RayQueryKHR) and device "
                                  "'{}' has no ray query; no module or set layout is created on it",
                                  GetStringShaderStage( stage ), caps.DeviceName );
                return Common::MakeFormattedError( "Shader '{}': {}", m_ShaderName, m_DeviceRefusal );
            }
        }
        // The same for the ray tracing PIPELINE's stages (RayGen, Miss, ClosestHit, AnyHit): a named pass of them
        // is never registered on such a device (ShaderService::Register), but a shader made of such passes alone
        // has its first one as its default program (Programs/Terrain/TerrainRayHit.shader), which must not reach
        // vkCreateShaderModule with the RayTracingKHR capability either.
        if ( !caps.Has( Capability::RayTracingPipeline ) || !caps.Has( Capability::AccelerationStructure ) )
        {
            for ( const auto& [stage, spirv] : stages )
            {
                if ( !Core::Formats::IsRayTracingStage( stage ) )
                    continue;
                m_DeviceRefusal =
                     std::format( "its {} stage is a ray tracing pipeline stage and device '{}' has no "
                                  "ray tracing pipeline; no module or set layout is created on it",
                                  GetStringShaderStage( stage ), caps.DeviceName );
                return Common::MakeFormattedError( "Shader '{}': {}", m_ShaderName, m_DeviceRefusal );
            }
        }
        m_DeviceRefusal.clear();

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
            VkShaderModuleCreateInfo ci      = { .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                 .codeSize = (uint32_t)( spirv.size() * 4 ),
                                                 .pCode    = spirv.data() };
            VkShaderModule           module  = VK_NULL_HANDLE;
            VkResult                 created = VK_SUCCESS;
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

        // The cell's material layout: the template's row/textures and the push block every stage declares,
        // held to each other. A disagreement is a load failure naming the template and the cell — the
        // pipeline would otherwise be built with one stage reading bytes another stage does not lay out.
        auto cell = ShaderReflection::ReconcileCellLayout( m_ProgramMeta, stages, m_ShaderPath.stem().string(),
                                                           m_PassName );
        if ( !cell.Errors.empty() )
        {
            for ( const auto& message : cell.Errors )
                LOG_ERROR( "Shader '{}': {}", m_ShaderName, message );
            discard();
            return Common::MakeFormattedError( "Shader '{}': {} material-layout disagreement(s); first: {}",
                                               m_ShaderName, cell.Errors.size(), cell.Errors.front() );
        }
        if ( reflection.PushConstantRanges )
            reflection.PushConstantRanges->Size = cell.Layout.PushSize;

        // The per-stage limits and the set layouts are part of the transaction: committing first left a shader
        // the limit check had refused with modules and stage infos, so IsCompiled() held and the pipeline was
        // still built on it (VUID-VkGraphicsPipelineCreateInfo-layout-07988, VK_ERROR_INITIALIZATION_FAILED).
        std::vector<DescriptorSetLayoutRef> layouts;
        {
            const Core::ScopedShaderPhase layoutTimer( Core::ShaderPhase::Reflect );
            auto                          laidOut = CreateDescriptorsLayout( reflection, layouts );
            if ( !laidOut.IsSuccess() )
            {
                discard();
                return laidOut;
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
        m_MaterialLayout                 = std::move( cell.Layout );
        m_DescriptorSetLayouts           = std::move( layouts );
        return BOOLSUCCESS;
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
        auto diagnostics = ShaderReflection::ReflectStage( spirv, stage, into );
        // Shared memory above the device's maxComputeSharedMemorySize is not an error the driver must report
        // (Vulkan guarantees only 16 KiB); the program is refused here, by name and with both numbers, so its
        // owner refuses itself instead of dispatching something undefined.
        if ( stage == Core::Formats::ShaderStage::Compute )
        {
            const uint64_t shared  = ShaderReflection::ReflectWorkgroupMemoryBytes( spirv );
            const uint32_t allowed = EngineContext::GetInstance().GetCapabilities().MaxComputeSharedMemorySize;
            if ( shared > allowed )
                diagnostics.push_back( std::format( "declares {} bytes of shared memory (std430); the device's "
                                                    "maxComputeSharedMemorySize is {}",
                                                    shared, allowed ) );
        }
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

    Common::BoolResultStr
    VulkanShader::CreateDescriptorsLayout( const ShaderResource::ReflectionData& reflection,
                                           std::vector<DescriptorSetLayoutRef>&  layouts ) const
    {
        const auto logical = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        auto       device  = logical->GetVulkanLogicalDevice();

        // Every stage is held to the device's per-stage limits here, by name, before any layout exists: past
        // them the pipeline layout is invalid (VUID-VkPipelineLayoutCreateInfo-descriptorType-03016 and kin) and
        // MoltenVK fails the pipeline with no word of which resource tipped it (16 samplers per stage there).
        const DeviceCaps&              caps = logical->GetPhysicalDevice()->GetDeviceCaps();
        const std::vector<std::string> overrun =
             ShaderReflection::PerStageLimitRefusals( reflection, caps.PerStage, caps.DescriptorIndexing );
        for ( const auto& message : overrun )
        {
            LOG_ERROR( "Shader '{}': {}", m_ShaderName, message );
        }
        if ( !overrun.empty() )
            return Common::MakeFormattedError(
                 "Shader '{}' exceeds {} per-stage descriptor limit(s) of '{}'; first: {}", m_ShaderName,
                 overrun.size(), caps.DeviceName, overrun.front() );
        for ( const auto& [setIndex, descriptorSet] : reflection.ShaderDescriptorSets )
        {
            // The binding list comes from the device-free reflection unit, so the shape a pipeline
            // layout and a descriptor set have to agree on is computed by code a test can run without
            // Vulkan (Tests/Engine/ShaderCacheKey drives it over the real engine shaders).
            const auto bindings = ShaderReflection::BuildLayoutBindings( descriptorSet );

            if ( setIndex >= layouts.size() )
                layouts.resize( setIndex + 1 );

            auto layout = std::make_shared<VulkanDescriptorSetLayout>( device, setIndex, bindings, m_ShaderName );
            if ( layout->Handle() == VK_NULL_HANDLE )
                return Common::MakeFormattedError( "Shader '{}': descriptor set layout {} could not be created",
                                                   m_ShaderName, setIndex );

            layouts[setIndex] = std::move( layout );
        }

        // The bindless set's layout is the table's own (update-after-bind, variable count), shared by every
        // pipeline that samples the table, so the one live table set binds to all of them. Reflection has already
        // refused anything else declared in that set.
        if ( reflection.UsesBindlessTable )
        {
            const auto logicalDevice = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
            if ( kBindlessSetIndex >= layouts.size() )
                layouts.resize( kBindlessSetIndex + 1 );
            layouts[kBindlessSetIndex] = logicalDevice->GetBindlessTextureTable().GetLayout();
        }

        // A set the shader skips below one it uses gets an EMPTY layout: a pipeline layout cannot hold
        // VK_NULL_HANDLE in pSetLayouts (RawHandles), and a pass allocates and binds sets 0..N-1 contiguously.
        for ( size_t setIndex = 0; setIndex < layouts.size(); ++setIndex )
        {
            if ( layouts[setIndex] )
                continue;
            auto empty = std::make_shared<VulkanDescriptorSetLayout>( device, static_cast<uint32_t>( setIndex ),
                                                                      std::vector<VkDescriptorSetLayoutBinding>{},
                                                                      m_ShaderName );
            if ( empty->Handle() == VK_NULL_HANDLE )
                return Common::MakeFormattedError(
                     "Shader '{}': empty descriptor set layout {} could not be created", m_ShaderName, setIndex );
            layouts[setIndex] = std::move( empty );
        }

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

    namespace
    {
        // The material-route texture models: every single separate texture of @p dimension, each paired with its
        // own sampler state `<name>Sampler` in the same set when the shader declares one. A pass's inputs are
        // among them too; MaterialExecutor makes a property only for the ones the template's Properties block
        // names. A texture2DArray is a 2D model (LANDSCAPE-MAT-2's layer arrays, `Texture2DArray` in Properties):
        // its Image2D carries the layers, and the view the image makes for them is the 2D array view.
        template <typename Model>
        std::vector<Model> TextureModels( const ShaderResource::ReflectionData&                data,
                                          ShaderResources::ShaderLayout::SampledImageDimension dimension )
        {
            using Dimension = ShaderResources::ShaderLayout::SampledImageDimension;
            std::vector<Model> models;
            for ( const auto& [set, dSet] : data.ShaderDescriptorSets )
                for ( const auto& [binding, image] : dSet.SampledImages )
                {
                    const bool sameDimension =
                         image.Dimension == dimension ||
                         ( dimension == Dimension::Texture2D && image.Dimension == Dimension::Texture2DArray );
                    if ( !sameDimension || image.ArraySize != 1 )
                        continue;
                    Model model;
                    model.BindingPoint  = binding;
                    model.DescriptorSet = image.DescriptorSet;
                    model.Name          = image.Name;
                    model.ShaderStage   = image.ShaderStage;
                    const std::string samplerName =
                         std::format( "{}{}", image.Name, Core::Formats::kMaterialTextureSamplerSuffix );
                    for ( const auto& [samplerBinding, sampler] : dSet.Samplers )
                        if ( sampler.Name == samplerName )
                            model.SamplerBindingPoint = samplerBinding;
                    models.push_back( std::move( model ) );
                }
            return models;
        }
    } // namespace

    const std::vector<ShaderResources::ShaderLayout::ImageCubeSampler>
    VulkanShader::GetUniformImageCubeModels() const
    {
        return TextureModels<ShaderResources::ShaderLayout::ImageCubeSampler>(
             m_ReflectionData, ShaderResources::ShaderLayout::SampledImageDimension::Cube );
    }

    bool VulkanShader::SamplesTexture( const std::string_view name ) const
    {
        for ( const auto& [set, dSet] : m_ReflectionData.ShaderDescriptorSets )
            for ( const auto& [binding, image] : dSet.SampledImages )
                if ( image.Name == name )
                    return true;
        return false;
    }

    const std::vector<ShaderResources::ShaderLayout::Image2DSampler> VulkanShader::GetUniformImage2DModels() const
    {
        return TextureModels<ShaderResources::ShaderLayout::Image2DSampler>(
             m_ReflectionData, ShaderResources::ShaderLayout::SampledImageDimension::Texture2D );
    }

} // namespace Desert::Graphic::API::Vulkan
