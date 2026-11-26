// THE MEDIA MODULE'S STARTUP (UE: MediaAssets' StartupModule). Owns the UI's video service (Runtime::VideoService:
// one MediaPlayer + MediaTexture + MediaAudioOutput per open WebM) and hands it to ResourceRegistry as
// Runtime::IVideoService, which is how Engine code (the asset resolver, the UI canvas) and the hosts reach it
// without naming this module. Called once by the executable that links the module
// (Desert/Launch/LinkedModules.cpp).

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Engine/Runtime/Services/Video/VideoService.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Media
{
    void StartupModule()
    {
        // CONSTRUCTED AFTER THE IMAGE SERVICE, SO DESTROYED BEFORE IT: the clips hold Texture2Ds, which unregister
        // from RenderCore's ImageService in their destructors (ResourceRegistry.cpp,
        // ImageServiceConstructedFirst). In a monolithic executable the statics share one destruction order, so
        // the order is fixed here. Without the image service no clip can be shown, so no video service is provided
        // (the asset resolver and the UI canvas then report every clip by name).
        if ( Runtime::GetImageService() == nullptr )
        {
            LOG_ERROR( "Media: RenderCore has no image service; the video service is not provided" );
            return;
        }
        // THE MODULE OWNS ITS ONE INSTANCE (UE: a module's state lives in its IModuleInterface object): it lives
        // and dies with the module's code, so Engine (ResourceRegistry) keeps only a non-owning pointer and never
        // runs a destructor whose code is in an unloaded DLL.
        static Runtime::VideoService videoService;
        Runtime::ResourceRegistry::ProvideVideoService( &videoService );
    }
} // namespace Desert::Media
