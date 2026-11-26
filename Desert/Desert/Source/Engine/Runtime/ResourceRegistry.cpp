#include "ResourceRegistry.hpp"

#include <memory>
#include <Engine/Runtime/Services/AnimatedImage/AnimatedImageService.hpp>
#include <Engine/Runtime/Services/CloudLayout/CloudLayoutService.hpp>
#include <Engine/Runtime/Services/CloudModelling/CloudModellingService.hpp>
#include <Engine/Runtime/Services/CloudNoise/CloudNoiseService.hpp>
#include <Engine/Runtime/Services/CloudType/CloudTypeService.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Runtime/Services/Icon/IconService.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Engine/Runtime/Services/Lights/IesProfileService.hpp>
#include <Engine/Runtime/Services/Lights/RectLightLtcService.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/Skybox/SkyboxService.hpp>
#include <Engine/Runtime/Services/Subsurface/SubsurfaceProfileService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>
#include <Engine/Runtime/Services/UITheme/UIThemeService.hpp>
#include <Engine/Runtime/Services/IVideoService.hpp>
#include <Engine/Runtime/Services/Weather/WeatherPresetService.hpp>
#include <Engine/Runtime/Services/WorldParameters/WorldParametersService.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Runtime/Services/Physics/PhysicsAssetService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

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
        // test, an early exit) still runs the static destructors. Touching the ImageService getter (RenderCore's
        // Runtime::GetImageService) before the holder's own static makes the order a property of the code rather than of which
        // service somebody happened to ask for first.
        void ImageServiceConstructedFirst()
        {
            (void)GetImageService();
        }

        // The Media module's video service, handed over by its StartupModule (ProvideVideoService); the module
        // owns the instance, so it is destroyed with the module, before the RenderCore image service it holds
        // views of.
        IVideoService* s_VideoService = nullptr;
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

    IVideoService* ResourceRegistry::GetVideoService()
    {
        return s_VideoService;
    }

    void ResourceRegistry::ProvideVideoService( IVideoService* service )
    {
        s_VideoService = service;
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

    PhysicsAssetService* ResourceRegistry::GetPhysicsAssetService()
    {
        static PhysicsAssetService physicsAssetService;
        return &physicsAssetService;
    }

    WorldParametersService* ResourceRegistry::GetWorldParametersService()
    {
        static WorldParametersService worldParametersService;
        return &worldParametersService;
    }

    WeatherPresetService* ResourceRegistry::GetWeatherPresetService()
    {
        static WeatherPresetService weatherPresetService;
        return &weatherPresetService;
    }

    SubsurfaceProfileService* ResourceRegistry::GetSubsurfaceProfileService()
    {
        static SubsurfaceProfileService subsurfaceProfileService;
        return &subsurfaceProfileService;
    }

    IesProfileService* ResourceRegistry::GetIesProfileService()
    {
        static IesProfileService iesProfileService;
        return &iesProfileService;
    }

    RectLightLtcService* ResourceRegistry::GetRectLightLtcService()
    {
        static RectLightLtcService rectLightLtcService;
        return &rectLightLtcService;
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
        GetSkyboxService()->BindAssetManager( assets );
        for ( const Core::ModuleService& service : Core::ModuleHooks().Services )
            service.Bind( assets );
        GetPhysicsAssetService()->BindAssetManager( assets );
        GetWorldParametersService()->BindAssetManager( assets );
        GetWeatherPresetService()->BindAssetManager( assets );
        GetSubsurfaceProfileService()->BindAssetManager( assets );
    }

    void ResourceRegistry::ClearAll()
    {
        // Order matters in one place only: the ImageService holds the VkImages that the material, skybox,
        // font, icon and cloud services hand out views of, so it goes LAST.
        GetMaterialService()->Clear();
        GetWorldParametersService()->Clear();
        GetWeatherPresetService()->Clear();
        GetSubsurfaceProfileService()->Clear();
        GetIesProfileService()->Clear();
        GetRectLightLtcService()->Clear();
        GetMeshService()->Clear();
        GetSkyboxService()->Clear();
        GetTextureService()->Clear();
        GetShaderService()->Clear();
        GetFontService()->Clear();
        GetIconService()->Clear();
        GetAnimatedImageService()->Clear();
        if ( s_VideoService != nullptr )
            s_VideoService->Clear();
        GetCloudNoiseService()->Clear();
        GetCloudTypeService()->Clear();
        GetCloudModellingService()->Clear();
        GetCloudLayoutService()->Clear();
        GetUIThemeService()->Clear();
        for ( const Core::ModuleService& service : Core::ModuleHooks().Services )
            service.Clear();
        GetPhysicsAssetService()->Clear();
        GetImageService()->Clear();
    }

} // namespace Desert::Runtime
