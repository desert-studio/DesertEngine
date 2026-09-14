#include "ResourceRegistry.hpp"

namespace Desert::Runtime
{
    MeshService* ResourceRegistry::GetMeshService()
    {
        static MeshService meshService;
        return &meshService;
    }

    SkyboxService* ResourceRegistry::GetSkyboxService()
    {
        static SkyboxService skyboxService;
        return &skyboxService;
    }

    TextureService* ResourceRegistry::GetTextureService()
    {
        static TextureService textureService;
        return &textureService;
    }

    ShaderService* ResourceRegistry::GetShaderService()
    {
        static ShaderService shaderService;
        return &shaderService;
    }

    ImageService* ResourceRegistry::GetImageService()
    {
        static ImageService imageService;
        return &imageService;
    }

    MaterialService* ResourceRegistry::GetMaterialService()
    {
        static MaterialService materialService;
        return &materialService;
    }

    FontService* ResourceRegistry::GetFontService()
    {
        static FontService fontService;
        return &fontService;
    }

    IconService* ResourceRegistry::GetIconService()
    {
        static IconService iconService;
        return &iconService;
    }

    AnimatedImageService* ResourceRegistry::GetAnimatedImageService()
    {
        static AnimatedImageService animatedImageService;
        return &animatedImageService;
    }

    VideoService* ResourceRegistry::GetVideoService()
    {
        static VideoService videoService;
        return &videoService;
    }

    CloudNoiseService* ResourceRegistry::GetCloudNoiseService()
    {
        static CloudNoiseService cloudNoiseService;
        return &cloudNoiseService;
    }

    CloudTypeService* ResourceRegistry::GetCloudTypeService()
    {
        static CloudTypeService cloudTypeService;
        return &cloudTypeService;
    }

    CloudModellingService* ResourceRegistry::GetCloudModellingService()
    {
        static CloudModellingService cloudModellingService;
        return &cloudModellingService;
    }

    CloudLayoutService* ResourceRegistry::GetCloudLayoutService()
    {
        static CloudLayoutService cloudLayoutService;
        return &cloudLayoutService;
    }

    UIThemeService* ResourceRegistry::GetUIThemeService()
    {
        static UIThemeService uiThemeService;
        return &uiThemeService;
    }

    void ResourceRegistry::ClearAll()
    {
        // Order matters in one place only: the ImageService holds the VkImages that the material, skybox,
        // font, icon and cloud services hand out views of, so it goes LAST.
        GetMaterialService()->Clear();
        GetMeshService()->Clear();
        GetSkyboxService()->Clear();
        GetTextureService()->Clear();
        GetShaderService()->Clear();
        GetFontService()->Clear();
        GetIconService()->Clear();
        GetAnimatedImageService()->Clear();
        GetVideoService()->Clear();
        GetCloudNoiseService()->Clear();
        GetCloudTypeService()->Clear();
        GetCloudModellingService()->Clear();
        GetCloudLayoutService()->Clear();
        GetUIThemeService()->Clear();
        GetImageService()->Clear();
    }

} // namespace Desert::Runtime
