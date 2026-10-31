#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgPassBindings.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSharedGpuMemory.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <span>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingPipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <format>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/WriteDescriptorSetBuilder.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexLayout.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

namespace Desert::Graphic::API::Vulkan
{
    void VulkanRendererAPI::Init()
    {
#if DESERT_DEV_INSTRUMENTS
        m_GpuProfiler.Init();
#endif
    }
    void VulkanRendererAPI::Shutdown()
    {
#if DESERT_DEV_INSTRUMENTS
        m_GpuProfiler.Shutdown();
#endif
    }

    Common::BoolResultStr VulkanRendererAPI::BeginFrame()
    {
        // THE STRUCTURAL GATE FOR EVERY vkCmd* IN THIS FILE. Nothing below records a command without
        // m_CurrentCommandBuffer, and this is the only function that ever sets it — so leaving it null
        // here turns all forty-odd recording entry points into no-ops without a guard in each of them.
        // Application::Run reads this result and ends the run, which is the intended exit.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            m_CurrentCommandBuffer = nullptr;
            return Common::MakeError( "the device is lost; no frame can be recorded. See the [DeviceLost] "
                                      "line above for the cause." );
        }

        // WAIT FOR GPU: THE FRAME'S CPU WAIT ON THE DEVICE, AND ITS FIRST STATEMENT (GPU-COST-11; UE: the RHI waits
        // the fence of frame N-k before it records frame N, and the wait is a stat row of its own, not part of
        // Present). This slot's fence is that of frame N-k (k = frames in flight): every object of the slot, the
        // geometry pool's released spans (GeometryPool::BeginFrame, below) and every deletion queued at frame
        // N-k or earlier are free once it has signalled, so the wait stands before all of them. Until the first
        // frame builds the frame loop (BeginRdgFrame) no frame was submitted and there is nothing to wait.
        // BeginSlot waits the same fence again further down; it is signalled by then and the call returns at once.
        if ( m_FrameLoop )
        {
            DESERT_PROFILE_SCOPE( "Wait for GPU" );
            const uint32_t slot = EngineContext::GetInstance().GetCurrentFrameIndex();
            if ( const Common::BoolResultStr waited = m_FrameLoop->WaitSlot( slot ); !waited )
            {
                m_CurrentCommandBuffer = nullptr;
                return Common::MakeFormattedError<bool>( "this frame's slot (frame N-k): {}", waited.GetError() );
            }
        }
        // The per-frame drain, under the fence just waited (VulkanAllocator::IsRetired), and the one-off command
        // buffers that have finished.
        {
            DESERT_PROFILE_SCOPE( "Vulkan: slot retire" );
            SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                 ->GetVulkanAllocator()
                 ->ProcessDeletionQueue();
            (void)CommandBufferAllocator::GetInstance().CollectFinished();
        }

        // ROLLED HERE AND NOT IN EndFrame, and the difference is observable. A lost device returns above
        // without recording anything, so rolling in EndFrame would either be skipped — leaving the last
        // GOOD frame's count standing as if it were the failed one's — or would zero the very number that
        // explains the failure. Rolling at the top means the readable count always belongs to the last
        // frame that actually recorded.
        DrawCounter::Roll();
        // The geometry pool's bind counters roll with the draw counter, and its released spans older than the
        // frames in flight return to the free lists before this frame uploads anything (GeometryPool).
        GeometryPool::Get().BeginFrame();
        // The virtual-geometry cluster tables follow the pool's frame rule: released rows retire, rows registered
        // since the last frame (StaticMesh::Invalidate) go up before any pass reads them.
        if ( const Common::BoolResultStr uploaded = VirtualGeometry::VirtualMeshTable::Get().RT_Upload();
             !uploaded )
            LOG_ERROR( "{}; virtual-geometry meshes registered since the last frame are not on the device",
                       uploaded.GetError() );

        // Before any view begins: a vendor runtime that refused a method since the catalog offered it withdraws
        // it here, and Resolve lowers it (VulkanPhysicalDevice::RefreshRuntimeOffers) - the frame's views read a
        // resolution that no longer names a method that cannot run.
        SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
             ->GetPhysicalDevice()
             ->RefreshRuntimeOffers();

        // ONE PATH, WINDOW OR NOT. The slot's fence was waited at the top ("Wait for GPU"); BeginRdgFrame began
        // its objects; the frame records into a graphics command buffer of the slot, already begun. A window is
        // only an output of PresentFinalImage's submission (Desert/Tests/Engine/EngineHost runs this same frame
        // with none). vkBeginCommandBuffer cannot report VK_ERROR_DEVICE_LOST (only out-of-memory), so its
        // failure is the refusal below and nothing more.
        m_FrameSubmissions.clear();
        if ( const Common::BoolResultStr rdg = BeginRdgFrame(); !rdg )
        {
            m_CurrentCommandBuffer = nullptr;
            return Common::MakeFormattedError<bool>( "render graph frame: {}", rdg.GetError() );
        }
        const Common::ResultStr<VkCommandBuffer> frame =
             m_FrameLoop->GetQueueObjects().BeginCommandBuffer( RDG::Pipe::Graphics );
        if ( !frame )
        {
            m_CurrentCommandBuffer = nullptr;
            return Common::MakeFormattedError<bool>( "the frame's command buffer: {}", frame.GetError() );
        }
        m_CurrentCommandBuffer = frame.GetValue();
        m_BoundGeometry.reset(); // a freshly begun buffer holds no geometry binding

        // Resolve the previous results and reset this frame's queries. Must be here: vkCmdResetQueryPool
        // is illegal inside a render pass, and this is the one point in the frame where the command buffer
        // is recording and no pass is open.
#if DESERT_DEV_INSTRUMENTS
        m_GpuProfiler.BeginFrame();
#endif

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::EndFrame()
    {
        // A command buffer that was never begun cannot be ended, and on the device-lost path BeginFrame
        // deliberately left it null — so this is already a no-op there. The gate is here for the other
        // order: a loss discovered mid-frame, between BeginFrame and here.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            m_CurrentCommandBuffer = nullptr;
            return Common::MakeError( "the device is lost; the frame is abandoned rather than closed." );
        }

        if ( IsRecording() )
        {
#if DESERT_DEV_INSTRUMENTS
            m_GpuProfiler.EndFrame();
#endif

            VK_CHECK_RESULT_BOOL( vkEndCommandBuffer( m_CurrentCommandBuffer ) );
            // The frame's graphics tail: the last submission, which signals RenderComplete and the fence.
            m_FrameSubmissions.push_back(
                 { RDG::Pipe::Graphics, m_RdgQueues.GraphicsQueue, m_CurrentCommandBuffer, {}, {}, {} } );
            m_CurrentCommandBuffer = nullptr;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::PresentFinalImage()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; nothing is submitted or presented." );

        // REFUSE TO SUBMIT A BUFFER THAT DID NOT CLOSE. EndFrame is what calls vkEndCommandBuffer, and
        // submitting a still-recording buffer is undefined behaviour that the validation layers report
        // as a driver-side error with no line of ours in it. Returning the message here puts the cause
        // in the frame's own result, which Application::Run now reads.
        {
            DESERT_PROFILE_SCOPE( "Vulkan: EndFrame" );
            const auto ended = EndFrame();
            if ( !ended.IsSuccess() )
                return Common::MakeError( ended.GetError() );
        }

        // THE FRAME'S ONE SUBMISSION. A window is an output of it WHEN THE FRAME HOLDS ITS IMAGE (acquired late,
        // at the back-buffer import): the acquired image is waited by the first graphics entry recorded after
        // the acquire, its render-complete signalled by the last entry, and it presents afterwards. A frame that
        // holds no image (no present graph ran, or the late acquire was out of date) and a frame with no window
        // (EngineHost) are submitted without one.
        const auto             window = m_Window.lock();
        VulkanSwapChainOutput* output =
             window ? SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() )->GetOutput().get() : nullptr;
        const uint32_t                   slot = EngineContext::GetInstance().GetCurrentFrameIndex();
        std::optional<VulkanFrameOutput> frameOutput;
        if ( output != nullptr && output->HasAcquiredImage() )
        {
            frameOutput                         = output->GetFrameOutput();
            frameOutput->FirstEntryAfterAcquire = m_BackBufferFirstEntry;
        }
        {
            DESERT_PROFILE_SCOPE( "Vulkan: frame submit" );
            const Common::BoolResultStr submitted =
                 m_FrameLoop->Submit( slot, m_FrameSubmissions, frameOutput ? &*frameOutput : nullptr );
            m_FrameSubmissions.clear();
            m_BackBufferFirstEntry = 0;
            if ( !submitted )
                return Common::MakeError( submitted.GetError() );
        }

        // With an output, present (a frame holding no image presents nothing: Present returns). Without one the
        // frame is synchronous: THIS slot's fence is waited, so the caller may read back what the frame wrote
        // (EngineHost) -- the slot's fence, not the device.
        if ( output != nullptr )
            output->Present();
        else if ( const Common::BoolResultStr waited = m_FrameLoop->WaitSlot( slot ); !waited )
            return Common::MakeFormattedError<bool>( "the frame's slot: {}", waited.GetError() );

        // The submit and the present are where an asynchronous loss surfaces. Saying so HERE, in this
        // frame's own result, is what carries it up to Application::Run -- and nothing below (a fence wait,
        // the deletion queue) may run on a dead device.
        if ( Graphic::DeviceLost::IsLost() )
            return Common::MakeError( "the device was lost while submitting or presenting this frame." );

