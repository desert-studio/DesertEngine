
#pragma once

#include <memory>

// Every service is declared, not included (BUILD-FAST-2): this header sits under ECS/System/System.hpp and therefore
// under Core/Scene.hpp, so the 27 service headers it used to include -- and the materials, view frames, RDG and
// asset serialisation behind them -- were compiled into ~450 objects, and an edit to any of them rebuilt all of them.
// A caller that uses a service includes that service's header: Engine/Runtime/Services/<Kind>/<Name>Service.hpp.
namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Runtime
{
    class MeshService;
    class MaterialService;
    class SkyboxService;
    class TextureService;
    class ShaderService;
    class ImageService;
    class FontService;
    class IconService;
    class AnimatedImageService;
    class VideoService;
    class CloudNoiseService;
    class CloudTypeService;
    class CloudModellingService;
    class CloudLayoutService;
    class UIThemeService;
    class FoliageTypeService;
    class WorldParametersService;
    class WeatherPresetService;
    class SubsurfaceProfileService;
    class IesProfileService;
    class RectLightLtcService;
    class PhysicsAssetService;
} // namespace Desert::Runtime

namespace Desert::Runtime
{
    // It is worth coming up with an approach where resources will be automatically registered

    // NOTE:
    // ResourceRegistry is allowed ONLY in runtime/render layer.
    // ECS systems must not resolve GPU resources directly.
    class ResourceRegistry final
    {
    public:
        static MaterialService* GetMaterialService();
        static MeshService*     GetMeshService();
        static SkyboxService*   GetSkyboxService();
        static TextureService*  GetTextureService();
        static ShaderService*   GetShaderService();
        static ImageService*    GetImageService();
        static FontService*     GetFontService();
        static IconService*     GetIconService();

        static AnimatedImageService* GetAnimatedImageService();
        static VideoService*         GetVideoService();
        static CloudNoiseService*    GetCloudNoiseService();
        static CloudTypeService*     GetCloudTypeService();
        // The sculpted hero-cloud bodies (`.dcmv`), slot A of the cloud field's seam.
        static CloudModellingService* GetCloudModellingService();
        // The painted cloud layouts (`.dclayout`) — where the artist says the weather is. Consumed by the
        // placement BAKE and never by a shader, which is the one service here that owns nothing on the GPU
        // by design rather than by accident.
        static CloudLayoutService* GetCloudLayoutService();
        // The UI themes (`.detheme`) a canvas can point at, flattened once per process rather than once
        // per viewport. Owns nothing on the GPU: the one device object a theme reaches (a font atlas)
        // belongs to FontService, which is where this one binds its font paths.
        static UIThemeService* GetUIThemeService();

        // The `.defoliage` types foliage fields name (FO-5): read on demand, held as the assets themselves, so
        // the renderer's cull distance follows a paint-panel edit. Owns nothing on the GPU.
        static FoliageTypeService* GetFoliageTypeService();

        // The `.dfrac` fractures destructible entities name (DST-03b): read on demand, held as the assets.

        // The `.dworldparams` collections scenes name (WTH-PARAMS): read on first ask, held as the assets.
        static WorldParametersService* GetWorldParametersService();
        // The `.dweatherpreset` presets weather zones name (WTH-STATE): read on first ask, held as the assets.
        static WeatherPresetService* GetWeatherPresetService();
        // The `.dsssprofile` profiles materials name and their atlas rows (SSS-PROFILE).
        static SubsurfaceProfileService* GetSubsurfaceProfileService();
        // The .ies profiles the local lights name and their atlas rows (LIGHTS-RECT-IES, UE FIESAtlas).
        static IesProfileService* GetIesProfileService();
        // The rect lights' GGX LTC tables (UE LTCMat / LTCAmp), one constant image made on first use.
        static RectLightLtcService* GetRectLightLtcService();
        // The `.dephysasset` physics assets ragdolls name (RAG1b): read on demand, held as the assets, matched
        // against the mesh's skeleton by GUID.
        static PhysicsAssetService* GetPhysicsAssetService();

        // Clear() every service above. Called once, from Renderer::Shutdown(), i.e. from ~Application and
        // therefore inside main. WHY IT HAS TO BE SAID OUT LOUD: each service is a function-local static,
        // so its destructor runs at __cxa_finalize — after the VkDevice and the VMA allocator are gone —
        // and a GPU wrapper released then writes through a dangling allocator. That was the segfault the
        // headless capture used to exit 139 with. This list lives HERE, beside the getters it has to agree
        // with, so that adding a service and forgetting to release it is one screen of code, not two files.
        static void ClearAll();

        // Hands the three on-demand cloud services the manager they create registry-row shells in (AL1-2).
        // Called by each host right after its AssetManager exists, before any scene resolves a cloud.
        static void BindOnDemandAssets( const std::weak_ptr<Assets::AssetManager>& assets );
    };
} // namespace Desert::Runtime
