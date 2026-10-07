#pragma once

#include <Engine/Graphic/Materials/Properties/MaterialProperty.hpp>

#include <Engine/Core/Formats/DefaultTexture.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Core/Formats/SamplerState.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/ShaderResources/UniformImage2D.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>

#include <optional>

namespace Desert::Graphic
{
    class Texture2DProperty : public MaterialProperty
    {
    public:
        // A Texture2D property exists only for a material parameter (a `Properties` texture of the
        // material's schema), and it is born holding that parameter's default: the declared `= "kind"`, or
        // White when none is declared (ShaderParam::DefaultTexture). The first Apply binds it if nothing
        // else was assigned, so no slot of a material ever shows "whatever was bound last".
        Texture2DProperty( std::shared_ptr<ShaderResources::UniformImage2D> uniform,
                           Core::Formats::DefaultTextureKind                defaultTexture )
             : m_Uniform( uniform ), m_Default( defaultTexture )
        {
        }

        // THE ONE RULE for "this slot holds nothing the material chose": point it at the schema's default.
        // false only when DefaultTextures could not produce that image (Resolve() has said which kind).
        bool RestoreDefault()
        {
            const Image2D* image = DefaultTextures::Get().Resolve( m_Default );
            if ( !image )
                return false;
            SetImage( image, RDG::Access::SampledGraphics );
            return true;
        }

        Core::Formats::DefaultTextureKind GetDefault() const
        {
            return m_Default;
        }

        void Apply( MaterialBackend* backend ) override
        {
            if ( m_Texture == nullptr && !RestoreDefault() )
                return;

            // The uniform is shared by every view, so it is re-pointed once per write, not per view.
            if ( m_UniformVersion != GetVersion() )
            {
                m_Uniform->SetImage2D( m_Texture, m_Declared );
                m_UniformVersion = GetVersion();
            }
            // Asked every time: whether THIS view's set is behind is the set's record, not a flag here.
            backend->ApplyTexture2D( this );
        }

        /// Point this sampler at @p texture. NEVER null — "empty" is a picture, not the absence of one,
        /// and the only thing that knows which picture is the shader schema
        /// (`Material::BindSchemaDefaultTexture`).
        ///
        /// WHY THE REFUSAL IS LOUD AND NOT A NO-OP. A null here used to be accepted and then quietly
        /// dropped by `Apply()` below — the property went dirty, the write was skipped, the dirty window
        /// drained, and the descriptor went on pointing at the LAST image assigned. That is how a
        /// material could be given a texture and never have it taken away (М9): the file said the slot
        /// was empty and the surface kept drawing the old map, with nothing in between to notice. The
        /// route back to a default exists now, so a null reaching here is a caller that has not been
        /// told about it, and it says so with the binding it is about.
        /// Every binding names the access it is read under: a raster node's declared access (the graph has put
        /// the image in that layout when the node's draws run; the image's own record is stale until the graph
        /// ends), or, for an image outside the graph (an asset or default texture, resident in
        /// SHADER_READ_ONLY from its upload on), RDG::Access::SampledGraphics. An access that does not leave
        /// the image sampleable is refused and the binding keeps its image.
        void SetImage( const Image2D* texture, RDG::Access declared )
        {
            if ( !texture )
            {
                LOG_ERROR( "[Materials] Texture2DProperty at binding {} was given a null image. Unbinding "
                           "goes through Material::BindSchemaDefaultTexture, which binds the SHADER's own "
                           "default for the slot; this call would have left the previous texture bound and "
                           "said nothing.",
                           m_Uniform ? m_Uniform->GetBinding() : 0u );
                return;
            }
            const RDG::ImageLayout layout = RDG::GetAccessState( declared ).Layout;
            if ( layout != RDG::ImageLayout::ShaderReadOnly && layout != RDG::ImageLayout::General )
            {
                LOG_ERROR( "[Materials] Texture2DProperty at binding {} was given an image declared as {}, which "
                           "does not leave it sampleable; the binding keeps its previous image",
                           m_Uniform ? m_Uniform->GetBinding() : 0u, RDG::GetAccessName( declared ) );
                return;
            }

            m_Texture  = texture;
            m_Declared = declared;
            NoteWritten();
        }

        /// The slot's sampling state (MAT1s): the material's override or the template's, as
        /// Assets::MaterialData::SlotSampler resolved it. The backend binds the cached sampler for a
        /// non-default state and the image's own sampler for the default one.
        void SetSamplerState( const Core::Formats::SamplerState& state )
        {
            if ( state == m_Sampler )
                return;
            m_Sampler = state;
            NoteWritten();
        }

        [[nodiscard]] const Core::Formats::SamplerState& GetSamplerState() const
        {
            return m_Sampler;
        }

        const auto& GetUniform() const
        {
            return m_Uniform;
        }

    private:
        std::shared_ptr<ShaderResources::UniformImage2D> m_Uniform;
        Core::Formats::DefaultTextureKind                m_Default;
        const Image2D*                                   m_Texture        = nullptr;
        RDG::Access m_Declared = RDG::Access::SampledGraphics; // the access m_Texture is read under
        Core::Formats::SamplerState                      m_Sampler;
        uint64_t                                         m_UniformVersion = PropertyVersion::kNeverWritten;
    };
} // namespace Desert::Graphic