#pragma once

#include <Editor/Core/AssetSizeMap.hpp>
#include <Editor/Core/IPanel.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // "Size Map" (UE: Content Browser > right-click > Size Map..., AssetManagerEditor's SSizeMap): the selected
    // assets and everything their registry rows name, as nested boxes sized by the bytes each file occupies on
    // disk. Opened by the Content Browser command (ContentBrowserCommand::SizeMap, which calls OpenSizeMap); a
    // double-click on a box looks inside it, Up goes back. The tree is SizeMap::Build's (Editor/Core/AssetSizeMap).
    class SizeMapPanel final : public IPanel
    {
    public:
        SizeMapPanel();
        ~SizeMapPanel() override;
        SizeMapPanel( const SizeMapPanel& )            = delete;
        SizeMapPanel& operator=( const SizeMapPanel& ) = delete;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 760.0f, 560.0f };
        }

        void OnUIRender() override;

        // The map of @p files (asset files, or folders: a folder stands for every registry row under it), shown.
        [[nodiscard]] Common::BoolResultStr ShowFor( const std::vector<std::filesystem::path>& files );

    private:
        [[nodiscard]] Common::BoolResultStr Rebuild();
        void DrawBox( int node, const SizeMap::Rect& box, int depth );
        void DrawTooltip( int node ) const;

        Common::Utils::AssetRegistry m_Registry; // the snapshot the map was built from
        std::vector<std::string>     m_RootKeys;
        SizeMap::Tree                m_Tree;
        int                          m_ViewRoot = -1;
        std::vector<int>             m_ViewHistory; // the boxes double-clicked into, oldest first
        std::string                  m_Error;
        int                          m_Hovered = -1;
    };

    // The Content Browser's way in: shows the Size Map panel on @p files. Refused, naming why: no Size Map panel
    // in this editor, a file outside every content root, and anything SizeMap::Build refuses.
    [[nodiscard]] Common::BoolResultStr OpenSizeMap( const std::vector<std::filesystem::path>& files );
} // namespace Desert::Editor
