#pragma once

#include <Engine/Graphic/Materials/MaterialBinder.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    // THE LAYOUT A HANDLE WAS RESOLVED AGAINST (UE: an FShaderParameter is bound once per shader map, and a
    // recompile makes a new map). Shader::GetSerial with Shader::GetCodeGeneration names exactly one
    // MaterialLayout (Shader.hpp: the layout is replaced only by a build that bumps the generation), so a handle
    // whose stamp equals the material's current one indexes the current layout; any other stamp - a hot reload, a
    // material moved onto another program, a handle never resolved - is re-resolved by name before the write.
    // {0, 0} is "no program": a handle resolved there misses until the material has one.
    struct MaterialLayoutStamp
    {
        uint64_t Serial     = 0;
        uint64_t Generation = 0;

        [[nodiscard]] bool operator==( const MaterialLayoutStamp& ) const noexcept = default;
    };

    // A row parameter of a DataDrivenMaterial, resolved once per layout (DataDrivenMaterial::ResolveParam) and
    // written by slot (DataDrivenMaterial::SetParam( MaterialParamHandle&, ... )): the per-frame writer holds
    // one per parameter it writes and pays a stamp compare, not a walk of the layout's names.
    struct MaterialParamHandle
    {
        static constexpr uint32_t kNone = ~0u;

        MaterialLayoutStamp Stamp;
        uint32_t            Slot  = kNone; // row slot (Offset / kMaterialParamSlotSize); kNone = not a row value
        uint32_t            Param = kNone; // index in MaterialLayout::Params (its declared type)

        [[nodiscard]] bool IsValue() const noexcept
        {
            return Slot != kNone;
        }
    };

    // A push-constant field of a Material, resolved once per layout (MaterialBinder::ResolvePush) and written at
    // its offset (Material::WritePushField( MaterialPushHandle&, ... )). Status is the resolution's verdict for
    // the size this call site writes; a size mismatch keeps being reported per write, as before.
    struct MaterialPushHandle
    {
        MaterialLayoutStamp       Stamp;
        MaterialBinder::PushWrite Status   = MaterialBinder::PushWrite::Absent;
        uint32_t                  Offset   = 0;
        bool                      Resolved = false;
    };

    // The push fields Material's own per-draw setters write (SetMaterialIndex, SetPushMatrix, ...): one handle
    // each, held by the material, so a draw's push writes index the layout instead of searching it by name.
    struct MaterialPushHandles
    {
        MaterialPushHandle MaterialIndex;
        MaterialPushHandle Transform;
        MaterialPushHandle PrimitiveIndex;
        MaterialPushHandle WindA;
        MaterialPushHandle WindB;
        MaterialPushHandle PrimitiveFlags;
        MaterialPushHandle ShadowSlope;
        MaterialPushHandle BoneOffset;
    };
} // namespace Desert::Graphic
