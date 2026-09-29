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
        if ( !f )
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
} // namespace Desert::Graphic::MaterialBinder