        // The frame counter advances; the next slot's fence is NOT waited here (GPU-COST-11). It used to be, right
        // after the submit, so this row carried the whole GPU throttle and the CPU sat idle for everything between
        // here and the next frame's recording. The wait is the first thing the next frame does
        // (VulkanRendererAPI::BeginFrame, row "Wait for GPU"), on the fence of frame N-k, and the deletion drain
        // that needs it follows it there.
        Engine::FrameManager::GetInstance().NextFrame();
        if ( BindlessTextureTable* bindless = TryGetBindlessTextureTable() )
            bindless->EndFrame( Engine::FrameManager::GetInstance().GetAbsoluteFrameCount(),
                                Engine::FrameManager::GetInstance().GetMaxFramesInFlight() );
        return BOOLSUCCESS;
    }

    bool VulkanRendererAPI::IsRecording()
    {
        if ( m_CurrentCommandBuffer != VK_NULL_HANDLE && !Graphic::DeviceLost::AllowWork() )
            m_CurrentCommandBuffer = nullptr;
        return m_CurrentCommandBuffer != nullptr;
    }

    Common::BoolResultStr VulkanRendererAPI::BeginRenderPass( const RenderPass* renderPass, bool clearFrame )
    {
        if ( !IsRecording() )
            return Common::MakeError( "No active command buffer" );

        const auto framebuffer       = renderPass->GetSpecification().TargetFramebuffer;
        const auto vulkanFramebuffer = sp_cast<VulkanFramebuffer>( framebuffer );
        // A framebuffer whose attachment failed to create has no VkFramebuffer. Beginning a pass on it is
        // undefined behaviour (MoltenVK dereferences it), so the pass is refused by name instead.
        if ( vulkanFramebuffer->GetVKFramebuffer() == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>(
                 "render pass '{}' has no VkFramebuffer (its attachments failed to create); "
                 "the pass is not recorded",
                 renderPass->GetSpecification().DebugName );

        VkRenderPassBeginInfo renderPassInfo = {
             .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
             .renderPass  = clearFrame ? vulkanFramebuffer->GetVKRenderPass()
                                       : vulkanFramebuffer->GetVKRenderPassLoad(),
             .framebuffer = vulkanFramebuffer->GetVKFramebuffer(),
             .renderArea  = {
                   .offset = { 0, 0 },
                   .extent = { framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() } } };

        const auto& clearSpec = renderPass->GetSpecification().ClearColor;
        std::vector<VkClearValue> clearValues;
        for ( const auto& attachment : framebuffer->GetSpecification().Attachments.Attachments )
        {
            VkClearValue clearValue{};
            if ( Graphic::Utils::IsDepthFormat( attachment.Format ) )
            {
                clearValue.depthStencil = { clearSpec.DepthStencil.x, static_cast<uint32_t>( clearSpec.DepthStencil.y ) };
            }
            else
            {
                clearValue.color = { { clearSpec.Color.r, clearSpec.Color.g, clearSpec.Color.b, clearSpec.Color.a } };
            }
            clearValues.push_back( clearValue );
        }

        renderPassInfo.clearValueCount = static_cast<uint32_t>( clearValues.size() );
        renderPassInfo.pClearValues    = clearValues.data();

        // Name the region so a RenderDoc/NSight capture is a readable tree ("DeferredGBufferPass",
        // "RSMPass", "SSRTracePass"...) instead of a flat run of draws. The spec already carries the
        // name for logging; this is the same string, handed to the debugger.
        VKUtils::BeginDebugLabel( m_CurrentCommandBuffer,
                                  renderPass->GetSpecification().DebugName.c_str() );

        m_OpenRenderPass =
             CompatibleRenderPassKeyOf( framebuffer->GetSpecification(), framebuffer->GetSpecification().Samples );
        vkCmdBeginRenderPass( m_CurrentCommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE );
        SetViewportAndScissor( framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::EndRenderPass()
    {
        if ( IsRecording() )
        {
            vkCmdEndRenderPass( m_CurrentCommandBuffer );
            m_OpenRenderPass.reset();

            // The implicit final subpass dependency uses dstStageMask=BOTTOM_OF_PIPE and
            // dstAccessMask=0, which makes color writes *available* (flushed from the
            // attachment cache) but NOT *visible* to subsequent fragment-shader texture reads.
            // Without this barrier the shader texture cache (L1) is never invalidated, so the
            // next pass — or the matching pass in the next frame — samples stale data, producing
            // every-other-frame flickering with 2+ frames in flight.
            VkMemoryBarrier memBarrier = {
                .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            };
            vkCmdPipelineBarrier( m_CurrentCommandBuffer,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0,
                                  1, &memBarrier,
                                  0, nullptr,
                                  0, nullptr );

            // Closes the region opened by BeginRenderPass. It opens exactly
            // one, so this stays balanced — an unmatched Begin corrupts the capture's tree.
            VKUtils::EndDebugLabel( m_CurrentCommandBuffer );
        }
        return BOOLSUCCESS;
    }

    void VulkanRendererAPI::BeginDebugLabel( const char* name )
    {
        if ( IsRecording() )
            VKUtils::BeginDebugLabel( m_CurrentCommandBuffer, name );
    }

    void VulkanRendererAPI::EndDebugLabel()
    {
        if ( IsRecording() )
            VKUtils::EndDebugLabel( m_CurrentCommandBuffer );
    }

    bool VulkanRendererAPI::BindGraphicsPipeline( const GraphicsPipeline* pipeline )
    {
        if ( !pipeline )
            return false;

        // The pipeline is resolved against the render pass this draw is recorded in: the graph's, when an exec
        // lambda records inside a graph-opened pass, else the one this API opened. Not const: a pass at another
        // sample count than the pipeline was built at creates (once) the variant for it.
        const RdgRenderPassKey* openPass = nullptr;
        if ( m_RdgBackend )
        {
            if ( const std::optional<RdgRenderPassKey>& graphPass = m_RdgBackend->GetOpenRenderPass();
                 graphPass.has_value() )
                openPass = &*graphPass;
        }
        if ( openPass == nullptr && m_OpenRenderPass.has_value() )
            openPass = &*m_OpenRenderPass;
        if ( openPass == nullptr )
        {
            // No pass, so nothing to resolve the pipeline against. Latched by name like an unbuilt pipeline.
            if ( m_WarnedUnbuiltPipelines
                      .insert( std::format( "{}#outside-pass", pipeline->GetSpecification().DebugName ) )
                      .second )
                LOG_ERROR( "[Renderer] pipeline '{}' bound outside a render pass; not drawn",
                           pipeline->GetSpecification().DebugName );
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-static-cast-downcast):
        // only the Vulkan API makes pipelines; binding builds the per-pass variant into the pipeline's own cache
        auto* vulkanPipeline = const_cast<VulkanPipeline*>( static_cast<const VulkanPipeline*>( pipeline ) );
        if ( VkPipeline bound = vulkanPipeline->GetVkPipelineFor( *openPass ); bound != VK_NULL_HANDLE )
        {
            vkCmdBindPipeline( m_CurrentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bound );
            return true;
        }
        // Still in the driver (PSO1): not an error, and not drawn. An ENGINE pipeline is counted by
        // PipelineBuilds and the content gate waits for it, so no gated frame is shown with this draw missing;
        // a MATERIAL draw never gets here compiling — MeshRenderer draws the default surface instead (AL1-12).
        if ( vulkanPipeline->GetBuildState() == VulkanPipeline::BuildState::Compiling )
            return false;

        // See the declaration: this is where "a shader that will not compile will not draw" stops being
        // a sentence in a log message and starts being what happens. The name is the pipeline's, because
        // that is what a reader can look up; the shader's own error was already written once, at compile.
        const std::string& name = vulkanPipeline->GetSpecification().DebugName;
        if ( m_WarnedUnbuiltPipelines.insert( name ).second )
        {
            LOG_ERROR( "[Renderer] pipeline '{}' was never built (its shader has no compiled stages) — "
                       "everything drawn through it is skipped until the shader compiles.",
                       name.empty() ? "<unnamed>" : name );
        }
        return false;
    }

    const std::shared_ptr<VertexBuffer>& VulkanRendererAPI::DefaultVertexStreams( const uint32_t capacity )
    {
        // White colour and UV1 (0,0) in every vertex — what UE's GNullColorVertexBuffer and a missing TexCoord1
        // give a material. Grown, never shrunk; the replaced buffer is released through the allocator's
        // per-frame deletion queue, so a command buffer still in flight keeps reading valid memory.
        if ( m_DefaultVertexStreams == nullptr || capacity > m_DefaultVertexStreamsCapacity )
        {
            // THE RENDERER'S ROW (the device's own, no instance): this grows lazily inside whichever view's ledger
            // scope drew first, and outlives that view (VIEW-CLOSE-LEAK). A category scope clears that view's instance.
            const ResourceAttributionScope rendererRows( ResourceOwner::Device );
            std::vector<MeshVertexStreams> defaults( capacity );
            // The device's own, shared by every view: grown by whichever view first draws more vertices, so
            // without this scope it was booked to THAT view and outlived its close (EDITOR-LOG-CLEAN).
            const ResourceAttributionScope owned( ResourceOwner::Device );
            m_DefaultVertexStreams = VertexBuffer::Create(
                 defaults.data(), static_cast<uint32_t>( defaults.size() * sizeof( MeshVertexStreams ) ) );
            const auto uploaded = m_DefaultVertexStreams->RT_Invalidate();
            DESERT_VERIFY( uploaded.IsSuccess(), "the default vertex-streams buffer could not be uploaded" );
            m_DefaultVertexStreamsCapacity = capacity;
        }
        return m_DefaultVertexStreams;
    }

    VkBuffer VulkanRendererAPI::VisibleRowIdentity( const uint32_t rows )
    {
        if ( m_VisibleRowIdentity == nullptr || rows > m_VisibleRowIdentityRows )
        {
            // Doubled past the request, so a scene that grows by one draw does not reallocate every frame.
            const uint32_t        capacity = std::max( { rows, m_VisibleRowIdentityRows * 2u, 1024u } );
            std::vector<uint32_t> identity( capacity );
            for ( uint32_t row = 0; row < capacity; ++row )
                identity[row] = row;
            const ResourceAttributionScope owned( ResourceOwner::Device );
            m_VisibleRowIdentity = VertexBuffer::Create(
                 identity.data(), static_cast<uint32_t>( identity.size() * sizeof( uint32_t ) ) );
            const auto uploaded = m_VisibleRowIdentity->RT_Invalidate();
            DESERT_VERIFY( uploaded.IsSuccess(), "the instance id identity buffer could not be uploaded" );
            m_VisibleRowIdentityRows = capacity;
        }
        return sp_cast<API::Vulkan::VulkanVertexBuffer>( m_VisibleRowIdentity )->GetVulkanBuffer();
    }

    void VulkanRendererAPI::SubmitPulled( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                          const MaterialExecutor* materialExecutor )
    {
        if ( !IsRecording() || vertexCount == 0 )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( !BindGraphicsPipeline( pipeline ) )
            return;

        if ( materialExecutor )
        {
            // RENDER-THREADS M3: the material was applied by its prepare; the record only binds it.
            if ( auto recordable = materialExecutor->CheckRecordable(); !recordable )
            {
                LOG_ERROR( "VulkanRendererAPI::SubmitPulled: {}", recordable.GetError() );
                return;
            }
            auto vkBackend = static_cast<VulkanMaterialBackend*>( materialExecutor->GetMaterialBackend().get() );
            if ( !vkBackend->HasDescriptorSets() )
            {
                LOG_WARN( "VulkanRendererAPI::SubmitPulled: MaterialExecutor has no valid descriptor sets!" );
                return;
            }
            uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            if ( !vkBackend->BindDescriptorSets( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                                 VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex ) )
                return;
        }

        // The graphics pipeline enables VK_DYNAMIC_STATE_LINE_WIDTH, so it must be set before drawing.
        // Widths > 1 require the wideLines device feature — MoltenVK lacks it, so force 1.0 there (a
        // wider value is a validation error and gets dropped anyway).
        const float safeWidth = Graphic::RenderConfig::WideLines ? std::clamp( lineWidth, 1.0f, 10.0f ) : 1.0f;
        vkCmdSetLineWidth( m_CurrentCommandBuffer, safeWidth );

        // Vertexless: the DebugLine vertex shader pulls each vertex from the Lines storage buffer by
        // gl_VertexIndex. The pipeline's topology groups them: 2 per segment (Lines), 3 per triangle.
        DrawCounted( vertexCount, 1, 0, 0 );
    }

    namespace
    {
        // A pipeline holds the push range its layout was built with; its shader is re-reflected by every
        // recompile. When the two name different stages the pipeline was built from an earlier compile of that
        // shader and has not been rebuilt since: pushes keep to the layout (that is what the GPU validates
        // against), and this says which pipeline is stale, once per layout, with both stage sets.
        void
        ReportPushStagesApart( const VulkanPipeline& pipeline, std::string_view shaderName,
                               const std::optional<ShaderResources::ShaderLayout::PushConstantRange>& reflected )
        {
            const auto&              range        = pipeline.GetPushConstantRange();
            const VkShaderStageFlags layoutStages = range.has_value() ? range->stageFlags : 0;
            const VkShaderStageFlags shaderStages =
                 reflected.has_value() ? static_cast<VkShaderStageFlags>( reflected->ShaderStage ) : 0;
            if ( layoutStages == shaderStages )
                return;
            static std::mutex                           mutex;
            static std::unordered_set<VkPipelineLayout> reported;
            const std::scoped_lock                      lock( mutex );
            if ( !reported.insert( pipeline.GetVkPipelineLayout() ).second )
                return;
            LOG_ERROR( "Pipeline '{}': its layout's push stages 0x{:x} are not shader '{}''s current 0x{:x}; the "
                       "pipeline was built from an earlier compile of the shader and was not rebuilt",
                       pipeline.GetSpecification().DebugName, layoutStages, shaderName, shaderStages );
        }
        const VkDescriptorSetLayoutBinding* FindLayoutBinding( const VulkanDescriptorSetLayout& layout,
                                                               uint32_t                         binding )
        {
            for ( const VkDescriptorSetLayoutBinding& entry : layout.Bindings() )
            {
                if ( entry.binding == binding )
                    return &entry;
            }
            return nullptr;
        }

        // One descriptor set per set of @p layouts, allocated for THIS exec from the graph's per-frame pools,
        // with every resolved PassBindings entry written at its slot. The caller fills the other route's slots
        // into the returned sets before they are bound.
        Common::ResultStr<std::vector<VkDescriptorSet>>
        WriteRdgPassSets( VulkanRdgPassDescriptors&                  descriptors,
                          const std::vector<DescriptorSetLayoutRef>& layouts,
                          std::span<const RdgResolvedEntry> entries, std::string_view pass )
        {
            using Sets = std::vector<VkDescriptorSet>;
            Sets sets;
            sets.reserve( layouts.size() );
            // The bindless table's set is not allocated per pass: the one live table set (update-after-bind) is
            // bound as it is, and slots written into it later reach commands already recorded. A layout is the
            // table's only when the shader reflected the table (VulkanShader::CreateDescriptorsLayout), so the
            // table exists whenever this matches.
            const VulkanBindlessTextureTable* bindless   = BindlessTableIn( layouts );
            const auto                        isBindless = [bindless]( size_t set )
            { return bindless != nullptr && set == kBindlessSetIndex; };
            for ( size_t index = 0; index < layouts.size(); ++index )
            {
                if ( !layouts[index] )
                    return Common::MakeFormattedError<Sets>( "{}: the pipeline has no layout for set {}", pass,
                                                             index );
                if ( isBindless( index ) )
                {
                    sets.push_back( bindless->GetDescriptorSet() );
                    continue;
                }
                const Common::ResultStr<VkDescriptorSet> set = descriptors.Allocate( layouts[index]->Handle() );
                if ( !set )
                    return Common::MakeFormattedError<Sets>( "{}: set {}: {}", pass, index, set.GetError() );
                sets.push_back( set.GetValue() );
            }
            for ( const RdgResolvedEntry& entry : entries )
            {
                const std::string_view name = entry.Texture != nullptr
                                                   ? std::string_view( entry.Texture->ShaderName )
                                                   : std::string_view( entry.Buffer->ShaderName );
                if ( entry.Slot.Set >= sets.size() )
                    return Common::MakeFormattedError<Sets>( "{}: '{}' is in set {}, the pipeline has {} sets",
                                                             pass, name, entry.Slot.Set, sets.size() );
                if ( isBindless( entry.Slot.Set ) )
                    return Common::MakeFormattedError<Sets>(
                         "{}: '{}' is bound into set {}, the bindless texture table's; the table is written only "
                         "through BindlessTextureTable::Register",
                         pass, name, entry.Slot.Set );
                const VkDescriptorSetLayoutBinding* binding =
                     FindLayoutBinding( *layouts[entry.Slot.Set], entry.Slot.Binding );
                if ( binding == nullptr )
                    return Common::MakeFormattedError<Sets>(
                         "{}: '{}' (set {}, binding {}) is not in the pipeline's layout", pass, name,
                         entry.Slot.Set, entry.Slot.Binding );
                VkDescriptorSet       set     = sets[entry.Slot.Set];
                Common::BoolResultStr written = Common::MakeSuccess( true );
                if ( entry.Kind == RDG::ShaderResourceKind::Sampler )
                {
                    // A shared sampler state: no image, written as VK_DESCRIPTOR_TYPE_SAMPLER.
                    if ( binding->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER || !entry.Texture->Sampler )
                        return Common::MakeFormattedError<Sets>(
                             "{}: '{}' is bound as a sampler into a slot of descriptor type {}", pass, name,
                             static_cast<int>( binding->descriptorType ) );
                    const Common::ResultStr<VkSampler> made = descriptors.GetSampler( *entry.Texture->Sampler );
                    if ( !made )
                        return Common::MakeFormattedError<Sets>( "{}: '{}': {}", pass, name, made.GetError() );
                    descriptors.WriteSampler( set, entry.Slot.Binding, made.GetValue() );
                }
                else if ( entry.Texture != nullptr )
                {
                    const RDG::BoundTexture& texture = *entry.Texture;
                    const bool               storage = entry.Kind == RDG::ShaderResourceKind::StorageTexture;
                    const VkImageLayout      layout =
                         storage ? VK_IMAGE_LAYOUT_GENERAL
                                      : RdgVulkanLayout( RDG::GetAccessState( texture.Declared ).Layout );
                    if ( texture.Sampler )
                        return Common::MakeFormattedError<Sets>(
                             "{}: '{}' carries a sampler but its slot has none (descriptor type {}); a separate "
                             "texture is read through a Sampler slot of the pass",
                             pass, name, static_cast<int>( binding->descriptorType ) );
                    written =
                         descriptors.WriteTexture( set, entry.Slot.Binding, binding->descriptorType,
                                                   texture.Texture, texture.Range, layout, entry.Slot.Element );
                }
                else if ( entry.Kind == RDG::ShaderResourceKind::AccelerationStructure )
                {
                    written = descriptors.WriteAccelerationStructure(
                         set, entry.Slot.Binding, binding->descriptorType, entry.Buffer->Buffer );
                }
                else
                {
                    written = descriptors.WriteBuffer( set, entry.Slot.Binding, binding->descriptorType,
                                                       entry.Buffer->Buffer );
                }
                if ( !written.IsSuccess() )
                    return Common::MakeFormattedError<Sets>( "{}: '{}': {}", pass, name, written.GetError() );
            }
            return Common::MakeSuccess( std::move( sets ) );
        }

        // The push constants of the draw: the block's, or the other route's when the block has none (the
        // resolution already refused both, and neither while the shader declares a range).
        std::span<const std::byte> ChoosePushConstants( const RDG::PassBindings&   bindings,
                                                        std::span<const std::byte> other )
        {
            return bindings.GetPushConstants().empty() ? other : bindings.GetPushConstants();
        }
    } // namespace

    RDG::ShaderBindingLayout VulkanRendererAPI::GetBindingLayout( const Shader& shader ) const
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        const auto& vulkan = static_cast<const VulkanShader&>( shader );
        return MakeShaderBindingLayout( vulkan.GetReflectionData(), vulkan.GetName() );
    }

    RDG::OtherRouteFill VulkanRendererAPI::GetPipelineRouteFill( const ComputePipeline& pipeline ) const
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        const auto& compute = static_cast<const VulkanPipelineCompute&>( pipeline );
        const auto* shader  = dynamic_cast<const VulkanShader*>( compute.GetShader().get() );
        if ( shader == nullptr )
            return {};
        // The same set-0 keys DispatchCompute hands ResolveRdgPassBindings as the other route.
        RdgOtherRoute other;
        for ( const uint32_t binding : compute.GetBoundBindings() )
            other.Filled.push_back( RdgSlotKey{ .Set = 0, .Binding = binding } );
        other.PushConstants = !compute.GetBoundPushConstants().empty();
        return MakeOtherRouteFill( shader->GetReflectionData(), other );
    }

    Common::BoolResultStr VulkanRendererAPI::DispatchCompute( const RDG::PassBindings& bindings,
                                                              const ComputePipeline&   pipeline,
                                                              uint32_t groupCountX, uint32_t groupCountY,
                                                              uint32_t groupCountZ )
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if ( const Common::BoolResultStr bound = BindComputePassState( bindings, pipeline, cmd ); !bound )
            return bound;
        vkCmdDispatch( cmd, groupCountX, groupCountY, groupCountZ );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::DispatchComputeIndirect( const RDG::PassBindings& bindings,
                                                                      const ComputePipeline&   pipeline,
                                                                      const RDG::BufferRef     args,
                                                                      const uint64_t           offset )
    {
        const RDG::PassContext& context = bindings.GetContext();
        if ( offset % 4u != 0u )
            return Common::MakeFormattedError( "{}: dispatch arguments at byte {}, not a multiple of four",
                                               context.GetPassName(), offset );
        const auto declared = context.GetBuffer( args, RDG::Access::IndirectArgs );
        if ( !declared )
            return Common::MakeFormattedError( "{}: dispatch arguments: {}", context.GetPassName(),
                                               declared.GetError() );
        if ( offset + sizeof( VkDispatchIndirectCommand ) > declared.GetValue().Desc->Bytes )
            return Common::MakeFormattedError( "{}: dispatch arguments at byte {} past the end of '{}' ({} bytes)",
                                               context.GetPassName(), offset, declared.GetValue().Name,
                                               declared.GetValue().Desc->Bytes );
        const auto buffer = VulkanRdgBackend::BufferOf( declared.GetValue() );
        if ( !buffer )
            return Common::MakeFormattedError( "{}: dispatch arguments: {}", context.GetPassName(),
                                               buffer.GetError() );
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if ( const Common::BoolResultStr bound = BindComputePassState( bindings, pipeline, cmd ); !bound )
            return bound;
        vkCmdDispatchIndirect( cmd, buffer.GetValue()->GetBuffer(), buffer.GetValue()->GetOffset() + offset );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::BindComputePassState( const RDG::PassBindings& bindings,
                                                                   const ComputePipeline&   pipeline,
                                                                   VkCommandBuffer&         out )
    {
        const RDG::PassContext&                  context = bindings.GetContext();
        const std::string_view                   pass    = context.GetPassName();
        const Common::ResultStr<VkCommandBuffer> cmd     = VulkanRdgBackend::CommandBufferOf( context );
        if ( !cmd )
            return Common::MakeError( cmd.GetError() );
        const Common::ResultStr<VulkanRdgPassDescriptors*> descriptors =
             VulkanRdgBackend::DescriptorsOf( context );
        if ( !descriptors )
            return Common::MakeError( descriptors.GetError() );

        auto& compute =
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-static-cast-downcast):
             // only the Vulkan API makes pipelines; dispatch writes the pipeline's own descriptor ring
             const_cast<VulkanPipelineCompute&>( static_cast<const VulkanPipelineCompute&>( pipeline ) );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        auto* shader = static_cast<VulkanShader*>( compute.GetShader().get() );
        if ( shader == nullptr || compute.GetVkPipeline() == VK_NULL_HANDLE )
            return Common::MakeFormattedError( "{}: compute pipeline '{}' is not built", pass,
                                               compute.GetSpecification().DebugName );

        // The other route: the pipeline's own setters (asset textures, out-of-graph buffers), set 0.
        RdgOtherRoute other;
        for ( const uint32_t binding : compute.GetBoundBindings() )
            other.Filled.push_back( RdgSlotKey{ .Set = 0, .Binding = binding } );
        other.PushConstants = !compute.GetBoundPushConstants().empty();

        const auto resolved =
             ResolveRdgPassBindings( shader->GetReflectionData(), shader->GetName(), bindings, other );
        if ( !resolved )
            return Common::MakeError( resolved.GetError() );
        const auto sets =
             WriteRdgPassSets( *descriptors.GetValue(), compute.GetLayouts(), resolved.GetValue(), pass );
        if ( !sets )
            return Common::MakeError( sets.GetError() );
        if ( !other.Filled.empty() )
        {
            if ( sets.GetValue().empty() )
                return Common::MakeFormattedError( "{}: the pipeline's setters fill set 0, which it has not",
                                                   pass );
            if ( const Common::BoolResultStr written = compute.WriteBoundResources( sets.GetValue()[0] );
                 !written.IsSuccess() )
                return Common::MakeFormattedError( "{}: {}", pass, written.GetError() );
        }

        vkCmdBindPipeline( cmd.GetValue(), VK_PIPELINE_BIND_POINT_COMPUTE, compute.GetVkPipeline() );
        if ( !sets.GetValue().empty() )
            vkCmdBindDescriptorSets( cmd.GetValue(), VK_PIPELINE_BIND_POINT_COMPUTE, compute.GetVkPipelineLayout(),
                                     0, static_cast<uint32_t>( sets.GetValue().size() ), sets.GetValue().data(), 0,
                                     nullptr );
        const std::span<const std::byte> push = ChoosePushConstants( bindings, compute.GetBoundPushConstants() );
        if ( !push.empty() )
            vkCmdPushConstants( cmd.GetValue(), compute.GetVkPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                static_cast<uint32_t>( push.size() ), push.data() );
        out = cmd.GetValue();
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::TraceRays( const RDG::PassBindings&                   bindings,
                                                        const RayTracingPipeline&                  pipeline,
                                                        const RayTracing::RayTracingHitGroupTable& hits,
                                                        const uint32_t width, const uint32_t height )
    {
        const RDG::PassContext&                  context = bindings.GetContext();
        const std::string_view                   pass    = context.GetPassName();
        const Common::ResultStr<VkCommandBuffer> cmd     = VulkanRdgBackend::CommandBufferOf( context );
        if ( !cmd )
            return Common::MakeError( cmd.GetError() );
        const Common::ResultStr<VulkanRdgPassDescriptors*> descriptors =
             VulkanRdgBackend::DescriptorsOf( context );
        if ( !descriptors )
            return Common::MakeError( descriptors.GetError() );

        auto& traced =
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-static-cast-downcast):
             // only the Vulkan API makes pipelines; a launch writes the pipeline's own shader binding table
             const_cast<VulkanRayTracingPipeline&>( static_cast<const VulkanRayTracingPipeline&>( pipeline ) );
        const auto* program = dynamic_cast<const VulkanShader*>( traced.GetSpecification().Program.get() );
        if ( program == nullptr || traced.GetVkPipeline() == VK_NULL_HANDLE )
            return Common::MakeFormattedError( "{}: ray tracing pipeline '{}' is not built", pass,
                                               traced.GetSpecification().DebugName );

        // The program's reflection is the pipeline's layout (VulkanRayTracingPipeline::Build): the pass's
        // bindings resolve against it alone, as a compute dispatch's against its program; no setter route.
        const auto resolved =
             ResolveRdgPassBindings( program->GetReflectionData(), program->GetName(), bindings, RdgOtherRoute{} );
        if ( !resolved )
            return Common::MakeError( resolved.GetError() );
        const auto sets =
             WriteRdgPassSets( *descriptors.GetValue(), traced.GetLayouts(), resolved.GetValue(), pass );
        if ( !sets )
            return Common::MakeError( sets.GetError() );

        vkCmdBindPipeline( cmd.GetValue(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, traced.GetVkPipeline() );
        if ( !sets.GetValue().empty() )
            vkCmdBindDescriptorSets(
                 cmd.GetValue(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, traced.GetVkPipelineLayout(), 0,
                 static_cast<uint32_t>( sets.GetValue().size() ), sets.GetValue().data(), 0, nullptr );
        const std::span<const std::byte> push = ChoosePushConstants( bindings, {} );
        if ( !push.empty() )
            vkCmdPushConstants( cmd.GetValue(), traced.GetVkPipelineLayout(), traced.GetPushConstantStages(), 0,
                                static_cast<uint32_t>( push.size() ), push.data() );
        if ( const Common::BoolResultStr launched = traced.RecordTraceRays( cmd.GetValue(), hits, width, height );
             !launched )
            return Common::MakeFormattedError( "{}: {}", pass, launched.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::DrawProcedural( const RDG::PassBindings&         bindings,
                                                             const GraphicsPipeline&          pipeline,
                                                             const MaterialExecutor*          material,
                                                             const std::span<const std::byte> push,
                                                             uint32_t vertexCount, uint32_t instanceCount )
    {
        if ( vertexCount == 0u || instanceCount == 0u )
            return Common::MakeFormattedError( "{}: a procedural draw of {} vertices x {} instances",
                                               bindings.GetContext().GetPassName(), vertexCount, instanceCount );
        if ( const Common::BoolResultStr bound = BindGraphicsPassState( bindings, pipeline, material, push );
             !bound || !bound.GetValue() )
            return bound;
        // The vertex stage builds its geometry from gl_VertexIndex / gl_InstanceIndex.
        DrawCounted( vertexCount, instanceCount, 0, 0 );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::DrawProceduralIndirect(
         const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const MaterialExecutor* material,
         const std::span<const std::byte> push, const RDG::BufferRef args, const uint64_t offset )
    {
        const RDG::PassContext& context = bindings.GetContext();
        if ( offset % 4u != 0u )
            return Common::MakeFormattedError( "{}: indirect arguments at byte {}, not a multiple of four",
                                               context.GetPassName(), offset );
        const auto declared = context.GetBuffer( args, RDG::Access::IndirectArgs );
        if ( !declared )
            return Common::MakeFormattedError( "{}: indirect arguments: {}", context.GetPassName(),
                                               declared.GetError() );
        if ( offset + sizeof( VkDrawIndirectCommand ) > declared.GetValue().Desc->Bytes )
            return Common::MakeFormattedError( "{}: indirect arguments at byte {} past the end of '{}' ({} bytes)",
                                               context.GetPassName(), offset, declared.GetValue().Name,
                                               declared.GetValue().Desc->Bytes );
        const auto buffer = VulkanRdgBackend::BufferOf( declared.GetValue() );
        if ( !buffer )
            return Common::MakeFormattedError( "{}: indirect arguments: {}", context.GetPassName(),
                                               buffer.GetError() );
        if ( const Common::BoolResultStr bound = BindGraphicsPassState( bindings, pipeline, material, push );
             !bound || !bound.GetValue() )
            return bound;
        vkCmdDrawIndirect( m_CurrentCommandBuffer, buffer.GetValue()->GetBuffer(), buffer.GetValue()->GetOffset() + offset, 1,
                           static_cast<uint32_t>( sizeof( VkDrawIndirectCommand ) ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::DrawIndexed( const RDG::PassBindings& bindings,
                                                          const GraphicsPipeline&  pipeline,
                                                          const MaterialExecutor*  material,
                                                          VertexBuffer& vertexBuffer, IndexBuffer& indexBuffer,
                                                          uint32_t indexCount, uint32_t firstIndex )
    {
        if ( indexCount == 0u )
            return Common::MakeFormattedError( "{}: an indexed draw of zero indices",
                                               bindings.GetContext().GetPassName() );
        if ( const Common::BoolResultStr bound = BindGraphicsPassState( bindings, pipeline, material, {} );
             !bound || !bound.GetValue() )
            return bound;
        const VkDeviceSize offsets[] = { 0 };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        VkBuffer vbuffer = static_cast<API::Vulkan::VulkanVertexBuffer&>( vertexBuffer ).GetVulkanBuffer();
        vkCmdBindVertexBuffers( m_CurrentCommandBuffer, 0, 1, &vbuffer, offsets );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        VkBuffer ibuffer = static_cast<API::Vulkan::VulkanIndexBuffer&>( indexBuffer ).GetVulkanBuffer();
        vkCmdBindIndexBuffer( m_CurrentCommandBuffer, ibuffer, 0, VK_INDEX_TYPE_UINT32 );
        m_BoundGeometry.reset(); // bindings 0 and the index buffer now hold this batch's, not a mesh's
        // Vertices are addressed absolutely, only firstIndex selects the batch's slice.
        DrawIndexedCounted( indexCount, 1, firstIndex, 0, 0 );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::BindMeshBuffers( const std::string_view     pass,
                                                              const GraphicsPipeline&    pipeline,
                                                              const MeshGeometryBinding& geometry,
                                                              const VkBuffer visibleRows, const bool shared )
    {
        if ( geometry.Vertices == nullptr )
            return Common::MakeFormattedError( "{}: mesh has no vertices on the GPU", pass );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        const auto* graphics = static_cast<const VulkanPipeline*>( &pipeline );
        // Binding 2, the instance id stream (GpuScene.glslh a_VisibleRow): exactly when the stage reads it. A
        // stage reading it with no stream given would read whatever binding 2 last held: refused, and so is a
        // stream handed to a stage that does not read it (the caller drew the wrong cell).
        if ( graphics->ReadsVisibleRow() != ( visibleRows != VK_NULL_HANDLE ) )
            return Common::MakeFormattedError(
                 "{}: pipeline '{}' {} the instance id stream but the draw {}", pass,
                 pipeline.GetSpecification().DebugName, graphics->ReadsVisibleRow() ? "reads" : "does not read",
                 visibleRows != VK_NULL_HANDLE ? "binds one" : "binds none (no binned rows for it)" );

        // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        const VkBuffer vbuffer = static_cast<const VulkanVertexBuffer*>( geometry.Vertices )->GetVulkanBuffer();
        // Binding 1, the optional streams (MeshVertexLayout): the geometry's own, or the shared default holding
        // at least as many vertices as this mesh - one pipeline, one stride either way. HasVertexStreams is read
        // off the pipeline's BUILT vertex input (layout and vertex-stage inputs,
        // VulkanPipeline::CreateVertexInputState): a pipeline whose shader reads no stream has no binding 1 and
        // gets no buffer there. It implies a layout.
        VkBuffer     sbuffer = VK_NULL_HANDLE;
        VkDeviceSize soffset = 0;
        if ( const auto& layout = pipeline.GetSpecification().Layout;
             graphics->HasVertexStreams() && layout.has_value() )
        {
            if ( geometry.Streams != nullptr )
            {
                sbuffer = static_cast<const VulkanVertexBuffer*>( geometry.Streams )->GetVulkanBuffer();
                soffset = geometry.StreamOffset;
            }
            else
            {
                const uint64_t size  = geometry.Vertices->GetSize();
                const uint64_t bytes = size - std::min( size, geometry.VertexOffset );
                sbuffer = sp_cast<VulkanVertexBuffer>(
                               DefaultVertexStreams(
                                    DefaultVertexStreamsFor( *layout, bytes, m_DefaultVertexStreamsCapacity ).Capacity ) )
                               ->GetVulkanBuffer();
            }
        }
        const VkBuffer ibuffer = geometry.Indices != nullptr
                                      ? static_cast<const VulkanIndexBuffer*>( geometry.Indices )->GetVulkanBuffer()
                                      : VK_NULL_HANDLE;
        // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)

        // THE BIND-SKIP (GeometryPool::RecordBind): the same buffers at the same offsets for the same pipeline on
        // the same command buffer are already bound, so nothing is recorded. Keyed by the VkBuffer handles, not the
        // buffer objects: a pool arena that grew keeps its object and changes its handle, and a draw after the
        // growth must bind the new one. Every pooled mesh's shared binding is its arena at offset 0, so the binned
        // passes bind once per pipeline switch.
        const BoundGeometry key{ m_CurrentCommandBuffer, &pipeline, vbuffer, geometry.VertexOffset, sbuffer,
                                 soffset,                ibuffer,   geometry.IndexOffset };
        if ( !m_BoundGeometry.has_value() || *m_BoundGeometry != key )
        {
            const VkDeviceSize voffset = geometry.VertexOffset;
            vkCmdBindVertexBuffers( m_CurrentCommandBuffer, 0, 1, &vbuffer, &voffset );
            if ( sbuffer != VK_NULL_HANDLE )
                vkCmdBindVertexBuffers( m_CurrentCommandBuffer, 1, 1, &sbuffer, &soffset );
            if ( ibuffer != VK_NULL_HANDLE )
                vkCmdBindIndexBuffer( m_CurrentCommandBuffer, ibuffer, geometry.IndexOffset, VK_INDEX_TYPE_UINT32 );
            m_BoundGeometry = key;
            GeometryPool::Get().RecordBind( shared );
        }
        if ( visibleRows != VK_NULL_HANDLE )
        {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers( m_CurrentCommandBuffer, GpuDriven::kVisibleRowVertexBinding, 1, &visibleRows,
                                    &zero );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::WalkMeshSubmeshes(
         const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
         const glm::mat4& transform, const MaterialExecutor& material, const std::span<const std::byte> drawPush,
         const SubmeshMask hiddenSubmeshMask, const VkBuffer visibleRows, const SubmeshDraw& draw )
    {
        const std::string_view pass = bindings.GetContext().GetPassName();
        // The push block is the per-submesh transform the material carries (written below for each submesh).
        if ( !bindings.GetPushConstants().empty() )
            return Common::MakeFormattedError(
                 "{}: a mesh draw's push constants are its per-submesh transform; the bindings carry their own",
                 pass );
        const MeshGeometry& geometry = mesh.GetGeometry();
        if ( !geometry.IsResident() )
            return Common::MakeFormattedError( "{}: mesh has no vertices on the GPU", pass );
        if ( const Common::BoolResultStr bound = BindGraphicsPassState( bindings, pipeline, &material, drawPush );
             !bound || !bound.GetValue() )
            return bound;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        const auto* graphics = static_cast<const VulkanPipeline*>( &pipeline );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        auto* shader = static_cast<VulkanShader*>( pipeline.GetSpecification().Shader.get() );

        // The per-mesh bind: the geometry at this mesh's own offsets, so every submesh range stays mesh-relative.
        if ( const Common::BoolResultStr buffers =
                  BindMeshBuffers( pass, pipeline, geometry.MeshBinding(), visibleRows, false );
             !buffers )
            return buffers;
        const uint32_t indexCount = geometry.Indices != nullptr ? geometry.IndexCount : 0u;

        // The push block of every submesh: the draw's (@p drawPush - the material's block with the draw's own
        // values over it, MeshDrawList::Record; the pipeline layout's full range, zero past what it holds) with
        // the submesh's transform over the first mat4. Stages and size are the LAYOUT's
        // (VulkanPipeline::GetPushConstantRange), never the shader's current reflection, which a recompile can
        // move past the layout this pipeline holds.
        const auto& pushConstant = graphics->GetPushConstantRange();
        ReportPushStagesApart( *graphics, shader->GetName(), shader->GetShaderPushConstant() );
        std::vector<std::byte> push;
        if ( pushConstant.has_value() && pushConstant->size > 0 )
        {
            if ( pushConstant->size < sizeof( glm::mat4 ) )
                return Common::MakeFormattedError(
                     "{}: shader '{}' push block of {} bytes cannot hold the transform", pass, shader->GetName(),
                     pushConstant->size );
            push.assign( pushConstant->size, std::byte{ 0 } );
            if ( !drawPush.empty() )
                std::memcpy( push.data(), drawPush.data(), std::min<size_t>( push.size(), drawPush.size() ) );
        }

        // The hidden-submesh mask and the per-submesh push block; @p draw records the submesh.
        const auto& submeshes = mesh.GetSubmeshes();
        uint32_t    drawn     = 0;
        for ( size_t si = 0; si < submeshes.size(); ++si )
        {
            if ( hiddenSubmeshMask.Hides( si ) )
                continue;
            const auto&     submesh        = submeshes[si];
            const glm::mat4 finalTransform = transform * submesh.Transform;
            if ( pushConstant.has_value() && pushConstant->size > 0 )
            {
                std::memcpy( push.data(), &finalTransform, sizeof( glm::mat4 ) );
                vkCmdPushConstants( m_CurrentCommandBuffer, graphics->GetVkPipelineLayout(),
                                    pushConstant->stageFlags, pushConstant->offset, pushConstant->size,
                                    push.data() );
            }
            if ( auto recorded = draw( drawn++, si, submesh, indexCount ); !recorded )
                return recorded;
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr
    VulkanRendererAPI::RenderMesh( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                   const Mesh& mesh, const glm::mat4& transform, const MaterialExecutor& material,
                                   const std::span<const std::byte> push, uint32_t instanceCount,
                                   uint32_t firstInstance, SubmeshMask hiddenSubmeshMask, uint32_t lodLevel )
    {
        DESERT_PROFILE_FUNC();
        const std::string_view pass = bindings.GetContext().GetPassName();
        if ( instanceCount == 0u )
            return Common::MakeFormattedError( "{}: a mesh draw of zero instances", pass );
        return WalkMeshSubmeshes(
             bindings, pipeline, mesh, transform, material, push, hiddenSubmeshMask, VK_NULL_HANDLE,
             [&]( uint32_t, const size_t si, const Submesh& submesh,
                  const uint32_t indexCount ) -> Common::BoolResultStr
             {
                 if ( indexCount == 0u )
                 {
                     DrawCounted( submesh.VertexCount, instanceCount, submesh.VertexOffset, firstInstance );
                     return Common::MakeSuccess( true );
                 }
                 const SubmeshIndexRange range = SubmeshIndices( submesh, lodLevel );
                 if ( range.First + range.Count > indexCount )
                     return Common::MakeFormattedError(
                          "{}: submesh {} reads indices [{}, {}) of a {}-index mesh", pass, si, range.First,
                          range.First + range.Count, indexCount );
                 DrawIndexedCounted( range.Count, instanceCount, range.First,
                                     static_cast<int32_t>( submesh.VertexOffset ), firstInstance );
                 return Common::MakeSuccess( true );
             } );
    }

    Common::BoolResultStr VulkanRendererAPI::RenderMeshIndirect(
         const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
         const glm::mat4& transform, const MaterialExecutor& material, const std::span<const std::byte> push,
         const RDG::BufferRef args, const uint64_t offset, const SubmeshMask hiddenSubmeshMask,
         const MeshDrawArgumentsOrder order )
    {
        DESERT_PROFILE_FUNC();
        const RDG::PassContext& context = bindings.GetContext();
        if ( offset % 4u != 0u )
            return Common::MakeFormattedError( "{}: indirect arguments at byte {}, not a multiple of four",
                                               context.GetPassName(), offset );
        const auto declared = context.GetBuffer( args, RDG::Access::IndirectArgs );
        if ( !declared )
            return Common::MakeFormattedError( "{}: indirect arguments: {}", context.GetPassName(),
                                               declared.GetError() );
        const auto buffer = VulkanRdgBackend::BufferOf( declared.GetValue() );
        if ( !buffer )
            return Common::MakeFormattedError( "{}: indirect arguments: {}", context.GetPassName(),
                                               buffer.GetError() );
        const uint64_t bytes  = declared.GetValue().Desc->Bytes;
        const VkBuffer vkArgs = buffer.GetValue()->GetBuffer();
        // Not counted by DrawCounter: the instance counts are the GPU's (DrawProceduralIndirect likewise).
        // The VSM caster draw instances through its remap (firstInstance = RemapBase), never a binned id stream.
        return WalkMeshSubmeshes(
             bindings, pipeline, mesh, transform, material, push, hiddenSubmeshMask, VK_NULL_HANDLE,
             [&]( const uint32_t drawn, const size_t si, const Submesh&,
                  const uint32_t indexCount ) -> Common::BoolResultStr
             {
                 const uint64_t record =
                      order == MeshDrawArgumentsOrder::Drawn ? uint64_t{ drawn } : uint64_t{ si };
                 const uint64_t at = offset + ( record * kMeshDrawArgumentsBytes );
                 if ( at + kMeshDrawArgumentsBytes > bytes )
                     return Common::MakeFormattedError(
                          "{}: indirect arguments of drawn submesh {} at byte {} past the end of '{}' ({} bytes)",
                          context.GetPassName(), drawn, at, declared.GetValue().Name, bytes );
                 // Not in the frame's draw count (its instances are the GPU's), but a CPU draw of its mesh pass.
                 DrawCounter::RecordCpuIndirect();
                 if ( indexCount == 0u )
                     vkCmdDrawIndirect( m_CurrentCommandBuffer, vkArgs, at, 1,
                                        static_cast<uint32_t>( sizeof( VkDrawIndirectCommand ) ) );
                 else
                     vkCmdDrawIndexedIndirect( m_CurrentCommandBuffer, vkArgs, at, 1,
                                               static_cast<uint32_t>( sizeof( VkDrawIndexedIndirectCommand ) ) );
                 return Common::MakeSuccess( true );
             } );
    }

    Common::BoolResultStr VulkanRendererAPI::RenderMeshIndirectGroup( const RDG::PassBindings&     bindings,
                                                                      const GraphicsPipeline&      pipeline,
                                                                      const MeshGeometryBinding&   geometry,
                                                                      const MeshIndirectGroupDraw& draw )
    {
        DESERT_PROFILE_FUNC();
        const RDG::PassContext& context = bindings.GetContext();
        const std::string_view  pass    = context.GetPassName();
        if ( draw.MaxDraws == 0u )
            return Common::MakeSuccess( true );
        if ( draw.Stride < sizeof( VkDrawIndexedIndirectCommand ) || draw.Stride % 4u != 0u ||
             draw.ArgsOffset % 4u != 0u )
            return Common::MakeFormattedError(
                 "{}: indirect group of stride {} at byte {}: the stride holds a VkDrawIndexedIndirectCommand "
                 "and both are multiples of four",
                 pass, draw.Stride, draw.ArgsOffset );
        if ( geometry.Indices == nullptr )
            return Common::MakeFormattedError( "{}: an indirect group's geometry has no index buffer", pass );
        const auto declared = context.GetBuffer( draw.Args, RDG::Access::IndirectArgs );
        if ( !declared )
            return Common::MakeFormattedError( "{}: indirect arguments: {}", pass, declared.GetError() );
        const auto args = VulkanRdgBackend::BufferOf( declared.GetValue() );
        if ( !args )
            return Common::MakeFormattedError( "{}: indirect arguments: {}", pass, args.GetError() );
        const uint64_t end = draw.ArgsOffset + ( uint64_t{ draw.MaxDraws - 1u } * draw.Stride ) +
                             sizeof( VkDrawIndexedIndirectCommand );
        if ( end > declared.GetValue().Desc->Bytes )
            return Common::MakeFormattedError(
                 "{}: {} indirect records at byte {} (stride {}) pass the end of '{}' ({} bytes)", pass,
                 draw.MaxDraws, draw.ArgsOffset, draw.Stride, declared.GetValue().Name,
                 declared.GetValue().Desc->Bytes );
        if ( draw.MaxDrawsPerCall == 0u )
            return Common::MakeFormattedError( "{}: an indirect group of zero draws per call", pass );
        if ( !draw.SplitLabels.empty() && draw.SplitLabels.size() != draw.MaxDraws )
            return Common::MakeFormattedError( "{}: {} draw split labels for an indirect group of {} records",
                                               pass, draw.SplitLabels.size(), draw.MaxDraws );

        if ( const Common::BoolResultStr bound = BindGraphicsPassState( bindings, pipeline, draw.Material, {} );
             !bound || !bound.GetValue() )
            return bound;
        // Binding 2: the identity row stream, so a_VisibleRow = gl_InstanceIndex names the row of the cull's
        // VisibleInstances the record's firstInstance points into (BindMeshBuffers refuses a pipeline reading
        // a_VisibleRow without it, and a stream handed to one that does not).
        const VkBuffer visibleRows =
             draw.VisibleRows != 0u ? VisibleRowIdentity( draw.VisibleRows ) : VK_NULL_HANDLE;
        // The group's shared binding (the pool arena at offset 0): bound once per pipeline switch, the records'
        // FirstIndex / VertexOffset are pool-absolute (DrawCommandCache).
        if ( const Common::BoolResultStr buffers = BindMeshBuffers( pass, pipeline, geometry, visibleRows, true );
             !buffers )
            return buffers;

        // Not counted by DrawCounter: the instance counts are the GPU's (RenderMeshIndirect likewise).
        // One fixed table (decision 10-28): every bin's record is drawn, a culled one with InstanceCount 0.
        const VkBuffer vkArgs = args.GetValue()->GetBuffer();
        // The mesh pass's issued bins and calls (DrawCounter::LastFrameMeshPass).
        // MaxDraws > 0 here; maxDrawIndirectCount may be 2^32 - 1, so no rounding-up sum.
        // Under the mesh draw split every record is its own call inside its own timestamp pair (the bin's row);
        // otherwise MaxDrawsPerCall records per call, as without the profiler.
        const uint32_t perCall = draw.SplitLabels.empty() ? draw.MaxDrawsPerCall : 1u;
        DrawCounter::RecordBins( draw.MaxDraws, ( ( draw.MaxDraws - 1u ) / perCall ) + 1u );
        for ( uint32_t first = 0; first < draw.MaxDraws; first += perCall )
        {
            const uint32_t                                      count = std::min( draw.MaxDraws - first, perCall );
            std::optional<Common::Profiling::GpuDrawSplitScope> bin;
            if ( !draw.SplitLabels.empty() )
            {
                const DrawSplitLabel& label = draw.SplitLabels[first];
                bin.emplace( pass, label.Mesh, label.Material );
                bin->Add( 1, label.TrianglesPerInstance, true );
            }
            vkCmdDrawIndexedIndirect( m_CurrentCommandBuffer, vkArgs,
                                      draw.ArgsOffset + ( uint64_t{ first } * draw.Stride ), count, draw.Stride );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::RenderClusterIndirectGroup( const RDG::PassBindings&        bindings,
                                                                         const GraphicsPipeline&         pipeline,
                                                                         const ClusterIndirectGroupDraw& draw )
    {
        DESERT_PROFILE_FUNC();
        const RDG::PassContext& context = bindings.GetContext();
        const std::string_view  pass    = context.GetPassName();
        if ( draw.ArgsOffset % 4u != 0u )
            return Common::MakeFormattedError( "{}: cluster group arguments at byte {}: not a multiple of four",
                                               pass, draw.ArgsOffset );
        const auto declared = context.GetBuffer( draw.Args, RDG::Access::IndirectArgs );
        if ( !declared )
            return Common::MakeFormattedError( "{}: cluster group arguments: {}", pass, declared.GetError() );
        if ( draw.ArgsOffset + sizeof( VkDrawIndirectCommand ) > declared.GetValue().Desc->Bytes )
            return Common::MakeFormattedError(
                 "{}: cluster group arguments at byte {} pass the end of '{}' ({} bytes)", pass, draw.ArgsOffset,
                 declared.GetValue().Name, declared.GetValue().Desc->Bytes );
        const auto args = VulkanRdgBackend::BufferOf( declared.GetValue() );
        if ( !args )
            return Common::MakeFormattedError( "{}: cluster group arguments: {}", pass, args.GetError() );
        const auto rowsDeclared = context.GetBuffer( draw.Rows, RDG::Access::VertexIndex );
        if ( !rowsDeclared )
            return Common::MakeFormattedError( "{}: cluster group rows: {}", pass, rowsDeclared.GetError() );
        const auto rows = VulkanRdgBackend::BufferOf( rowsDeclared.GetValue() );
        if ( !rows )
            return Common::MakeFormattedError( "{}: cluster group rows: {}", pass, rows.GetError() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        if ( !static_cast<const VulkanPipeline*>( &pipeline )->ReadsVisibleRow() )
            return Common::MakeFormattedError( "{}: pipeline '{}' does not read the instance id stream: not a "
                                               "`.VirtualGeometry` cell",
                                               pass, pipeline.GetSpecification().DebugName );

        if ( const Common::BoolResultStr bound = BindGraphicsPassState( bindings, pipeline, draw.Material, {} );
             !bound || !bound.GetValue() )
            return bound;
        // Binding 2 only: the cell reads its vertices through the cluster (no binding 0 / 1 attribute is consumed,
        // VulkanShaderReflection feeds the consumed locations only), so the bind-skip key of the meshes'
        // buffers (m_BoundGeometry) is untouched; the next mesh draw binds binding 2 itself.
        const VkBuffer     rowBuffer = rows.GetValue()->GetBuffer();
        const VkDeviceSize zero      = 0;
        vkCmdBindVertexBuffers( m_CurrentCommandBuffer, GpuDriven::kVisibleRowVertexBinding, 1, &rowBuffer,
                                &zero );
        DrawCounter::RecordBins( 1u, 1u );
        vkCmdDrawIndirect( m_CurrentCommandBuffer, args.GetValue()->GetBuffer(), draw.ArgsOffset, 1,
                           static_cast<uint32_t>( sizeof( VkDrawIndirectCommand ) ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::BindGraphicsPassState( const RDG::PassBindings&         bindings,
                                                                    const GraphicsPipeline&          pipeline,
                                                                    const MaterialExecutor*          material,
                                                                    const std::span<const std::byte> drawPush )
    {
        const RDG::PassContext&                  context = bindings.GetContext();
        const std::string_view                   pass    = context.GetPassName();
        const Common::ResultStr<VkCommandBuffer> cmd     = VulkanRdgBackend::CommandBufferOf( context );
        if ( !cmd )
            return Common::MakeError( cmd.GetError() );
        // BindGraphicsPipeline and DrawCounted record into the renderer's current command buffer, which the
        // graph backend points at the segment the pass runs on (VulkanRdgBackend::SetRecordingListener).
        if ( cmd.GetValue() != m_CurrentCommandBuffer )
            return Common::MakeFormattedError(
                 "{}: the renderer records into another command buffer than the pass's segment", pass );
        const Common::ResultStr<VulkanRdgPassDescriptors*> descriptors =
             VulkanRdgBackend::DescriptorsOf( context );
        if ( !descriptors )
            return Common::MakeError( descriptors.GetError() );

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        const auto* graphics = static_cast<const VulkanPipeline*>( &pipeline );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        auto* shader = static_cast<VulkanShader*>( pipeline.GetSpecification().Shader.get() );
        if ( shader == nullptr )
            return Common::MakeFormattedError( "{}: pipeline '{}' has no shader", pass,
                                               pipeline.GetSpecification().DebugName );

        // The other route: what the material wrote with a resource of its own (uniform values, asset
        // textures), never a slot holding only its fallback.
        RdgOtherRoute                       other;
        VulkanMaterialBackend::WrittenSlots materialSlots;
        std::span<const std::byte>          materialPush;
        if ( material != nullptr )
        {
            // RENDER-THREADS M3 (UE: the lambda binds what the setup built): the material's upload and descriptor
            // writes ran in its prepare, before the graph recorded; here it is only read.
            if ( auto recordable = material->CheckRecordable(); !recordable )
                return Common::MakeFormattedError( "{}: {}", pass, recordable.GetError() );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes material
            // backends
            auto* backend = static_cast<VulkanMaterialBackend*>( material->GetMaterialBackend().get() );
            if ( backend->HasDescriptorSets() )
            {
                const uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
                auto           written    = backend->GetWrittenSlots( frameIndex );
                if ( !written )
                    return Common::MakeFormattedError( "{}: {}", pass, written.GetError() );
                materialSlots = written.GetValue();
                for ( const uint32_t binding : materialSlots.Bindings )
                    other.Filled.push_back( RdgSlotKey{ .Set = 0, .Binding = binding } );
            }
            const auto& pc = material->GetPushConstantBuffer();
            if ( pc.Size != 0u )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a byte view of the push-constant
                // block (std::byte may alias any object)
                materialPush        = std::span<const std::byte>( reinterpret_cast<const std::byte*>( pc.Data ),
                                                                  static_cast<size_t>( pc.Size ) );
                other.PushConstants = true;
            }
        }
        // The draw's own push block replaces the material's (it is a copy of it with the draw's values over it):
        // the material is read, never written, to record the draw.
        if ( !drawPush.empty() )
        {
            if ( materialPush.empty() )
                return Common::MakeFormattedError( "{}: the draw carries a push block, its material has none",
                                                   pass );
            if ( drawPush.size() != materialPush.size() )
                return Common::MakeFormattedError( "{}: the draw's push block is {} bytes, its material's {}",
                                                   pass, drawPush.size(), materialPush.size() );
            materialPush = drawPush;
        }

        const ShaderResource::ReflectionData& reflection = shader->GetReflectionData();
        const auto resolved = ResolveRdgPassBindings( reflection, shader->GetName(), bindings, other );
        if ( !resolved )
            return Common::MakeError( resolved.GetError() );
        const std::vector<DescriptorSetLayoutRef>& layouts = graphics->GetLayouts();
        const auto sets = WriteRdgPassSets( *descriptors.GetValue(), layouts, resolved.GetValue(), pass );
        if ( !sets )
            return Common::MakeError( sets.GetError() );

        // The material's written slots are copied into this exec's set 0 from the material's own set.
        if ( !materialSlots.Bindings.empty() )
        {
            if ( sets.GetValue().empty() )
                return Common::MakeFormattedError( "{}: the material fills set 0, which the pipeline has not",
                                                   pass );
            std::vector<VkCopyDescriptorSet> copies;
            copies.reserve( materialSlots.Bindings.size() );
            for ( const uint32_t binding : materialSlots.Bindings )
            {
                const VkDescriptorSetLayoutBinding* entry = FindLayoutBinding( *layouts[0], binding );
                if ( entry == nullptr )
                    return Common::MakeFormattedError( "{}: material binding {} is not in the pipeline's set 0",
                                                       pass, binding );
                VkCopyDescriptorSet copy{};
                copy.sType           = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
                copy.srcSet          = materialSlots.Set;
                copy.srcBinding      = binding;
                copy.dstSet          = sets.GetValue()[0];
                copy.dstBinding      = binding;
                copy.descriptorCount = entry->descriptorCount;
                copies.push_back( copy );
            }
            VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                   ->GetVulkanLogicalDevice();
            vkUpdateDescriptorSets( device, 0, nullptr, static_cast<uint32_t>( copies.size() ), copies.data() );
        }

        // A pipeline still in the driver (PSO1) is not drawn this frame and is not an error: the content gate waits
        // for engine pipelines (BindGraphicsPipeline). Success(false) tells the draw to skip.
        if ( graphics->GetBuildState() == VulkanPipeline::BuildState::Compiling )
            return Common::MakeSuccess( false );
        if ( !BindGraphicsPipeline( &pipeline ) )
            return Common::MakeFormattedError( "{}: pipeline '{}' could not be bound in the open render pass",
                                               pass, pipeline.GetSpecification().DebugName );
        if ( !sets.GetValue().empty() )
            vkCmdBindDescriptorSets(
                 cmd.GetValue(), VK_PIPELINE_BIND_POINT_GRAPHICS, graphics->GetVkPipelineLayout(), 0,
                 static_cast<uint32_t>( sets.GetValue().size() ), sets.GetValue().data(), 0, nullptr );
        // Stages and size are the pipeline LAYOUT's range (VulkanPipeline::GetPushConstantRange), not the shader's
        // current reflection: the two part ways when the shader is re-reflected while this pipeline keeps its
        // layout.
        const std::span<const std::byte> push      = ChoosePushConstants( bindings, materialPush );
        const auto&                      pushRange = graphics->GetPushConstantRange();
        ReportPushStagesApart( *graphics, shader->GetName(), reflection.PushConstantRanges );
        if ( !push.empty() && pushRange.has_value() )
        {
            if ( push.size() > pushRange->size )
                return Common::MakeFormattedError( "{}: pipeline '{}' push range is {} bytes, the pass pushes {}",
                                                   pass, pipeline.GetSpecification().DebugName, pushRange->size,
                                                   push.size() );
            vkCmdPushConstants( cmd.GetValue(), graphics->GetVkPipelineLayout(), pushRange->stageFlags,
                                pushRange->offset, static_cast<uint32_t>( push.size() ), push.data() );
        }
        return Common::MakeSuccess( true );
    }

    void VulkanRendererAPI::ComputeImageBeginWrite( Image* image )
    {
        if ( !IsRecording() )
            return;
        // IVulkanImage, not VulkanImage2D: 2D targets, cubes and volumes all transition the same way,
        // and each supplies its own aspect mask and layer count.
        auto* vkImage = dynamic_cast<IVulkanImage*>( image );
        if ( !vkImage )
            return;

        // Make prior graphics writes (the scene color attachment, and any other sampled inputs produced
        // by earlier fragment passes) available + visible to the upcoming compute reads.
        VkMemoryBarrier barrier{ .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier( m_CurrentCommandBuffer,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );

        // Transition the target to GENERAL for storage writes (its last use was a fragment sample).
        vkImage->TransitionLayout( m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT );
    }

    void VulkanRendererAPI::ComputeImageEndWrite( Image* image )
    {
        if ( !IsRecording() )
            return;
        auto* vkImage = dynamic_cast<IVulkanImage*>( image );
        if ( !vkImage )
            return;

        // Back to SHADER_READ_ONLY so the fragment stage (tonemap) can sample the bloom result.
        vkImage->TransitionLayout( m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
    }

    void VulkanRendererAPI::ComputeImageBeginRead( Image* image )
    {
        if ( !IsRecording() )
            return;
        auto* vkImage = dynamic_cast<IVulkanImage*>( image );
        if ( !vkImage )
            return;

        // Source stage covers both producers this is used for: a depth attachment written by the
        // late-fragment depth test, and a colour attachment written by fragment output. The aspect mask
        // is the image's own, so a D24S8 attachment is barriered as DEPTH|STENCIL.
        vkImage->TransitionLayout(
             m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT );
    }

    void VulkanRendererAPI::ComputeImageEndRead( Image* image )
    {
        if ( !IsRecording() )
            return;
        auto* vkImage = dynamic_cast<IVulkanImage*>( image );
        if ( !vkImage )
            return;

        // Hand the image back in the layout its owner expects to find it in next frame — for the scene
        // depth attachment that is DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Leaving it in SHADER_READ_ONLY would
        // make the next depth pass begin from the wrong layout, which the render pass does not fix.
        vkImage->TransitionLayout(
             m_CurrentCommandBuffer, vkImage->GetDefaultLayout(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_SHADER_READ_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
    }

    namespace
    {
        // RDG-FAULT1 FrameFault: the frame-fatal externals the graph left without a picture are cleared to opaque
        // black on @p commandBuffer (recorded after the graph's segments), then put in the state the graph would
        // have left them in (an Extract's final access: Present for the swapchain image) with their records
        // updated, so acquire -> present and the next frame's import stay intact and the window shows black.
        Common::BoolResultStr ClearFrameFaultExternals( VkCommandBuffer commandBuffer, RDG::Builder& graph,
                                                        const RDG::FrameFault& fault )
        {
            const auto stages = []( RDG::PipelineStageFlags flags )
            {
                const VkPipelineStageFlags vk = RdgVulkanStages( flags );
                return vk != 0 ? vk : static_cast<VkPipelineStageFlags>( VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT );
            };
            for ( const RDG::FrameFaultExternal& entry : fault.Externals )
            {
                const uint32_t        resource = entry.Resource;
                RDG::ExternalTexture* external = graph.FindExternalTexture( resource );
                if ( external == nullptr )
                    return Common::MakeError(
                         std::format( "frame fault: resource {} is not an external texture", resource ) );
                auto* texture = dynamic_cast<VulkanRdgTexture*>( external->Physical.get() );
                if ( texture == nullptr )
                    return Common::MakeError( "frame fault: an external texture without a Vulkan image" );
                if ( texture->GetAspect() != VK_IMAGE_ASPECT_COLOR_BIT )
                    return Common::MakeError( "frame fault: a FrameFatal external that is not a colour image" );

                const RDG::TextureDesc& desc = external->Desc;
                const RDG::AccessState  dst  = RDG::GetAccessState( RDG::Access::CopyDst );
                const RDG::AccessState after = entry.FinalAccess ? RDG::GetAccessState( *entry.FinalAccess ) : dst;

                std::vector<VkImageSubresourceRange> ranges;
                for ( uint32_t layer = 0; layer < desc.Layers; ++layer )
                {
                    for ( uint32_t mip = 0; mip < desc.Mips; ++mip )
                    {
                        const RDG::AccessState& before =
                             external->SubresourceStates[desc.SubresourceIndex( mip, layer )];
                        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1 };
                        VkImageMemoryBarrier          barrier{};
                        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        barrier.srcAccessMask       = RdgVulkanAccess( before.Memory );
                        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.oldLayout           = RdgVulkanLayout( before.Layout );
                        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.image               = texture->GetImage();
                        barrier.subresourceRange    = range;
                        vkCmdPipelineBarrier( commandBuffer, stages( before.Stages ),
                                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                              &barrier );
                        ranges.push_back( range );
                    }
                }
                const VkClearColorValue black{ { 0.0f, 0.0f, 0.0f, 1.0f } };
                vkCmdClearColorImage( commandBuffer, texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      &black, static_cast<uint32_t>( ranges.size() ), ranges.data() );
                if ( after != dst )
                {
                    VkImageMemoryBarrier barrier{};
                    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask       = RdgVulkanAccess( after.Memory );
                    barrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.newLayout           = RdgVulkanLayout( after.Layout );
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image               = texture->GetImage();
                    barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.Mips, 0, desc.Layers };
                    vkCmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, stages( after.Stages ), 0,
                                          0, nullptr, 0, nullptr, 1, &barrier );
                }
                external->SubresourceStates.assign( external->SubresourceStates.size(), after );
                if ( external->RecordStates )
                {
                    if ( const auto recorded = external->RecordStates( external->SubresourceStates, true );
                         !recorded )
                        return Common::MakeError( recorded.GetError() );
                }
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::BoolResultStr VulkanRendererAPI::ExecuteGraph( RDG::Builder& graph )
    {
        // The failures of this function itself are logged here; the graph's own faults (and a FrameFault) are
        // logged once per fault by the backend's PassFaultReporter. A caller never logs the result.
        const auto fail = []( std::string message ) -> Common::BoolResultStr
        {
            LOG_ERROR( "[Renderer] ExecuteGraph: {}", message );
            return Common::MakeError( message );
        };
        if ( !IsRecording() )
            return fail( "No active command buffer" );

        if ( !m_RdgBackend )
            return fail( "the render graph frame objects were not begun (BeginFrame)" );
        // The second writer of m_CurrentCommandBuffer after BeginFrame: it ends the frame's command buffer and
        // re-arms a fresh one after the graph, so it asks the device-lost gate like BeginFrame does.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            m_CurrentCommandBuffer = nullptr;
            return fail( "the device is lost; the graph is not recorded" );
        }
        // The sink the profiler holds now: GPU timing can be switched on and off between frames.
#if DESERT_DEV_INSTRUMENTS
        m_RdgDevice.Profiler = ::Common::Profiling::Profiler::Get().GetGpuSink();
#endif
        // RDG-CONTRACTS B(3): the graph records into its own segment command buffers, so the frame command
        // buffer is split here: what was recorded before the graph is submitted before the graph.
        const VkResult ended = vkEndCommandBuffer( m_CurrentCommandBuffer );
        if ( ended != VK_SUCCESS )
        {
            m_CurrentCommandBuffer = nullptr;
            (void)NoteIfDeviceLost( ended, "vkEndCommandBuffer", __FILE__, __LINE__ );
            return fail(
                 std::format( "vkEndCommandBuffer before a graph failed: {}", VkResultToString( ended ) ) );
        }
        m_FrameSubmissions.push_back(
             { RDG::Pipe::Graphics, m_RdgQueues.GraphicsQueue, m_CurrentCommandBuffer, {}, {}, {} } );
        // Ended: from here until the graph backend arms its first buffer nothing may record into it.
        m_CurrentCommandBuffer                    = nullptr;
        Common::BoolResultStr            executed = graph.Execute( *m_RdgBackend );
        std::vector<VulkanRdgSubmission> segments = m_RdgBackend->TakeSubmissions();
        // A graph that failed inside an async segment never reached its EndPipeSegment: what records after the
        // graph is on the graphics queue whatever the graph left behind.
        if ( m_RdgDevice.Profiler != nullptr )
            m_RdgDevice.Profiler->SetQueue( ::Common::Profiling::GpuQueue::Graphics );
        m_FrameSubmissions.insert( m_FrameSubmissions.end(), std::make_move_iterator( segments.begin() ),
                                   std::make_move_iterator( segments.end() ) );

        // Recording continues after the graph in a fresh graphics command buffer of this frame slot. A loss
        // noted while the graph recorded (a segment's vkEndCommandBuffer) must not re-arm the frame.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            m_CurrentCommandBuffer = nullptr;
            return fail( "the device was lost while the graph recorded" );
        }
        const Common::ResultStr<VkCommandBuffer> next =
             m_FrameLoop->GetQueueObjects().BeginCommandBuffer( RDG::Pipe::Graphics );
        if ( !next )
        {
            m_CurrentCommandBuffer = nullptr;
            return fail( next.GetError() );
        }
        m_CurrentCommandBuffer = next.GetValue();
        m_BoundGeometry.reset(); // a freshly begun buffer holds no geometry binding
        // A FrameFault: the graph's output has no picture, so its frame-fatal externals are cleared to black on
        // the re-armed buffer and the frame presents that.
        if ( const std::optional<RDG::FrameFault>& frame = graph.GetExecuteReport().Frame )
        {
            DESERT_PROFILE_PASS( "RDG: FrameFault clear" ); // recorded outside every pass of the graph
            if ( const auto cleared = ClearFrameFaultExternals( m_CurrentCommandBuffer, graph, *frame ); !cleared )
                return fail( std::format( "graph '{}': {}", graph.GetName(), cleared.GetError() ) );
        }
        return executed;
    }

    void VulkanRendererAPI::SetGraphRecordingTarget( VkCommandBuffer commandBuffer )
    {
        // The in-graph writer of the recording target (see the field). The graph only runs inside the gated
        // ExecuteGraph; a loss noted mid-graph still disarms here rather than arming the next segment.
        if ( commandBuffer != VK_NULL_HANDLE && !Graphic::DeviceLost::AllowWork() )
        {
            m_CurrentCommandBuffer = nullptr;
            return;
        }
        m_CurrentCommandBuffer = commandBuffer;
        // A segment's buffer may be one begun again since it last recorded (same handle, empty state).
        m_BoundGeometry.reset();
    }

    Common::BoolResultStr VulkanRendererAPI::BeginRdgFrame()
    {
        const auto     device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        const uint32_t slots  = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( !m_RdgBackend )
        {
            m_RdgDevice.Device        = device->GetVulkanLogicalDevice();
            m_RdgDevice.Allocator     = VulkanAllocator::GetVMAAllocator();
            m_RdgDevice.CmdBeginLabel = fpCmdBeginDebugUtilsLabelEXT;
            m_RdgDevice.CmdEndLabel   = fpCmdEndDebugUtilsLabelEXT;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties( device->GetPhysicalDevice()->GetVulkanPhysicalDevice(), &properties );
            m_RdgDevice.Limits        = properties.limits;
            // The same sharing rule VulkanAllocator creates every engine resource with (VulkanResourceSharing.hpp).
            m_RdgDevice.Sharing       = device->GetPhysicalDevice()->GetResourceSharing();
            m_RdgPool                 = std::make_unique<VulkanRdgPool>( m_RdgDevice, slots );
            m_RdgBackend              = std::make_unique<VulkanRdgBackend>( m_RdgDevice, *m_RdgPool );
            // B2: while a graph runs, every renderer's Record() goes through this API, so this API records
            // into the command buffer of the segment the running pass is on, never into the frame's own
            // (ended before the graph) or another segment's. ExecuteGraph re-arms a fresh one after.
            m_RdgBackend->SetRecordingListener( [this]( VkCommandBuffer commandBuffer )
                                                { SetGraphRecordingTarget( commandBuffer ); } );
            m_RdgTransients  = std::make_unique<VulkanRdgTransientAllocator>( m_RdgDevice, slots );
            m_RdgDescriptors = std::make_unique<VulkanRdgPassDescriptors>(
                 m_RdgDevice.Device, EngineSamplerCache(), slots, device->GetRayTracingFunctions().Complete() );

            // The compute queue is the graph's AsyncCompute pipe only when the catalog offers async compute
            // (CapabilityCatalog::AsyncCompute: a separate compute family, and not a portability device whose
            // Metal backend overlaps encoders itself) - VulkanPhysicalDevice falls back to the graphics family
            // when there is none. Otherwise the queue set gets no compute queue, every pass runs on Graphics and
            // the backend's fallback log says so once.
            const uint32_t graphicsFamily = device->GetPhysicalDevice()->GetGraphicsFamily();
            const uint32_t computeFamily  = device->GetPhysicalDevice()->GetComputeFamily();
            const bool     separate       = computeFamily != graphicsFamily &&
                                  EngineContext::GetInstance().GetCapabilities().Catalog.AsyncCompute;
            m_FrameLoop                   = std::make_unique<VulkanFrameLoop>(
                 m_RdgDevice.Device, graphicsFamily,
                 separate ? std::optional<uint32_t>( computeFamily ) : std::nullopt, slots );
            m_RdgQueues.GraphicsQueue  = device->GetGraphicsQueue();
            m_RdgQueues.GraphicsFamily = graphicsFamily;
            m_RdgQueues.ComputeQueue   = separate ? device->GetComputeQueue() : VK_NULL_HANDLE;
            m_RdgQueues.ComputeFamily  = computeFamily;
            m_RdgQueues.Objects        = &m_FrameLoop->GetQueueObjects();
        }
        // The slot's fence FIRST: every object begun below may still be in use by the slot's previous frame.
        const uint32_t slot = EngineContext::GetInstance().GetCurrentFrameIndex();
        if ( Common::BoolResultStr begun = m_FrameLoop->BeginSlot( slot ); !begun )
            return begun;
        m_RdgPool->BeginFrame();
        m_RdgTransients->BeginFrameSlot( slot );
        m_RdgDescriptors->BeginFrameSlot( slot );
        return m_RdgBackend->BeginFrame( slot, m_RdgQueues, *m_RdgTransients, *m_RdgDescriptors );
    }

    void VulkanRendererAPI::RetireGraphTexture( std::shared_ptr<RDG::IPhysicalTexture> texture )
    {
        if ( m_RdgPool )
            m_RdgPool->Retire( std::move( texture ) );
    }

    namespace
    {
        // The graph's handle on an engine image, made once and kept by the image (VulkanImage2D::GetGraphTexture).
        // The views a render pass binds are this handle's: a handle made per frame destroyed them when the frame's
        // texture table went away, while the command buffer that bound them was still recording
        // (VUID-...-recording on every later command). One handle per image, so every import of it into a
        // graph (ImportImage) hands out the same views.
        const std::shared_ptr<RDG::IPhysicalTexture>& GraphTextureOf( IVulkanImage& image )
        {
            if ( !image.GetGraphTexture() )
            {
                const VulkanImageResource& resource = image.GetResource();
                VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                       ->GetVulkanLogicalDevice();
                image.SetGraphTexture(
                     VulkanRdgTexture::Wrap( device, resource.Image, resource.Format, image.GetGraphDesc() ) );
            }
            return image.GetGraphTexture();
        }
    } // namespace

    Common::BoolResultStr VulkanRendererAPI::ImportBackBuffer( RDG::ExternalTexture& into )
    {
        const auto window = m_Window.lock();
        if ( !window )
            return Common::MakeError( "ImportBackBuffer: the window is gone" );
        const auto swapChain = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );

        // THE LATE ACQUIRE (PRESENT-COST; UE resolves the viewport's back buffer at the final pass the same way).
        // The frame's last pass imports the back buffer, so by now the layers' update and every scene graph of
        // the frame are recorded, and the CPU waits on the display (FIFO, a full present queue) only after it has
        // built the frame instead of before. Everything recorded so far is in m_FrameSubmissions or in the open
        // frame command buffer, which is pushed at exactly the current size: no entry below that index can touch
        // the image, so the acquire semaphore is waited by the first graphics entry from there on
        // (VulkanFrameOutput::FirstEntryAfterAcquire), not by the frame's first.
        VulkanSwapChainOutput& output = *swapChain->GetOutput();
        if ( !output.HasAcquiredImage() )
        {
            if ( !output.AcquireImage() )
                return Common::MakeError(
                     "ImportBackBuffer: no swapchain image for this frame (the acquire was out of date and the "
                     "swapchain is rebuilt at the next frame boundary, or it failed; see the line above). The "
                     "frame is submitted without being presented." );
            m_BackBufferFirstEntry = m_FrameSubmissions.size();
        }
        return swapChain->ImportBackBuffer( into );
    }

    Common::BoolResultStr VulkanRendererAPI::ImportImage( const std::shared_ptr<Image>& image,
                                                          RDG::ExternalTexture&         into )
    {
        // ONE path for a 2D image, a volume and a cube: everything kind-specific is the image's own
        // IVulkanImage::GetGraphDesc (a volume answers Tex3D, its depth and one layer, so the barriers the
        // graph plans on it cover [mips) x layer 0 and its views are 3D).
        const std::shared_ptr<IVulkanImage> vulkanImage = std::dynamic_pointer_cast<IVulkanImage>( image );
        if ( !vulkanImage )
            return Common::MakeError( "ImportImage: not a Vulkan image" );
        const VulkanImageResource& resource = vulkanImage->GetResource();
        const RDG::TextureDesc     desc     = vulkanImage->GetGraphDesc();
        if ( resource.Image == VK_NULL_HANDLE )
            return Common::MakeError( std::format( "ImportImage: the {}x{}x{} image has no VkImage",
                                                   desc.Size.Width, desc.Size.Height, desc.Size.Depth ) );
        const std::optional<RDG::ImageLayout> layout = RdgLayoutFromVulkan( resource.Layout );
        if ( !layout )
            return Common::MakeError( std::format( "ImportImage: image layout {} has no render-graph layout",
                                                   static_cast<int>( resource.Layout ) ) );

        into.Desc = desc;
        into.SubresourceStates.assign( desc.SubresourceCount(), RDG::RecordedLayoutState( *layout ) );
        for ( uint32_t index = 0; index < desc.SubresourceCount(); ++index )
        {
            const std::optional<RDG::ImageLayout> own = RdgLayoutFromVulkan( resource.LayoutOf( index ) );
            if ( own )
                into.SubresourceStates[index] = RDG::RecordedLayoutState( *own );
        }
        into.Physical                          = GraphTextureOf( *vulkanImage );
        const std::weak_ptr<IVulkanImage> weak = vulkanImage;
        // Called after every barrier the graph records on this image and once at the end: the image's record is
        // always the layout the GPU has each subresource in. While a graph has the subresources in different
        // layouts (a mip chain mid-walk) the whole-image layout is UNDEFINED, so a bind of the whole image fails
        // validation loudly, and each mip's own layout is recorded. At the end of the graph the subresources
        // must agree; the next frame imports from the record.
        into.RecordStates = [weak]( const std::vector<RDG::AccessState>& states,
                                    bool                                 graphEnded ) -> Common::BoolResultStr
        {
            const std::shared_ptr<IVulkanImage> target = weak.lock();
            if ( !target )
                return Common::MakeError( "the imported image was destroyed before its graph finished" );
            if ( states.empty() )
                return Common::MakeSuccess( true );
            const auto mixed = std::find_if( states.begin(), states.end(), [&]( const RDG::AccessState& state )
                                             { return state.Layout != states.front().Layout; } );
            if ( mixed != states.end() && graphEnded )
                return Common::MakeError(
                     std::format( "its subresources end in different layouts ({} and {}), the image records one",
                                  static_cast<int>( states.front().Layout ), static_cast<int>( mixed->Layout ) ) );
            std::vector<VkImageLayout> layouts;
            layouts.reserve( states.size() );
            for ( const RDG::AccessState& state : states )
                layouts.push_back( RdgVulkanLayout( state.Layout ) );
            target->RecordLayouts( std::move( layouts ) );
            return Common::MakeSuccess( true );
        };
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr
    VulkanRendererAPI::ImportBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer,
                                     RDG::ExternalBuffer&                                   into )
    {
        using ShaderResources::API::Vulkan::VulkanStorageBuffer;
        const std::shared_ptr<VulkanStorageBuffer> vulkanBuffer =
             std::dynamic_pointer_cast<VulkanStorageBuffer>( buffer );
        if ( !vulkanBuffer )
            return Common::MakeError( "ImportBuffer: not a Vulkan storage buffer" );
        ShaderResources::API::Vulkan::ViewCopyBinding binding;
        const Common::BoolResultStr                   bound =
             vulkanBuffer->BindActiveCopy( Engine::FrameManager::GetInstance().GetCurrentFrameIndex(), binding );
        if ( !bound )
            return Common::MakeError( std::format( "ImportBuffer: {}", bound.GetError() ) );
        if ( binding.Info.buffer == VK_NULL_HANDLE || binding.Info.range == 0 )
            return Common::MakeError( "ImportBuffer: the buffer's copy for this frame has no VkBuffer" );

        // The record names the copy the last graph imported; a different copy (a per-frame buffer's next
        // frame, or a copy rebuilt after a resize) starts untouched with a fresh handle.
        VulkanStorageBuffer::GraphRecord& record = vulkanBuffer->GetGraphRecord();
        if ( record.CopyId != binding.CopyId || !record.Physical )
        {
            record.CopyId   = binding.CopyId;
            record.Physical = VulkanRdgBuffer::Wrap( binding.Info.buffer, binding.Info.range );
            record.State    = RDG::GetAccessState( RDG::Access::None );
        }
        into.Desc.Bytes                               = binding.Info.range;
        into.Physical                                 = record.Physical;
        into.State                                    = record.State;
        const std::weak_ptr<VulkanStorageBuffer> weak = vulkanBuffer;
        const uint64_t                           copy = binding.CopyId;
        into.RecordFinalState = [weak, copy]( const RDG::AccessState& state ) -> Common::BoolResultStr
        {
            const std::shared_ptr<VulkanStorageBuffer> target = weak.lock();
            if ( !target )
                return Common::MakeError( "the imported buffer was destroyed before its graph finished" );
            VulkanStorageBuffer::GraphRecord& record = target->GetGraphRecord();
            // Another graph imported a different copy since: this state belongs to a copy no longer recorded.
            if ( record.CopyId == copy )
                record.State = state;
            return Common::MakeSuccess( true );
        };
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::shared_ptr<SharedGpuBuffer>>
    VulkanRendererAPI::CreateSharedBuffer( const std::string_view name, const uint64_t bytes )
    {
        return CreateVulkanSharedBuffer( name, bytes );
    }

    Common::ResultStr<std::shared_ptr<SharedGpuTimeline>>
    VulkanRendererAPI::CreateSharedTimeline( const std::string_view name )
    {
        return CreateVulkanSharedTimeline( name );
    }

    Common::BoolResultStr VulkanRendererAPI::ImportVertexBuffer( const std::shared_ptr<VertexBuffer>& buffer,
                                                                 RDG::ExternalBuffer&                 into )
    {
        const std::shared_ptr<VulkanVertexBuffer> vulkanBuffer =
             std::dynamic_pointer_cast<VulkanVertexBuffer>( buffer );
        if ( !vulkanBuffer )
            return Common::MakeError( "ImportVertexBuffer: not a Vulkan vertex buffer" );
        const VkBuffer handle = vulkanBuffer->GetVulkanBuffer();
        if ( handle == VK_NULL_HANDLE || vulkanBuffer->GetSize() == 0 )
            return Common::MakeError( "ImportVertexBuffer: the vertex buffer has not been built" );

        // A rebuilt buffer (Invalidate) is a new VkBuffer that no graph has touched.
        VulkanVertexBuffer::GraphRecord& record = vulkanBuffer->GetGraphRecord();
        if ( record.Buffer != handle || !record.Physical )
        {
            record.Buffer   = handle;
            record.Physical = VulkanRdgBuffer::Wrap( handle, vulkanBuffer->GetSize() );
            record.State    = RDG::GetAccessState( RDG::Access::None );
        }
        into.Desc.Bytes                              = vulkanBuffer->GetSize();
        into.Physical                                = record.Physical;
        into.State                                   = record.State;
        const std::weak_ptr<VulkanVertexBuffer> weak = vulkanBuffer;
        into.RecordFinalState = [weak, handle]( const RDG::AccessState& state ) -> Common::BoolResultStr
        {
            const std::shared_ptr<VulkanVertexBuffer> target = weak.lock();
            if ( !target )
                return Common::MakeError( "the imported vertex buffer was destroyed before its graph finished" );
            VulkanVertexBuffer::GraphRecord& record = target->GetGraphRecord();
            if ( record.Buffer == handle )
                record.State = state;
            return Common::MakeSuccess( true );
        };
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::ImportVertexBufferRange( const std::shared_ptr<VertexBuffer>& buffer,
                                                                      const uint64_t offset, const uint64_t bytes,
                                                                      RDG::ExternalBuffer& into )
    {
        const std::shared_ptr<VulkanVertexBuffer> vulkanBuffer =
             std::dynamic_pointer_cast<VulkanVertexBuffer>( buffer );
        if ( !vulkanBuffer )
            return Common::MakeError( "ImportVertexBufferRange: not a Vulkan vertex buffer" );
        const VkBuffer handle = vulkanBuffer->GetVulkanBuffer();
        const uint64_t size   = vulkanBuffer->GetSize();
        if ( handle == VK_NULL_HANDLE || size == 0 )
            return Common::MakeError( "ImportVertexBufferRange: the vertex buffer has not been built" );
        if ( bytes == 0 || offset > size || bytes > size - offset )
            return Common::MakeFormattedError( "ImportVertexBufferRange: the span [{}, +{}) is outside the {}-byte "
                                               "buffer",
                                               offset, bytes, size );
        if ( vulkanBuffer->GetUsage() != BufferUsage::Pooled )
        {
            // A mesh's own buffer (a GPU-written twin) holds that mesh alone from byte 0: the whole buffer, with
            // its state record, since a graph may write it.
            if ( offset != 0 )
                return Common::MakeError( "ImportVertexBufferRange: a span at a non-zero offset of a buffer a "
                                          "graph may write (only a GeometryPool buffer is imported by span)" );
            return ImportVertexBuffer( buffer, into );
        }
        // A storage descriptor of the span needs minStorageBufferOffsetAlignment (<= 256 by the spec): the pool
        // starts every span on a 256-byte boundary of each of its buffers (GeometryPool.cpp kSpanByteAlignment).
        into.Desc.Bytes       = bytes;
        into.Physical         = VulkanRdgBuffer::Wrap( handle, bytes, offset );
        into.State            = RDG::GetAccessState( RDG::Access::None );
        into.RecordFinalState = []( const RDG::AccessState& ) -> Common::BoolResultStr { return BOOLSUCCESS; };
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::CopyDepthImage( Image2D* src, Image2D* dst )
    {
        if ( !IsRecording() )
            return Common::MakeError( "CopyDepthImage: no command buffer is recording" );
        auto* vsrc = dynamic_cast<VulkanImage2D*>( src );
        auto* vdst = dynamic_cast<VulkanImage2D*>( dst );
        if ( !vsrc || !vdst )
            return Common::MakeError( "CopyDepthImage: the source or the destination is not a Vulkan 2D image" );
        // vkCmdCopyImage needs equal sample counts: a multisampled target depth cannot take the single-sample
        // G-buffer depth by copy (that needs a depth resolve, which this is not).
        const uint32_t srcSamples = src->GetImageSpecification().Samples;
        const uint32_t dstSamples = dst->GetImageSpecification().Samples;
        if ( srcSamples != dstSamples || src->GetWidth() != dst->GetWidth() ||
             src->GetHeight() != dst->GetHeight() )
        {
            const std::string message = std::format(
                 "CopyDepthImage: the source depth is {}x{} with {} sample(s), the destination {}x{} with "
                 "{} sample(s); a copy needs the same size and sample count",
                 src->GetWidth(), src->GetHeight(), srcSamples, dst->GetWidth(), dst->GetHeight(), dstSamples );
            LOG_ERROR( "[Renderer] {}", message );
            return Common::MakeError( message );
        }

        // The frame graph's Copy node ("Deferred: DepthResolve") put src in TRANSFER_SRC and dst in
        // TRANSFER_DST before this runs, and places the transitions after it.
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.extent         = { src->GetWidth(), src->GetHeight(), 1 };
        vkCmdCopyImage( m_CurrentCommandBuffer, vsrc->GetResource().Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        vdst->GetResource().Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::CopyGraphDepthToLayer( const RDG::PassContext& context,
                                                                    const RDG::TextureRef   source,
                                                                    const RDG::TextureRef   target,
                                                                    const uint32_t          targetLayer )
    {
        const auto commands = VulkanRdgBackend::CommandBufferOf( context );
        if ( !commands )
            return Common::MakeError( commands.GetError() );
        const auto from = context.GetTexture( source, RDG::Access::CopySrc );
        if ( !from )
            return Common::MakeError( from.GetError() );
        const auto to =
             context.GetTexture( target, RDG::Access::CopyDst, RDG::SubresourceRange::Layer( targetLayer ) );
        if ( !to )
            return Common::MakeError( to.GetError() );
        const auto fromImage = VulkanRdgBackend::TextureOf( from.GetValue() );
        if ( !fromImage )
            return Common::MakeError( fromImage.GetError() );
        const auto toImage = VulkanRdgBackend::TextureOf( to.GetValue() );
        if ( !toImage )
            return Common::MakeError( toImage.GetError() );

        const RDG::TextureDesc& src = *from.GetValue().Desc;
        const RDG::TextureDesc& dst = *to.GetValue().Desc;
        if ( src.Size.Width != dst.Size.Width || src.Size.Height != dst.Size.Height ||
             src.Samples != dst.Samples || targetLayer >= dst.Layers )
            return Common::MakeError( std::format(
                 "CopyGraphDepthToLayer: '{}' is {}x{} with {} sample(s), layer {} of '{}' ({}x{}, {} sample(s), "
                 "{} layer(s)); a copy needs the same size and sample count and an existing layer",
                 from.GetValue().Name, src.Size.Width, src.Size.Height, src.Samples, targetLayer,
                 to.GetValue().Name, dst.Size.Width, dst.Size.Height, dst.Samples, dst.Layers ) );

        // The graph's Copy node put the source in TRANSFER_SRC and the target layer in TRANSFER_DST.
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, targetLayer, 1 };
        region.extent         = { src.Size.Width, src.Size.Height, 1 };
        vkCmdCopyImage( commands.GetValue(), fromImage.GetValue()->GetImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, toImage.GetValue()->GetImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::CopyGraphBufferToTexture( const RDG::PassContext& context,
                                                                       const RDG::BufferRef    source,
                                                                       const RDG::TextureRef   target )
    {
        const auto commands = VulkanRdgBackend::CommandBufferOf( context );
        if ( !commands )
            return Common::MakeError( commands.GetError() );
        const auto from = context.GetBuffer( source, RDG::Access::CopySrc );
        if ( !from )
            return Common::MakeError( from.GetError() );
        const auto to = context.GetTexture( target, RDG::Access::CopyDst );
        if ( !to )
            return Common::MakeError( to.GetError() );
        const auto fromBuffer = VulkanRdgBackend::BufferOf( from.GetValue() );
        if ( !fromBuffer )
            return Common::MakeError( fromBuffer.GetError() );
        const auto toImage = VulkanRdgBackend::TextureOf( to.GetValue() );
        if ( !toImage )
            return Common::MakeError( toImage.GetError() );

        const RDG::TextureDesc& dst = *to.GetValue().Desc;
        const uint64_t bytes = Core::Formats::CalculateImageSize( dst.Size.Width, dst.Size.Height, dst.Format );
        if ( dst.Dim != RDG::TextureDim::Tex2D || dst.Samples != 1 || from.GetValue().Desc->Bytes < bytes )
            return Common::MakeError( std::format(
                 "CopyGraphBufferToTexture: '{}' ({} bytes) into '{}' ({}x{}, {} sample(s), {} bytes at mip 0); a "
                 "texel upload needs a single-sample 2D target and a buffer holding all of its mip 0",
                 from.GetValue().Name, from.GetValue().Desc->Bytes, to.GetValue().Name, dst.Size.Width,
                 dst.Size.Height, dst.Samples, bytes ) );

        // The graph's Copy node put the buffer in TRANSFER_READ and the texture in TRANSFER_DST.
        VkBufferImageCopy region{};
        region.bufferOffset      = fromBuffer.GetValue()->GetOffset();
        region.bufferRowLength   = 0; // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent       = { dst.Size.Width, dst.Size.Height, 1 };
        vkCmdCopyBufferToImage( commands.GetValue(), fromBuffer.GetValue()->GetBuffer(),
                                toImage.GetValue()->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
        return BOOLSUCCESS;
    }

    void VulkanRendererAPI::SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height )
    {
        if ( !IsRecording() )
            return;
        VkRect2D scissor = { .offset = { x, y }, .extent = { width, height } };
        vkCmdSetScissor( m_CurrentCommandBuffer, 0, 1, &scissor );
    }

    void VulkanRendererAPI::SetViewportRegion( int32_t x, int32_t y, uint32_t width, uint32_t height )
    {
        if ( !IsRecording() || width == 0 || height == 0 )
            return;
        // The engine's convention (SetViewportAndScissor): negative height, origin at the rectangle's bottom.
        const VkViewport viewport{ static_cast<float>( x ),
                                   static_cast<float>( y ) + static_cast<float>( height ),
                                   static_cast<float>( width ),
                                   -static_cast<float>( height ),
                                   0.0f,
                                   1.0f };
        const VkRect2D   scissor{ { x, y }, { width, height } };
        vkCmdSetViewport( m_CurrentCommandBuffer, 0, 1, &viewport );
        vkCmdSetScissor( m_CurrentCommandBuffer, 0, 1, &scissor );
    }

    void VulkanRendererAPI::ClearRegion( const Framebuffer& framebuffer, int32_t x, int32_t y, uint32_t width,
                                         uint32_t height, const glm::vec4& color, float depth )
    {
        if ( !IsRecording() || width == 0 || height == 0 )
            return;
        const auto&                    formats = framebuffer.GetSpecification().Attachments.Attachments;
        std::vector<VkClearAttachment> attachments;
        attachments.reserve( formats.size() );
        uint32_t colorIndex = 0;
        for ( const auto& attachment : formats )
        {
            if ( Graphic::Utils::IsDepthFormat( attachment.Format ) )
            {
                VkClearAttachment clear{};
                // Asked of the format, as ClearAttachments does: a stencil aspect only where the format has one.
                clear.aspectMask              = GetImageVulkanAspect( attachment.Format );
                clear.clearValue.depthStencil = { depth, 0 };
                attachments.push_back( clear );
                continue;
            }
            VkClearAttachment clear{};
            clear.aspectMask       = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment  = colorIndex++;
            clear.clearValue.color = { { color.r, color.g, color.b, color.a } };
            attachments.push_back( clear );
        }
        const VkClearRect rect{
             .rect = { .offset = { x, y }, .extent = { width, height } }, .baseArrayLayer = 0, .layerCount = 1 };
        vkCmdClearAttachments( m_CurrentCommandBuffer, static_cast<uint32_t>( attachments.size() ),
                               attachments.data(), 1, &rect );
    }

    // Same shape as VulkanContext::OnResize: the RendererAPI interface requires it and the Vulkan backend
    // has nothing to do here, because SceneRenderer::OnResize already drives the swapchain and every
    // framebuffer directly a few lines after it calls this.
    void VulkanRendererAPI::ResizeWindowEvent( uint32_t /*width*/, uint32_t /*height*/ )
    {
    }

    void VulkanRendererAPI::WaitDeviceIdle()
    {
        // Nothing is in flight on a lost device and nothing ever will be, so this can only return
        // VK_ERROR_DEVICE_LOST. Skipping it is not a shortcut: the callers use it to make teardown safe,
        // and teardown after a loss is safe by construction because no work is outstanding.
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        // Through the device, not vkDeviceWaitIdle here: it must hold the queue lock (VK1).
        EngineContext::GetInstance().GetDevice()->WaitIdle();
    }
    // The window swapchain's composite wrapper: the format every pipeline drawing into the back buffer is
    // built against. Asked of the swapchain itself, so it exists from the first frame on, before any pass.
    std::shared_ptr<Framebuffer> VulkanRendererAPI::GetCompositeFramebuffer() const
    {
        const auto window = m_Window.lock();
        if ( !window )
        {
            return nullptr;
        }
        const auto vulkanSwap = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );
        return vulkanSwap ? vulkanSwap->GetCompositeFramebuffer() : nullptr;
    }
    void VulkanRendererAPI::SetViewportAndScissor( const uint32_t width, const uint32_t height )
    {
        if ( !IsRecording() )
            return;
        if ( width == 0 || height == 0 )
            return;

        // Modern Vulkan: use negative height to flip Y coordinate system to match OpenGL (Y-up)
        VkViewport viewport = { .x        = 0.0f,
                                .y        = (float)height,
                                .width    = (float)width,
                                .height   = -(float)height,
                                .minDepth = 0.0f,
                                .maxDepth = 1.0f };
        vkCmdSetViewport( m_CurrentCommandBuffer, 0, 1, &viewport );

        VkRect2D scissor = { .offset = { 0, 0 }, .extent = { width, height } };
        vkCmdSetScissor( m_CurrentCommandBuffer, 0, 1, &scissor );
    }

    void VulkanRendererAPI::ClearAttachments( const std::vector<VkClearValue>&    clearValues,
                                              const std::shared_ptr<Framebuffer>& framebuffer )
    {
        if ( !IsRecording() )
            return;
        uint32_t                       attachmentCount = (uint32_t)clearValues.size();
        std::vector<VkClearAttachment> attachments( attachmentCount );
        std::vector<VkClearRect>       clearRects( attachmentCount );
        for ( uint32_t i = 0; i < attachmentCount; i++ )
        {
            attachments[i] = {
                 .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = i, .clearValue = clearValues[i] };
            // Asked of the format, not assumed: the scene depth attachment is DEPTH32F (depth-only)
            // since reversed-Z, and naming a stencil aspect a format does not carry is a validation
            // error rather than a harmless extra bit. GetImageVulkanAspect is the same answer the image
            // views themselves are built from, so a clear can never name an aspect the view does not.
            const auto format = framebuffer->GetSpecification().Attachments.Attachments[i].Format;
            if ( Graphic::Utils::IsDepthFormat( format ) )
                attachments[i].aspectMask = GetImageVulkanAspect( format );
            clearRects[i] = {
                 .rect           = { .offset = { 0, 0 },
                                     .extent = { framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() } },
                 .baseArrayLayer = 0,
                 .layerCount     = 1 };
        }
        vkCmdClearAttachments( m_CurrentCommandBuffer, attachmentCount, attachments.data(), attachmentCount,
                               clearRects.data() );
    }

    // ── ОДНА ВОРОНКА НА ВСЕ ОТРИСОВКИ (см. довод в заголовке) ────────────────────────────────────────
    //
    // Счёт идёт ДО вызова: если драйвер упадёт на этом вызове, число уже названо, и последний кадр
    // скажет, сколько успел. Обратный порядок терял бы ровно тот кадр, который надо объяснить.
    void VulkanRendererAPI::DrawIndexedCounted( uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                                int32_t vertexOffset, uint32_t firstInstance )
    {
        DrawCounter::Record( instanceCount );
        vkCmdDrawIndexed( m_CurrentCommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset,
                          firstInstance );
    }

    void VulkanRendererAPI::DrawCounted( uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                                         uint32_t firstInstance )
    {
        DrawCounter::Record( instanceCount );
        vkCmdDraw( m_CurrentCommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance );
    }

} // namespace Desert::Graphic::API::Vulkan
