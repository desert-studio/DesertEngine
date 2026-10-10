#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief The entries an asset editor adds to the toolbar every asset editor has (UE's FToolBarBuilder, filled
     * through FAssetEditorToolkit::AddToolbarExtender).
     *
     * THE FRAME OWNS THE COMMON PART. Save and Browse are drawn by the asset-editor frame (DocumentWellView) for
     * every document whose subject is an asset, from ISubjectDocument::SaveDocument / GetDiskState and the
     * subject's file — no editor repeats them. An editor only APPENDS its own entries, in its own section, through
     * ISubjectDocument::ExtendToolbar; the frame draws them after a separator, in the order they were added.
     *
     * DATA, NOT DRAWING. An entry is a label and closures, so the frame is the one place that decides how a
     * toolbar button looks (LevelToolbar::ToolbarButton, the level toolbar's own face) and an editor cannot draw a
     * second style of button into the strip.
     */
    class AssetEditorToolbar
    {
    public:
        struct Choice
        {
            std::string           Label;
            std::function<void()> Run;
        };

        struct Entry
        {
            enum class Kind
            {
                Separator,
                Button, // Run on click; Checked (optional) draws it pressed
                Combo   // a drop-down of Choices; Current is the label it shows, Chosen the index ticked
            };

            Kind                  Type = Kind::Button;
            std::string           Icon;
            std::string           Label;
            std::string           Tooltip;
            std::function<void()> Run;
            std::function<bool()> Checked;
            std::function<bool()> Enabled;
            std::vector<Choice>   Choices;
            std::string           Current;
            std::size_t           Chosen = 0;
        };

        void AddSeparator()
        {
            Entry entry;
            entry.Type = Entry::Kind::Separator;
            m_Entries.push_back( std::move( entry ) );
        }

        void AddButton( std::string icon, std::string label, std::string tooltip, std::function<void()> run,
                        std::function<bool()> checked = {}, std::function<bool()> enabled = {} )
        {
            Entry entry;
            entry.Type    = Entry::Kind::Button;
            entry.Icon    = std::move( icon );
            entry.Label   = std::move( label );
            entry.Tooltip = std::move( tooltip );
            entry.Run     = std::move( run );
            entry.Checked = std::move( checked );
            entry.Enabled = std::move( enabled );
            m_Entries.push_back( std::move( entry ) );
        }

        void AddCombo( std::string icon, std::string current, std::string tooltip, std::vector<Choice> choices,
                       std::size_t chosen )
        {
            Entry entry;
            entry.Type    = Entry::Kind::Combo;
            entry.Icon    = std::move( icon );
            entry.Current = std::move( current );
            entry.Tooltip = std::move( tooltip );
            entry.Choices = std::move( choices );
            entry.Chosen  = chosen;
            m_Entries.push_back( std::move( entry ) );
        }

        [[nodiscard]] const std::vector<Entry>& Entries() const
        {
            return m_Entries;
        }

    private:
        std::vector<Entry> m_Entries;
    };
} // namespace Desert::Editor
