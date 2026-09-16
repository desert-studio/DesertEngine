#pragma once

#include <Editor/Core/GraphCanvas/GraphCanvas.hpp>

#include <Engine/Assets/Serialization/ShaderGraph.hpp>

// ── WHAT THE SHADER GRAPH ASKS THE CANVAS TO DRAW ────────────────────────────────────────────────────
//
// The shader graph's half of the shared canvas layer, and it is deliberately the SMALLER half: this
// document already issues stable ids of its own (`Document::NextId`, written into every `.dgraph`), which
// is the correct answer to identity and not a thing to replace. Renumbering it would rewrite the links of
// every committed graph in the tree to buy nothing.
//
// So it declares its ids rather than being given them — `ElementLedger::See` — and what it gains from the
// move is the half it got WRONG: the freshness rule. `m_ApplyPositions` was one bool for a whole
// document, true on its first frame and false forever after, so a node created from the palette
// afterwards needed a second `ed::SetNodePosition` call at the creation site to be placed at all. Per
// element, that special case does not exist.
namespace Desert::Editor::Graph
{
    namespace SGF = ::Desert::Assets::Serialization::ShaderGraph;

    /// Builds this frame's plan. Brackets @p ledger's frame itself, for the reason `PlanAnimGraph` does.
    [[nodiscard]] CanvasPlan PlanShaderGraph( const SGF::Document& doc, ElementLedger& ledger );
} // namespace Desert::Editor::Graph
