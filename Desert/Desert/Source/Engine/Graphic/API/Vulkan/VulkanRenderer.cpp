#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/Core/RenderTask.hpp>
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
#include <string>
#include <type_traits>
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
#include <Engine/Graphic/GpuDriven/IndirectDrawSubmission.hpp>
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
        // Everything handed to the submit thread is out before anything it names is released.
        if ( m_FrameLoop )
            m_FrameLoop->Drain();
#if DESERT_DEV_INSTRUMENTS
        m_GpuProfiler.Shutdown();
#endif
    }

    Common::BoolResultStr VulkanRendererAPI::BeginFrame()
    {
        if ( const Common::BoolResultStr waited = WaitFrameSlot(); !waited )
            return waited;
        return BeginFrameRecording();
    }

    Common::BoolResultStr VulkanRendererAPI::WaitFrameSlot()
    {
        // THE STRUCTURAL GATE FOR EVERY vkCmd* IN THIS FILE. Nothing below records a command without
        // RecordingSlot().CommandBuffer, and this is the only function that ever sets it — so leaving it null
        // here turns all forty-odd recording entry points into no-ops without a guard in each of them.
        // Application::Run reads this result and ends the run, which is the intended exit.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return Common::MakeError( "the device is lost; no frame can be recorded. See the [DeviceLost] "
                                      "line above for the cause." );
        }
        // The frame state is one FrameSlotState per slot (RecordingSlot); a swapchain of more images than slots
        // is refused by name before any slot index past the array is formed.
        if ( const uint32_t inFlight = Engine::FrameManager::GetInstance().GetMaxFramesInFlight();
             inFlight > k_MaxFrameSlots )
        {
            return Common::MakeFormattedError<bool>(
                 "{} frames in flight; the renderer keeps frame state (FrameSlotState) for at most {}", inFlight,
                 k_MaxFrameSlots );
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
                RecordingSlot().CommandBuffer = nullptr;
                return Common::MakeFormattedError<bool>( "this frame's slot (frame N-k): {}", waited.GetError() );
            }
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::BeginFrameRecording()
    {
        // THE RENDER TASK'S HALF (OVERLAP-ON-1). Application::Run enqueues it behind the previous frame's submit,
        // present, so the render graph's single-slot frame objects and the GPU profiler below are begun in frame
        // order on the thread that records them, while the game thread ticks this frame. The loss gate again: the
        // half runs later than the game thread's wait.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return Common::MakeError( "the device is lost; no frame can be recorded. See the [DeviceLost] "
                                      "line above for the cause." );
        }
        // THE FRAME BOUNDARY'S FENCE (RenderTask::FrameEndSync, UE FFrameEndSync), moved here from the top of
        // Renderer::BeginFrame (FRAME-SLOT-1). The frame's own state - its submission list, its recording command
        // buffer, its back-buffer entry - is its slot's (FrameSlotState) and a job records into the slot it was
        // enqueued for, so the slot's GPU wait above does not need the previous frame's job finished. Everything
        // below does: the deletion drain, the draw counters and the geometry pool, the virtual-geometry table,
        // the render graph's frame objects (frame loop queue objects, transient heaps, pass descriptors, backend)
        // and the GPU profiler, which bind ONE slot at a time.
        ::Desert::Engine::RenderTask::Get().FrameEndSync();
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
        //
        // THE FRAME HEAD'S STAGES, EACH A ROW (THREAD-OVERLAP-1): "Renderer: BeginFrame" measured 0.78 ms of self
        // time on the field turn with "Wait for GPU" at 0.005 ms, all of it on the game thread before the render
        // task has a job; each stage below is a row of its own so the profile names where it goes.
        {
            DESERT_PROFILE_SCOPE( "Vulkan: draw counter + geometry pool" );
            DrawCounter::Roll();
            // The geometry pool's bind counters roll with the draw counter, and its released spans older than the
            // frames in flight return to the free lists before this frame uploads anything (GeometryPool).
            GeometryPool::Get().BeginFrame();
        }
        // The virtual-geometry cluster tables follow the pool's frame rule: released rows retire, rows registered
        // since the last frame (StaticMesh::Invalidate) go up before any pass reads them.
        {
            DESERT_PROFILE_SCOPE( "Vulkan: virtual-geometry table upload" );
            if ( const Common::BoolResultStr uploaded = VirtualGeometry::VirtualMeshTable::Get().RT_Upload();
                 !uploaded )
            {
                LOG_ERROR( "{}; virtual-geometry meshes registered since the last frame are not on the device",
                           uploaded.GetError() );
            }
        }

        // Before any view begins: a vendor runtime that refused a method since the catalog offered it withdraws
        // it here, and Resolve lowers it (VulkanPhysicalDevice::RefreshRuntimeOffers) - the frame's views read a
        // resolution that no longer names a method that cannot run.
        {
            DESERT_PROFILE_SCOPE( "Vulkan: refresh runtime offers" );
            SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                 ->GetPhysicalDevice()
                 ->RefreshRuntimeOffers();
        }

        // ONE PATH, WINDOW OR NOT. The slot's fence was waited at the top ("Wait for GPU"); BeginRdgFrame began
        // its objects; the frame records into a graphics command buffer of the slot, already begun. A window is
        // only an output of PresentFinalImage's submission (Desert/Tests/Engine/EngineHost runs this same frame
        // with none). vkBeginCommandBuffer cannot report VK_ERROR_DEVICE_LOST (only out-of-memory), so its
        // failure is the refusal below and nothing more.
        RecordingSlot().Submissions.clear();
        const Common::BoolResultStr rdg = [this]
        {
            DESERT_PROFILE_SCOPE( "Vulkan: begin RDG frame" );
            return BeginRdgFrame();
        }();
        if ( !rdg )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return Common::MakeFormattedError<bool>( "render graph frame: {}", rdg.GetError() );
        }
        const Common::ResultStr<VkCommandBuffer> frame = [this]
        {
            DESERT_PROFILE_SCOPE( "Vulkan: frame command buffer" );
            return m_FrameLoop->GetQueueObjects().BeginCommandBuffer( RDG::Pipe::Graphics );
        }();
        if ( !frame )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return Common::MakeFormattedError<bool>( "the frame's command buffer: {}", frame.GetError() );
        }
        RecordingSlot().CommandBuffer = frame.GetValue();
        ForgetBoundState(); // a freshly begun buffer holds no binding

        // Resolve the previous results and reset this frame's queries. Must be here: vkCmdResetQueryPool
        // is illegal inside a render pass, and this is the one point in the frame where the command buffer
        // is recording and no pass is open.
