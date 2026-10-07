#include <Editor/Widgets/UIHelper/UICacheTextureImGui.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>

#include <vulkan/vulkan.h>

#include <ImGui/backends/imgui_impl_vulkan.h>

namespace Desert::Editor::UI
{
    UICacheTextureImGui& UICacheTextureImGui::Get()
    {
        static UICacheTextureImGui s_Cache;
        return s_Cache;
    }

    void UICacheTextureImGui::BindPool( VkDescriptorPool pool )
    {
        m_Pool = pool;
    }

    void UICacheTextureImGui::ReleasePool()
    {
        m_Ids.Forget();
        m_Pool = VK_NULL_HANDLE;
    }

    void UICacheTextureImGui::Retire( ImTextureID id ) const
    {
        if ( id == nullptr || m_Pool == VK_NULL_HANDLE )
            return;
        // Deferred, not vkFreeDescriptorSets now: the frames already recorded may still sample the set, and
        // the allocator frees it when the ring comes back round to this frame (as it does for every pool).
        auto context =
             SP_CAST( Graphic::API::Vulkan::VulkanContext, EngineContext::GetInstance().GetRendererContext() );
        context->GetVulkanAllocator()->RT_FreeDescriptorSets( m_Pool,
                                                              { reinterpret_cast<VkDescriptorSet>( id ) } );
    }

    ImTextureID UICacheTextureImGui::AddTextureCache( const std::shared_ptr<Graphic::Image2D>& image )
    {
        if ( !image || m_Pool == VK_NULL_HANDLE )
            return nullptr;

        if ( Graphic::RendererAPI::GetAPIType() != Graphic::RendererAPIType::Vulkan )
            return nullptr;

        const auto  vulkanImage = sp_cast<Graphic::API::Vulkan::VulkanImage2D>( image );
        const auto& res         = vulkanImage->GetResource();
        const auto  retire      = [this]( ImTextureID id ) { Retire( id ); };

        // Released and not re-created: there is nothing to show, and the set written for the old view goes.
        if ( res.ImageView == VK_NULL_HANDLE )
        {
            (void)m_Ids.Drop( image, retire );
            return nullptr;
        }

        return m_Ids.Acquire(
             image, vulkanImage->GetResourceGeneration(),
             [&res]() -> ImTextureID
             {
                 // ImGui samples the image in its fragment shader: the descriptor names the layout of that
                 // declared access, not the layout the image happens to record when it is first shown (a
                 // scene's final image is still a colour attachment before its first frame graph ran). Whoever
                 // renders the image leaves it in this state -- the scene's frame graph extracts its final
                 // image as SampledGraphics.
                 const VkImageLayout sampled = Graphic::API::Vulkan::RdgVulkanLayout(
                      Graphic::RDG::GetAccessState( Graphic::RDG::Access::SampledGraphics ).Layout );
                 return ImGui_ImplVulkan_AddTexture( res.Sampler, res.ImageView, sampled );
             },
             retire );
    }

    std::size_t UICacheTextureImGui::RetireReleased()
    {
        return m_Ids.Sweep( [this]( ImTextureID id ) { Retire( id ); } );
    }

} // namespace Desert::Editor::UI
