#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/ShaderGraph.hpp>

namespace Desert::Assets
{
    /**
     * @brief A shader graph on disk (`.dgraph`), in the engine's ONE asset system.
     *
     * WHY IT EXISTS AT ALL, since nothing at runtime reads a graph. The value is the HANDLE, not the
     * payload: `Editor::NodeGraphPanel` was the editor's last window that edited something without being
     * a document, and a document is keyed on a subject whose owner is a 64-bit id that resolves back to
     * the data. Refusal U7-2 named four things in the way of that; three of them were this class not
     * existing, and the fourth — `New`/`Load`/double-click replacing the open graph with no prompt —
     * stops being expressible once one window is one graph.
     *
     * IT PARSES, AND THE REFUSAL IS THE POINT. `Load()` could have kept the bytes and let the panel parse
     * them, which would have been fewer lines and would have made a malformed `.dgraph` open an empty
     * window with no word about why. A graph that will not parse is an ERROR carrying reflect-cpp's own
     * message, so the document is never created and the browser says which file and what was wrong.
     *
     * WHAT IT DOES NOT DO: bring the graph up to the current node catalogue. That needs
     * `Editor::ShaderGraph::Specs()`, it is an authoring act rather than a format one, and doing it here
     * would silently rewrite an artist's graph on a load that nothing asked to modify — see the note on
     * Serialization/ShaderGraph.hpp for where the line is drawn and why.
     */
    class ShaderGraphAsset final : public AssetBase
    {
    public:
        ShaderGraphAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and parses the file. Missing, empty or malformed is an ERROR carrying the reason — never
        /// a quietly substituted empty graph, which would open as a blank canvas over a file that has
        /// content in it and invite the artist to save over their own work.
        Common::BoolResultStr Load() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Serialization::ShaderGraph::Document& GetData() const
        {
            return m_Data;
        }

        /// What to show in a slot and in the document's title bar: the graph's own `Name` when it has one,
        /// the file's stem when it does not.
        [[nodiscard]] const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        /// Bumped by every successful Load, monotonically per instance. The open document holds the
        /// revision it read, so a graph reloaded from disk under it is visible as a change rather than as
        /// two silently diverging copies.
        [[nodiscard]] uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::ShaderGraph;
        }

        /// Writes a graph to disk, creating the directory if needed. Static because saving is what CREATES
        /// an asset: writing through an instance would mean an instance had to exist for a file that does
        /// not.
        static Common::BoolResultStr Save( const Common::Filepath&                        filepath,
                                           const Serialization::ShaderGraph::Document& doc );

    private:
        Serialization::ShaderGraph::Document m_Data;
        std::string                          m_DisplayName;
        bool                                 m_Ready    = false;
        uint32_t                             m_Revision = 0;
    };
} // namespace Desert::Assets
