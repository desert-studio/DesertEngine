#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Core/Core.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class EditorCamera;
    class Scene;
} // namespace Desert::Core

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class FileExplorerPanel;
    class WorldPartitionPanel;

    // THE CONTENT ROOT'S FILES, WALKED ONCE PER PALETTE BUILD. Several groups list files — Assets (the mesh drop,
    // the folders), Foliage (the palette's meshes and types), Open (the openable assets) — and they sit in
    // different places of the palette's order. Each walking the disk for itself was the second walk the first
    // one existed to avoid; so the registry takes this census once when a build begins
    // (CommandRegistry::OnBuildBegin) and every provider that lists files reads it.
    class AssetFileCensus
    {
    public:
        // Walks the content root (loose files and a mounted .dpak alike, ListFilesRecursive).
        void Take();
        [[nodiscard]] const std::vector<std::filesystem::path>& Files() const
        {
            return m_Files;
        }

    private:
        std::vector<std::filesystem::path> m_Files;
    };

    // The Assets groups of the command palette (and the World Partition conversion, the Scene entry the World
    // Partition window's button runs). Three calls because the group sits in three places of the palette's
    // order. Every slot is the EDITOR'S, read when an entry RUNS.
    class AssetCommands
    {
    public:
        using ActiveCamera = std::function<::Desert::Core::EditorCamera*()>;
        using ShowFolder   = std::function<Common::BoolResultStr( const std::string& )>;

        AssetCommands( FileExplorerPanel* const& explorer, WorldPartitionPanel* const& worldPartition,
                       const std::shared_ptr<::Desert::Core::Scene>&        mainScene,
                       const std::shared_ptr<::Desert::Assets::AssetManager>& assets, const AssetFileCensus& files,
                       ActiveCamera activeCamera, ShowFolder showFolder );

        // Convert to World Partition, rename, Assign Skeleton, the Assets window's entries, asset creation.
        void AppendSelectionCommands( std::vector<PaletteCommand>& commands );
        // The mesh drop, the Import Options window, the Content Browser's commands, the Import Settings.
        void AppendImportCommands( std::vector<PaletteCommand>& commands );
        // "Open folder: <path>", one per folder that holds content.
        void AppendFolderCommands( std::vector<PaletteCommand>& commands );

    private:
        // "Assets | Assign Skeleton…": the selected .skmesh / .anim onto a registered .skeleton
        // (CheckSkeletonAssignment first; a clip is saved after). Named, not a lambda, so the palette entry binds
        // it.
        [[nodiscard]] Common::BoolResultStr AssignSkeletonFromPalette( const std::string& subject,
                                                                       const std::string& skeleton ) const;

        [[nodiscard]] FileExplorerPanel* Explorer() const
        {
            return *m_Explorer;
        }
        [[nodiscard]] WorldPartitionPanel* WorldPartition() const
        {
            return *m_WorldPartition;
        }
        [[nodiscard]] const std::shared_ptr<::Desert::Core::Scene>& MainScene() const
        {
            return *m_MainSceneSlot;
        }
        [[nodiscard]] const std::shared_ptr<::Desert::Assets::AssetManager>& Manager() const
        {
            return *m_AssetsSlot;
        }
        [[nodiscard]] ::Desert::Core::EditorCamera* ActiveEditorCamera() const
        {
            return m_ActiveCamera();
        }
        [[nodiscard]] Common::BoolResultStr ShowFolderInBrowser( const std::string& folder ) const
        {
            return m_ShowFolder( folder );
        }

        FileExplorerPanel* const*                              m_Explorer;
        WorldPartitionPanel* const*                            m_WorldPartition;
        const std::shared_ptr<::Desert::Core::Scene>*          m_MainSceneSlot;
        const std::shared_ptr<::Desert::Assets::AssetManager>* m_AssetsSlot;
        const AssetFileCensus*                                 m_Files;
        ActiveCamera                                           m_ActiveCamera;
        ShowFolder                                             m_ShowFolder;
    };
} // namespace Desert::Editor
