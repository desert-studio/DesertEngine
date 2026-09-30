#pragma once

#include <Engine/Core/Formats/MaterialLayout.hpp>

#include <Common/Core/Memory/Buffer.hpp>

#include <cstdint>
#include <string_view>

namespace Desert::Graphic::MaterialBinder
{
    // THE ONE PLACE A MATERIAL'S BYTES ARE PLACED. Every push field a material writes (Transform,
    // MaterialIndex, BoneOffset, WindA/WindB) is looked up BY NAME in the cell's reconciled
    // Core::Formats::MaterialLayout (Graphic::Shader::GetMaterialLayout) — no offset is spelled in C++, so a
    // field moved or added in Common/MaterialTransport.glslh reaches the push bytes with no edit here.
    //
    // A field the cell does not declare (BoneOffset on a static cell, the wind on a non-instanced one) is
    // Absent and nothing is written: the layout says so, not a size test. A value whose size is not the
    // field's compiled size is refused rather than written across its neighbour.

    enum class PushWrite
    {
        Written,
        Absent,
        SizeMismatch,
    };

    struct PushSlot
    {
        PushWrite Status = PushWrite::Absent;
        uint32_t  Offset = 0;
    };

    inline PushSlot ResolvePush( const Core::Formats::MaterialLayout& layout, std::string_view field,
                                 uint32_t size )
    {
        const auto* f = layout.FindPush( field );
        if ( f == nullptr )
            return {};
        if ( f->Size != size )
            return { PushWrite::SizeMismatch, f->Offset };
        return { PushWrite::Written, f->Offset };
    }

    inline PushWrite WritePush( Common::Memory::Buffer& push, const Core::Formats::MaterialLayout& layout,
                                std::string_view field, const void* value, uint32_t size )
    {
        const PushSlot slot = ResolvePush( layout, field, size );
        if ( slot.Status == PushWrite::Written )
            push.Write( value, size, slot.Offset );
        return slot.Status;
    }

    // ── The row ───────────────────────────────────────────────────────────────────
    //
    // The same rule for the `Materials[]` row: a parameter lands at the offset the cell's layout gives it
    // (BuildMaterialLayout, held to the SPIR-V by ReconcileMaterialLayout), found by name. A cell whose
    // layout carries no row gets an empty row, and every write to it misses.

    // The row one material starts from: every parameter at its `Properties ... = default`, at its offset.
    inline Core::Formats::MaterialParamRow DefaultRow( const Core::Formats::MaterialLayout& layout )
    {
        Core::Formats::MaterialParamRow row( layout.RowStride / Core::Formats::kMaterialParamSlotSize );
        for ( const auto& p : layout.Params )
            row[p.Offset / Core::Formats::kMaterialParamSlotSize] = p.Default;
        return row;
    }

    // Write a parameter by name into its whole slot (the components past its width are the generated
    // struct's padding). False when the cell's row has no such parameter; the row is then untouched.
    inline bool WriteRowParam( const Core::Formats::MaterialLayout& layout, Core::Formats::MaterialParamRow& row,
                               std::string_view name, const glm::vec4& value )
    {
        const auto* p = layout.FindParam( name );
        if ( p == nullptr )
            return false;
        const uint32_t slot = p->Offset / Core::Formats::kMaterialParamSlotSize;
        if ( slot >= row.size() )
            return false;
        row[slot] = value;
        return true;
    }

    // The row a material asset asks for: the layout's defaults, then every persisted `{Name, Value}` the
    // layout knows. The ONE builder behind every DataDrivenMaterial.
    template <class NamedValues>
    Core::Formats::MaterialParamRow BuildRow( const Core::Formats::MaterialLayout& layout,
                                              const NamedValues&                   values )
    {
        Core::Formats::MaterialParamRow row = DefaultRow( layout );
        for ( const auto& v : values )
            WriteRowParam( layout, row, v.Name, v.Value );
        return row;
    }
} // namespace Desert::Graphic::MaterialBinder
