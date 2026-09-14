
#pragma once

#include <Engine/Assets/AssetManager.hpp>

#include "Services/Mesh/MeshService.hpp"
#include "Services/Material/MaterialService.hpp"
#include "Services/Skybox/SkyboxService.hpp"
#include "Services/Texture/TextureService.hpp"
#include "Services/Shader/ShaderService.hpp"
#include "Services/Image/ImageService.hpp"
#include "Services/Font/FontService.hpp"
#include "Services/Icon/IconService.hpp"
#include "Services/AnimatedImage/AnimatedImageService.hpp"
#include "Services/Video/VideoService.hpp"
#include "Services/CloudNoise/CloudNoiseService.hpp"
#include "Services/CloudType/CloudTypeService.hpp"
#include "Services/CloudModelling/CloudModellingService.hpp"
#include "Services/CloudLayout/CloudLayoutService.hpp"
#include "Services/UITheme/UIThemeService.hpp"

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

        // Clear() every service above. Called once, from Renderer::Shutdown(), i.e. from ~Application and
        // therefore inside main. WHY IT HAS TO BE SAID OUT LOUD: each service is a function-local static,
        // so its destructor runs at __cxa_finalize — after the VkDevice and the VMA allocator are gone —
        // and a GPU wrapper released then writes through a dangling allocator. That was the segfault the
        // headless capture used to exit 139 with. This list lives HERE, beside the getters it has to agree
        // with, so that adding a service and forgetting to release it is one screen of code, not two files.
        static void ClearAll();
    };
} // namespace Desert::Runtime