#if DESERT_DEV_INSTRUMENTS
        {
            DESERT_PROFILE_SCOPE( "GPU profiler: resolve + reset" );
            m_GpuProfiler.BeginFrame();
        }
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
            RecordingSlot().CommandBuffer = nullptr;
            return Common::MakeError( "the device is lost; the frame is abandoned rather than closed." );
        }

        if ( IsRecording() )
        {
#if DESERT_DEV_INSTRUMENTS
            m_GpuProfiler.EndFrame();
#endif

            VK_CHECK_RESULT_BOOL( vkEndCommandBuffer( RecordingSlot().CommandBuffer ) );
            // The frame's graphics tail: the last submission, which signals RenderComplete and the fence.
            RecordingSlot().Submissions.push_back(
                 { RDG::Pipe::Graphics, m_RdgQueues.GraphicsQueue, { RecordingSlot().CommandBuffer }, {}, {},
                   {} } );
            RecordingSlot().CommandBuffer = nullptr;
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
            frameOutput->FirstEntryAfterAcquire = RecordingSlot().BackBufferFirstEntry;
        }
        {
            // THE HAND-OFF (VK-SUBMIT-THREAD): the frame's submissions, its output and its held image go to the
            // frame loop's submit thread, which submits and presents them; this thread takes the next frame.
            DESERT_PROFILE_SCOPE( "Vulkan: frame hand-off" );
            const VulkanFrameOutput* const presentTo = frameOutput ? &*frameOutput : nullptr;
            const Common::BoolResultStr    submitted =
                 m_FrameLoop->Submit( slot, RecordingSlot().Submissions, presentTo, output );
            RecordingSlot().Submissions.clear();
            RecordingSlot().BackBufferFirstEntry = 0;
            if ( !submitted )
                return Common::MakeError( submitted.GetError() );
        }

        // Without an output the frame is synchronous: THIS slot's fence is waited (once the submit thread has
        // submitted it), so the caller may read back what the frame wrote (EngineHost) -- the slot's fence, not
        // the device. With one, the submit thread presents (a frame holding no image presents nothing).
        if ( output == nullptr )
        {
            if ( const Common::BoolResultStr waited = m_FrameLoop->WaitSlot( slot ); !waited )
                return Common::MakeFormattedError<bool>( "the frame's slot: {}", waited.GetError() );
        }

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
        // The counter itself moves on the game thread (Renderer::PresentFinalImage inline; Application::Run when
        // the present is the render task's job, OVERLAP-ON-1). The bindless frees are judged against the count
        // after this frame, as before; this thread counts the frame's own (FrameManager::ScopedRecordingFrame).
        if ( BindlessTextureTable* bindless = TryGetBindlessTextureTable() )
        {
            const uint64_t afterThisFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() + 1;
            bindless->EndFrame( afterThisFrame, Engine::FrameManager::GetInstance().GetMaxFramesInFlight() );
        }
        return BOOLSUCCESS;
    }

    bool VulkanRendererAPI::IsRecording()
    {
        // RDG-PARALLEL-1: a graph chunk on a task worker records into its own context; a loss there stops that
        // chunk's recording only (the frame's target is the dispatching thread's to disarm).
        if ( VulkanRecordContext* const chunk = VulkanRecordContext::Bound(); chunk != nullptr )
        {
            if ( chunk->CommandBuffer != VK_NULL_HANDLE && !Graphic::DeviceLost::AllowWork() )
                chunk->CommandBuffer = VK_NULL_HANDLE;
            return chunk->CommandBuffer != VK_NULL_HANDLE;
        }
        // A game-thread record while the views record on the render task joins them first (RenderTask).
        ::Desert::Engine::RenderTask::RecordGuard( "a graphics API recording entry point" );
        if ( RecordingSlot().CommandBuffer != VK_NULL_HANDLE && !Graphic::DeviceLost::AllowWork() )
            RecordingSlot().CommandBuffer = nullptr;
        return RecordingSlot().CommandBuffer != nullptr;
    }

    VulkanRendererAPI::FrameSlotState& VulkanRendererAPI::RecordingSlot()
    {
        // The slot of the frame THIS thread records (FrameManager::GetCurrentFrameIndex: a job's own slot on the
        // render task, the game thread's slot elsewhere). BeginFrame refuses more frames in flight than
        // k_MaxFrameSlots, and the slot index moves only after a frame has begun (NextFrame).
        return m_FrameSlots[Engine::FrameManager::GetInstance().GetCurrentFrameIndex()];
    }

    const VulkanRendererAPI::FrameSlotState& VulkanRendererAPI::RecordingSlot() const
    {
        return m_FrameSlots[Engine::FrameManager::GetInstance().GetCurrentFrameIndex()];
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
        VKUtils::BeginDebugLabel( RecordingTarget(),
                                  renderPass->GetSpecification().DebugName.c_str() );

        m_OpenRenderPass =
             CompatibleRenderPassKeyOf( framebuffer->GetSpecification(), framebuffer->GetSpecification().Samples );
        vkCmdBeginRenderPass( RecordingTarget(), &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE );
        SetViewportAndScissor( framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::EndRenderPass()
    {
        if ( IsRecording() )
        {
            vkCmdEndRenderPass( RecordingTarget() );
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
            vkCmdPipelineBarrier( RecordingTarget(),
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0,
                                  1, &memBarrier,
                                  0, nullptr,
                                  0, nullptr );

            // Closes the region opened by BeginRenderPass. It opens exactly
            // one, so this stays balanced — an unmatched Begin corrupts the capture's tree.
            VKUtils::EndDebugLabel( RecordingTarget() );
        }
        return BOOLSUCCESS;
    }

    void VulkanRendererAPI::BeginDebugLabel( const char* name )
    {
        if ( IsRecording() )
            VKUtils::BeginDebugLabel( RecordingTarget(), name );
    }

    void VulkanRendererAPI::EndDebugLabel()
    {
        if ( IsRecording() )
            VKUtils::EndDebugLabel( RecordingTarget() );
    }

    bool VulkanRendererAPI::BindGraphicsPipeline( const GraphicsPipeline*    pipeline,
                                                  const VulkanBuiltPipeline* record )
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
            if ( WarnOnce( std::format( "{}#outside-pass", pipeline->GetSpecification().DebugName ) ) )
                LOG_ERROR( "[Renderer] pipeline '{}' bound outside a render pass; not drawn",
                           pipeline->GetSpecification().DebugName );
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-static-cast-downcast):
        // only the Vulkan API makes pipelines; binding builds the per-pass variant into the pipeline's own cache
        auto* vulkanPipeline = const_cast<VulkanPipeline*>( static_cast<const VulkanPipeline*>( pipeline ) );
        // ONE RECORD: the caller's (its layout and push range are then the bound pipeline's), else the current.
        const VulkanBuiltPipeline& built = record != nullptr ? *record : vulkanPipeline->GetBuilt();
        if ( VkPipeline bound = vulkanPipeline->GetVkPipelineFor( built, *openPass ); bound != VK_NULL_HANDLE )
        {
            // CPU-FRAME-6: recorded only when the command buffer holds another pipeline (m_BoundGraphics).
            Recording().BoundGraphics.On( RecordingTarget() );
            if ( Recording().BoundGraphics.Pipeline != bound )
            {
                vkCmdBindPipeline( RecordingTarget(), VK_PIPELINE_BIND_POINT_GRAPHICS, bound );
                Recording().BoundGraphics.Pipeline = bound;
            }
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
        if ( WarnOnce( name ) )
        {
            LOG_ERROR( "[Renderer] pipeline '{}' was never built (its shader has no compiled stages) — "
                       "everything drawn through it is skipped until the shader compiles.",
                       name.empty() ? "<unnamed>" : name );
        }
        return false;
    }

    VkBuffer VulkanRendererAPI::DefaultVertexStreams( const uint32_t capacity )
    {
        const std::lock_guard<std::mutex> grow( m_GrowLock );
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        return static_cast<const VulkanVertexBuffer*>( m_DefaultVertexStreams.get() )->GetVulkanBuffer();
    }

    VkBuffer VulkanRendererAPI::VisibleRowIdentity( const uint32_t rows )
    {
        // The draw's path (see the declaration): no lock, no reference count, while the capacity holds.
        if ( rows <= m_VisibleRowIdentityRows.load( std::memory_order_acquire ) )
            return m_VisibleRowIdentityBuffer.load( std::memory_order_relaxed );
        const std::lock_guard<std::mutex> grow( m_GrowLock );
        const uint32_t                    held = m_VisibleRowIdentityRows.load( std::memory_order_relaxed );
        if ( m_VisibleRowIdentity == nullptr || rows > held )
        {
            // Doubled past the request, so a scene that grows by one draw does not reallocate every frame.
            const uint32_t        capacity = std::max( { rows, held * 2u, 1024u } );
            std::vector<uint32_t> identity( capacity );
            for ( uint32_t row = 0; row < capacity; ++row )
                identity[row] = row;
            const ResourceAttributionScope owned( ResourceOwner::Device );
            m_VisibleRowIdentity = VertexBuffer::Create(
                 identity.data(), static_cast<uint32_t>( identity.size() * sizeof( uint32_t ) ) );
            const auto uploaded = m_VisibleRowIdentity->RT_Invalidate();
            DESERT_VERIFY( uploaded.IsSuccess(), "the instance id identity buffer could not be uploaded" );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
            const VkBuffer handle =
                 static_cast<const VulkanVertexBuffer*>( m_VisibleRowIdentity.get() )->GetVulkanBuffer();
            // The handle first, then the rows that publish it (VisibleRowIdentity's lock-free read).
            m_VisibleRowIdentityBuffer.store( handle, std::memory_order_relaxed );
            m_VisibleRowIdentityRows.store( capacity, std::memory_order_release );
        }
        return m_VisibleRowIdentityBuffer.load( std::memory_order_relaxed );
    }

    void VulkanRendererAPI::SubmitPulled( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                          const MaterialExecutor* materialExecutor )
    {
        if ( !IsRecording() || vertexCount == 0 )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        // One read of the published build: the bind and the descriptor sets below use the same layout.
        const VulkanBuiltPipeline& built = vulkanPipeline->GetBuilt();
        if ( !BindGraphicsPipeline( pipeline, &built ) )
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
            // The material binds its own sets past the renderer's record of what is bound (m_BoundGraphics).
            Recording().BoundGraphics.ForgetSets();
            if ( !vkBackend->BindDescriptorSets( RecordingTarget(), built.PipelineLayout,
                                                 VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex ) )
                return;
        }

        // The graphics pipeline enables VK_DYNAMIC_STATE_LINE_WIDTH, so it must be set before drawing.
        // Widths > 1 require the wideLines device feature — MoltenVK lacks it, so force 1.0 there (a
        // wider value is a validation error and gets dropped anyway).
        const float safeWidth = Graphic::RenderConfig::WideLines ? std::clamp( lineWidth, 1.0f, 10.0f ) : 1.0f;
        vkCmdSetLineWidth( RecordingTarget(), safeWidth );

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
        ReportPushStagesApart( const VulkanPipeline& pipeline, const VulkanBuiltPipeline& built,
                               std::string_view                                                       shaderName,
                               const std::optional<ShaderResources::ShaderLayout::PushConstantRange>& reflected )
        {
            const auto&              range        = built.PushConstantRange;
            const VkShaderStageFlags layoutStages = range.has_value() ? range->stageFlags : 0;
            const VkShaderStageFlags shaderStages =
                 reflected.has_value() ? static_cast<VkShaderStageFlags>( reflected->ShaderStage ) : 0;
            if ( layoutStages == shaderStages )
                return;
            static std::mutex                           mutex;
            static std::unordered_set<VkPipelineLayout> reported;
            const std::scoped_lock                      lock( mutex );
            if ( !reported.insert( built.PipelineLayout ).second )
                return;
            LOG_ERROR( "Pipeline '{}': its layout's push stages 0x{:x} are not shader '{}''s current 0x{:x}; the "
                       "pipeline was built from an earlier compile of the shader and was not rebuilt",
                       pipeline.GetSpecification().DebugName, layoutStages, shaderName, shaderStages );
        }
        // RDG-PARALLEL-9 (RDG-REC-2): by the layout's table, not a walk of its bindings.
        const VkDescriptorSetLayoutBinding* FindLayoutBinding( const VulkanDescriptorSetLayout& layout,
                                                               uint32_t                         binding )
        {
            return layout.FindBinding( binding );
        }

        // A non-dispatchable handle as a key word: its value is compared, never turned back into a handle.
        template <typename Handle>
        uint64_t HandleWord( Handle handle )
        {
            if constexpr ( std::is_pointer_v<Handle> )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): compared, never dereferenced
                return static_cast<uint64_t>( reinterpret_cast<std::uintptr_t>( handle ) );
            }
            else
            {
                return static_cast<uint64_t>( handle );
            }
        }

        // RDG-PARALLEL-7 (RDG-REC-3b): one set of the pipeline's layout in the exec being written.
        struct RdgSetPlan
        {
            std::vector<uint64_t>               Key;           // VulkanRdgPassDescriptors::AcquireCached's
            VulkanRdgPassDescriptors::CachedSet Cached;        // Set is bound; Hit: nothing is written into it
            bool                                Fresh = false; // a cache miss, published after its writes
        };
        // Per thread (chunk workers write their own execs), reused so that a steady frame allocates nothing here.
        struct RdgSetScratch
        {
            std::vector<RdgSetPlan>       Sets;
            std::vector<VkDescriptorType> Types;    // per entry: its layout binding's descriptor type
            std::vector<VkSampler>        Samplers; // per entry: a Sampler entry's shared sampler
        };
        thread_local RdgSetScratch t_SetScratch;

        // The material's written slots (@p material) copied from the material's own set into @p set, set 0 of the
        // sets WriteRdgPassSets writes for one exec (a set that is not a cache hit, before it is published).
        // RDG-PARALLEL-9: the copies are built in a per-thread scratch, so a steady miss allocates nothing here.
        Common::BoolResultStr CopyMaterialSlots( VkDescriptorSet set, const VulkanDescriptorSetLayout& layout,
                                                 const VulkanMaterialBackend::WrittenSlots& material,
                                                 std::string_view                           pass )
        {
            thread_local std::vector<VkCopyDescriptorSet> t_Copies;
            std::vector<VkCopyDescriptorSet>&             copies = t_Copies;
            copies.clear();
            for ( const uint32_t binding : material.Bindings )
            {
                const VkDescriptorSetLayoutBinding* entry = FindLayoutBinding( layout, binding );
                if ( entry == nullptr )
                    return Common::MakeFormattedError( "{}: material binding {} is not in the pipeline's set 0",
                                                       pass, binding );
                VkCopyDescriptorSet copy{};
                copy.sType           = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
                copy.srcSet          = material.Set;
                copy.srcBinding      = binding;
                copy.dstSet          = set;
                copy.dstBinding      = binding;
                copy.descriptorCount = entry->descriptorCount;
                copies.push_back( copy );
            }
            if ( copies.empty() )
                return Common::MakeSuccess( true );
            VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                   ->GetVulkanLogicalDevice();
            vkUpdateDescriptorSets( device, 0, nullptr, static_cast<uint32_t>( copies.size() ), copies.data() );
            return Common::MakeSuccess( true );
        }

        // RDG-PARALLEL-9: the first word of a set-0 key's material part (WriteRdgPassSets). Every entry's part
        // starts with (binding << 32) | element, which is never all ones, so a key of entries alone never equals
        // a key with a material.
        constexpr uint64_t kMaterialKeyMarker = ~uint64_t{ 0 };

        // One descriptor set per set of @p layouts with every resolved PassBindings entry written at its slot.
        // RDG-PARALLEL-7 (RDG-REC-3b; UE FVulkanDescriptorSetCache): a set that holds only the pass's entries
        // comes from the content cache (VulkanRdgPassDescriptors::AcquireCached), keyed by the identities of what
        // the entries name; an equal set an earlier exec wrote is bound again with nothing allocated or written,
        // a miss is written here and published once its batch flushed. @p otherRouteFillsSetZero: the caller
        // writes the other route's slots into set 0 after this returns (pipeline setters), so that set is
        // allocated for THIS exec from the graph's per-frame pools, as before. @p material (null for compute and
        // trace): RDG-PARALLEL-9 (UE: a material's descriptor set is cached by what it holds) - the material's
        // written slots are copied into set 0 here, and set 0 is cached like any other, its key carrying the
        // material set and its copy record's identity and version; an equal key binds the set the first exec
        // wrote and copied, with nothing allocated, written or copied.
        Common::ResultStr<std::vector<VkDescriptorSet>>
        WriteRdgPassSets( VulkanRdgPassDescriptors&                  descriptors,
                          const std::vector<DescriptorSetLayoutRef>& layouts,
                          std::span<const RdgResolvedEntry> entries, std::string_view pass,
                          bool otherRouteFillsSetZero, const VulkanMaterialBackend::WrittenSlots* material )
        {
            using Sets = std::vector<VkDescriptorSet>;
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
            }
            RdgSetScratch& scratch = t_SetScratch;
            scratch.Sets.resize( layouts.size() );
            for ( size_t index = 0; index < layouts.size(); ++index )
            {
                RdgSetPlan& plan = scratch.Sets[index];
                plan.Key.clear();
                plan.Key.push_back( HandleWord( layouts[index]->Handle() ) );
                plan.Cached = VulkanRdgPassDescriptors::CachedSet{};
                plan.Fresh  = false;
            }
            scratch.Types.assign( entries.size(), VK_DESCRIPTOR_TYPE_MAX_ENUM );
            scratch.Samplers.assign( entries.size(), VK_NULL_HANDLE );
            const auto nameOf = []( const RdgResolvedEntry& entry )
            {
                if ( entry.Texture != nullptr )
                    return std::string_view( entry.Texture->ShaderName );
                return std::string_view( entry.Buffer->ShaderName );
            };
            const auto layoutOf = []( const RdgResolvedEntry& entry )
            {
                if ( entry.Kind == RDG::ShaderResourceKind::StorageTexture )
                    return VK_IMAGE_LAYOUT_GENERAL;
                return RdgVulkanLayout( RDG::GetAccessState( entry.Texture->Declared ).Layout );
            };

            // 1. Every entry is checked against the layout and appends what it names to its set's key: its slot,
            //    kind and type, then the identity of the resource (never a view or buffer handle) and its range,
            //    layout or sampler. Nothing is allocated yet.
            for ( size_t at = 0; at < entries.size(); ++at )
            {
                const RdgResolvedEntry& entry = entries[at];
                const std::string_view  name  = nameOf( entry );
                if ( entry.Slot.Set >= layouts.size() )
                    return Common::MakeFormattedError<Sets>( "{}: '{}' is in set {}, the pipeline has {} sets",
                                                             pass, name, entry.Slot.Set, layouts.size() );
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
                scratch.Types[at]          = binding->descriptorType;
                std::vector<uint64_t>& key = scratch.Sets[entry.Slot.Set].Key;
                key.push_back( ( static_cast<uint64_t>( entry.Slot.Binding ) << 32U ) | entry.Slot.Element );
                key.push_back( ( static_cast<uint64_t>( entry.Kind ) << 32U ) |
                               static_cast<uint64_t>( binding->descriptorType ) );
                if ( entry.Kind == RDG::ShaderResourceKind::Sampler )
                {
                    // A shared sampler state: no image, written as VK_DESCRIPTOR_TYPE_SAMPLER. The sampler cache
                    // owns it for the device's life, so its handle is its identity.
                    if ( binding->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER || !entry.Texture->Sampler )
                        return Common::MakeFormattedError<Sets>(
                             "{}: '{}' is bound as a sampler into a slot of descriptor type {}", pass, name,
                             static_cast<int>( binding->descriptorType ) );
                    const Common::ResultStr<VkSampler> made = descriptors.GetSampler( *entry.Texture->Sampler );
                    if ( !made )
                        return Common::MakeFormattedError<Sets>( "{}: '{}': {}", pass, name, made.GetError() );
                    scratch.Samplers[at] = made.GetValue();
                    key.push_back( HandleWord( made.GetValue() ) );
                }
                else if ( entry.Texture != nullptr )
                {
                    const RDG::BoundTexture& texture = *entry.Texture;
                    if ( texture.Sampler )
                        return Common::MakeFormattedError<Sets>(
                             "{}: '{}' carries a sampler but its slot has none (descriptor type {}); a separate "
                             "texture is read through a Sampler slot of the pass",
                             pass, name, static_cast<int>( binding->descriptorType ) );
                    const Common::ResultStr<VulkanRdgTexture*> physical =
                         VulkanRdgBackend::TextureOf( texture.Texture );
                    if ( !physical )
                        return Common::MakeFormattedError<Sets>( "{}: '{}': {}", pass, name, physical.GetError() );
                    key.push_back( physical.GetValue()->GetIdentity() );
                    key.push_back( ( static_cast<uint64_t>( texture.Range.BaseMip ) << 32U ) |
                                   texture.Range.MipCount );
                    key.push_back( ( static_cast<uint64_t>( texture.Range.BaseLayer ) << 32U ) |
                                   texture.Range.LayerCount );
                    key.push_back( static_cast<uint64_t>( layoutOf( entry ) ) );
                }
                else
                {
                    // A buffer range or an acceleration structure: fixed for the wrapping object's life.
                    const Common::ResultStr<VulkanRdgBuffer*> physical =
                         VulkanRdgBackend::BufferOf( entry.Buffer->Buffer );
                    if ( !physical )
                        return Common::MakeFormattedError<Sets>( "{}: '{}': {}", pass, name, physical.GetError() );
                    key.push_back( physical.GetValue()->GetIdentity() );
                    key.push_back( physical.GetValue()->GetOffset() );
                    key.push_back( physical.GetValue()->GetSize() );
                    key.push_back( HandleWord( physical.GetValue()->GetAccelerationStructure() ) );
                }
            }

            // 1b. The material's part of set 0's key, after every entry's: the material set, then its copy
            //     record's identity and version (DescriptorCopyRecord). While they are unchanged the material set
            //     holds the same descriptors at the same bindings, so the copies of an earlier exec are current.
            const bool copiesMaterial = material != nullptr && !material->Bindings.empty();
            if ( copiesMaterial )
            {
                if ( layouts.empty() )
                    return Common::MakeFormattedError<Sets>(
                         "{}: the material fills set 0, which the pipeline has not", pass );
                if ( isBindless( 0 ) )
                    return Common::MakeFormattedError<Sets>(
                         "{}: the material fills set 0, the bindless texture table's", pass );
                std::vector<uint64_t>& key = scratch.Sets[0].Key;
                key.push_back( kMaterialKeyMarker );
                key.push_back( HandleWord( material->Set ) );
                key.push_back( material->Identity );
                key.push_back( material->Version );
            }

            // 2. The sets: the bindless table's as it is, set 0 of an other-route caller from this frame's pools,
            //    every other one from the cache.
            Sets       sets( layouts.size(), VK_NULL_HANDLE );
            const auto discardFresh = [&descriptors, &scratch]()
            {
                for ( const RdgSetPlan& plan : scratch.Sets )
                {
                    if ( plan.Fresh )
                        descriptors.DiscardCached( plan.Cached );
                }
            };
            for ( size_t index = 0; index < layouts.size(); ++index )
            {
                RdgSetPlan& plan = scratch.Sets[index];
                if ( isBindless( index ) )
                {
                    sets[index]     = bindless->GetDescriptorSet();
                    plan.Cached.Hit = true; // no entry of the pass is written into it (refused above)
                    continue;
                }
                if ( index == 0 && otherRouteFillsSetZero )
                {
                    const Common::ResultStr<VkDescriptorSet> set = descriptors.Allocate( layouts[index]->Handle() );
                    if ( !set )
                    {
                        discardFresh();
                        return Common::MakeFormattedError<Sets>( "{}: set {}: {}", pass, index, set.GetError() );
                    }
                    sets[index] = set.GetValue();
                    continue;
                }
                const Common::ResultStr<VulkanRdgPassDescriptors::CachedSet> cached =
                     descriptors.AcquireCached( layouts[index]->Handle(), plan.Key );
                if ( !cached )
                {
                    discardFresh();
                    return Common::MakeFormattedError<Sets>( "{}: set {}: {}", pass, index, cached.GetError() );
                }
                plan.Cached = cached.GetValue();
                plan.Fresh  = !plan.Cached.Hit;
                sets[index] = plan.Cached.Set;
            }

            // 3. The entries of every set that is not a cache hit. RDG-PARALLEL-6 (RDG-REC-3a): they go out in one
            //    vkUpdateDescriptorSets when the batch closes, before the caller binds them. No caller holds a
            //    batch open around this, so the batch flushes here.
            std::string failure;
            {
                VulkanRdgPassDescriptors::WriteBatch batch( descriptors );
                for ( size_t at = 0; at < entries.size() && failure.empty(); ++at )
                {
                    const RdgResolvedEntry& entry = entries[at];
                    if ( scratch.Sets[entry.Slot.Set].Cached.Hit )
                        continue;
                    const VkDescriptorSet  set     = sets[entry.Slot.Set];
                    const VkDescriptorType type    = scratch.Types[at];
                    Common::BoolResultStr  written = Common::MakeSuccess( true );
                    if ( entry.Kind == RDG::ShaderResourceKind::Sampler )
                    {
                        descriptors.WriteSampler( set, entry.Slot.Binding, scratch.Samplers[at] );
                    }
                    else if ( entry.Texture != nullptr )
                    {
                        const RDG::BoundTexture& texture = *entry.Texture;
                        written = descriptors.WriteTexture( set, entry.Slot.Binding, type, texture.Texture,
                                                            texture.Range, layoutOf( entry ), entry.Slot.Element );
                    }
                    else if ( entry.Kind == RDG::ShaderResourceKind::AccelerationStructure )
                    {
                        written = descriptors.WriteAccelerationStructure( set, entry.Slot.Binding, type,
                                                                          entry.Buffer->Buffer );
                    }
                    else
                    {
                        written = descriptors.WriteBuffer( set, entry.Slot.Binding, type, entry.Buffer->Buffer );
                    }
                    if ( !written.IsSuccess() )
                        failure = std::format( "{}: '{}': {}", pass, nameOf( entry ), written.GetError() );
                }
            }
            if ( !failure.empty() )
            {
                discardFresh();
                return Common::MakeFormattedError<Sets>( "{}", failure );
            }

            // 3b. The material's slots into set 0 when it is not a cache hit: after the batch flushed and before
            //     step 4 publishes the set. The pass's entries never name these bindings (the resolution refused
            //     a slot both routes fill), so the order of the two writes does not matter.
            if ( copiesMaterial && !scratch.Sets[0].Cached.Hit )
            {
                const Common::BoolResultStr copied = CopyMaterialSlots( sets[0], *layouts[0], *material, pass );
                if ( !copied )
                {
                    discardFresh();
                    return Common::MakeFormattedError<Sets>( "{}", copied.GetError() );
                }
            }

            // 4. Findable only now that their descriptors are submitted: another chunk never binds a set that is
            //    still being written (an update after a bind would invalidate its command buffer).
            for ( const RdgSetPlan& plan : scratch.Sets )
            {
                if ( plan.Fresh )
                    descriptors.PublishCached( plan.Key, plan.Cached );
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

        // RDG-PARALLEL-6: the shader's slot table (VulkanShader::GetRdgSlots). A shader that was never reflected
        // has an empty ReflectionData, and the table of an empty reflection is the empty table.
        const RdgSlotTable& SlotsOf( const VulkanShader& shader )
        {
            static const RdgSlotTable kUnreflected;
            const std::shared_ptr<const RdgSlotTable>& slots = shader.GetRdgSlots();
            if ( slots )
                return *slots;
            return kUnreflected;
        }
    } // namespace

    RDG::ShaderBindingLayout VulkanRendererAPI::GetBindingLayout( const Shader& shader ) const
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        const auto& vulkan = static_cast<const VulkanShader&>( shader );
        RDG::ShaderBindingLayout layout =
             MakeShaderBindingLayout( vulkan.GetReflectionData(), SlotsOf( vulkan ), vulkan.GetName() );
        // FPS-ARCH-BINDINGS: the table the layout's slot order is; a reload makes a new table (and the layout
        // caches a new layout by the shader's generation), so a block declared against it resolves by index.
        layout.Backend = vulkan.GetRdgSlots();
        return layout;
    }

    std::shared_ptr<const RDG::OtherRouteFill>
    VulkanRendererAPI::GetPipelineRouteFill( const ComputePipeline& pipeline ) const
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        return static_cast<const VulkanPipelineCompute&>( pipeline ).GetRouteFill();
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
        // THE BUILD READ ONCE: the pipeline, its layout and the set layouts bound below are of one build even
        // if the game thread rebuilds this pipeline meanwhile (the replaced record outlives this frame).
        const VulkanPipelineCompute::BuiltCompute& built = compute.GetBuilt();
        if ( shader == nullptr || built.Pipeline == VK_NULL_HANDLE )
            return Common::MakeFormattedError( "{}: compute pipeline '{}' is not built", pass,
                                               compute.GetSpecification().DebugName );

        // The other route: the pipeline's own setters (asset textures, out-of-graph buffers), set 0.
        // FPS-CPU2-VKRECORD: the thread's scratch (a context records on one thread), so no slot list is allocated.
        thread_local RdgOtherRoute t_Other;
        RdgOtherRoute&             other = t_Other;
        other.Filled.clear();
        for ( const uint32_t binding : compute.GetBoundBindings() )
            other.Filled.push_back( RdgSlotKey{ .Set = 0, .Binding = binding } );
        other.PushConstants = !compute.GetBoundPushConstants().empty();
        const bool setters  = !other.Filled.empty();

        // FPS-CPU2-VKRECORD (GPU-COST-36's memo, as the graphics draws use it): the sets an earlier dispatch of
        // this pass exec wrote from the same block, shader and layout are bound again; only the first resolves
        // and writes. A pipeline with setters is kept out: its set 0 is written for each dispatch
        // (WriteBoundResources).
        VulkanRdgPassSetMemo* memo = nullptr;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the shader's address is the memo's identity
        const VulkanRdgPassSetMemo::Key key{ .Block          = bindings.GetBlock(),
                                             .Shader         = reinterpret_cast<std::uintptr_t>( shader ),
                                             .Layout         = built.Layout,
                                             .MaterialPush   = other.PushConstants,
                                             .BlockPushBytes = bindings.GetPushConstants().size() };
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        std::optional<std::span<const VkDescriptorSet>> kept;
        if ( !setters )
        {
            const Common::ResultStr<VulkanRdgPassSetMemo*> memoOf = VulkanRdgBackend::SetMemoOf( context );
            if ( !memoOf )
                return Common::MakeError( memoOf.GetError() );
            memo = memoOf.GetValue();
            kept = memo->Find( key );
        }
        std::vector<VkDescriptorSet> fresh; // a memo miss's sets (the writer's own vector, moved)
        if ( !kept.has_value() )
        {
            // FPS-ARCH-BINDINGS: the thread's scratch (consumed by WriteRdgPassSets below, before any other
            // resolve on this thread), so a dispatch allocates no entry list.
            thread_local std::vector<RdgResolvedEntry> t_Resolved;
            const Common::BoolResultStr resolved = ResolveRdgPassBindings(
                 shader->GetReflectionData(), SlotsOf( *shader ), shader->GetName(), bindings, other, t_Resolved );
            if ( !resolved.IsSuccess() )
                return Common::MakeError( resolved.GetError() );
            auto written =
                 WriteRdgPassSets( *descriptors.GetValue(), built.Layouts, t_Resolved, pass, setters, nullptr );
            if ( !written )
                return Common::MakeError( written.GetError() );
            fresh = std::move( written.GetValue() );
            if ( setters )
            {
                if ( fresh.empty() )
                    return Common::MakeFormattedError( "{}: the pipeline's setters fill set 0, which it has not",
                                                       pass );
                if ( const Common::BoolResultStr filled = compute.WriteBoundResources( built, fresh[0] );
                     !filled.IsSuccess() )
                    return Common::MakeFormattedError( "{}: {}", pass, filled.GetError() );
                kept = std::span<const VkDescriptorSet>( fresh );
            }
            else
                kept = memo->Keep( key, fresh );
        }
        const std::span<const VkDescriptorSet> sets = *kept;

        // FPS-CPU2-VKRECORD (CPU-FRAME-6's skip, for compute): the pipeline and the sets are recorded only when
        // the command buffer does not hold exactly these, bound with this layout and no foreign bind since.
        VulkanBoundCompute& boundCompute = Recording().BoundCompute;
        const bool          known        = boundCompute.On( cmd.GetValue() );
        if ( !known || boundCompute.Pipeline != built.Pipeline )
        {
            vkCmdBindPipeline( cmd.GetValue(), VK_PIPELINE_BIND_POINT_COMPUTE, built.Pipeline );
            boundCompute.Pipeline = built.Pipeline;
        }
        if ( !sets.empty() &&
             ( boundCompute.SetsLayout != built.Layout || !std::ranges::equal( boundCompute.Sets, sets ) ) )
        {
            vkCmdBindDescriptorSets( cmd.GetValue(), VK_PIPELINE_BIND_POINT_COMPUTE, built.Layout, 0,
                                     static_cast<uint32_t>( sets.size() ), sets.data(), 0, nullptr );
            boundCompute.SetsLayout = built.Layout;
            boundCompute.Sets.assign( sets.begin(), sets.end() );
        }
        const std::span<const std::byte> push = ChoosePushConstants( bindings, compute.GetBoundPushConstants() );
        if ( !push.empty() )
            vkCmdPushConstants( cmd.GetValue(), built.Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
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

        // ONE RECORD PER DISPATCH: `traced` is built whole by RayTracingPipeline::Create and never rebuilt in
        // place, so its pipeline, layout, set layouts, push stages and SBT handles below are one snapshot. The
        // caller holds its own reference for this exec (the pass captured it); a republish retires the old one
        // past the frames in flight (RayTracingPipeline::Retire). Only the per-slot SBT buffers are written,
        // under the pipeline's m_SlotsMutex.
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
        thread_local std::vector<RdgResolvedEntry> t_Resolved; // FPS-ARCH-BINDINGS: as DispatchCompute's
        const Common::BoolResultStr resolved =
             ResolveRdgPassBindings( program->GetReflectionData(), SlotsOf( *program ), program->GetName(), bindings,
                                     RdgOtherRoute{}, t_Resolved );
        if ( !resolved.IsSuccess() )
            return Common::MakeError( resolved.GetError() );
        const auto sets =
             WriteRdgPassSets( *descriptors.GetValue(), traced.GetLayouts(), t_Resolved, pass, false, nullptr );
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
        if ( const Common::BoolResultStr bound =
                  BindGraphicsPassState( bindings, pipeline, material, push, PassStatePush::Record );
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
        if ( const Common::BoolResultStr bound =
                  BindGraphicsPassState( bindings, pipeline, material, push, PassStatePush::Record );
             !bound || !bound.GetValue() )
            return bound;
        vkCmdDrawIndirect( RecordingTarget(), buffer.GetValue()->GetBuffer(), buffer.GetValue()->GetOffset() + offset, 1,
                           static_cast<uint32_t>( sizeof( VkDrawIndirectCommand ) ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr
    VulkanRendererAPI::DrawProceduralIndexed( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                              const MaterialExecutor*          material,
                                              const std::span<const std::byte> push, IndexBuffer& indexBuffer,
                                              const std::span<const ProceduralIndexedRange> ranges )
    {
        const std::string_view pass = bindings.GetContext().GetPassName();
        if ( ranges.empty() )
            return Common::MakeFormattedError( "{}: an indexed procedural draw of no range", pass );
        const uint64_t indices = indexBuffer.GetCount();
        for ( const ProceduralIndexedRange& range : ranges )
        {
            if ( range.IndexCount == 0u || range.InstanceCount == 0u )
                return Common::MakeFormattedError( "{}: an indexed procedural draw of {} indices x {} instances",
                                                   pass, range.IndexCount, range.InstanceCount );
            if ( uint64_t{ range.FirstIndex } + range.IndexCount > indices )
                return Common::MakeFormattedError( "{}: indices [{}, {}) past the end of a {}-index buffer", pass,
                                                   range.FirstIndex,
                                                   uint64_t{ range.FirstIndex } + range.IndexCount, indices );
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        VkBuffer ibuffer = static_cast<API::Vulkan::VulkanIndexBuffer&>( indexBuffer ).GetVulkanBuffer();
        if ( ibuffer == VK_NULL_HANDLE )
            return Common::MakeFormattedError(
                 "{}: the index buffer of an indexed procedural draw is not uploaded", pass );
        if ( const Common::BoolResultStr bound =
                  BindGraphicsPassState( bindings, pipeline, material, push, PassStatePush::Record );
             !bound || !bound.GetValue() )
            return bound;
        vkCmdBindIndexBuffer( RecordingTarget(), ibuffer, 0, VK_INDEX_TYPE_UINT32 );
        Recording().BoundGeometry.reset(); // the index buffer now holds this draw's, not a mesh's
        // No vertex buffer: the vertex stage builds its geometry from gl_VertexIndex / gl_InstanceIndex.
        for ( const ProceduralIndexedRange& range : ranges )
            DrawIndexedCounted( range.IndexCount, range.InstanceCount, range.FirstIndex, range.VertexOffset,
                                range.FirstInstance );
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
        if ( const Common::BoolResultStr bound =
                  BindGraphicsPassState( bindings, pipeline, material, {}, PassStatePush::Record );
             !bound || !bound.GetValue() )
            return bound;
        const VkDeviceSize offsets[] = { 0 };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        VkBuffer vbuffer = static_cast<API::Vulkan::VulkanVertexBuffer&>( vertexBuffer ).GetVulkanBuffer();
        vkCmdBindVertexBuffers( RecordingTarget(), 0, 1, &vbuffer, offsets );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes buffers
        VkBuffer ibuffer = static_cast<API::Vulkan::VulkanIndexBuffer&>( indexBuffer ).GetVulkanBuffer();
        vkCmdBindIndexBuffer( RecordingTarget(), ibuffer, 0, VK_INDEX_TYPE_UINT32 );
        Recording().BoundGeometry.reset(); // bindings 0 and the index buffer now hold this batch's, not a mesh's
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
                sbuffer = DefaultVertexStreams(
                     DefaultVertexStreamsFor( *layout, bytes, m_DefaultVertexStreamsCapacity ).Capacity );
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
        const BoundGeometry key{ RecordingTarget(), &pipeline, vbuffer, geometry.VertexOffset, sbuffer,
                                 soffset,                ibuffer,   geometry.IndexOffset };
        if ( !Recording().BoundGeometry.has_value() || *Recording().BoundGeometry != key )
        {
            const VkDeviceSize voffset = geometry.VertexOffset;
            vkCmdBindVertexBuffers( RecordingTarget(), 0, 1, &vbuffer, &voffset );
            if ( sbuffer != VK_NULL_HANDLE )
                vkCmdBindVertexBuffers( RecordingTarget(), 1, 1, &sbuffer, &soffset );
            if ( ibuffer != VK_NULL_HANDLE )
                vkCmdBindIndexBuffer( RecordingTarget(), ibuffer, geometry.IndexOffset, VK_INDEX_TYPE_UINT32 );
            Recording().BoundGeometry = key;
            GeometryPool::Get().RecordBind( shared );
        }
        // CPU-DRAW-BIND: binding 2 is recorded only when this command buffer does not hold the stream already.
        if ( visibleRows != VK_NULL_HANDLE &&
             !Recording().BoundVisibleRows.Holds( RecordingTarget(), visibleRows ) )
        {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers( RecordingTarget(), GpuDriven::kVisibleRowVertexBinding, 1, &visibleRows,
                                    &zero );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::WalkMeshSubmeshes(
         const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
         const glm::mat4& transform, const MaterialExecutor& material, const std::span<const std::byte> drawPush,
         const SubmeshMask hiddenSubmeshMask, const VkBuffer visibleRows, const SubmeshRunDraw& draw,
         MeshWalkBound* const bound )
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

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        const auto* graphics = static_cast<const VulkanPipeline*>( &pipeline );
        const auto& submeshes = mesh.GetSubmeshes();
        // CPU-FRAME-6: the loop below pushes the layout's whole range (same stages, offset and size as the bind's
        // push, which is never larger) before each submesh's draw, so when one is drawn at all the bind's push of
        // the draw's block is dead: overwritten before any draw reads it. Without a visible submesh nothing is
        // drawn and the bind pushes as before.
        // One read of the published build for the bind, the sets and every submesh's push (deferred deletion
        // keeps it live for the frames in flight even when a rebuild publishes a newer one meanwhile).
        const VulkanBuiltPipeline& built         = graphics->GetBuilt();
        const auto&                pushConstant  = built.PushConstantRange;
        bool                       drawsAnything = false;
        for ( size_t si = 0; si < submeshes.size() && !drawsAnything; ++si )
            drawsAnything = !hiddenSubmeshMask.Hides( si );
        const PassStatePush bindPush =
             drawsAnything && pushConstant.has_value() && pushConstant->size >= sizeof( glm::mat4 )
                  ? PassStatePush::CallerCoversRange
                  : PassStatePush::Record;
        // FPS-CPU2-VKRECORD: a batch's walk whose pass state the walk before it bound (MeshWalkBound) binds none
        // of it again - the same block, shader, layout and material are a memo hit, the pipeline and sets are
        // bound already, and CallerCoversRange records no push. Anything else binds and remembers what it bound.
        const bool passStateBound = bound != nullptr && bindPush == PassStatePush::CallerCoversRange &&
                                    bound->Pipeline == &pipeline && bound->Built == &built &&
                                    bound->Material == &material && bound->PushBytes == drawPush.size();
        if ( !passStateBound )
        {
            if ( bound != nullptr )
                *bound = {};
            if ( const Common::BoolResultStr state =
                      BindGraphicsPassState( bindings, pipeline, &material, drawPush, bindPush, &built );
                 !state || !state.GetValue() )
                return state;
            if ( bound != nullptr && bindPush == PassStatePush::CallerCoversRange )
                *bound = MeshWalkBound{
                     .Pipeline = &pipeline, .Built = &built, .Material = &material, .PushBytes = drawPush.size() };
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes shaders
        auto* shader = static_cast<VulkanShader*>( pipeline.GetSpecification().Shader.get() );

        // The per-mesh bind: the geometry at this mesh's own offsets, so every submesh range stays mesh-relative.
        // A batch's walk of the geometry the walk before it bound for this pass state (no id stream) skips it.
        const bool buffersBound = passStateBound && visibleRows == VK_NULL_HANDLE && bound->Geometry == &geometry;
        if ( !buffersBound )
        {
            if ( const Common::BoolResultStr buffers =
                      BindMeshBuffers( pass, pipeline, geometry.MeshBinding(), visibleRows, false );
                 !buffers )
            {
                if ( bound != nullptr )
                    *bound = {};
                return buffers;
            }
            if ( bound != nullptr && bound->Pipeline != nullptr )
                bound->Geometry = visibleRows == VK_NULL_HANDLE ? &geometry : nullptr;
        }
        const uint32_t indexCount = geometry.Indices != nullptr ? geometry.IndexCount : 0u;

        // The push block of every submesh: the draw's (@p drawPush - the material's block with the draw's own
        // values over it, MeshDrawList::Record; the pipeline layout's full range, zero past what it holds) with
        // the submesh's transform over the first mat4. Stages and size are the LAYOUT's
        // (VulkanPipeline::GetPushConstantRange), never the shader's current reflection, which a recompile can
        // move past the layout this pipeline holds (pushConstant, above). The layout-vs-reflection stage check ran
        // in BindGraphicsPassState, against the same reflection (CPU-DRAW-BIND: once per draw, not twice).
        // CPU-FRAME-5: the renderer's scratch (recording is one thread's), so a draw allocates no push block.
        std::vector<std::byte>& push = Recording().SubmeshPush;
        push.clear();
        if ( pushConstant.has_value() && pushConstant->size > 0 )
        {
            if ( pushConstant->size < sizeof( glm::mat4 ) )
                return Common::MakeFormattedError(
                     "{}: shader '{}' push block of {} bytes cannot hold the transform", pass, shader->GetName(),
                     pushConstant->size );
            // CPU-DRAW-BIND: each byte written once - the draw's block, then zeros past it to the range's end.
            const size_t copied = std::min<size_t>( pushConstant->size, drawPush.size() );
            push.assign( drawPush.begin(), drawPush.begin() + static_cast<std::ptrdiff_t>( copied ) );
            push.resize( pushConstant->size, std::byte{ 0 } );
        }

        // The hidden-submesh mask and the push block of each run (SubmeshRun); @p draw records the run.
        // RENDER-JOB-CPU-1: the submeshes after a drawn one join its run while they are drawn too and carry the
        // same submesh transform - the same push bytes (an equal product of equal factors), which the command
        // buffer still holds after the run's earlier draws, as nothing records between them.
        uint32_t drawn = 0;
        for ( size_t si = 0; si < submeshes.size(); )
        {
            if ( hiddenSubmeshMask.Hides( si ) )
            {
                ++si;
                continue;
            }
            size_t end = si + 1;
            while ( end < submeshes.size() && !hiddenSubmeshMask.Hides( end ) &&
                    submeshes[end].Transform == submeshes[si].Transform )
                ++end;
            const glm::mat4 finalTransform = transform * submeshes[si].Transform;
            if ( pushConstant.has_value() && pushConstant->size > 0 )
            {
                std::memcpy( push.data(), &finalTransform, sizeof( glm::mat4 ) );
                vkCmdPushConstants( RecordingTarget(), built.PipelineLayout,
                                    pushConstant->stageFlags, pushConstant->offset, pushConstant->size,
                                    push.data() );
            }
            const SubmeshRun run{ .FirstDrawn = drawn,
                                  .First      = si,
                                  .Count      = static_cast<uint32_t>( end - si ),
                                  .IndexCount = indexCount };
            const Common::BoolResultStr recorded = draw( run );
            if ( !recorded )
            {
                if ( bound != nullptr )
                    *bound = {};
                return recorded;
            }
            // FPS-CPU2-VKRECORD: the draw is complete (a compacted bin's one call) - the runs after it would
            // only push blocks no draw of this walk reads (the next draw pushes its own before drawing).
            if ( !recorded.GetValue() )
                break;
            drawn += run.Count;
            si = end;
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
        const auto& submeshes = mesh.GetSubmeshes();
        // A run's submeshes are one CPU draw each (each its own index range), after the run's one push.
        return WalkMeshSubmeshes(
             bindings, pipeline, mesh, transform, material, push, hiddenSubmeshMask, VK_NULL_HANDLE,
             [&]( const SubmeshRun& run ) -> Common::BoolResultStr
             {
                 const uint32_t indexCount = run.IndexCount;
                 for ( size_t si = run.First; si < run.First + run.Count; ++si )
                 {
                     const Submesh& submesh = submeshes[si];
                     if ( indexCount == 0u )
                     {
                         DrawCounted( submesh.VertexCount, instanceCount, submesh.VertexOffset, firstInstance );
                         continue;
                     }
                     const SubmeshIndexRange range = SubmeshIndices( submesh, lodLevel );
                     if ( range.First + range.Count > indexCount )
                         return Common::MakeFormattedError(
                              "{}: submesh {} reads indices [{}, {}) of a {}-index mesh", pass, si, range.First,
                              range.First + range.Count, indexCount );
                     DrawIndexedCounted( range.Count, instanceCount, range.First,
                                         static_cast<int32_t>( submesh.VertexOffset ), firstInstance );
                 }
                 return Common::MakeSuccess( true );
             } );
    }

    Common::BoolResultStr VulkanRendererAPI::RenderMeshIndirect(
         const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
         const glm::mat4& transform, const MaterialExecutor& material, const std::span<const std::byte> push,
         const RDG::BufferRef args, const uint64_t offset, const SubmeshMask hiddenSubmeshMask,
         const MeshDrawArgumentsOrder order, const uint32_t groups )
    {
        DESERT_PROFILE_FUNC();
        const RDG::PassContext& context = bindings.GetContext();
        if ( offset % 4u != 0u )
            return Common::MakeFormattedError( "{}: indirect arguments at byte {}, not a multiple of four",
                                               context.GetPassName(), offset );
        if ( groups == 0u || ( groups > 1u && order != MeshDrawArgumentsOrder::Submesh ) )
            return Common::MakeFormattedError(
                 "{}: {} indirect record groups (one group per draw, several only in submesh order)",
                 context.GetPassName(), groups );
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
        // RENDER-CPU-COST-1: a submesh's record of every group is one multi-draw at a stride of one group (the
        // mesh's submesh count of records), split where the device's per-call limit says - as the binned groups
        // are issued (RenderMeshIndirectGroup; one record per call without multiDrawIndirect).
        // RENDER-JOB-CPU-1: of one group, a run's records (SubmeshRun) are adjacent and drawn in the run's order:
        // one multi-draw at a record's stride, split the same way.
        const auto&    capabilities = EngineContext::GetInstance().GetCapabilities();
        const uint32_t perCall      = GpuDriven::SelectIndirectSubmission(
                                     { .MultiDrawIndirect    = capabilities.SupportsMultiDrawIndirect,
                                       .MaxDrawIndirectCount = capabilities.MaxDrawIndirectCount } )
                                     .MaxDrawsPerCall;
        const uint64_t groupBytes = uint64_t{ mesh.GetSubmeshes().size() } * kMeshDrawArgumentsBytes;
        if ( groups > 1u && groupBytes > uint64_t{ UINT32_MAX } )
            return Common::MakeFormattedError( "{}: an indirect record group of {} bytes cannot be a draw stride",
                                               context.GetPassName(), groupBytes );
        // Not counted by DrawCounter: the instance counts are the GPU's (DrawProceduralIndirect likewise).
        // The VSM caster draw instances through its remap (firstInstance = RemapBase), never a binned id stream.
        return WalkMeshSubmeshes(
             bindings, pipeline, mesh, transform, material, push, hiddenSubmeshMask, VK_NULL_HANDLE,
             [&]( const SubmeshRun& run ) -> Common::BoolResultStr
             {
                 const uint64_t record =
                      order == MeshDrawArgumentsOrder::Drawn ? uint64_t{ run.FirstDrawn } : uint64_t{ run.First };
                 const uint64_t at   = offset + ( record * kMeshDrawArgumentsBytes );
                 const uint64_t last = at + ( uint64_t{ run.Count - 1u } * kMeshDrawArgumentsBytes ) +
                                       ( uint64_t{ groups - 1u } * groupBytes );
                 if ( last + kMeshDrawArgumentsBytes > bytes )
                     return Common::MakeFormattedError(
                          "{}: indirect arguments of drawn submeshes [{}, {}) at byte {} ({} groups) past the "
                          "end of '{}' ({} bytes)",
                          context.GetPassName(), run.FirstDrawn, run.FirstDrawn + run.Count, at, groups,
                          declared.GetValue().Name, bytes );
                 // One record: its own size (a group of one submesh is the record). Several: a run's records are
                 // a record apart, a submesh's groups a group's bytes - each at least a record and a multiple of
                 // four (kMeshDrawArgumentsBytes is 20).
                 uint32_t oneRecord = static_cast<uint32_t>( sizeof( VkDrawIndexedIndirectCommand ) );
                 if ( run.IndexCount == 0u )
                     oneRecord = static_cast<uint32_t>( sizeof( VkDrawIndirectCommand ) );
                 const auto issue = [&]( const uint64_t from, const uint32_t count, const uint64_t apart )
                 {
                     // Not in the frame's draw count (its instances are the GPU's), but a CPU draw of its mesh
                     // pass: one per call recorded.
                     DrawCounter::RecordCpuIndirect();
                     uint32_t recordStride = oneRecord;
                     if ( count > 1u )
                         recordStride = static_cast<uint32_t>( apart );
                     if ( run.IndexCount == 0u )
                         vkCmdDrawIndirect( RecordingTarget(), vkArgs, from, count, recordStride );
                     else
                         vkCmdDrawIndexedIndirect( RecordingTarget(), vkArgs, from, count, recordStride );
                 };
                 if ( groups == 1u )
                 {
                     for ( uint32_t first = 0; first < run.Count; first += perCall )
                     {
                         const uint32_t count = std::min( run.Count - first, perCall );
                         const uint64_t from  = at + ( uint64_t{ first } * kMeshDrawArgumentsBytes );
                         issue( from, count, kMeshDrawArgumentsBytes );
                     }
                     return Common::MakeSuccess( true );
                 }
                 // Several groups: each submesh of the run its groups' multi-draw, in the order drawn before.
                 for ( uint32_t k = 0; k < run.Count; ++k )
                 {
                     const uint64_t submeshAt = at + ( uint64_t{ k } * kMeshDrawArgumentsBytes );
                     for ( uint32_t first = 0; first < groups; first += perCall )
                     {
                         const uint32_t count = std::min( groups - first, perCall );
                         issue( submeshAt + ( uint64_t{ first } * groupBytes ), count, groupBytes );
                     }
                 }
                 return Common::MakeSuccess( true );
             } );
    }

    Common::BoolResultStr VulkanRendererAPI::RenderMeshIndirectCount(
         const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
         const glm::mat4& transform, const MaterialExecutor& material, const std::span<const std::byte> push,
         const RDG::BufferRef args, const uint64_t offset, const RDG::BufferRef count, const uint64_t countOffset,
         const uint32_t maxDraws, const SubmeshMask hiddenSubmeshMask )
    {
        // A lone bin is a batch of one: one implementation of what a compacted bin records.
        const BinDraw bin{ .Pipeline          = &pipeline,
                           .Mesh              = &mesh,
                           .Material          = &material,
                           .Transform         = transform,
                           .Push              = push,
                           .Args              = args,
                           .Offset            = offset,
                           .Count             = count,
                           .CountOffset       = countOffset,
                           .MaxDraws          = maxDraws,
                           .HiddenSubmeshMask = hiddenSubmeshMask };
        return RenderMeshIndirectCountBins( bindings, std::span<const BinDraw>( &bin, 1 ) );
    }

    namespace
    {
        // FPS-CPU2-VKRECORD: an indirect argument buffer of a batch, resolved once. The bins / groups naming the
        // buffer of the same pass context the one before them named reuse it: PassContext::GetBuffer scans the
        // pass's declared uses and BufferOf checks the backend, both invariant over a pass.
        struct ResolvedIndirectBuffer
        {
            const RDG::PassContext* Context = nullptr;
            RDG::BufferRef          Ref{};
            VkBuffer                Buffer = VK_NULL_HANDLE;
            uint64_t                Bytes  = 0;
            std::string_view        Name;
        };

        // @p resolved names @p ref of @p context (declared Access::IndirectArgs) afterwards. Error: GetBuffer's
        // or BufferOf's text, for the caller to prefix with what the buffer is.
        Common::BoolResultStr ResolveIndirectBuffer( const RDG::PassContext& context, const RDG::BufferRef ref,
                                                     ResolvedIndirectBuffer& resolved )
        {
            if ( resolved.Context == &context && resolved.Ref == ref && resolved.Buffer != VK_NULL_HANDLE )
                return Common::MakeSuccess( true );
            resolved            = {};
            const auto declared = context.GetBuffer( ref, RDG::Access::IndirectArgs );
            if ( !declared )
                return Common::MakeError( declared.GetError() );
            const auto buffer = VulkanRdgBackend::BufferOf( declared.GetValue() );
            if ( !buffer )
                return Common::MakeError( buffer.GetError() );
            resolved = ResolvedIndirectBuffer{ .Context = &context,
                                               .Ref     = ref,
                                               .Buffer  = buffer.GetValue()->GetBuffer(),
                                               .Bytes   = declared.GetValue().Desc->Bytes,
                                               .Name    = declared.GetValue().Name };
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::BoolResultStr VulkanRendererAPI::RenderMeshIndirectCountBins( const RDG::PassBindings& bindings,
                                                                          const std::span<const BinDraw> bins )
    {
        // ONE row per batch, under the name the per-bin call had, so a profile before and after compares rows.
        DESERT_PROFILE_SCOPE( "VulkanRendererAPI::RenderMeshIndirectCount" );
        if ( bins.empty() )
            return Common::MakeSuccess( true );
        // Invariant over the batch (UE SubmitMeshDrawCommandsRange): the pass, the device limits, the stride.
        const RDG::PassContext& context      = bindings.GetContext();
        const std::string_view  pass         = context.GetPassName();
        const auto&             capabilities = EngineContext::GetInstance().GetCapabilities();
        if ( !capabilities.SupportsDrawIndirectCount )
            return Common::MakeFormattedError(
                 "{}: a compacted bin's draw needs drawIndirectCount, which this device lacks", pass );
        // The stride is a record (kGpuDrawBinCommandBytes = kMeshDrawArgumentsBytes = 20): at least a
        // VkDrawIndexedIndirectCommand and a multiple of four, as vkCmdDrawIndexedIndirectCount asks.
        const auto             stride = static_cast<uint32_t>( kMeshDrawArgumentsBytes );
        ResolvedIndirectBuffer commands;
        ResolvedIndirectBuffer counts;
        // UE FMeshDrawCommandStateCache: the pass state and mesh buffers the bin before bound (MeshWalkBound).
        MeshWalkBound bound;
        bool          allRecorded = true;
        for ( size_t i = 0; i < bins.size(); ++i )
        {
            const BinDraw& bin = bins[i];
            if ( bin.Pipeline == nullptr || bin.Mesh == nullptr || bin.Material == nullptr )
                return Common::MakeFormattedError( "{}: compacted bin {} has no pipeline, mesh or material", pass,
                                                   i );
            if ( bin.MaxDraws == 0u )
                continue;
            if ( bin.Offset % 4u != 0u || bin.CountOffset % 4u != 0u )
                return Common::MakeFormattedError(
                     "{}: compacted bin {} at byte {}, its count at byte {}: both must be multiples of four", pass,
                     i, bin.Offset, bin.CountOffset );
            if ( bin.MaxDraws > capabilities.MaxDrawIndirectCount )
                return Common::MakeFormattedError(
                     "{}: compacted bin {} of {} draws passes maxDrawIndirectCount ({})", pass, i, bin.MaxDraws,
                     capabilities.MaxDrawIndirectCount );
            if ( const Common::BoolResultStr resolved = ResolveIndirectBuffer( context, bin.Args, commands );
                 !resolved )
                return Common::MakeFormattedError( "{}: compacted bin {}: commands: {}", pass, i,
                                                   resolved.GetError() );
            const uint64_t end = bin.Offset + ( uint64_t{ bin.MaxDraws } * kMeshDrawArgumentsBytes );
            if ( end > commands.Bytes )
                return Common::MakeFormattedError(
                     "{}: compacted bin {}: {} commands at byte {} pass the end of '{}' ({} bytes)", pass, i,
                     bin.MaxDraws, bin.Offset, commands.Name, commands.Bytes );
            if ( const Common::BoolResultStr resolved = ResolveIndirectBuffer( context, bin.Count, counts );
                 !resolved )
                return Common::MakeFormattedError( "{}: compacted bin {}: count: {}", pass, i,
                                                   resolved.GetError() );
            if ( bin.CountOffset + sizeof( uint32_t ) > counts.Bytes )
                return Common::MakeFormattedError(
                     "{}: compacted bin {}: the count at byte {} passes the end of '{}' ({} bytes)", pass, i,
                     bin.CountOffset, counts.Name, counts.Bytes );
            const VkBuffer vkArgs  = commands.Buffer;
            const VkBuffer vkCount = counts.Buffer;
            // The walk binds what differs from the bin before (pass state, mesh buffers) and the push block of the
            // first drawn run; the bin's commands name every record's own indices, so that run's call is the
            // bin's only one and the walk ends with it (no push blocks for the runs after).
            const Common::BoolResultStr walked = WalkMeshSubmeshes(
                 bindings, *bin.Pipeline, *bin.Mesh, bin.Transform, *bin.Material, bin.Push, bin.HiddenSubmeshMask,
                 VK_NULL_HANDLE,
                 [&]( const SubmeshRun& run ) -> Common::BoolResultStr
                 {
                     if ( run.IndexCount == 0u )
                         return Common::MakeFormattedError( "{}: compacted bin {}: its mesh is not indexed", pass,
                                                            i );
                     // One CPU call of the mesh pass; its draws are the GPU's (RenderMeshIndirect likewise).
                     DrawCounter::RecordCpuIndirect();
                     vkCmdDrawIndexedIndirectCount( RecordingTarget(), vkArgs, bin.Offset, vkCount,
                                                    bin.CountOffset, bin.MaxDraws, stride );
                     return Common::MakeSuccess( false );
                 },
                 &bound );
            if ( !walked )
                return Common::MakeFormattedError( "{}: compacted bin {}: {}", pass, i, walked.GetError() );
            // Success(false): the bin's pipeline is still compiling (BindGraphicsPassState) - skipped, not drawn.
            if ( !walked.GetValue() )
                allRecorded = false;
        }
        return Common::MakeSuccess( allRecorded );
    }

    Common::BoolResultStr
    VulkanRendererAPI::RenderMeshIndirectGroup( const std::span<const MeshIndirectGroupBatchEntry> groups )
    {
        DESERT_PROFILE_FUNC();
        // FPS-CPU2-VKRECORD (UE FMeshDrawCommandStateCache): nothing records between two groups of one call but
        // their geometry binds and draws, so what the group before bound stays bound - the argument buffer's
        // resolution, the pass state of (block, pipeline, carrier) with its push, the geometry and id stream.
        ResolvedIndirectBuffer     args;
        const RDG::PassBindings*   boundBindings = nullptr; // null: no pass state known
        const GraphicsPipeline*    boundPipeline = nullptr;
        const MaterialExecutor*    boundMaterial = nullptr;
        const MeshGeometryBinding* boundGeometry = nullptr; // null: no geometry known
        VkBuffer                   boundRows     = VK_NULL_HANDLE;
        uint32_t                   identityRows  = 0;
        VkBuffer                   identity      = VK_NULL_HANDLE;
        bool                       allRecorded   = true;
        for ( size_t g = 0; g < groups.size(); ++g )
        {
            const MeshIndirectGroupBatchEntry& entry = groups[g];
            if ( entry.Bindings == nullptr || entry.Pipeline == nullptr || entry.Geometry == nullptr )
                return Common::MakeFormattedError(
                     "indirect group {} of the batch has no binding block, pipeline or geometry", g );
            const RDG::PassBindings&     bindings = *entry.Bindings;
            const GraphicsPipeline&      pipeline = *entry.Pipeline;
            const MeshGeometryBinding&   geometry = *entry.Geometry;
            const MeshIndirectGroupDraw& draw     = entry.Draw;
            const RDG::PassContext&      context  = bindings.GetContext();
            const std::string_view       pass     = context.GetPassName();
            if ( draw.MaxDraws == 0u )
                continue;
            if ( draw.Stride < sizeof( VkDrawIndexedIndirectCommand ) || draw.Stride % 4u != 0u ||
                 draw.ArgsOffset % 4u != 0u )
                return Common::MakeFormattedError(
                     "{}: indirect group {} of stride {} at byte {}: the stride holds a "
                     "VkDrawIndexedIndirectCommand and both are multiples of four",
                     pass, g, draw.Stride, draw.ArgsOffset );
            if ( geometry.Indices == nullptr )
                return Common::MakeFormattedError( "{}: indirect group {}: its geometry has no index buffer", pass,
                                                   g );
            if ( const Common::BoolResultStr resolved = ResolveIndirectBuffer( context, draw.Args, args );
                 !resolved )
                return Common::MakeFormattedError( "{}: indirect group {}: arguments: {}", pass, g,
                                                   resolved.GetError() );
            const uint64_t end = draw.ArgsOffset + ( uint64_t{ draw.MaxDraws - 1u } * draw.Stride ) +
                                 sizeof( VkDrawIndexedIndirectCommand );
            if ( end > args.Bytes )
                return Common::MakeFormattedError(
                     "{}: indirect group {}: {} records at byte {} (stride {}) pass the end of '{}' ({} bytes)",
                     pass, g, draw.MaxDraws, draw.ArgsOffset, draw.Stride, args.Name, args.Bytes );
            if ( draw.MaxDrawsPerCall == 0u )
                return Common::MakeFormattedError( "{}: indirect group {} of zero draws per call", pass, g );
            if ( !draw.SplitLabels.empty() && draw.SplitLabels.size() != draw.MaxDraws )
                return Common::MakeFormattedError( "{}: indirect group {}: {} draw split labels for {} records",
                                                   pass, g, draw.SplitLabels.size(), draw.MaxDraws );

            // The pass state, unless the group before bound exactly it: the same block, pipeline and carrier are
            // a memo hit, an already-bound pipeline and sets, and the same push block (Record pushes the
            // carrier's, which nothing between the two groups overwrote).
            if ( boundBindings != &bindings || boundPipeline != &pipeline || boundMaterial != draw.Material )
            {
                boundBindings = nullptr;
                boundGeometry = nullptr;
                const Common::BoolResultStr bound =
                     BindGraphicsPassState( bindings, pipeline, draw.Material, {}, PassStatePush::Record );
                if ( !bound )
                    return Common::MakeFormattedError( "{}: indirect group {}: {}", pass, g, bound.GetError() );
                if ( !bound.GetValue() )
                {
                    allRecorded = false; // its pipeline is still compiling: not drawn this frame
                    continue;
                }
                boundBindings = &bindings;
                boundPipeline = &pipeline;
                boundMaterial = draw.Material;
            }
            // Binding 2: the identity row stream, so a_VisibleRow = gl_InstanceIndex names the row of the cull's
            // VisibleInstances the record's firstInstance points into (BindMeshBuffers refuses a pipeline reading
            // a_VisibleRow without it, and a stream handed to one that does not).
            if ( draw.VisibleRows != identityRows )
            {
                identityRows = draw.VisibleRows;
                identity     = VK_NULL_HANDLE;
                if ( draw.VisibleRows != 0u )
                    identity = VisibleRowIdentity( draw.VisibleRows );
            }
            // The group's shared binding (the pool arena at offset 0): bound once per pipeline switch, the
            // records' FirstIndex / VertexOffset are pool-absolute (DrawCommandCache).
            if ( boundGeometry != &geometry || boundRows != identity )
            {
                boundGeometry = nullptr;
                if ( const Common::BoolResultStr buffers =
                          BindMeshBuffers( pass, pipeline, geometry, identity, true );
                     !buffers )
                    return Common::MakeFormattedError( "{}: indirect group {}: {}", pass, g, buffers.GetError() );
                boundGeometry = &geometry;
                boundRows     = identity;
            }

            // Not counted by DrawCounter: the instance counts are the GPU's (RenderMeshIndirect likewise).
            // One fixed table (decision 10-28): every bin's record is drawn, a culled one with InstanceCount 0.
            // The mesh pass's issued bins and calls (DrawCounter::LastFrameMeshPass).
            // MaxDraws > 0 here; maxDrawIndirectCount may be 2^32 - 1, so no rounding-up sum.
            // Under the mesh draw split every record is its own call inside its own timestamp pair (the bin's
            // row); otherwise MaxDrawsPerCall records per call, as without the profiler.
            const uint32_t perCall = draw.SplitLabels.empty() ? draw.MaxDrawsPerCall : 1u;
            DrawCounter::RecordBins( draw.MaxDraws, ( ( draw.MaxDraws - 1u ) / perCall ) + 1u );
            for ( uint32_t first = 0; first < draw.MaxDraws; first += perCall )
            {
                const uint32_t count = std::min( draw.MaxDraws - first, perCall );
                std::optional<Common::Profiling::GpuDrawSplitScope> bin;
                if ( !draw.SplitLabels.empty() )
                {
                    const DrawSplitLabel& label = draw.SplitLabels[first];
                    bin.emplace( pass, label.Mesh, label.Material );
                    bin->Add( 1, label.TrianglesPerInstance, true );
                }
                vkCmdDrawIndexedIndirect( RecordingTarget(), args.Buffer,
                                          draw.ArgsOffset + ( uint64_t{ first } * draw.Stride ), count,
                                          draw.Stride );
            }
        }
        return Common::MakeSuccess( allRecorded );
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

        if ( const Common::BoolResultStr bound =
                  BindGraphicsPassState( bindings, pipeline, draw.Material, {}, PassStatePush::Record );
             !bound || !bound.GetValue() )
            return bound;
        // Binding 2 only: the cell reads its vertices through the cluster (no binding 0 / 1 attribute is consumed,
        // VulkanShaderReflection feeds the consumed locations only), so the bind-skip key of the meshes'
        // buffers (m_BoundGeometry) is untouched; binding 2's own record (BoundVisibleRows) names this buffer, so
        // the next mesh draw binds its stream again.
        const VkBuffer rowBuffer = rows.GetValue()->GetBuffer();
        if ( !Recording().BoundVisibleRows.Holds( RecordingTarget(), rowBuffer ) )
        {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers( RecordingTarget(), GpuDriven::kVisibleRowVertexBinding, 1, &rowBuffer, &zero );
        }
        DrawCounter::RecordBins( 1u, 1u );
        vkCmdDrawIndirect( RecordingTarget(), args.GetValue()->GetBuffer(), draw.ArgsOffset, 1,
                           static_cast<uint32_t>( sizeof( VkDrawIndirectCommand ) ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRendererAPI::BindGraphicsPassState( const RDG::PassBindings&         bindings,
                                                                    const GraphicsPipeline&          pipeline,
                                                                    const MaterialExecutor*          material,
                                                                    const std::span<const std::byte> drawPush,
                                                                    const PassStatePush              pushMode,
                                                                    const VulkanBuiltPipeline*       record )
    {
        const RDG::PassContext&                  context = bindings.GetContext();
        const std::string_view                   pass    = context.GetPassName();
        const Common::ResultStr<VkCommandBuffer> cmd     = VulkanRdgBackend::CommandBufferOf( context );
        if ( !cmd )
            return Common::MakeError( cmd.GetError() );
        // BindGraphicsPipeline and DrawCounted record into the renderer's current command buffer, which the
        // graph backend points at the segment the pass runs on (VulkanRdgBackend::SetRecordingListener).
        if ( cmd.GetValue() != RecordingTarget() )
            return Common::MakeFormattedError(
                 "{}: the renderer records into another command buffer than the pass's segment", pass );
        const Common::ResultStr<VulkanRdgPassDescriptors*> descriptors =
             VulkanRdgBackend::DescriptorsOf( context );
        if ( !descriptors )
            return Common::MakeError( descriptors.GetError() );

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes pipelines
        const auto* graphics = static_cast<const VulkanPipeline*>( &pipeline );
        // ONE RECORD for the layouts the sets are written for, the layout they and the push are recorded with
        // and the pipeline bound: the caller's read, else one read here.
        const VulkanBuiltPipeline& built = record != nullptr ? *record : graphics->GetBuilt();
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
        const std::vector<DescriptorSetLayoutRef>& layouts = built.Layouts;
        // GPU-COST-36: the sets an earlier draw of this pass exec wrote from the same block, shader, layout and
        // material are bound again (VulkanRdgPassSetMemo); only the first draw of the combination resolves,
        // allocates, writes and copies.
        const Common::ResultStr<VulkanRdgPassSetMemo*> memo = VulkanRdgBackend::SetMemoOf( context );
        if ( !memo )
            return Common::MakeError( memo.GetError() );
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the shader's and material's addresses are
        // the memo's identities, compared and never turned back into pointers
        const VulkanRdgPassSetMemo::Key key{ .Block            = bindings.GetBlock(),
                                             .Shader           = reinterpret_cast<std::uintptr_t>( shader ),
                                             .Layout           = built.PipelineLayout,
                                             .Material         = reinterpret_cast<std::uintptr_t>( material ),
                                             .MaterialSet      = materialSlots.Set,
                                             .MaterialBindings = materialSlots.Bindings,
                                             .MaterialPush     = other.PushConstants,
                                             .BlockPushBytes   = bindings.GetPushConstants().size() };
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        std::optional<std::span<const VkDescriptorSet>> kept = memo.GetValue()->Find( key );
        if ( !kept.has_value() )
        {
            // FPS-CPU2-VKRECORD: the miss's own row, so a draw's cost splits into memo hit (the caller's row
            // minus this) and miss (resolve + write + descriptor cache).
            DESERT_PROFILE_SCOPE( "VulkanRendererAPI::BindGraphicsPassState: set memo miss" );
            // CPU-FRAME-5: the other route's filled slots are read only by the resolution, so a draw the memo
            // serves builds none.
            for ( const uint32_t binding : materialSlots.Bindings )
                other.Filled.push_back( RdgSlotKey{ .Set = 0, .Binding = binding } );
            thread_local std::vector<RdgResolvedEntry> t_Resolved; // FPS-ARCH-BINDINGS: as DispatchCompute's
            const Common::BoolResultStr resolved =
                 ResolveRdgPassBindings( reflection, SlotsOf( *shader ), shader->GetName(), bindings, other,
                                         t_Resolved );
            if ( !resolved.IsSuccess() )
                return Common::MakeError( resolved.GetError() );
            // RDG-PARALLEL-9: set 0 with the material's slots comes from the descriptor cache as well; the copies
            // are made inside, on a miss only.
            auto written = WriteRdgPassSets( *descriptors.GetValue(), layouts, t_Resolved, pass, false,
                                             &materialSlots );
            if ( !written )
                return Common::MakeError( written.GetError() );
            kept = memo.GetValue()->Keep( key, written.GetValue() );
        }
        const std::span<const VkDescriptorSet> sets = *kept;

        // A pipeline still in the driver (PSO1) is not drawn this frame and is not an error: the content gate waits
        // for engine pipelines (BindGraphicsPipeline). Success(false) tells the draw to skip.
        if ( graphics->GetBuildState() == VulkanPipeline::BuildState::Compiling )
            return Common::MakeSuccess( false );
        if ( !BindGraphicsPipeline( &pipeline, &built ) )
            return Common::MakeFormattedError( "{}: pipeline '{}' could not be bound in the open render pass",
                                               pass, pipeline.GetSpecification().DebugName );
        // CPU-FRAME-6: the sets are recorded only when the command buffer does not hold exactly these, bound with
        // this layout (m_BoundGraphics; a layout that differs is never assumed compatible).
        if ( !sets.empty() )
        {
            const VkPipelineLayout layout = built.PipelineLayout;
            Recording().BoundGraphics.On( cmd.GetValue() );
            if ( Recording().BoundGraphics.SetsLayout != layout ||
                 !std::ranges::equal( Recording().BoundGraphics.Sets, sets ) )
            {
                vkCmdBindDescriptorSets( cmd.GetValue(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                                         static_cast<uint32_t>( sets.size() ), sets.data(), 0, nullptr );
                Recording().BoundGraphics.SetsLayout = layout;
                Recording().BoundGraphics.Sets.assign( sets.begin(), sets.end() );
            }
        }
        // Stages and size are the pipeline LAYOUT's range (VulkanPipeline::GetPushConstantRange), not the shader's
        // current reflection: the two part ways when the shader is re-reflected while this pipeline keeps its
        // layout.
        const std::span<const std::byte> push      = ChoosePushConstants( bindings, materialPush );
        const auto&                      pushRange = built.PushConstantRange;
        ReportPushStagesApart( *graphics, built, shader->GetName(), reflection.PushConstantRanges );
        if ( !push.empty() && pushRange.has_value() )
        {
            if ( push.size() > pushRange->size )
                return Common::MakeFormattedError( "{}: pipeline '{}' push range is {} bytes, the pass pushes {}",
                                                   pass, pipeline.GetSpecification().DebugName, pushRange->size,
                                                   push.size() );
            if ( pushMode == PassStatePush::Record )
                vkCmdPushConstants( cmd.GetValue(), built.PipelineLayout, pushRange->stageFlags,
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
        vkCmdPipelineBarrier( RecordingTarget(),
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );

        // Transition the target to GENERAL for storage writes (its last use was a fragment sample).
        vkImage->TransitionLayout( RecordingTarget(), VK_IMAGE_LAYOUT_GENERAL,
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
        vkImage->TransitionLayout( RecordingTarget(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
             RecordingTarget(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
             RecordingTarget(), vkImage->GetDefaultLayout(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
        // The second writer of the recording target after BeginFrame: it ends the frame's command buffer and
        // re-arms a fresh one after the graph, so it asks the device-lost gate like BeginFrame does.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return fail( "the device is lost; the graph is not recorded" );
        }
        // The sink the profiler holds now: GPU timing can be switched on and off between frames.
#if DESERT_DEV_INSTRUMENTS
        m_RdgDevice.Profiler = ::Common::Profiling::Profiler::Get().GetGpuSink();
        // RDG-PARALLEL-3: a graph chunk's scopes take their order as the chunk is replayed.
        m_RdgDevice.ChunkOrder = &m_GpuProfiler;
#endif
        // RDG-CONTRACTS B(3): the graph records into its own segment command buffers, so the frame command
        // buffer is split here: what was recorded before the graph is submitted before the graph.
        const VkResult ended = vkEndCommandBuffer( RecordingSlot().CommandBuffer );
        if ( ended != VK_SUCCESS )
        {
            RecordingSlot().CommandBuffer = nullptr;
            (void)NoteIfDeviceLost( ended, "vkEndCommandBuffer", __FILE__, __LINE__ );
            return fail(
                 std::format( "vkEndCommandBuffer before a graph failed: {}", VkResultToString( ended ) ) );
        }
        RecordingSlot().Submissions.push_back(
             { RDG::Pipe::Graphics, m_RdgQueues.GraphicsQueue, { RecordingSlot().CommandBuffer }, {}, {}, {} } );
        // Ended: from here until the graph backend arms its first buffer nothing may record into it.
        RecordingSlot().CommandBuffer                    = nullptr;
        Common::BoolResultStr            executed = graph.Execute( *m_RdgBackend );
        std::vector<VulkanRdgSubmission> segments = m_RdgBackend->TakeSubmissions();
        // A graph that failed inside an async segment never reached its EndPipeSegment: what records after the
        // graph is on the graphics queue whatever the graph left behind.
        if ( m_RdgDevice.Profiler != nullptr )
            m_RdgDevice.Profiler->SetQueue( ::Common::Profiling::GpuQueue::Graphics );
        std::vector<VulkanRdgSubmission>& submissions = RecordingSlot().Submissions;
        submissions.insert( submissions.end(), std::make_move_iterator( segments.begin() ),
                            std::make_move_iterator( segments.end() ) );

        // Recording continues after the graph in a fresh graphics command buffer of this frame slot. A loss
        // noted while the graph recorded (a segment's vkEndCommandBuffer) must not re-arm the frame.
        if ( !Graphic::DeviceLost::AllowWork() )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return fail( "the device was lost while the graph recorded" );
        }
        const Common::ResultStr<VkCommandBuffer> next =
             m_FrameLoop->GetQueueObjects().BeginCommandBuffer( RDG::Pipe::Graphics );
        if ( !next )
        {
            RecordingSlot().CommandBuffer = nullptr;
            return fail( next.GetError() );
        }
        RecordingSlot().CommandBuffer = next.GetValue();
        ForgetBoundState(); // a freshly begun buffer holds no binding
        // A FrameFault: the graph's output has no picture, so its frame-fatal externals are cleared to black on
        // the re-armed buffer and the frame presents that.
        if ( const std::optional<RDG::FrameFault>& frame = graph.GetExecuteReport().Frame )
        {
            DESERT_PROFILE_PASS( "RDG: FrameFault clear" ); // recorded outside every pass of the graph
            if ( const auto cleared = ClearFrameFaultExternals( RecordingSlot().CommandBuffer, graph, *frame );
                 !cleared )
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
            RecordingSlot().CommandBuffer = nullptr;
            return;
        }
        RecordingSlot().CommandBuffer = commandBuffer;
        // A segment's buffer may be one begun again since it last recorded (same handle, empty state).
        ForgetBoundState();
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
            // CPU-FRAME-6: a pass exec may record through anything (ImGui, NGX), so what this API last bound is
            // not known across a pass's begin or end (m_BoundGraphics).
            m_RdgBackend->SetPassBoundaryListener( [this]() { ForgetBoundState(); } );
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
        // EACH STAGE A ROW (VK-SUBMIT-THREAD): "Vulkan: begin RDG frame" measured 0.22-0.25 ms on the field turn
        // with no row under it, so the profile names which stage it is.
        DESERT_PROFILE_PHASES( stages );
        DESERT_PROFILE_PHASE( stages, "RDG frame: slot fence + command pools" );
        // The slot's fence FIRST: every object begun below may still be in use by the slot's previous frame.
        const uint32_t slot = EngineContext::GetInstance().GetCurrentFrameIndex();
        if ( Common::BoolResultStr begun = m_FrameLoop->BeginSlot( slot ); !begun )
            return begun;
        DESERT_PROFILE_PHASE( stages, "RDG frame: physical pool" );
        m_RdgPool->BeginFrame();
        DESERT_PROFILE_PHASE( stages, "RDG frame: transient heaps" );
        m_RdgTransients->BeginFrameSlot( slot );
        DESERT_PROFILE_PHASE( stages, "RDG frame: pass descriptors" );
        m_RdgDescriptors->BeginFrameSlot( slot );
        DESERT_PROFILE_PHASE( stages, "RDG frame: backend" );
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
        // built the frame instead of before. Everything recorded so far is in the slot's Submissions or in the open
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
            RecordingSlot().BackBufferFirstEntry = RecordingSlot().Submissions.size();
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
        // RENDER-CPU-COST-2: a raw cast, no reference count touched; the owning handle is made only when the
        // import below is built anew. CPU-GRASS-VARIETY-1: a static downcast, not a dynamic_cast per import (the
        // grass varieties import ~18 buffers each a frame): on the Vulkan backend every StorageBuffer is made by
        // StorageBuffer::Create / CreateReadback / CreateDeviceLocal, and each makes a VulkanStorageBuffer.
        if ( buffer == nullptr )
            return Common::MakeError( "ImportBuffer: no storage buffer" );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the factory makes only this type
        auto* const vulkanBuffer = static_cast<VulkanStorageBuffer*>( buffer.get() );
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
        // RENDER-CPU-COST-2 (UE: a pooled buffer is wrapped for the graph once; RegisterExternalBuffer of it each
        // frame only reads the state it carries). @p into already imports this very copy - the record's handle is
        // made anew for every other copy and for every other buffer, so an equal handle names this one - and its
        // hand-back already writes into this copy's record: only the state the last graph left changes.
        if ( into.Physical != nullptr && into.Physical == record.Physical && into.RecordFinalState )
        {
            into.Desc.Bytes = binding.Info.range;
            into.State      = record.State;
            return Common::MakeSuccess( true );
        }
        into.Desc.Bytes = binding.Info.range;
        into.Physical   = record.Physical;
        into.State      = record.State;
        // Aliasing the caller's owner: the hand-back must not keep the buffer alive past its owner.
        const std::weak_ptr<VulkanStorageBuffer> weak =
             std::shared_ptr<VulkanStorageBuffer>( buffer, vulkanBuffer );
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
        vkCmdCopyImage( RecordingTarget(), vsrc->GetResource().Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        vdst->GetResource().Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::CopyGraphDepthToLayer( const RDG::PassContext&           context,
                                                                    const RDG::TextureRef             source,
                                                                    const RDG::TextureRef             target,
                                                                    const uint32_t                    targetLayer,
                                                                    const std::span<const glm::uvec4> regions )
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

        // The graph's Copy node put the source in TRANSFER_SRC and the target layer in TRANSFER_DST. One region
        // per asked rectangle (the same texels in both images), kept on the image; none asked: the whole layer.
        std::vector<VkImageCopy> copies;
        const auto               add = [&copies, targetLayer]( const VkOffset3D offset, const VkExtent3D extent )
        {
            VkImageCopy region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            region.srcOffset      = offset;
            region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, targetLayer, 1 };
            region.dstOffset      = offset;
            region.extent         = extent;
            copies.push_back( region );
        };
        if ( regions.empty() )
            add( { 0, 0, 0 }, { src.Size.Width, src.Size.Height, 1 } );
        for ( const glm::uvec4& rect : regions )
        {
            const uint32_t x = std::min( rect.x, src.Size.Width );
            const uint32_t y = std::min( rect.y, src.Size.Height );
            const uint32_t w = std::min( rect.z, src.Size.Width - x );
            const uint32_t h = std::min( rect.w, src.Size.Height - y );
            if ( w == 0u || h == 0u )
                continue;
            add( { static_cast<int32_t>( x ), static_cast<int32_t>( y ), 0 }, { w, h, 1 } );
        }
        if ( copies.empty() )
            return BOOLSUCCESS;
        vkCmdCopyImage( commands.GetValue(), fromImage.GetValue()->GetImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, toImage.GetValue()->GetImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>( copies.size() ),
                        copies.data() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::ScrollGraphDepthIntoLayer( const RDG::PassContext& context,
                                                                        const RDG::TextureRef   source,
                                                                        const RDG::TextureRef   target,
                                                                        const uint32_t          targetLayer,
                                                                        const int32_t           shiftX,
                                                                        const int32_t           shiftY,
                                                                        const float             clearDepth )
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
                 "ScrollGraphDepthIntoLayer: '{}' is {}x{} with {} sample(s), layer {} of '{}' ({}x{}, {} "
                 "sample(s), {} layer(s)); a scroll needs the same size and sample count and an existing layer",
                 from.GetValue().Name, src.Size.Width, src.Size.Height, src.Samples, targetLayer,
                 to.GetValue().Name, dst.Size.Width, dst.Size.Height, dst.Samples, dst.Layers ) );

        // The graph's Copy node put the source in TRANSFER_SRC and the target layer in TRANSFER_DST. The layer at the
        // far plane first: the texels no source texel lands on are the strips the caller draws next.
        const VkImage                  targetImage = toImage.GetValue()->GetImage();
        const VkClearDepthStencilValue clear{ clearDepth, 0u };
        const VkImageSubresourceRange  range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, targetLayer, 1 };
        vkCmdClearDepthStencilImage( commands.GetValue(), targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                                     1, &range );
        const int32_t width  = static_cast<int32_t>( src.Size.Width ) - std::abs( shiftX );
        const int32_t height = static_cast<int32_t>( src.Size.Height ) - std::abs( shiftY );
        if ( width <= 0 || height <= 0 )
            return BOOLSUCCESS;
        // The copy writes texels the clear wrote: transfer write after transfer write.
        VkMemoryBarrier written{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier( commands.GetValue(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              1, &written, 0, nullptr, 0, nullptr );
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.srcOffset      = { std::max( -shiftX, 0 ), std::max( -shiftY, 0 ), 0 };
        region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, targetLayer, 1 };
        region.dstOffset      = { std::max( shiftX, 0 ), std::max( shiftY, 0 ), 0 };
        region.extent         = { static_cast<uint32_t>( width ), static_cast<uint32_t>( height ), 1 };
        vkCmdCopyImage( commands.GetValue(), fromImage.GetValue()->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::CopyGraphLayerToDepth( const RDG::PassContext& context,
                                                                    const RDG::TextureRef   source,
                                                                    const uint32_t          sourceLayer,
                                                                    const RDG::TextureRef   target )
    {
        const auto commands = VulkanRdgBackend::CommandBufferOf( context );
        if ( !commands )
            return Common::MakeError( commands.GetError() );
        const auto from =
             context.GetTexture( source, RDG::Access::CopySrc, RDG::SubresourceRange::Layer( sourceLayer ) );
        if ( !from )
            return Common::MakeError( from.GetError() );
        const auto to = context.GetTexture( target, RDG::Access::CopyDst );
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
             src.Samples != dst.Samples || sourceLayer >= src.Layers )
            return Common::MakeError( std::format(
                 "CopyGraphLayerToDepth: layer {} of '{}' ({}x{}, {} sample(s), {} layer(s)) into '{}' ({}x{}, {} "
                 "sample(s)); a copy needs the same size and sample count and an existing layer",
                 sourceLayer, from.GetValue().Name, src.Size.Width, src.Size.Height, src.Samples, src.Layers,
                 to.GetValue().Name, dst.Size.Width, dst.Size.Height, dst.Samples ) );

        // The graph's Copy node put the source layer in TRANSFER_SRC and the target in TRANSFER_DST.
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, sourceLayer, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.extent         = { dst.Size.Width, dst.Size.Height, 1 };
        vkCmdCopyImage( commands.GetValue(), fromImage.GetValue()->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        toImage.GetValue()->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
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
        vkCmdSetScissor( RecordingTarget(), 0, 1, &scissor );
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
        vkCmdSetViewport( RecordingTarget(), 0, 1, &viewport );
        vkCmdSetScissor( RecordingTarget(), 0, 1, &scissor );
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
        vkCmdClearAttachments( RecordingTarget(), static_cast<uint32_t>( attachments.size() ),
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
        vkCmdSetViewport( RecordingTarget(), 0, 1, &viewport );

        VkRect2D scissor = { .offset = { 0, 0 }, .extent = { width, height } };
        vkCmdSetScissor( RecordingTarget(), 0, 1, &scissor );
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
        vkCmdClearAttachments( RecordingTarget(), attachmentCount, attachments.data(), attachmentCount,
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
        vkCmdDrawIndexed( RecordingTarget(), indexCount, instanceCount, firstIndex, vertexOffset,
                          firstInstance );
    }

    void VulkanRendererAPI::DrawCounted( uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                                         uint32_t firstInstance )
    {
        DrawCounter::Record( instanceCount );
        vkCmdDraw( RecordingTarget(), vertexCount, instanceCount, firstVertex, firstInstance );
    }

} // namespace Desert::Graphic::API::Vulkan
