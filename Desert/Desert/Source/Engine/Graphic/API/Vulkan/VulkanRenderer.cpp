#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/WriteDescriptorSetBuilder.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/DrawCounters.hpp>

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

        // ROLLED HERE AND NOT IN EndFrame, and the difference is observable. A lost device returns above
        // without recording anything, so rolling in EndFrame would either be skipped — leaving the last
        // GOOD frame's count standing as if it were the failed one's — or would zero the very number that
        // explains the failure. Rolling at the top means the readable count always belongs to the last
        // frame that actually recorded.
        DrawCounter::Roll();

        auto window = m_Window.lock();
        if ( !window )
            return Common::MakeError( "Window is null" );

        auto swapChain         = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );
        m_CurrentCommandBuffer = swapChain->GetVulkanQueue()->GetDrawCommandBuffer();

        // Reset descriptor update tracking for this frame
        // This requires access to materials, which might be hard here.
        // A better approach is to have MaterialBackend reset itself.

        VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        const VkResult           begun     = vkBeginCommandBuffer( m_CurrentCommandBuffer, &beginInfo );
        if ( begun != VK_SUCCESS )
        {
            // THE HANDLE IS DROPPED, not merely reported. It was assigned above, so returning the error
            // alone would leave a buffer that is NOT recording sitting in m_CurrentCommandBuffer — and
            // every vkCmd* in this file, plus EndFrame's vkEndCommandBuffer, treats a non-null value there
            // as "recording". Nulling it is what makes the invariant this class relies on true in the
            // failure case as well as the success case.
            m_CurrentCommandBuffer = nullptr;
            (void)NoteIfDeviceLost( begun, "vkBeginCommandBuffer", __FILE__, __LINE__ );
            return Common::MakeFormattedError<bool>( "vkBeginCommandBuffer failed: {}",
                                                     VkResultToString( begun ) );
        }

        // Resolve the previous results and reset this frame's queries. Must be here: vkCmdResetQueryPool
        // is illegal inside a render pass, and this is the one point in the frame where the command buffer
        // is recording and no pass is open.
#if DESERT_DEV_INSTRUMENTS
        m_GpuProfiler.BeginFrame( m_CurrentCommandBuffer );
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
            m_GpuProfiler.EndFrame( m_CurrentCommandBuffer );
