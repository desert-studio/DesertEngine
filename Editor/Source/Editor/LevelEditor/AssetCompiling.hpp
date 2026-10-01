#pragma once

// ASSET COMPILING (UE: FAssetCompilingManager). The editor's work on content that runs beside the session rather
// than in the boot: the mesh cook queued on the JobSystem after the reveal (AL1-11), the reload of an asset whose
// cook landed while the scene named it Pending, and the .demat/.shader live reload. Also the palette's two
// actions on the content as a whole: releasing unused assets and rebuilding the content registry.
//
// A member of EditorLayer BY VALUE; the asset manager arrives by reference. It knows nothing of EditorLayer: the
// start-up calls StartBackgroundCook at the reveal, the frame calls DrainBackgroundCook and TickHotReload.

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Import/BackgroundCook.hpp>
#include <Engine/Runtime/AssetHotReload.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Common
{
    class Timestep;
}

namespace Desert::Editor
{
    class AssetCompiling
    {
    public:
        explicit AssetCompiling( std::shared_ptr<Assets::AssetManager>& assetManager )
             : m_AssetManager( assetManager )
        {
        }

        // The startup mesh cook, queued once at the reveal; see Editor/Import/BackgroundCook.hpp.
        void StartBackgroundCook();
        // Applies the cooks that landed since the last frame (main thread).
        void DrainBackgroundCook();
        // Picks up edited .demat/.shader files; runs BEFORE scene rendering so a shader-triggered pipeline
        // invalidation never touches an in-recording frame.
        void TickHotReload( const Common::Timestep& ts, Desert::Core::Scene* activeScene );

        // "Release unused assets", "Rebuild Content Registry".
        void AppendActionCommands( std::vector<PaletteCommand>& commands );

        // The queue the status bar reads (empty until the reveal).
        [[nodiscard]] const std::unique_ptr<BackgroundCookQueue>& CookQueue() const
        {
            return m_BackgroundCook;
        }

    private:
        void ReloadRecookedMesh( const std::filesystem::path& source );

        std::shared_ptr<Assets::AssetManager>& m_AssetManager;

        std::unique_ptr<BackgroundCookQueue>  m_BackgroundCook;
        std::chrono::steady_clock::time_point m_BackgroundCookStart;
        std::size_t                           m_BackgroundCookChanged  = 0;
        std::size_t                           m_BackgroundCookFailed   = 0;
        bool                                  m_BackgroundCookReported = false;
        Runtime::AssetHotReload               m_AssetHotReload; // .demat/.shader live reload
    };
} // namespace Desert::Editor
