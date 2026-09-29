#pragma once

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Desert::Core::Formats
{
    // ── THE ONE PARAMETER TRANSPORT ─────────────────────────────────────────────────────────────────
    //
    // Every material in this engine, whatever built it, delivers its parameters as a ROW of a shared
    // `Materials[]` storage buffer, and each draw names its own row with a push constant. This header is
    // the contract between the two halves of that: DShaderParser emits the GLSL struct, and
    // Graphic::DataDrivenMaterial fills the bytes.
    //
    // WHY THERE IS ONLY ONE TRANSPORT NOW. There used to be two. `MaterialPBR` used the storage buffer;
    // everything born from the DSL's `Properties Binding(n)` — graph materials, the terrain and the SDF
    // text — got a per-material `uniform MaterialUB` block instead. A block IS the parameters, so a
    // material holds exactly ONE set of values, and MeshRenderer keys one material per SHADER: three
    // spheres differing only in a graph parameter all rendered the FIRST one's colour
    // (Resources/Assets/Scenes/MAT_ProbeSharedBlock.desce; the control, the same entity alone, rendered
    // its own). A push constant is snapshotted by Vulkan at record time, so the storage-buffer transport
    // cannot fail that way — which is why it is the one that survived. The cost of the other direction
    // was measured too: one draw per object instead of a batch is 0.756 ms -> 17.121 ms of mesh-pass CPU
    // at a thousand meshes (see Graphic/Materials/Mesh/MeshVertexPath.hpp for the full table).
    //
    // ── WHY EVERY PARAMETER OWNS A WHOLE 16-BYTE SLOT ───────────────────────────────────────────────
    //
    // The C++ packer and the generated GLSL struct are two statements of one layout, and this engine's
    // most expensive defects are all two places that had to agree with nothing checking that they did. A
    // packer that implemented std430 would be exactly that shape: correct member by member, and wrong the
    // first time somebody writes `vec3` next to `float`.
    //
    // So the layout is made trivial instead of checked: the parser pads every parameter out to 16 bytes,
    // and therefore parameter i lives at 16*i in std140 and std430 alike, whatever its type. The C++ side
    // needs no layout rules at all — a row is a `std::vector<glm::vec4>`, one entry per parameter, in
    // schema order.
    //
    // The relation is still asserted rather than trusted: Desert/Tests/Engine/ShaderCacheKey reflects the
    // real SPIR-V of the shipped shaders and holds every member offset to 16*i.

    // One parameter's slot in the row. Not sizeof(glm::vec4) by coincidence: the row IS a vec4 array.
    inline constexpr uint32_t kMaterialParamSlotSize = 16;

    // The reflected name of the storage block the row is written into, shared by every block the DSL
    // generates (MaterialPBR's included). Materials bind by NAME, so this
    // string is what makes "one transport" true at the CPU as well as in the shader.
    inline constexpr const char* kMaterialRowBlockName = "Materials";

    // Where the row index rides. The mesh push block is `mat4 Transform; uint MaterialIndex;`, so 64 —
    // and MaterialPBR::kPushMaterialIndexOffset is defined FROM this rather than beside it, because a
    // second copy of the number is how the two transports would drift apart again.
    inline constexpr uint32_t kMaterialTransformPushOffset = 0;
    inline constexpr uint32_t kMaterialIndexPushOffset     = 64; // sizeof( glm::mat4 )
    // The length of THE push block (Common/MaterialTransport.glslh) in every stage of every pipeline that
    // carries a row: Transform, MaterialIndex, the skinned BoneOffset at 68 and the instanced wind tail at
    // 80..112. One length because the parser injects that header into the fragment stage, so a vertex stage
    // with a longer block of its own would leave one pipeline with two.
    inline constexpr uint32_t kMaterialTransportPushSize = 112;

    // Parameters that occupy a slot. Textures are descriptors, not row bytes, so they are not counted.
    inline uint32_t MaterialParamSlotCount( const ShaderProgramMeta& meta )
    {
        uint32_t count = 0;
        for ( const auto& p : meta.Params )
            if ( !p.IsTexture )
                ++count;
        return count;
    }

    // Which slot a named parameter occupies, or nothing when the shader has no such parameter. OPTIONAL
    // and not "count means absent": a caller that treats a miss as slot 0 would write one parameter's
    // value over another's, which is precisely the class of failure this whole header exists to remove.
    inline std::optional<uint32_t> MaterialParamSlot( const ShaderProgramMeta& meta, std::string_view name )
    {
        uint32_t slot = 0;
        for ( const auto& p : meta.Params )
        {
            if ( p.IsTexture )
                continue;
            if ( p.Name == name )
                return slot;
            ++slot;
        }
        return std::nullopt;
    }

    // The bytes one draw reads. Sized by MaterialParamSlotCount and indexed by MaterialParamSlot.
    using MaterialParamRow = std::vector<glm::vec4>;

    // Every numeric param at its `Properties ... = default` value, in slot order.
    inline MaterialParamRow MaterialParamDefaultRow( const ShaderProgramMeta& meta )
    {
        MaterialParamRow row;
        row.reserve( MaterialParamSlotCount( meta ) );
        for ( const auto& p : meta.Params )
            if ( !p.IsTexture )
                row.push_back( p.Default );
        return row;
    }

    // Write a param by name into its whole slot (the components past its width are the generated struct's
    // padding). False when the shader has no such numeric param; the row is then untouched.
    inline bool SetMaterialParam( const ShaderProgramMeta& meta, MaterialParamRow& row, std::string_view name,
                                  const glm::vec4& value )
    {
        const auto slot = MaterialParamSlot( meta, name );
        if ( !slot || *slot >= row.size() )
            return false;
        row[*slot] = value;
        return true;
    }

    // The row a material asset asks for: schema defaults, then every persisted `{Name, Value}` the schema
    // knows. The ONE builder behind DataDrivenMaterial and MaterialPBR alike, so an authored value reaches
    // the same bytes whichever class draws it.
    template <class NamedValues>
    MaterialParamRow BuildMaterialParamRow( const ShaderProgramMeta& meta, const NamedValues& values )
    {
        MaterialParamRow row = MaterialParamDefaultRow( meta );
        for ( const auto& v : values )
            SetMaterialParam( meta, row, v.Name, v.Value );
        return row;
    }

    // The 2D texture slots a material asset fills, BY THE MANIFEST: every `Texture2D` the schema declares,
    // each with the handle the asset names for it (`handleOf(name)`; 0 when it names none, which the caller
    // binds as the slot's schema default). A cube slot (bound from the environment) and a non-texture asset
    // reference (its own service reads it) are not this walk's. The ONE walk behind DataDrivenMaterial and
    // MaterialPBR alike, so a slot a shader adds is bound without anybody adding a line for it.
    template <class HandleOf, class Bind>
    void ForEachMaterialTextureSlot( const ShaderProgramMeta& meta, HandleOf&& handleOf, Bind&& bind )
    {
        for ( const auto& p : meta.Params )
        {
            if ( !p.IsTexture || p.IsCubeTexture || p.IsAssetRef() )
                continue;
            bind( p, static_cast<uint64_t>( handleOf( p.Name ) ) );
        }
    }
} // namespace Desert::Core::Formats
