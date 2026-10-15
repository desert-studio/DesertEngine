#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    class Image2D;

    // Generic, data-driven material built from ANY shader by name — no per-shader C++ class. Parameters
    // come from the shader's `Properties` schema (UI metadata + defaults), and the cell's MaterialLayout
    // (MaterialBinder) says where each lands in the row the shader reads. This is what makes
    // arbitrary shaders assignable with dynamic params.
    //
    // WHAT THIS CLASS NO LONGER IS. It used to write its parameters into a per-material `uniform
    // MaterialUB` block by reflected field name. A block IS the parameters, so the material held exactly
    // one set of values — and the renderer keys ONE material per shader, so several objects drawn with
    // one graph shader all rendered the values of whichever draw wrote last. It now holds a ROW instead,
    // and the renderer packs every draw's row into one `Materials[]` storage buffer and names each draw's
    // row with a push constant, exactly as the old lit class did. See
    // Engine/Core/Formats/MaterialParamRow.hpp for the measurement, the probe scene and the layout rule.
    //
    // A CONSEQUENCE WORTH STATING: the row is plain CPU memory, so none of the frame-in-flight machinery
    // that guarded the old block applies to it. The renderer uploads every row of a draw group before
    // recording the group's first draw, every frame — there is no value that was written once and owes
    // itself to a copy nobody served, which is what `FlushParameterBuffers` existed to pay for.
    class DataDrivenMaterial : public Material
    {
    public:
        explicit DataDrivenMaterial( const std::string& shaderName )
             : Material( "DDM_" + shaderName, std::string( shaderName ) ), m_ShaderName( shaderName )
        {
            if ( auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName ) )
                m_Schema = shader->GetProgramMeta();

            ApplyDefaults();
        }

        const Core::Formats::ShaderProgramMeta& GetSchema() const
        {
            return m_Schema;
        }
        const std::string& GetShaderName() const
        {
            return m_ShaderName;
        }

        // The bytes one draw reads, in schema order. The renderer copies this into the shared
        // `Materials[]` buffer at the row it then names on the push constant.
        //
        // The texture slots are written HERE, from each row texture's property, at the moment the row is copied:
        // the table slot of the image the property draws now (the schema default while nothing is assigned or
        // the texture is still Pending) and the sampler of its slot state. A texture assigned, a default restored
        // or a sampler state changed (MaterialService writes the property directly) reaches the next copy with no
        // write path of its own to forget.
        const Core::Formats::MaterialParamRow& GetParamRow() const
        {
            WriteTextureSlots();
            return m_Row;
        }

        // Write a scalar/vector param by name. The whole slot is written whatever the parameter's
        // declared width: the components past it are the generated struct's own padding, so there is
        // nothing there to damage, and the alternative — a per-type byte count — is a second statement of
        // a layout that the cell's MaterialLayout deliberately has only one of.
        bool SetParam( const std::string& name, const glm::vec4& value )
        {
            return MaterialBinder::WriteRowParam( GetMaterialLayout(), m_Row, name, value );
        }

        // The name the override producers use. Identical to SetParam now — it was a separate entry point
        // only because the old transport needed a different byte count and a different flush for it.
        bool SetParamRaw( const std::string& name, const glm::vec4& value )
        {
            return SetParam( name, value );
        }

        // Bind a texture by its sampler name (the Properties texture2D name).
        //
        // A NULL IMAGE IS "NONE", NOT A REFUSAL, and that is the whole of М9's second half. This used to
        // `return false` on null, which made an empty slot INEXPRESSIBLE: the editor could take the
        // handle out of the `.demat` and the descriptor went on pointing at the last texture assigned, so
        // the file said one thing and the surface drew another with nothing in between to notice. Null
        // now means the shader's own `Properties … = "white"` default, which is what an empty slot looks
        // like by the shader author's definition.
        //
        // A ROW texture (Core::Formats::IsRowTexture: every 2D texture of a template with a row and no
        // TextureBinding) is a row slot read through the bindless table, so it is per draw like any value. A
        // TextureBinding template's texture is still a descriptor of the material's set, shared by every draw
        // the material records; for those the RENDERER keys materials apart by texture set
        // (MeshRenderer::DrawGenericMeshes).
        //
        // @return false only when @p name is not a Texture2D of this shader at all.
        bool SetTexture( const std::string& name, const Image2D* image )
        {
            if ( !image )
                return BindSchemaDefaultTexture( name );

            if ( auto* tex = Get<Texture2DProperty>( name ) )
            {
                tex->SetImage( image, RDG::Access::SampledGraphics );
                return true;
            }
            return false;
        }

        // Seed every numeric param with its `Properties ... = default` value, at the cell layout's offset.
        void ApplyDefaults()
        {
            m_Row = MaterialBinder::DefaultRow( GetMaterialLayout() );
        }

    private:
        void WriteTextureSlots() const
        {
            for ( const auto& p : GetMaterialLayout().Params )
            {
                if ( !p.IsTexture )
                    continue;
                const uint32_t           slot    = p.Offset / Core::Formats::kMaterialParamSlotSize;
                const Texture2DProperty* texture = Get<Texture2DProperty>( p.Name );
                if ( slot >= m_Row.size() || texture == nullptr )
                    continue;
                const BindlessTextureSlot image = texture->GetBindlessSlot();
                // Slot 0 is the table's magenta "missing" texture: an image with no slot (a render target, a
                // storage image, a multisampled one) or one sampled through its stored encoding is not
                // something the table holds, and the surface shows it rather than something plausible.
                if ( ( !image.IsValid() || texture->SamplesEncoded() ) && !m_ReportedTextureSlot )
                {
                    m_ReportedTextureSlot = true;
                    LOG_ERROR(
                         "[Materials] a material of shader '{}': row texture '{}' {}; it draws the "
                         "bindless table's missing texture (reported once per material)",
                         m_ShaderName, p.Name,
                         texture->SamplesEncoded()
                              ? "asks for the image's stored encoding, which the bindless table does not hold"
                              : "has no bindless slot: only uploaded, single-sample, non-attachment, "
                                "non-storage images are in the table" );
                }
                m_Row[slot] = glm::vec4( glm::uintBitsToFloat( image.Index ),
                                         glm::uintBitsToFloat( texture->GetBindlessSampler().Index ), 0.0f, 0.0f );
            }
        }

        std::string                      m_ShaderName;
        Core::Formats::ShaderProgramMeta m_Schema;
        // Mutable for the texture slots only: they are a projection of the texture properties, refreshed each
        // time the row is read (GetParamRow).
        mutable Core::Formats::MaterialParamRow m_Row;
        mutable bool                            m_ReportedTextureSlot = false;
    };
} // namespace Desert::Graphic
