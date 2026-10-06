#pragma once

#include <cstddef>
#include <cstdint>
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

        // The one log line for a material that falls back: names the material ASSET first (many materials share a
        // shader), then its shader, the reason and the default it draws instead.
        [[nodiscard]] static std::string Report( const std::string& materialName, const std::string& shaderName,
                                                 const std::string& fault, const std::string& defaultName )
        {
            return std::format( "[UIMaterial] material '{}' (shader '{}') cannot draw ({}), so its draws use the "
                                "default UI material '{}' instead of failing the whole UI pass",
                                materialName, shaderName, fault, defaultName );
        }

        // @p fault: empty when the draw's bindings are complete, else the reason (RowFault, then the pass setup's
        // own ValidatePassBindings). Reported once per @p materialName.
        [[nodiscard]] Verdict Admit( const std::string& materialName, const std::string& fault )
        {
            if ( fault.empty() )
                return Verdict::Draws;
            return m_Reported.insert( materialName ).second ? Verdict::DefaultFirstReport : Verdict::Default;
        }

        // THE DECISION FOR ONE MATERIAL DRAW - UIMaterialCache::DrawableOrDefault is this over the real entries,
        // called by Render2D::Resolve once per draw in the setup (PreparedDraws). @p entry: the entry the command
        // resolved to (`.Error`, `.AssetName`). @p errorEntry(): the default UI material's entry, null when it
        // cannot be built. @p prepare( const Entry& ) -> std::string: prepares the draw (UIMaterialCache::
        // PrepareDraw: row, push, validation) and returns why it cannot record, empty when it can. @p shaderOf(
        // const Entry& ) -> std::string names its shader for the report; @p log( const std::string& ) takes the
        // ONE report per material. Returns the entry the draw binds: @p entry, the default in its place, or null
        // when even the default cannot draw (the draw is then skipped in setup and exec alike).
        template <class Entry, class ErrorEntry, class Prepare, class ShaderOf, class Log>
        [[nodiscard]] const Entry* Choose( const Entry& entry, ErrorEntry&& errorEntry, Prepare&& prepare,
                                           ShaderOf&& shaderOf, Log&& log, const std::string& defaultName )
        {
            if ( !entry.Error )
            {
                const std::string fault = prepare( entry );
                switch ( Admit( entry.AssetName, fault ) )
                {
                    case Verdict::Draws:
                        return &entry;
                    case Verdict::DefaultFirstReport:
                    {
                        log( Report( entry.AssetName, shaderOf( entry ), fault, defaultName ) );
                        break;
                    }
                    case Verdict::Default:
                        break;
                }
            }
            const Entry* fallback = entry.Error ? &entry : errorEntry();
            if ( !fallback )
            {
                return nullptr;
            }
            const std::string fault = prepare( *fallback );
            if ( fault.empty() )
            {
                return fallback;
            }
            if ( Admit( defaultName, fault ) == Verdict::DefaultFirstReport )
            {
                log( std::format( "[UIMaterial] the default UI material '{}' cannot draw either ({}); those draws "
                                  "are skipped",
                                  defaultName, fault ) );
            }
            return nullptr;
        }

        // A UI MATERIAL FOLLOWS ITS SHADER'S HOT RELOAD. @p entry records the reload generation of the shader it
        // was built from (`.ShaderGeneration`, Shader::GetReloadGeneration - the key ShaderBindingLayoutCache
        // re-derives a layout on). When @p generation differs, @p rebuild( entry ) rebuilds it from the reloaded
        // shader (pipeline + runtime material + row from the new parameter layout) and returns whether it did;
        // without this the entry keeps the old schema's row, RowFault refuses it, and the material draws the
        // default until restart. The generation is recorded either way: a failed rebuild is tried once per reload,
        // not once per frame. Returns whether the entry was rebuilt.
        template <class Entry, class Rebuild>
        static bool RebuildIfReloaded( Entry& entry, const uint32_t generation, Rebuild&& rebuild )
        {
            if ( entry.ShaderGeneration == generation )
            {
                return false;
            }
            const bool rebuilt     = rebuild( entry );
            entry.ShaderGeneration = generation;
            return rebuilt;
        }

        [[nodiscard]] std::size_t ReportedCount() const
        {
            return m_Reported.size();
        }

    private:
        std::unordered_set<std::string> m_Reported;
    };
} // namespace Desert::Graphic::Render2D
