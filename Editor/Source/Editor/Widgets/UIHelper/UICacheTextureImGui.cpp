#include <Editor/Widgets/UIHelper/UICacheTextureImGui.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <vulkan/vulkan.h>

#include <ImGui/backends/imgui_impl_vulkan.h>

#include <unordered_map>

namespace Desert::Editor::UI
{
    const void* UICacheTextureImGui::AddTextureCache( const std::shared_ptr<Graphic::Image2D>& image )
    {
        if ( !image )
            return nullptr;

        if ( Graphic::RendererAPI::GetAPIType() == Graphic::RendererAPIType::Vulkan )
        {
            static std::unordered_map<VkImageView, ImTextureID> g_TextureCache;

            auto        vulkanImage = sp_cast<Graphic::API::Vulkan::VulkanImage2D>( image );
            const auto& res         = vulkanImage->GetResource();

            if ( res.ImageView == VK_NULL_HANDLE )
                return nullptr;

            auto it = g_TextureCache.find( res.ImageView );
            if ( it != g_TextureCache.end() )
            {
                return it->second;
            }

            ImTextureID textureID = ImGui_ImplVulkan_AddTexture( res.Sampler, res.ImageView, res.Layout );

            g_TextureCache[res.ImageView] = textureID;
            return textureID;
        }

        return nullptr;
    }

} // namespace Desert::Editor::UI
