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

        // A material texture read through the bindless table: the slot holds a BindlessTexture
        // (Common/BindlessTextures.glslh: table slot, sampler index, two reserved words), written from the
        // material's Texture2DProperty when the row is copied (Graphic::DataDrivenMaterial::GetParamRow), never
        // from a persisted value; its default image is the schema's (Texture2DProperty).
        bool IsTexture = false;
    };

    // The GLSL type of a texture slot in the row (Common/BindlessTextures.glslh): exactly one 16-byte slot.
    inline constexpr std::string_view kMaterialTextureGlslType = "BindlessTexture";

    // A bound texture property: a SEPARATE texture at Binding read through its own sampler state at SamplerBinding
    // (`<Name>Sampler`, kMaterialTextureSamplerSuffix), which the material writes from the property's state.
    struct MaterialLayoutTexture
    {
        std::string        Name;
        uint32_t           Binding = 0;
        bool               IsCube  = false;
        DefaultTextureKind Default = DefaultTextureKind::White;
        uint32_t           SamplerBinding = 0;
    };

    // The name of a bound texture property's own sampler state: `<texture>Sampler`. Reflection pairs the two by it
    // (VulkanShader::GetUniformImage2DModels), the generator emits it (DShaderParser BuildAutoDeclarations), and a
    // hand-declared property texture follows it.
    inline constexpr std::string_view kMaterialTextureSamplerSuffix = "Sampler";

    struct MaterialLayoutPushField
    {
        std::string Name;
        uint32_t    Offset = 0;
        uint32_t    Size   = 0;
        ShaderStage Stages = ShaderStage::None;
    };

    // Which groups of SCENE state a template reads — the frame's contribution to a surface, as opposed to
    // the material's own row and textures. Filled by Graphic::ShaderReflection::ReconcileCellLayout from the
    // resource names the compiled stages declare (Graphic::SceneResources is the one name table), and read
    // by Graphic::SceneFrameBinding::ApplyTo, which writes exactly these groups. A capability of the TEMPLATE,
    // so any surface — the shipped lit one or a data-driven one — that samples the shadow map is handed the
    // cascades for that reason alone.
    enum class SceneRead : uint32_t
    {
        None        = 0,
        Camera      = 1u << 0,
        Time        = 1u << 1,
        Lights      = 1u << 2,
        Shadow      = 1u << 3,
        Environment = 1u << 4,
        CloudShadow = 1u << 5,
        // The view's per-primitive motion rows (Common/ObjectMotion.glslh `ObjectMotions`): every view-pass
        // static / skinned surface vertex stage reads its world from them.
        ObjectMotion = 1u << 6,
        // The view's per-(frame x view) palette buffer (Common/ObjectMotion.glslh `ObjectBones`): both frames'
        // palettes of every skinned primitive the view draws, named by the row's BoneOffset / PrevBoneOffset.
        // Read by the view-pass skinned surface vertex stage.
        ObjectBones = 1u << 7,
        // The world's parameters this frame (Common/WorldParameters.glslh `WorldParameters`): read by the cells
        // whose surface names one (`WorldParameter( "Name" )`).
        WorldParameters = 1u << 8,
        // The view's rain occlusion (Common/RainOcclusion.glslh `RainOcclusionUB`, `u_RainOcclusionMap`):
        // read by the cells that wet a surface. The block is written by SceneFrameBinding::ApplyTo, the map is a
        // pass parameter (BindSceneViewInputs).
        RainOcclusion = 1u << 9,
    };
    constexpr SceneRead operator|( SceneRead a, SceneRead b )
    {
        return static_cast<SceneRead>( static_cast<uint32_t>( a ) | static_cast<uint32_t>( b ) );
    }
    constexpr bool Reads( SceneRead set, SceneRead group )
    {
        return ( static_cast<uint32_t>( set ) & static_cast<uint32_t>( group ) ) != 0;
    }

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

        // Filled with the push fields, from every stage's declared resource names; None until then.
        SceneRead SceneReads = SceneRead::None;

        [[nodiscard]] const MaterialLayoutParam* FindParam( std::string_view name ) const
        {
            for ( const auto& p : Params )
                if ( p.Name == name )
                    return &p;
            return nullptr;
        }
        [[nodiscard]] const MaterialLayoutTexture* FindTexture( std::string_view name ) const
        {
            for ( const auto& t : Textures )
                if ( t.Name == name )
                    return &t;
            return nullptr;
        }
        [[nodiscard]] const MaterialLayoutPushField* FindPush( std::string_view name ) const
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

    // MaterialLayoutGlslType — the GLSL type of a row parameter — lives in MaterialParamRow.hpp beside
    // MaterialRowSlotValue, the bytes written for it: the two are one statement and are kept in one place.

    // Whether a texture property is read through the bindless table from the row: every 2D texture of a template
    // that carries a row and no TextureBinding, except EngineSet ones. An EngineSet texture (terrain heightmap,
    // weightmap) is the engine's per-draw input, read by the vertex stage, which has no row (DShaderParser emits
    // the row for fragment and compute only), so it stays a bound sampler. A cube has no bindless table.
    inline bool IsRowTexture( const MaterialLayoutBindings& bindings, const ShaderParam& p )
    {
        return bindings.Row && !bindings.FirstTexture && p.IsTexture && !p.IsCubeTexture && !p.EngineSet;
    }

    // THE builder. Params are the numeric properties in schema order (the walk MaterialParamSlot
    // repeats for MeshRenderer's instance overrides; ShippedShaderPasses holds the two equal), then the row
    // textures (IsRowTexture) in schema order AFTER every numeric one, so a numeric slot is where
    // MaterialParamSlot counts it; textures every texture property from FirstTexture upward.
    inline MaterialLayout BuildMaterialLayout( const ShaderProgramMeta& meta )
    {
        const MaterialLayoutBindings& bindings = meta.LayoutBindings;
        MaterialLayout                layout;
        if ( bindings.Row )
        {
            for ( const auto& p : meta.Params )
            {
                // Samplers are descriptors and asset references are CPU-side inputs: neither is a row field.
                if ( p.IsTexture || p.IsAssetRef() )
                    continue;
                layout.Params.push_back( { p.Name, p.Type,
                                           kMaterialParamSlotSize * static_cast<uint32_t>( layout.Params.size() ),
                                           MaterialParamValueSize( p.Type ), p.Default } );
            }
            for ( const auto& p : meta.Params )
            {
                if ( !IsRowTexture( bindings, p ) )
                    continue;
                MaterialLayoutParam texture{ p.Name, ShaderValueType::Struct,
                                             kMaterialParamSlotSize *
                                                  static_cast<uint32_t>( layout.Params.size() ),
                                             kMaterialParamSlotSize, glm::vec4( 0.0f ) };
                texture.IsTexture = true;
                layout.Params.push_back( texture );
            }
            if ( !layout.Params.empty() )
            {
                layout.RowBinding = bindings.Row;
                layout.RowStride  = kMaterialParamSlotSize * static_cast<uint32_t>( layout.Params.size() );
            }
        }
        if ( bindings.FirstTexture )
        {
            // The textures from FirstTexture upward, then each one's sampler state in the same order right after.
            uint32_t binding = *bindings.FirstTexture;
            for ( const auto& p : meta.Params )
                if ( p.IsTexture && p.ElementIndex < 0 ) // an Element texture is copied into its array
                    layout.Textures.push_back( { p.Name, binding++, p.IsCubeTexture, p.DefaultTexture } );
            for ( auto& texture : layout.Textures )
                texture.SamplerBinding = binding++;
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

        // Every uniform block, storage buffer and sampler the stage declares, by the name Material::Get
        // looks it up under — what SceneReads is classified from.
        std::vector<std::string> ResourceNames;
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
            case ShaderStage::RayGen:
                return "ray-generation";
            case ShaderStage::AnyHit:
                return "any-hit";
            case ShaderStage::ClosestHit:
                return "closest-hit";
            case ShaderStage::Miss:
                return "miss";
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
            if ( pushOwner == nullptr )
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
                        if ( member == nullptr )
                            fail( stage.Stage,
                                  std::format( "parameter '{}' is missing from the compiled row", p.Name ) );
                        else if ( member->Offset != p.Offset || member->Size != p.Size )
                            fail( stage.Stage,
                                  std::format( "parameter '{}' compiled at {}+{}, the template says {}+{}", p.Name,
                                               member->Offset, member->Size, p.Offset, p.Size ) );
                    }
                    for ( const auto& m : stage.RowMembers )
                        if ( !m.Name.starts_with( "_slotPad" ) && layout.FindParam( m.Name ) == nullptr )
                            fail( stage.Stage,
                                  std::format( "the compiled row has '{}', which the template does not declare",
                                               m.Name ) );
                }
            }

            // Textures: a generated sampler must sit where the template put it.
            for ( const auto& s : stage.Samplers )
                if ( const auto* t = layout.FindTexture( s.Name ); t != nullptr && t->Binding != s.Offset )
                    fail( stage.Stage, std::format( "texture '{}' compiled at binding {}, the template says {}",
                                                    s.Name, s.Offset, t->Binding ) );
            // A row texture is read through the bindless table; a sampler of the same name beside it is a second
            // descriptor the material never writes.
            for ( const auto& s : stage.Samplers )
                if ( const auto* p = layout.FindParam( s.Name ); p != nullptr && p->IsTexture )
                    fail( stage.Stage,
                          std::format(
                               "texture '{}' is a row texture read through the bindless table (u_Material.{}), "
                               "and the stage also declares a sampler of that name at binding {}; remove "
                               "the sampler declaration",
                               s.Name, s.Name, s.Offset ) );
        }
        return errors;
    }
} // namespace Desert::Core::Formats