#endif

            VK_CHECK_RESULT_BOOL( vkEndCommandBuffer( m_CurrentCommandBuffer ) );
            m_CurrentCommandBuffer = nullptr;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::PresentFinalImage()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; nothing is submitted or presented." );

        auto window = m_Window.lock();
        if ( !window )
            return Common::MakeError( "Window is null" );
        auto swapChain = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );

        // REFUSE TO SUBMIT A BUFFER THAT DID NOT CLOSE. EndFrame is what calls vkEndCommandBuffer, and
        // submitting a still-recording buffer is undefined behaviour that the validation layers report
        // as a driver-side error with no line of ours in it. Returning the message here puts the cause
        // in the frame's own result, which Application::Run now reads.
        const auto ended = EndFrame();
        if ( !ended.IsSuccess() )
            return Common::MakeError( ended.GetError() );

        swapChain->GetVulkanQueue()->Submit();
        swapChain->Present();

        // The submit and the present are where an asynchronous loss surfaces. Saying so HERE, in this
        // frame's own result, is what carries it up to Application::Run — before which the result of this
        // function was thrown away at three separate links.
        if ( Graphic::DeviceLost::IsLost() )
            return Common::MakeError( "the device was lost while submitting or presenting this frame." );

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

        vkCmdBeginRenderPass( m_CurrentCommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE );
        SetViewportAndScissor( framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::BeginSwapChainRenderPass()
    {
        if ( !IsRecording() )
            return Common::MakeError( "No active command buffer" );

        auto window            = m_Window.lock();
        auto vulkanSwap        = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );
        auto framebuffer       = vulkanSwap->GetCompositeFramebuffer();
        m_CompositeFramebuffer = framebuffer;

        uint32_t imageIndex = vulkanSwap->GetCurrentBufferIndex();

        VkRenderPassBeginInfo renderPassInfo = {
             .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
             .renderPass  = vulkanSwap->GetRenderPass(),
             .framebuffer = vulkanSwap->GetVKFramebuffers()[imageIndex],
             .renderArea  = {
                   .offset = { 0, 0 },
                   .extent = { framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() } } };

        VkClearValue clearValue        = { .color = { { 0.1f, 0.1f, 0.1f, 1.0f } } };
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues    = &clearValue;

        // Must open a region too: EndRenderPass closes one unconditionally, so skipping it here would
        // leave the labels unbalanced.
        VKUtils::BeginDebugLabel( m_CurrentCommandBuffer, "SwapChainPass" );

        vkCmdBeginRenderPass( m_CurrentCommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE );
        SetViewportAndScissor( framebuffer->GetFramebufferWidth(), framebuffer->GetFramebufferHeight() );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRendererAPI::EndRenderPass()
    {
        if ( IsRecording() )
        {
            vkCmdEndRenderPass( m_CurrentCommandBuffer );

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

            // Closes the region opened by BeginRenderPass / BeginSwapChainRenderPass. Both open exactly
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

        const auto* vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( vulkanPipeline->GetVkPipeline() != VK_NULL_HANDLE )
        {
            vkCmdBindPipeline( m_CurrentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               vulkanPipeline->GetVkPipeline() );
            return true;
        }

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

    void VulkanRendererAPI::RenderMesh( const GraphicsPipeline* pipeline, const Mesh* mesh,
                                        const glm::mat4 transform, const MaterialExecutor* materialExecutor,
                                        uint32_t instanceCount, uint32_t firstInstance,
                                        uint64_t hiddenSubmeshMask, uint32_t lodLevel )
    {
        // Aggregate cost of EVERY mesh draw call across ALL passes (geometry + 4 shadow cascades +
        // silhouette). The call-site scopes break it down per-pass; this row is the engine-wide total.
        DESERT_PROFILE_FUNC();

        if ( !IsRecording() )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( !BindGraphicsPipeline( pipeline ) )
            return;

        // Bind Descriptor Sets
        if ( materialExecutor )
        {
            DESERT_PROFILE_SCOPE( "Vk::RenderMesh BindMaterial" );
            materialExecutor->Apply();
            auto vkBackend = static_cast<VulkanMaterialBackend*>( materialExecutor->GetMaterialBackend().get() );

            if ( !vkBackend->HasDescriptorSets() )
            {
                LOG_WARN( "VulkanRendererAPI: MaterialExecutor has no valid descriptor sets!" );
                return;
            }

            uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            vkBackend->BindDescriptorSets( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                           VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex );
        }

        VkDeviceSize offsets[] = { 0 };
        auto vbuffer = sp_cast<API::Vulkan::VulkanVertexBuffer>( mesh->GetVertexBuffer() )->GetVulkanBuffer();
        vkCmdBindVertexBuffers( m_CurrentCommandBuffer, 0, 1, &vbuffer, offsets );

        if ( auto indexBuffer = mesh->GetIndexBuffer() )
        {
            auto ibuffer = sp_cast<API::Vulkan::VulkanIndexBuffer>( indexBuffer )->GetVulkanBuffer();
            vkCmdBindIndexBuffer( m_CurrentCommandBuffer, ibuffer, 0, VK_INDEX_TYPE_UINT32 );
        }

        const auto& submeshes = mesh->GetSubmeshes();
        for ( size_t si = 0; si < submeshes.size(); ++si )
        {
            // Per-submesh visibility: bit si set = hidden -> skip the draw (and its push/transform work).
            if ( si < 64 && ( ( hiddenSubmeshMask >> si ) & 1ull ) )
                continue;

            const auto&       submesh       = submeshes[si];
            MaterialExecutor* materialExec   = (MaterialExecutor*)materialExecutor;
            auto              finalTransform = transform * submesh.Transform;
            materialExec->PushConstant( &finalTransform, sizeof( glm::mat4 ) );

            const auto&   pcBuffer     = materialExecutor->GetPushConstantBuffer();
            VulkanShader* vulkanShader = (VulkanShader*)pipeline->GetSpecification().Shader.get();
            const auto&   pushConstant = vulkanShader->GetShaderPushConstant();
            if ( pushConstant.has_value() )
            {
                // Push the full reflected range so sub-blocks the caller wrote past the transform
                // (e.g. per-object PBR material params at offset sizeof(mat4)) are included. The
                // push buffer is zero-initialized, so any unwritten declared bytes are defined.
                const auto& pcInfo = *pushConstant;
                if ( pcInfo.Size > 0 )
                {
                    vkCmdPushConstants( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                        (VkShaderStageFlags)pcInfo.ShaderStage, 0, pcInfo.Size,
                                        pcBuffer.Data );
                }
            }

            if ( mesh->GetIndexBuffer() )
            {
                // LOD range for this submesh (LODs[0] is the original, so lodLevel 0 is identical to the
                // base range). Clamp to the last available level; fall back to the base range when the
                // submesh has no LOD chain (procedural / too small).
                uint32_t drawOffset = submesh.IndexOffset;
                uint32_t drawCount  = submesh.IndexCount;
                if ( !submesh.LODs.empty() )
                {
                    const uint32_t lvl = lodLevel < submesh.LODs.size()
                                              ? lodLevel
                                              : static_cast<uint32_t>( submesh.LODs.size() ) - 1;
                    drawOffset = submesh.LODs[lvl].IndexOffset;
                    drawCount  = submesh.LODs[lvl].IndexCount;
                }

                if ( drawOffset + drawCount > mesh->GetIndexBuffer()->GetCount() )
                {
                    LOG_ERROR(
                         "VulkanRendererAPI: Invalid index buffer access! Offset: {}, Count: {}, BufferSize: {}",
                         drawOffset, drawCount, mesh->GetIndexBuffer()->GetCount() );
                    continue;
                }

                DrawIndexedCounted( drawCount, instanceCount, drawOffset,
                                    static_cast<int32_t>( submesh.VertexOffset ), firstInstance );
            }
            else
            {
                DrawCounted( submesh.VertexCount, instanceCount, submesh.VertexOffset, firstInstance );
            }
        }
    }

    void VulkanRendererAPI::SubmitFullscreenQuad( const GraphicsPipeline* pipeline,
                                                  const MaterialExecutor* materialExecutor )
    {
        if ( !IsRecording() )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( !BindGraphicsPipeline( pipeline ) )
            return;

        // Bind Descriptor Sets
        if ( materialExecutor )
        {
            materialExecutor->Apply();
            auto vkBackend = static_cast<VulkanMaterialBackend*>( materialExecutor->GetMaterialBackend().get() );

            if ( !vkBackend->HasDescriptorSets() )
            {
                LOG_WARN( "VulkanRendererAPI: MaterialExecutor has no valid descriptor sets!" );
                return;
            }

            uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            vkBackend->BindDescriptorSets( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                           VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex );
        }

        const auto&   pcBuffer     = materialExecutor->GetPushConstantBuffer();
        VulkanShader* vulkanShader = (VulkanShader*)pipeline->GetSpecification().Shader.get();
        const auto&   pushConstant = vulkanShader->GetShaderPushConstant();
        if ( ( pcBuffer.Size != 0u ) && pushConstant.has_value() )
        {
            const auto& pcInfo = *pushConstant;
            vkCmdPushConstants( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                (VkShaderStageFlags)pcInfo.ShaderStage, 0, (uint32_t)pcBuffer.Size,
                                pcBuffer.Data );
        }

        DrawCounted( 6, 1, 0, 0 );
    }

    void VulkanRendererAPI::SubmitIndexed( const GraphicsPipeline* pipeline, VertexBuffer* vertexBuffer,
                                           IndexBuffer* indexBuffer, uint32_t indexCount, uint32_t firstIndex,
                                           const MaterialExecutor* materialExecutor )
    {
        if ( !IsRecording() || vertexBuffer == nullptr || indexBuffer == nullptr || indexCount == 0 )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( !BindGraphicsPipeline( pipeline ) )
            return;

        // Bind Descriptor Sets (the batch's texture + any UBOs)
        if ( materialExecutor )
        {
            materialExecutor->Apply();
            auto vkBackend = static_cast<VulkanMaterialBackend*>( materialExecutor->GetMaterialBackend().get() );

            // "NO SETS" HAS TWO CAUSES AND ONLY ONE OF THEM IS A DEFECT, and until Ю11 the second one
            // could not happen so both were refused together. A shader that publishes NO LAYOUTS
            // declares no descriptor resources at all — legal Vulkan, and what a purely procedural fill
            // driven by push constants looks like (`UIMatError`); there is simply nothing to bind, and
            // dropping the draw made such a program invisible. A shader that publishes layouts and has
            // no SETS is the real failure — allocation did not happen — and drawing it would sample
            // whatever the last material left bound, so that one is still refused by name.
            if ( !vkBackend->HasDescriptorSets() && !vkBackend->GetLayouts().empty() )
            {
                LOG_WARN( "VulkanRendererAPI::SubmitIndexed: MaterialExecutor has no valid descriptor sets!" );
                return;
            }

            if ( vkBackend->HasDescriptorSets() )
            {
                uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
                vkBackend->BindDescriptorSets( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                               VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex );
            }

            const auto&   pcBuffer     = materialExecutor->GetPushConstantBuffer();
            VulkanShader* vulkanShader = (VulkanShader*)pipeline->GetSpecification().Shader.get();
            const auto&   pushConstant = vulkanShader->GetShaderPushConstant();
            if ( ( pcBuffer.Size != 0u ) && pushConstant.has_value() )
            {
                const auto& pcInfo = *pushConstant;
                vkCmdPushConstants( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                    (VkShaderStageFlags)pcInfo.ShaderStage, 0, (uint32_t)pcBuffer.Size,
                                    pcBuffer.Data );
            }
        }

        VkDeviceSize offsets[] = { 0 };
        auto         vbuffer   = static_cast<API::Vulkan::VulkanVertexBuffer*>( vertexBuffer )->GetVulkanBuffer();
        vkCmdBindVertexBuffers( m_CurrentCommandBuffer, 0, 1, &vbuffer, offsets );

        auto ibuffer = static_cast<API::Vulkan::VulkanIndexBuffer*>( indexBuffer )->GetVulkanBuffer();
        vkCmdBindIndexBuffer( m_CurrentCommandBuffer, ibuffer, 0, VK_INDEX_TYPE_UINT32 );

        // Vertices are addressed absolutely (the batcher bakes base offsets into the indices), so the
        // vertex offset stays 0 and only firstIndex selects this batch's slice of the shared buffer.
        DrawIndexedCounted( indexCount, 1, firstIndex, 0, 0 );
    }

    void VulkanRendererAPI::SubmitLines( const GraphicsPipeline* pipeline, uint32_t vertexCount,
                                         float lineWidth, const MaterialExecutor* materialExecutor )
    {
        if ( !IsRecording() || vertexCount == 0 )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( !BindGraphicsPipeline( pipeline ) )
            return;

        if ( materialExecutor )
        {
            materialExecutor->Apply();
            auto vkBackend = static_cast<VulkanMaterialBackend*>( materialExecutor->GetMaterialBackend().get() );
            if ( !vkBackend->HasDescriptorSets() )
            {
                LOG_WARN( "VulkanRendererAPI::SubmitLines: MaterialExecutor has no valid descriptor sets!" );
                return;
            }
            uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            vkBackend->BindDescriptorSets( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                           VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex );
        }

        // The graphics pipeline enables VK_DYNAMIC_STATE_LINE_WIDTH, so it must be set before drawing.
        // Widths > 1 require the wideLines device feature — MoltenVK lacks it, so force 1.0 there (a
        // wider value is a validation error and gets dropped anyway).
        const float safeWidth = Graphic::RenderConfig::WideLines ? std::clamp( lineWidth, 1.0f, 10.0f ) : 1.0f;
        vkCmdSetLineWidth( m_CurrentCommandBuffer, safeWidth );

        // Vertexless: the DebugLine vertex shader pulls each endpoint from the Lines storage buffer by
        // gl_VertexIndex. Lines topology -> every 2 vertices form one segment.
        DrawCounted( vertexCount, 1, 0, 0 );
    }

    void VulkanRendererAPI::SubmitVertices( const GraphicsPipeline* pipeline, uint32_t vertexCount,
                                            const MaterialExecutor* materialExecutor )
    {
        if ( !IsRecording() || vertexCount == 0 )
            return;
        const auto vulkanPipeline = static_cast<const VulkanPipeline*>( pipeline );
        if ( !BindGraphicsPipeline( pipeline ) )
            return;

        if ( materialExecutor )
        {
            materialExecutor->Apply();
            auto vkBackend = static_cast<VulkanMaterialBackend*>( materialExecutor->GetMaterialBackend().get() );
            if ( !vkBackend->HasDescriptorSets() )
            {
                LOG_WARN( "VulkanRendererAPI::SubmitVertices: MaterialExecutor has no valid descriptor sets!" );
                return;
            }
            uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            vkBackend->BindDescriptorSets( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                           VK_PIPELINE_BIND_POINT_GRAPHICS, frameIndex );

            const auto&   pcBuffer     = materialExecutor->GetPushConstantBuffer();
            VulkanShader* vulkanShader = (VulkanShader*)pipeline->GetSpecification().Shader.get();
            const auto&   pushConstant = vulkanShader->GetShaderPushConstant();
            if ( ( pcBuffer.Size != 0u ) && pushConstant.has_value() )
            {
                // The REFLECTED size, not the buffer's. The push buffer is a fixed 128-byte scratch
                // (MaterialExecutor), so pushing pcBuffer.Size wrote past the range the pipeline layout
                // declares — a validation error the moment any vertexless draw gained a push constant,
                // which the terrain's material row index is the first to do. RenderMesh three hundred
                // lines up already used pcInfo.Size; this is the same line, and it was the odd one out.
                const auto& pcInfo = *pushConstant;
                if ( pcInfo.Size > 0 )
                {
                    vkCmdPushConstants( m_CurrentCommandBuffer, vulkanPipeline->GetVkPipelineLayout(),
                                        (VkShaderStageFlags)pcInfo.ShaderStage, 0, pcInfo.Size, pcBuffer.Data );
                }
            }
        }

        // Vertexless: the vertex shader synthesizes geometry from gl_VertexIndex. For a patch-list
        // (tessellation) pipeline, vertexCount = patchCount * PatchControlPoints.
        DrawCounted( vertexCount, 1, 0, 0 );
    }

    void VulkanRendererAPI::DispatchComputeCull( const ComputePipeline* pipeline, uint32_t groupCountX,
                                                 uint32_t groupCountY, uint32_t groupCountZ )
    {
        if ( !IsRecording() || pipeline == nullptr )
            return;

        const_cast<VulkanPipelineCompute*>( static_cast<const VulkanPipelineCompute*>( pipeline ) )
             ->RecordInFrame( m_CurrentCommandBuffer, groupCountX, groupCountY, groupCountZ );

        // Make the cull's storage writes (visible-instance buffer + the indirect args' instanceCount)
        // available + visible to the VERTEX stage (reads visible[]) and to the DRAW_INDIRECT stage.
        VkMemoryBarrier barrier{ .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                  VK_ACCESS_INDIRECT_COMMAND_READ_BIT };
        vkCmdPipelineBarrier( m_CurrentCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1,
                              &barrier, 0, nullptr, 0, nullptr );
    }

    void VulkanRendererAPI::DispatchComputeInFrame( const ComputePipeline* pipeline, uint32_t groupCountX,
                                                    uint32_t groupCountY, uint32_t groupCountZ )
    {
        if ( !IsRecording() || pipeline == nullptr )
            return;

        // Records bind + a fresh ring descriptor set + dispatch (no layout transitions, no submit).
        const_cast<VulkanPipelineCompute*>( static_cast<const VulkanPipelineCompute*>( pipeline ) )
             ->RecordInFrame( m_CurrentCommandBuffer, groupCountX, groupCountY, groupCountZ );

        // Make this dispatch's storage writes available + visible to the next dispatch's sampler/storage
        // reads-and-writes (e.g. a histogram clear before atomic accumulation) and to a later fragment
        // sample (e.g. tonemap reading the bloom result).
        VkMemoryBarrier barrier{ .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        vkCmdPipelineBarrier( m_CurrentCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                              1, &barrier, 0, nullptr, 0, nullptr );
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

    void VulkanRendererAPI::CopyDepthImage( Image2D* src, Image2D* dst )
    {
        if ( !IsRecording() || src == nullptr || dst == nullptr )
            return;
        // Same extent required (a multisampled target depth vs the single-sample G-buffer would be an
        // illegal copy — skip rather than fault; the grid just stays non-occluded under MSAA until a proper
        // resolve is added).
        if ( src->GetWidth() != dst->GetWidth() || src->GetHeight() != dst->GetHeight() )
            return;

        auto* vsrc = dynamic_cast<VulkanImage2D*>( src );
        auto* vdst = dynamic_cast<VulkanImage2D*>( dst );
        if ( !vsrc || !vdst )
            return;

        vsrc->TransitionLayout( m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
        vdst->TransitionLayout( m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        region.extent         = { src->GetWidth(), src->GetHeight(), 1 };

        vkCmdCopyImage( m_CurrentCommandBuffer, vsrc->GetResource().Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        vdst->GetResource().Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

        // Both back to the depth-attachment layout so the subsequent LOAD passes (forward-over-composite
        // meshes, then the grid/collider overlays) read + test them normally.
        vsrc->TransitionLayout( m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
        vdst->TransitionLayout( m_CurrentCommandBuffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
    }

    void VulkanRendererAPI::SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height )
    {
        if ( !IsRecording() )
            return;
        VkRect2D scissor = { .offset = { x, y }, .extent = { width, height } };
        vkCmdSetScissor( m_CurrentCommandBuffer, 0, 1, &scissor );
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

        const VkResult idle = vkDeviceWaitIdle(
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice() );
        if ( idle != VK_SUCCESS && !NoteIfDeviceLost( idle, "vkDeviceWaitIdle", __FILE__, __LINE__ ) )
            LOG_ERROR( "[Renderer] vkDeviceWaitIdle failed: {}", VkResultToString( idle ) );
    }
    std::shared_ptr<Framebuffer> VulkanRendererAPI::GetCompositeFramebuffer() const
    {
        return m_CompositeFramebuffer.lock();
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
