#include <Engine/Graphic/API/Vulkan/VulkanRayTracingPipeline.hpp>

#include <Common/Core/Logger.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/ShaderCompiler/BindlessSetIndex.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanBindlessTextureTable.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingFunctions.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSbtWrites.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // Every stage a binding of the pipeline's sets may be read from: the program's two and the hit cells'.
        constexpr VkShaderStageFlags kRayTracingStages =
             VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
             VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

        VulkanAllocator& Allocator()
        {
            return *SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                         ->GetVulkanAllocator();
        }

        constexpr VkDeviceSize AlignUp( VkDeviceSize value, VkDeviceSize alignment )
        {
            return alignment == 0 ? value : ( value + alignment - 1 ) / alignment * alignment;
        }

        // The stage create info of @p stage in @p shader, or null.
        const VkPipelineShaderStageCreateInfo* StageOf( const VulkanShader& shader, VkShaderStageFlagBits stage )
        {
            for ( const auto& info : shader.GetPipelineShaderStageCreateInfos() )
                if ( info.stage == stage )
                    return &info;
            return nullptr;
        }

        // The program's set-0.. bindings (not the bindless set), by (set, binding).
        struct ProgramBinding
        {
            uint32_t                     Set = 0;
            VkDescriptorSetLayoutBinding Binding{};
        };
    } // namespace

    VulkanRayTracingPipeline::VulkanRayTracingPipeline( RayTracingPipelineSpecification spec )
         : m_Specification( std::move( spec ) )
    {
    }

    VulkanRayTracingPipeline::~VulkanRayTracingPipeline()
    {
        Destroy();
    }

    void VulkanRayTracingPipeline::Destroy()
    {
        {
            const std::lock_guard lock( m_SlotsMutex );
            for ( auto& frame : m_Slots )
                for ( TableSlot& slot : frame )
                {
                    Release( slot.Sbt );
                    Release( slot.Rows );
                }
            m_Slots.clear();
        }
        // A rebuild mid-session (the hit group set changed) drops a pipeline the previous frames recorded
        // vkCmdTraceRaysKHR with: it waits for those frames in the deletion queue, as the tables above do.
        Allocator().RT_DestroyPipeline( m_Pipeline, m_Layout );
        m_Pipeline = VK_NULL_HANDLE;
        m_Layout   = VK_NULL_HANDLE;
    }

    Common::BoolResultStr VulkanRayTracingPipeline::Build()
    {
        const std::string_view name = m_Specification.DebugName;
        const auto             fail = [&]( const std::string& why ) -> Common::BoolResultStr
        { return Common::MakeFormattedError( "RayTracingPipeline '{}': {}", name, why ); };

        const auto logical = VulkanLogicalDevice::Current();
        if ( !logical )
            return fail( "no Vulkan device" );
        const DeviceCaps& caps = logical->GetPhysicalDevice()->GetDeviceCaps();
        if ( !caps.Has( Capability::RayTracingPipeline ) )
            return fail( std::format( "the device '{}' has no ray tracing pipeline (VK_KHR_ray_tracing_pipeline)",
                                      caps.DeviceName ) );
        const VkDevice device = logical->GetVulkanLogicalDevice();

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        const auto* program = dynamic_cast<const VulkanShader*>( m_Specification.Program.get() );
        if ( program == nullptr || !program->IsCompiled() )
            return fail( "the ray generation program is not compiled" );
        const VkPipelineShaderStageCreateInfo* raygen = StageOf( *program, VK_SHADER_STAGE_RAYGEN_BIT_KHR );
        const VkPipelineShaderStageCreateInfo* miss   = StageOf( *program, VK_SHADER_STAGE_MISS_BIT_KHR );
        if ( raygen == nullptr || miss == nullptr || program->GetPipelineShaderStageCreateInfos().size() != 2u )
            return fail(
                 std::format( "program '{}' must be exactly one RayGen and one Miss stage", program->GetName() ) );

        // THE LAYOUTS: the program's reflection with every binding visible to every ray tracing stage.
        const ShaderResource::ReflectionData& reflection = program->GetReflectionData();
        std::vector<ProgramBinding>           programBindings;
        m_SetLayouts.clear();
        for ( const auto& [setIndex, set] : reflection.ShaderDescriptorSets )
        {
            if ( setIndex == Core::kBindlessSetIndex )
                continue;
            std::vector<VkDescriptorSetLayoutBinding> bindings = ShaderReflection::BuildLayoutBindings( set );
            for ( VkDescriptorSetLayoutBinding& b : bindings )
            {
                b.stageFlags |= kRayTracingStages;
                programBindings.push_back( { setIndex, b } );
            }
            if ( setIndex >= m_SetLayouts.size() )
                m_SetLayouts.resize( setIndex + 1 );
            auto layout =
                 std::make_shared<VulkanDescriptorSetLayout>( device, setIndex, bindings, std::string( name ) );
            if ( layout->Handle() == VK_NULL_HANDLE )
                return fail( std::format( "descriptor set layout {} could not be created", setIndex ) );
            m_SetLayouts[setIndex] = std::move( layout );
        }

        // THE HIT CELLS share those sets: every set-0.. binding a cell declares must be the program's, of the same
        // kind and count; the bindless set is the table's own for all of them.
        bool usesBindless = reflection.UsesBindlessTable;
        for ( const auto& cell : m_Specification.HitGroups )
        {
            const auto* hit = dynamic_cast<const VulkanShader*>( cell.get() );
            if ( hit == nullptr || !hit->IsCompiled() )
                return fail( std::format( "hit cell '{}' is not compiled", cell ? cell->GetName() : "<null>" ) );
            if ( StageOf( *hit, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR ) == nullptr )
                return fail( std::format( "hit cell '{}' has no ClosestHit stage", hit->GetName() ) );
            if ( hit->GetReflectionData().PushConstantRanges )
                return fail( std::format( "hit cell '{}' declares a push block; a hit group's per-segment data is "
                                          "its shader record",
                                          hit->GetName() ) );
            usesBindless = usesBindless || hit->GetReflectionData().UsesBindlessTable;
            for ( const auto& [setIndex, set] : hit->GetReflectionData().ShaderDescriptorSets )
            {
                if ( setIndex == Core::kBindlessSetIndex )
                    continue;
                for ( const VkDescriptorSetLayoutBinding& b : ShaderReflection::BuildLayoutBindings( set ) )
                {
                    const auto same = std::find_if( programBindings.begin(), programBindings.end(),
                                                    [&]( const ProgramBinding& p ) {
                                                        return p.Set == setIndex && p.Binding.binding == b.binding;
                                                    } );
                    if ( same == programBindings.end() )
                        return fail( std::format( "hit cell '{}' reads set {} binding {}, which program '{}' does "
                                                  "not bind",
                                                  hit->GetName(), setIndex, b.binding, program->GetName() ) );
                    if ( same->Binding.descriptorType != b.descriptorType ||
                         same->Binding.descriptorCount != b.descriptorCount )
                        return fail( std::format(
                             "hit cell '{}' reads set {} binding {} as descriptor type {} x {}; "
                             "program '{}' binds type {} x {} there",
                             hit->GetName(), setIndex, b.binding, static_cast<int>( b.descriptorType ),
                             b.descriptorCount, program->GetName(),
                             static_cast<int>( same->Binding.descriptorType ), same->Binding.descriptorCount ) );
                }
            }
        }
        if ( usesBindless )
        {
            if ( Core::kBindlessSetIndex >= m_SetLayouts.size() )
                m_SetLayouts.resize( Core::kBindlessSetIndex + 1 );
            m_SetLayouts[Core::kBindlessSetIndex] = logical->GetBindlessTextureTable().GetLayout();
        }
        for ( std::size_t setIndex = 0; setIndex < m_SetLayouts.size(); ++setIndex )
        {
            if ( m_SetLayouts[setIndex] )
                continue;
            m_SetLayouts[setIndex] = std::make_shared<VulkanDescriptorSetLayout>(
                 device, static_cast<uint32_t>( setIndex ), std::vector<VkDescriptorSetLayoutBinding>{},
                 std::string( name ) );
            if ( m_SetLayouts[setIndex]->Handle() == VK_NULL_HANDLE )
                return fail( std::format( "empty descriptor set layout {} could not be created", setIndex ) );
        }

        std::vector<VkDescriptorSetLayout> handles;
        handles.reserve( m_SetLayouts.size() );
        for ( const auto& layout : m_SetLayouts )
            handles.push_back( layout->Handle() );
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = static_cast<uint32_t>( handles.size() );
        layoutInfo.pSetLayouts    = handles.data();
        VkPushConstantRange push{};
        if ( reflection.PushConstantRanges )
        {
            m_PushStages                      = kRayTracingStages;
            push                              = { .stageFlags = m_PushStages,
                                                  .offset     = reflection.PushConstantRanges->Offset,
                                                  .size       = reflection.PushConstantRanges->Size };
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges    = &push;
        }
        if ( vkCreatePipelineLayout( device, &layoutInfo, nullptr, &m_Layout ) != VK_SUCCESS )
            return fail( "vkCreatePipelineLayout failed" );

        // THE STAGES AND GROUPS: 0 = ray generation, 1 = miss, 2 + i = hit cell i.
        std::vector<VkPipelineShaderStageCreateInfo>      stages{ *raygen, *miss };
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        const auto                                        general = [&]( uint32_t stage )
        {
            VkRayTracingShaderGroupCreateInfoKHR g{ VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            g.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            g.generalShader      = stage;
            g.closestHitShader   = VK_SHADER_UNUSED_KHR;
            g.anyHitShader       = VK_SHADER_UNUSED_KHR;
            g.intersectionShader = VK_SHADER_UNUSED_KHR;
            groups.push_back( g );
        };
        general( 0 );
        general( 1 );
        for ( const auto& cell : m_Specification.HitGroups )
        {
            const auto&                          hit = static_cast<const VulkanShader&>( *cell );
            VkRayTracingShaderGroupCreateInfoKHR g{ VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            g.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            g.generalShader      = VK_SHADER_UNUSED_KHR;
            g.intersectionShader = VK_SHADER_UNUSED_KHR;
            g.closestHitShader   = static_cast<uint32_t>( stages.size() );
            stages.push_back( *StageOf( hit, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR ) );
            g.anyHitShader = VK_SHADER_UNUSED_KHR;
            if ( const auto* anyHit = StageOf( hit, VK_SHADER_STAGE_ANY_HIT_BIT_KHR ) )
            {
                g.anyHitShader = static_cast<uint32_t>( stages.size() );
                stages.push_back( *anyHit );
            }
            groups.push_back( g );
        }

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties{
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &properties;
        vkGetPhysicalDeviceProperties2( logical->GetPhysicalDevice()->GetVulkanPhysicalDevice(), &properties2 );
        m_HandleSize      = properties.shaderGroupHandleSize;
        m_HandleAlignment = properties.shaderGroupHandleAlignment;
        m_BaseAlignment   = properties.shaderGroupBaseAlignment;
        m_MaxStride       = properties.maxShaderGroupStride;

        const auto createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
             vkGetDeviceProcAddr( device, "vkCreateRayTracingPipelinesKHR" ) );
        const auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
             vkGetDeviceProcAddr( device, "vkGetRayTracingShaderGroupHandlesKHR" ) );
        m_CmdTraceRays =
             reinterpret_cast<PFN_vkCmdTraceRaysKHR>( vkGetDeviceProcAddr( device, "vkCmdTraceRaysKHR" ) );
        if ( createPipelines == nullptr || getHandles == nullptr || m_CmdTraceRays == nullptr )
            return fail( "the device exposes no ray tracing pipeline entry points" );

        // The ray generation traces; a hit stage does not (the payload carries the material back): depth 1.
        VkRayTracingPipelineCreateInfoKHR info{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        info.stageCount                   = static_cast<uint32_t>( stages.size() );
        info.pStages                      = stages.data();
        info.groupCount                   = static_cast<uint32_t>( groups.size() );
        info.pGroups                      = groups.data();
        info.maxPipelineRayRecursionDepth = 1;
        info.layout                       = m_Layout;
        if ( const VkResult made = createPipelines( device, VK_NULL_HANDLE, logical->GetPipelineCache(), 1, &info,
                                                    nullptr, &m_Pipeline );
             made != VK_SUCCESS )
        {
            m_Pipeline = VK_NULL_HANDLE;
            return fail( std::format( "vkCreateRayTracingPipelinesKHR failed ({}) for {} hit groups",
                                      static_cast<int>( made ), m_Specification.HitGroups.size() ) );
        }

        m_Handles.resize( static_cast<std::size_t>( groups.size() ) * m_HandleSize );
        if ( getHandles( device, m_Pipeline, 0, static_cast<uint32_t>( groups.size() ), m_Handles.size(),
                         m_Handles.data() ) != VK_SUCCESS )
            return fail( "vkGetRayTracingShaderGroupHandlesKHR failed" );
        return BOOLSUCCESS;
    }

    Common::ResultStr<VulkanRayTracingPipeline::HostBuffer>
    VulkanRayTracingPipeline::CreateHostBuffer( const char* tag, VkDeviceSize bytes,
                                                VkBufferUsageFlags usage ) const
    {
        const auto logical = VulkanLogicalDevice::Current();
        if ( !logical )
            return Common::MakeError<HostBuffer>( "no Vulkan device" );
        HostBuffer               out;
        const VkBufferCreateInfo info = DeviceAddressBufferInfo( bytes, usage );
        const auto               allocation =
             Allocator().RT_AllocateBuffer( tag, info, VMA_MEMORY_USAGE_CPU_TO_GPU, out.Buffer );
        if ( !allocation )
            return Common::MakeFormattedError<HostBuffer>( "{}: {}", tag, allocation.GetError() );
        out.Allocation = allocation.GetValue();
        out.Address =
             logical->GetRayTracingFunctions().BufferAddress( logical->GetVulkanLogicalDevice(), out.Buffer );
        return Common::MakeSuccess( out );
    }

    void VulkanRayTracingPipeline::Release( HostBuffer& buffer ) const
    {
        if ( buffer.Buffer != VK_NULL_HANDLE )
            Allocator().RT_DestroyBuffer( buffer.Buffer, buffer.Allocation );
        buffer = {};
    }

    VulkanRayTracingPipeline::TableSlot& VulkanRayTracingPipeline::SlotOfThisTrace()
    {
        const ::Desert::Engine::FrameManager& frames = ::Desert::Engine::FrameManager::GetInstance();
        const uint32_t              slots  = std::max( frames.GetMaxFramesInFlight(), 1u );
        // The ring is sized by the frame count the swapchain fixed; a change re-keys every slot, so the old
        // buffers go through the frame-delayed deletion and the new slots start empty.
        if ( m_Slots.size() != slots )
        {
            for ( auto& frame : m_Slots )
                for ( TableSlot& slot : frame )
                {
                    Release( slot.Sbt );
                    Release( slot.Rows );
                }
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        if ( const uint64_t frame = frames.GetAbsoluteFrameCount(); frame != m_SlotsFrame )
        {
            m_SlotsFrame    = frame;
            m_TracesInFrame = 0;
        }
        std::vector<TableSlot>& frame = m_Slots[frames.GetCurrentFrameIndex() % slots];
        const uint32_t          trace = m_TracesInFrame++;
        if ( frame.size() <= trace )
            frame.resize( trace + 1u );
        return frame[trace];
    }

    Common::BoolResultStr VulkanRayTracingPipeline::Reserve( TableSlot& slot, const VkDeviceSize sbtBytes,
                                                             const VkDeviceSize rowBytes )
    {
        const std::string_view name = m_Specification.DebugName;
        if ( slot.Sbt.Buffer == VK_NULL_HANDLE || slot.SbtCapacity < sbtBytes )
        {
            const VkDeviceSize capacity = std::max( sbtBytes, slot.SbtCapacity * 2u );
            Release( slot.Sbt );
            slot.Written.clear();
            slot.SbtCapacity = 0;
            // + the base alignment: the table starts on it, the allocation's own alignment may be smaller.
            auto sbt = CreateHostBuffer( "RayTracing.SBT", capacity + m_BaseAlignment,
                                         VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR );
            if ( !sbt )
                return Common::MakeFormattedError( "RayTracingPipeline '{}': {}", name, sbt.GetError() );
            slot.Sbt         = sbt.GetValue();
            slot.SbtCapacity = capacity;
            slot.Base        = AlignUp( slot.Sbt.Address, m_BaseAlignment ) - slot.Sbt.Address;
        }
        if ( slot.Rows.Buffer == VK_NULL_HANDLE || slot.RowCapacity < rowBytes )
        {
            const VkDeviceSize capacity = std::max( rowBytes, slot.RowCapacity * 2u );
            Release( slot.Rows );
            slot.RowsWritten.clear();
            slot.RowCapacity = 0;
            // The rows' address moves: every hit record relocated against the old one is stale.
            slot.Written.clear();
            auto rows =
                 CreateHostBuffer( "RayTracing.HitMaterialRows", capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT );
            if ( !rows )
                return Common::MakeFormattedError( "RayTracingPipeline '{}': {}", name, rows.GetError() );
            slot.Rows        = rows.GetValue();
            slot.RowCapacity = capacity;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRayTracingPipeline::RecordTraceRays(
         VkCommandBuffer cmd, const RayTracing::RayTracingHitGroupTable& table, uint32_t width, uint32_t height )
    {
        const std::string_view name = m_Specification.DebugName;
        if ( m_Pipeline == VK_NULL_HANDLE || m_CmdTraceRays == nullptr )
            return Common::MakeFormattedError( "RayTracingPipeline '{}' is not built", name );
        for ( const auto& record : table.Records )
            if ( record.HitGroup >= m_Specification.HitGroups.size() )
                return Common::MakeFormattedError(
                     "RayTracingPipeline '{}': a hit record names hit group {}, the pipeline has {}", name,
                     record.HitGroup, m_Specification.HitGroups.size() );

        // The regions (Vulkan spec, vkCmdTraceRaysKHR): each starts on shaderGroupBaseAlignment; a record is a
        // handle (+ the hit record's data) padded to shaderGroupHandleAlignment; the ray generation region's
        // stride equals its size.
        const VkDeviceSize raygenStride = AlignUp( m_HandleSize, m_BaseAlignment );
        const VkDeviceSize missStride   = AlignUp( m_HandleSize, m_HandleAlignment );
        const VkDeviceSize hitStride =
             AlignUp( m_HandleSize + sizeof( RayTracing::RayTracingHitRecordData ), m_HandleAlignment );
        if ( hitStride > m_MaxStride )
            return Common::MakeFormattedError( "RayTracingPipeline '{}': a hit record is {} bytes, the device's "
                                               "maxShaderGroupStride is {}",
                                               name, hitStride, m_MaxStride );
        const VkDeviceSize missOffset = raygenStride;
        const VkDeviceSize hitOffset  = AlignUp( missOffset + missStride, m_BaseAlignment );
        const std::size_t  hitCount   = std::max<std::size_t>( table.Records.size(), 1u );
        const VkDeviceSize sbtBytes   = hitOffset + hitStride * hitCount;
        const VkDeviceSize rowBytes =
             std::max<VkDeviceSize>( table.Rows.size() * sizeof( glm::vec4 ), sizeof( glm::vec4 ) );

        const std::lock_guard lock( m_SlotsMutex );
        TableSlot&            slot = SlotOfThisTrace();
        if ( const Common::BoolResultStr reserved = Reserve( slot, sbtBytes, rowBytes ); !reserved )
            return reserved;

        // This trace's table, MaterialRow relocated to the slot's row buffer.
        m_Table.assign( static_cast<std::size_t>( sbtBytes ), 0 );
        const auto handle = [&]( std::size_t group ) { return m_Handles.data() + group * m_HandleSize; };
        std::memcpy( m_Table.data(), handle( 0 ), m_HandleSize );
        std::memcpy( m_Table.data() + missOffset, handle( 1 ), m_HandleSize );
        for ( std::size_t i = 0; i < table.Records.size(); ++i )
        {
            const RayTracing::RayTracingHitRecord& record = table.Records[i];
            uint8_t* const                         at     = m_Table.data() + hitOffset + hitStride * i;
            std::memcpy( at, handle( 2u + record.HitGroup ), m_HandleSize );
            RayTracing::RayTracingHitRecordData data = record.Data;
            data.MaterialRow                         = slot.Rows.Address + record.Data.MaterialRow;
            std::memcpy( at + m_HandleSize, &data, sizeof( data ) );
        }
        if ( table.Records.empty() )
            std::memcpy( m_Table.data() + hitOffset, handle( 2u ), m_HandleSize );

        // THE DIRTY RECORDS. Record r covers [RecordBegin(r), RecordBegin(r + 1)) of the table: the ray generation
        // region, the miss region up to the hit region's start, then one hit stride each - every byte of the
        // table belongs to one record. A record is dirty when its bytes differ from what the slot holds or the
        // slot never held it; a run of dirty records is one write.
        const std::size_t recordCount = 2u + hitCount;
        const auto        recordBegin = [&]( std::size_t r ) -> std::size_t
        {
            if ( r == 0 )
                return 0;
            if ( r == 1 )
                return static_cast<std::size_t>( missOffset );
            return static_cast<std::size_t>( hitOffset + hitStride * ( r - 2u ) );
        };
        const auto recordDirty = [&]( std::size_t r )
        {
            const std::size_t from = recordBegin( r );
            const std::size_t to   = recordBegin( r + 1u );
            return slot.Written.size() < to ||
                   std::memcmp( slot.Written.data() + from, m_Table.data() + from, to - from ) != 0;
        };
        RayTracing::RayTracingSbtWriteCounts written;
        written.RecordsTotal = static_cast<uint32_t>( recordCount );
        // ONE FLUSH FOR THE PASS: the batch holds the table's and the rows' mappings, every run below is copied
        // without its own flush, and the batch's Flush() makes all of them visible in one vmaFlushAllocations
        // (no call at all on coherent memory) before the mappings are released.
        MappedWriteBatch batch;
        {
            std::optional<std::size_t> mapped;
            for ( std::size_t r = 0; r < recordCount; )
            {
                if ( !recordDirty( r ) )
                {
                    ++r;
                    continue;
                }
                std::size_t end = r + 1u;
                while ( end < recordCount && recordDirty( end ) )
                    ++end;
                if ( !mapped )
                {
                    MappedMemory mapping = Allocator().MapMemory( slot.Sbt.Allocation );
                    if ( !mapping )
                    {
                        slot.Written.clear(); // what the slot holds is no longer known
                        return Common::MakeFormattedError( "RayTracingPipeline '{}': the SBT could not be mapped: {}",
                                                           name, mapping.GetRefusal() );
                    }
                    mapped = batch.Hold( std::move( mapping ) );
                }
                const std::size_t from = recordBegin( r );
                const std::size_t to   = recordBegin( end );
                if ( const auto wrote = batch.Write( *mapped, m_Table.data() + from, to - from,
                                                     static_cast<std::size_t>( slot.Base ) + from );
                     !wrote )
                {
                    slot.Written.clear();
                    return Common::MakeFormattedError( "RayTracingPipeline '{}': {}", name, wrote.GetError() );
                }
                written.Records += static_cast<uint32_t>( end - r );
                r = end;
            }
        }
        slot.Written.swap( m_Table );

        // THE DIRTY ROWS, by the same rule over the 16-byte rows.
        {
            const std::size_t rowCount = table.Rows.size();
            const auto        rowDirty = [&]( std::size_t r )
            { return slot.RowsWritten.size() <= r || slot.RowsWritten[r] != table.Rows[r]; };
            written.RowsTotal = static_cast<uint32_t>( rowCount );
            std::optional<std::size_t> mapped;
            for ( std::size_t r = 0; r < rowCount; )
            {
                if ( !rowDirty( r ) )
                {
                    ++r;
                    continue;
                }
                std::size_t end = r + 1u;
                while ( end < rowCount && rowDirty( end ) )
                    ++end;
                if ( !mapped )
                {
                    MappedMemory mapping = Allocator().MapMemory( slot.Rows.Allocation );
                    if ( !mapping )
                    {
                        slot.RowsWritten.clear();
                        return Common::MakeFormattedError( "RayTracingPipeline '{}': the material rows could "
                                                           "not be mapped: {}",
                                                           name, mapping.GetRefusal() );
                    }
                    mapped = batch.Hold( std::move( mapping ) );
                }
                if ( const auto wrote = batch.Write( *mapped, table.Rows.data() + r, ( end - r ) * sizeof( glm::vec4 ),
                                                     r * sizeof( glm::vec4 ) );
                     !wrote )
                {
                    slot.RowsWritten.clear();
                    return Common::MakeFormattedError( "RayTracingPipeline '{}': {}", name, wrote.GetError() );
                }
                written.Rows += static_cast<uint32_t>( end - r );
                r = end;
            }
            slot.RowsWritten.assign( table.Rows.begin(), table.Rows.end() );
        }
        if ( const auto flushed = batch.Flush(); !flushed )
        {
            // Written into the mappings but not known to have reached the device: neither copy is trusted.
            slot.Written.clear();
            slot.RowsWritten.clear();
            return Common::MakeFormattedError( "RayTracingPipeline '{}': {}", name, flushed.GetError() );
        }
        if ( const auto finished = RayTracing::RayTracingSbtWrites::Add( written ) )
            LOG_INFO( "[RayTracing] SBT writes, previous frame: {} traces, {}/{} records, {}/{} material rows",
                      finished->Traces, finished->Records, finished->RecordsTotal, finished->Rows,
                      finished->RowsTotal );

        const VkDeviceAddress                 start = slot.Sbt.Address + slot.Base;
        const VkStridedDeviceAddressRegionKHR raygen{ start, raygenStride, raygenStride };
        const VkStridedDeviceAddressRegionKHR miss{ start + missOffset, missStride, missStride };
        const VkStridedDeviceAddressRegionKHR hits{ start + hitOffset, hitStride, hitStride * hitCount };
        const VkStridedDeviceAddressRegionKHR callable{ 0, 0, 0 };
        m_CmdTraceRays( cmd, &raygen, &miss, &hits, &callable, width, height, 1 );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::API::Vulkan
