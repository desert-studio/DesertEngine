#pragma once

#include <Engine/Graphic/Materials/Properties/MaterialProperty.hpp>

#include <Engine/Graphic/Texture.hpp>
#include <Engine/ShaderResources/UniformImage2D.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    class Texture2DProperty : public MaterialProperty
    {
    public:
        Texture2DProperty( std::shared_ptr<ShaderResources::UniformImage2D> uniform ) : m_Uniform( uniform )
        {
        }

        void Apply( MaterialBackend* backend ) override
        {
            if ( !m_Texture )
                return; // nothing assigned yet: every set keeps the fallback it was born with

            // The uniform is shared by every view, so it is re-pointed once per write, not per view.
            if ( m_UniformVersion != GetVersion() )
            {
                m_Uniform->SetImage2D( m_Texture );
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
        void SetImage( const Image2D* texture )
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

            m_Texture = texture;
            NoteWritten();
        }

        const auto& GetUniform() const
        {
            return m_Uniform;
        }

    private:
        std::shared_ptr<ShaderResources::UniformImage2D> m_Uniform;
        const Image2D*                                   m_Texture        = nullptr;
        uint64_t                                         m_UniformVersion = PropertyVersion::kNeverWritten;
    };
} // namespace Desert::Graphic