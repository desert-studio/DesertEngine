#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>

namespace Desert::Graphic::Render2D
{
    // WHAT A UI DRAW WITH A BROKEN MATERIAL DOES (RDG-FAULT1, UE's default-material fallback).
    //
    // Render2D, the editor UI pass and the runtime present declare one binding block per draw in setup. A UI
    // material whose shader reads the parameter row (`Materials[]`) but whose row is empty leaves that storage
    // buffer unwritten, and the setup validation refuses it - which faults the WHOLE UI / present node, so one
    // bad `.demat` blanks every widget on screen. UE's answer to a material that cannot render is the engine's
    // default material in its place, for that draw only. Here that is the UIMaterialCache's error fill (the
    // magenta hatch, the one owner of the default UI material), and the reason is logged ONCE per material.
    //
    // CPU-only on purpose: the decision and the once-only report are what the suite pins, without a device.
    class UIMaterialFallback final
    {
    public:
        enum class Verdict
        {
            Draws,              // the material's own bindings are complete
            DefaultFirstReport, // draws the default; this is the one frame the reason is logged
            Default,            // draws the default; already reported
        };

        // @p declaresRowBlock: the material's shader has the `Materials[]` row block. @p rowSlots: the vec4 slots
        // its parameter row holds. A row block with no row is a buffer nothing writes.
        [[nodiscard]] static bool RowFillsItsBlock( const bool declaresRowBlock, const std::size_t rowSlots )
        {
            return !declaresRowBlock || rowSlots > 0;
        }

        [[nodiscard]] Verdict Admit( const std::string& materialName, const bool declaresRowBlock,
                                     const std::size_t rowSlots )
        {
            if ( RowFillsItsBlock( declaresRowBlock, rowSlots ) )
                return Verdict::Draws;
            return m_Reported.insert( materialName ).second ? Verdict::DefaultFirstReport : Verdict::Default;
        }

        [[nodiscard]] std::size_t ReportedCount() const
        {
            return m_Reported.size();
        }

    private:
        std::unordered_set<std::string> m_Reported;
    };
} // namespace Desert::Graphic::Render2D
