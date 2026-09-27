#include "ResourceRegistry.hpp"

namespace Desert::Runtime
{
    namespace
    {
        // CONSTRUCTED FIRST, SO DESTROYED LAST. A Texture2D unregisters its image from the ImageService
        // in its destructor (Texture.hpp), and the services below hold Texture2Ds in function-local
        // statics. Statics are destroyed in the reverse order their construction COMPLETED
        // ([basic.start.term]), and a service is usually first asked for before any image exists -- so
        // without this call the texture service would be built before the image service, destroyed
        // after it, and every texture it still held would unregister into a destroyed object. Renderer::
        // Shutdown empties them through ClearAll() first, but a process that never reaches it (a tool, a
        // test, an early exit) still runs the static destructors. Touching the ImageService getter
        // before the holder's own static makes the order a property of the code rather than of which
        // service somebody happened to ask for first.
        void ImageServiceConstructedFirst()
        {
            (void)ResourceRegistry::GetImageService();
        }
    } // namespace

    MeshService* ResourceRegistry::GetMeshService()
    {
        static MeshService meshService( MakeGpuMeshUploader() );
        return &meshService;
    }

    SkyboxService* ResourceRegistry::GetSkyboxService()
    {
        static SkyboxService skyboxService;
        return &skyboxService;
    }

    TextureService* ResourceRegistry::GetTextureService()
    {
        ImageServiceConstructedFirst(); // holds Texture2Ds -- see the function
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
        ImageServiceConstructedFirst(); // holds Texture2Ds -- see the function
        static AnimatedImageService animatedImageService;
        return &animatedImageService;
    }

    VideoService* ResourceRegistry::GetVideoService()
    {
        ImageServiceConstructedFirst(); // holds Texture2Ds -- see the function
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

    LandscapeLayerInfoService* ResourceRegistry::GetLandscapeLayerInfoService()
    {
        static LandscapeLayerInfoService landscapeLayerInfoService;
        return &landscapeLayerInfoService;
    }

    LandscapeGrassTypeService* ResourceRegistry::GetLandscapeGrassTypeService()
    {
        static LandscapeGrassTypeService landscapeGrassTypeService;
        return &landscapeGrassTypeService;
    }

    void ResourceRegistry::BindOnDemandAssets( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        GetCloudNoiseService()->BindAssetManager( assets );
        GetCloudModellingService()->BindAssetManager( assets );
        GetCloudLayoutService()->BindAssetManager( assets );
        GetTextureService()->BindAssetManager( assets );
        GetMaterialService()->BindAssetManager( assets );
        GetMeshService()->BindAssetManager( assets );
        GetCloudTypeService()->BindAssetManager( assets );
        GetUIThemeService()->BindAssetManager( assets );
        GetLandscapeLayerInfoService()->BindAssetManager( assets );
        GetLandscapeGrassTypeService()->BindAssetManager( assets );
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
        GetLandscapeLayerInfoService()->Clear();
        GetLandscapeGrassTypeService()->Clear();
        GetImageService()->Clear();
    }

} // namespace Desert::Runtime
