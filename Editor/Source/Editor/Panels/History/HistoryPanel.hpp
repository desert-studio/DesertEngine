#pragma once

#include "../IPanel.hpp"

namespace Desert::Editor
{
    // Visible undo/redo history: lists the CommandHistory stacks (oldest at top), marks the current state,
    // and lets you jump by clicking an entry (undoes/redoes down/up to it). Hidden by default; View -> History.
    class HistoryPanel final : public IPanel
    {
    public:
        HistoryPanel() : IPanel( "History", /*showPanel=*/false )
        {
        }

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 300.0f, 420.0f };
        }
        void OnUIRender() override;
    };
} // namespace Desert::Editor
