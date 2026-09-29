#pragma once

#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::Formats
{
    // ── THE ONE LAYOUT OF A MATERIAL ────────────────────────────────────────────────────────────────
    //
    // Where every byte and binding a material fills lives: the parameter row (name -> offset/type), the
    // texture slots (name -> binding/default) and the push block (field -> offset/size/stages). It is the
    // FShaderParametersMetadata of this engine: one producer, and every consumer fills it BY NAME.
    //
    // ONE PRODUCER, TWO WITNESSES. BuildMaterialLayout derives Params and Textures from the parsed template,
    // and DShaderParser writes the generated GLSL FROM that result — so the generator cannot declare a
    // parameter the layout does not have. The push block is not the template's (Common/MaterialTransport.glslh
    // declares it), so it enters from the compiled SPIR-V: ReconcileMaterialLayout takes every stage's
    // reflection (Graphic::API::Vulkan::ShaderReflection::ReflectMaterialStage) and both fills the push
    // fields and refuses any disagreement — a stage whose row, texture binding or push block differs from
    // the layout or from another stage is an error naming the template and the cell. A new push field or
    // a new parameter therefore reaches the layout with no C++ edit; a stage with a 72-byte block beside a
    // 68-byte one (the T1b regression) is refused instead of merged into "the larger wins".
    //
    // Data only: no Vulkan, no SPIR-V library. The reflection side lives with the rest of the reflection.

    struct MaterialLayoutParam
    {
        std::string     Name;
        ShaderValueType Type   = ShaderValueType::Float;
        uint32_t        Offset = 0;      // bytes into the row; slot i sits at kMaterialParamSlotSize * i
        uint32_t        Size   = 0;      // bytes of VALUE; the rest of the slot is the generated padding
        glm::vec4       Default{ 0.0f }; // the `Properties ... = default` value the row starts from
    };

    struct MaterialLayoutTexture
    {
        std::string        Name;
        uint32_t           Binding = 0;
        bool               IsCube  = false;
        DefaultTextureKind Default = DefaultTextureKind::White;
    };

    struct MaterialLayoutPushField
    {
        std::string Name;
        uint32_t    Offset = 0;
        uint32_t    Size   = 0;
        ShaderStage Stages = ShaderStage::None;
    };

    struct MaterialLayout
    {
        std::optional<uint32_t>          RowBinding; // empty = the template carries no row
        uint32_t                         RowStride = 0;
        std::vector<MaterialLayoutParam> Params;

        std::vector<MaterialLayoutTexture> Textures;

        // Filled by ReconcileMaterialLayout from the SPIR-V; empty until then.
        std::vector<MaterialLayoutPushField> Push;
        uint32_t                             PushSize   = 0;
        ShaderStage                          PushStages = ShaderStage::None;

        const MaterialLayoutParam* FindParam( std::string_view name ) const
        {
            for ( const auto& p : Params )
                if ( p.Name == name )
                    return &p;
            return nullptr;
        }
        const MaterialLayoutTexture* FindTexture( std::string_view name ) const
        {
            for ( const auto& t : Textures )
                if ( t.Name == name )
                    return &t;
            return nullptr;
        }
        const MaterialLayoutPushField* FindPush( std::string_view name ) const
        {
            for ( const auto& f : Push )
                if ( f.Name == name )
                    return &f;
            return nullptr;
        }
    };

    // Bytes of value a row parameter of this type occupies — the GLSL type DShaderParser declares for it
    // (MaterialLayoutGlslType). Everything outside the float/int family is declared a vec4.
    constexpr uint32_t MaterialParamValueSize( ShaderValueType type )
    {
        switch ( type )
        {
            case ShaderValueType::Float:
            case ShaderValueType::Int:
            case ShaderValueType::Bool:
                return 4;
            case ShaderValueType::Float2:
                return 8;
            case ShaderValueType::Float3:
                return 12;
            default:
                return 16;
        }
    }

    constexpr const char* MaterialLayoutGlslType( ShaderValueType type )
    {
        switch ( type )
        {
            case ShaderValueType::Float:
                return "float";
            case ShaderValueType::Float2:
                return "vec2";
            case ShaderValueType::Float3:
                return "vec3";
            case ShaderValueType::Int:
            case ShaderValueType::Bool:
                return "int";
            default:
                return "vec4";
        }
    }

    // THE builder. Params are the numeric properties in schema order (the walk MaterialParamSlot
    // repeats for MeshRenderer's instance overrides; ShippedShaderPasses holds the two equal), textures every
    // texture property from FirstTexture upward.
    inline MaterialLayout BuildMaterialLayout( const ShaderProgramMeta& meta )
    {
        const MaterialLayoutBindings& bindings = meta.LayoutBindings;
        MaterialLayout                layout;
        if ( bindings.Row )
        {
            for ( const auto& p : meta.Params )
            {
                if ( p.IsTexture )
                    continue;
                layout.Params.push_back( { p.Name, p.Type,
                                           kMaterialParamSlotSize * static_cast<uint32_t>( layout.Params.size() ),
                                           MaterialParamValueSize( p.Type ), p.Default } );
            }
            if ( !layout.Params.empty() )
            {
                layout.RowBinding = bindings.Row;
                layout.RowStride  = kMaterialParamSlotSize * static_cast<uint32_t>( layout.Params.size() );
            }
        }
        if ( bindings.FirstTexture )
        {
            uint32_t binding = *bindings.FirstTexture;
            for ( const auto& p : meta.Params )
                if ( p.IsTexture )
                    layout.Textures.push_back( { p.Name, binding++, p.IsCubeTexture, p.DefaultTexture } );
        }
        return layout;
    }

    // ── What one compiled stage says ────────────────────────────────────────────────────────────────

    struct ReflectedLayoutMember
    {
        std::string Name;
        uint32_t    Offset = 0;
        uint32_t    Size   = 0;
    };

    struct ReflectedMaterialStage
    {
        ShaderStage Stage = ShaderStage::None;

        // The `Materials` block (kMaterialRowBlockName), when this stage declares it.
        std::optional<uint32_t>            RowBinding;
        uint32_t                           RowStride = 0;
        std::vector<ReflectedLayoutMember> RowMembers; // the padding members included

        std::vector<ReflectedLayoutMember> Samplers; // Offset carries the binding; Size unused

        std::optional<uint32_t>            PushSize; // empty = the stage declares no push block
        std::vector<ReflectedLayoutMember> PushMembers;
    };

    constexpr const char* MaterialLayoutStageName( ShaderStage stage )
    {
        switch ( stage )
        {
            case ShaderStage::Vertex:
                return "vertex";
            case ShaderStage::TessControl:
                return "tess-control";
            case ShaderStage::TessEvaluation:
                return "tess-evaluation";
            case ShaderStage::Fragment:
                return "fragment";
            case ShaderStage::Compute:
                return "compute";
            default:
                return "unknown";
        }
    }

    // Checks every stage against the layout and against each other, and fills the push fields from them.
    // Returns one message per disagreement, each naming `templateName`/`cellName`; empty = the layout holds.
    inline std::vector<std::string> ReconcileMaterialLayout( MaterialLayout& layout, std::string_view templateName,
                                                             std::string_view                           cellName,
                                                             const std::vector<ReflectedMaterialStage>& stages )
    {
        std::vector<std::string> errors;
        const auto               fail = [&]( ShaderStage stage, std::string what )
        {
            errors.push_back( std::format( "material '{}' cell '{}' ({} stage): {}", templateName,
                                           cellName.empty() ? "<default>" : cellName,
                                           MaterialLayoutStageName( stage ), what ) );
        };

        // The push block: the first stage that declares one defines it; every other must be identical.
        const ReflectedMaterialStage* pushOwner = nullptr;
        layout.Push.clear();
        layout.PushSize   = 0;
        layout.PushStages = ShaderStage::None;
        for ( const auto& stage : stages )
        {
            if ( !stage.PushSize )
                continue;
            if ( !pushOwner )
            {
                pushOwner       = &stage;
                layout.PushSize = *stage.PushSize;
                for ( const auto& m : stage.PushMembers )
                    layout.Push.push_back( { m.Name, m.Offset, m.Size, ShaderStage::None } );
            }
            else if ( *stage.PushSize != layout.PushSize )
            {
                fail( stage.Stage,
                      std::format( "push block is {} bytes, the {} stage's is {}; one pipeline has one block",
                                   *stage.PushSize, MaterialLayoutStageName( pushOwner->Stage ),
                                   layout.PushSize ) );
                continue;
            }
            else
            {
                bool same = stage.PushMembers.size() == pushOwner->PushMembers.size();
                for ( size_t i = 0; same && i < stage.PushMembers.size(); ++i )
                    same = stage.PushMembers[i].Name == pushOwner->PushMembers[i].Name &&
                           stage.PushMembers[i].Offset == pushOwner->PushMembers[i].Offset;
                if ( !same )
                {
                    fail( stage.Stage, std::format( "push block fields differ from the {} stage's",
                                                    MaterialLayoutStageName( pushOwner->Stage ) ) );
                    continue;
                }
            }
            layout.PushStages = static_cast<ShaderStage>( static_cast<uint32_t>( layout.PushStages ) |
                                                          static_cast<uint32_t>( stage.Stage ) );
        }
        for ( auto& f : layout.Push )
            f.Stages = layout.PushStages;

        for ( const auto& stage : stages )
        {
            // The row: what the generator declared must be what the compiler laid out.
            if ( stage.RowBinding )
            {
                if ( !layout.RowBinding )
                    fail( stage.Stage, "declares a Materials row the template does not" );
                else
                {
                    if ( *stage.RowBinding != *layout.RowBinding )
                        fail( stage.Stage, std::format( "Materials is at binding {}, the template says {}",
                                                        *stage.RowBinding, *layout.RowBinding ) );
                    if ( stage.RowStride != layout.RowStride )
                        fail( stage.Stage, std::format( "a row is {} bytes, the template says {}", stage.RowStride,
                                                        layout.RowStride ) );
                    for ( const auto& p : layout.Params )
                    {
                        const ReflectedLayoutMember* member = nullptr;
                        for ( const auto& m : stage.RowMembers )
                            if ( m.Name == p.Name )
                                member = &m;
                        if ( !member )
                            fail( stage.Stage,
                                  std::format( "parameter '{}' is missing from the compiled row", p.Name ) );
                        else if ( member->Offset != p.Offset || member->Size != p.Size )
                            fail( stage.Stage,
                                  std::format( "parameter '{}' compiled at {}+{}, the template says {}+{}", p.Name,
                                               member->Offset, member->Size, p.Offset, p.Size ) );
                    }
                    for ( const auto& m : stage.RowMembers )
                        if ( !m.Name.starts_with( "_slotPad" ) && !layout.FindParam( m.Name ) )
                            fail( stage.Stage,
                                  std::format( "the compiled row has '{}', which the template does not declare",
                                               m.Name ) );
                }
            }

            // Textures: a generated sampler must sit where the template put it.
            for ( const auto& s : stage.Samplers )
                if ( const auto* t = layout.FindTexture( s.Name ); t && t->Binding != s.Offset )
                    fail( stage.Stage, std::format( "texture '{}' compiled at binding {}, the template says {}",
                                                    s.Name, s.Offset, t->Binding ) );
        }
        return errors;
    }
} // namespace Desert::Core::Formats
