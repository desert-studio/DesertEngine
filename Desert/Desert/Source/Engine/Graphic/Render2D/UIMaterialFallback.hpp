#pragma once

#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <unordered_set>

namespace Desert::Graphic::Render2D
{
    // WHAT A UI DRAW WITH A BROKEN MATERIAL DOES (RDG-FAULT1, UE's default-material fallback).
    //
    // Render2D, the editor UI pass and the runtime present declare one binding block per draw in setup. A UI
    // material whose parameter row does not fit its shader's parameter layout (empty, short, another size, other
    // fields) or whose block the setup validation would refuse for any other reason is refused - which faults the
    // WHOLE UI / present node, so one bad `.demat` blanks every widget on screen. UE's answer to a material that
    // cannot render is the engine's default material in its place, for that draw only. Here that is the
    // UIMaterialCache's error fill (the magenta hatch, the one owner of the default UI material), and the reason
    // is logged ONCE per material.
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

        // Why a parameter row does not satisfy the parameter layout of the shader that records the draw, or empty
        // when it does. @p declaresRowBlock: that shader has the `Materials[]` row block. @p layoutFields: its
        // non-texture parameters in slot order (Core::Formats::MaterialParamSlot). @p rowFields: the same list
        // for the schema the material's row was built from. @p rowSlots: the vec4 slots the row holds.
        // The binding validation sees only "written or not"; a SHORT row, a row of another size, or a row whose
        // fields are not the shader's would be written and read past its end / at the wrong slot - so this is
        // checked first, and only a row that passes is ever written (UIMaterialCache::PrepareDraw).
        [[nodiscard]] static std::string RowFault( const bool                         declaresRowBlock,
                                                   const std::span<const std::string> layoutFields,
                                                   const std::span<const std::string> rowFields,
                                                   const std::size_t                  rowSlots )
        {
            if ( !declaresRowBlock )
                return {};
            if ( layoutFields.empty() )
                return "the shader reads the parameter row but its parameter layout has no field";
            if ( rowSlots != layoutFields.size() )
                return std::format( "the parameter row holds {} slot(s), the shader's parameter layout has {}",
                                    rowSlots, layoutFields.size() );
            for ( std::size_t slot = 0; slot < layoutFields.size(); ++slot )
                if ( slot >= rowFields.size() || rowFields[slot] != layoutFields[slot] )
                    return std::format( "the parameter row has no field '{}' at slot {}", layoutFields[slot],
                                        slot );
            return {};
        }

        // @p fault: empty when the draw's bindings are complete, else the reason (RowFault, then the pass setup's
        // own ValidatePassBindings). Reported once per @p materialName.
        [[nodiscard]] Verdict Admit( const std::string& materialName, const std::string& fault )
        {
            if ( fault.empty() )
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
