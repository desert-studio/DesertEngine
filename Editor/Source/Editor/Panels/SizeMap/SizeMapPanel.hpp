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
    // disk, each box in its asset type's Content Browser colour. Opened by the Content Browser command
    // (ContentBrowserCommand::SizeMap, which calls OpenSizeMap); a double-click on a box looks inside it, Up goes
    // back. The tree is SizeMap::Build's (Editor/Core/AssetSizeMap).
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

        // The map of @p assets and of every registry row under each of @p folders (UE: Size Map on a folder maps
        // the assets in it), shown and brought forward. Refused, naming why: a path outside every content root, a
        // folder with no registry content, and anything SizeMap::Build refuses; the refusal is drawn in the
        // window.
        [[nodiscard]] Common::BoolResultStr ShowFor( std::vector<std::filesystem::path> assets,
                                                     std::vector<std::filesystem::path> folders );

    private:
        // Resolves the request against a fresh copy of the registry, so Refresh also takes in a folder's new rows.
        [[nodiscard]] Common::BoolResultStr Rebuild();
        void DrawBox( int node, const SizeMap::Rect& box, int depth );
        void DrawTooltip( int node ) const;

        Common::Utils::AssetRegistry       m_Registry; // the copy the map was built from
        std::vector<std::filesystem::path> m_Assets;   // the request, as asked
        std::vector<std::filesystem::path> m_Folders;
        SizeMap::Tree                      m_Tree;
        int                                m_ViewRoot = -1;
        std::vector<int>                   m_ViewHistory; // the boxes double-clicked into, oldest first
        std::string                        m_Error;
        int                                m_Hovered = -1;
    };

    // The Content Browser's way in: shows the Size Map panel on @p assets and @p folders. Refused, naming why: no
    // Size Map panel in this editor, and anything SizeMapPanel::ShowFor refuses.
    [[nodiscard]] Common::BoolResultStr OpenSizeMap( std::vector<std::filesystem::path> assets,
                                                     std::vector<std::filesystem::path> folders );
} // namespace Desert::Editor
