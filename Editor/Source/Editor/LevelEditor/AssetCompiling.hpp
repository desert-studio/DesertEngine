#pragma once

// ASSET COMPILING (UE: FAssetCompilingManager). The editor's work on content that runs beside the session rather
// than in the boot: the count of on-demand cooks a load's DDC miss started (CookMeshSource; nothing is cooked at
// startup, UE's editor does not cook the content tree either) and the .demat/.shader live reload. Also the
// palette's two actions on the content as a whole: releasing unused assets and rebuilding the content registry.
//
// A member of EditorLayer BY VALUE; the asset manager arrives by reference. It knows nothing of EditorLayer: the
// frame calls TickHotReload.

#include <Editor/Core/CommandPalette.hpp>
#include <Engine/Runtime/AssetHotReload.hpp>

#include <cstddef>
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

namespace Desert::Animation
{
    class AnimationLibrary;
}
namespace Common
{
    class Timestep;
}

namespace Desert::Editor
{
    class FileExplorerPanel;
    class ImportManager;

    class AssetCompiling
    {
    public:
        AssetCompiling( std::shared_ptr<Assets::AssetManager>&        assetManager,
                        std::unique_ptr<Animation::AnimationLibrary>& animationLibrary,
                        std::unique_ptr<ImportManager>&               importManager )
             : m_AssetManager( assetManager ), m_AnimationLibrary( animationLibrary ),
               m_ImportManager( importManager )
        {
        }

        // Picks up edited .demat/.shader files; runs BEFORE scene rendering so a shader-triggered pipeline
        // invalidation never touches an in-recording frame.
        void TickHotReload( const Common::Timestep& ts, Desert::Core::Scene* activeScene );

        // "Release unused assets", "Rebuild Content Registry".
        void AppendActionCommands( std::vector<PaletteCommand>& commands );

        // File > Rebuild Cooked Assets: force re-cook of Cooked/ from sources, re-index the clips, drop the
        // active scene's cached material instances and refresh the asset panel.
        void RebuildCookedAssets( Desert::Core::Scene* activeScene, FileExplorerPanel* fileExplorer );

        // Assets still compiling beside the session (UE FAssetCompilingManager::GetNumRemainingAssets): the
        // on-demand mesh cooks running now (MeshSourceCooksRunning).
        [[nodiscard]] std::size_t RemainingAssets() const;

    private:
        std::shared_ptr<Assets::AssetManager>&        m_AssetManager;
        std::unique_ptr<Animation::AnimationLibrary>& m_AnimationLibrary;
        std::unique_ptr<ImportManager>&               m_ImportManager;

        Runtime::AssetHotReload m_AssetHotReload; // .demat/.shader live reload
    };
} // namespace Desert::Editor
