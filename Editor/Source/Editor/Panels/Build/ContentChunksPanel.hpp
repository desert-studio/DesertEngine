#pragma once

#include "../IPanel.hpp"

#include <Common/Content/ContentChunks.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // WHICH ARCHIVE EACH PART OF THE PROJECT SHIPS IN, and the place the division is edited.
    //
    // UE's pattern is Project Settings -> Packaging plus the Asset Manager's chunk rules: the author
    // names a few roots per chunk and the tool derives the rest. The pattern is kept; the letter is
    // not — there are no Primary Asset Labels here, because the roots ARE the labels
    // (ContentChunks.hpp says why).
    //
    // THE PANEL HOLDS NO SCHEME. It draws and edits ProjectChunkScheme() — the one session the Build
    // Settings panel and the palette use too — and derives its folder view with the packager's own
    // BuildChunkPlan/ChunkFor, so what it shows is what Build will write. Its members are text fields
    // whose contents are HANDED to that session and a cache of the derived preview; the census
    // Desert/Tests/Editor/BuildSettingsConsumers names every one of them with its consumer.
    class ContentChunksPanel final : public IPanel
    {
    public:
        ContentChunksPanel() : IPanel( "Content Chunks", /*showPanel=*/false )
        {
        }

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 620.0f, 680.0f };
        }

        void OnUIRender() override;

    private:
        void RefreshPreview();

        std::array<char, 128> m_ChunkNameInput{}; // handed to AddChunk / RenameChunk, never stored as a name
        std::array<char, 256> m_KeyInput{};       // handed to AddRoot / AddPin

        // The derived preview of the DRAFT against the live registry, rebuilt when the session's
        // revision moves or on Refresh — never kept as a second scheme.
        std::size_t                                  m_PreviewRevision = static_cast<std::size_t>( -1 );
        std::vector<std::string>                     m_PreviewNames;
        std::vector<Common::Content::ChunkFolderRow> m_PreviewFolders;
        std::size_t                                  m_PreviewUnresolved = 0;
        std::string                                  m_PreviewError;
    };
} // namespace Desert::Editor
