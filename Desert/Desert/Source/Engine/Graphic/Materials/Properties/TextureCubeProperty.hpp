#pragma once

#include <Engine/Graphic/Materials/Properties/MaterialProperty.hpp>

#include <Engine/ShaderResources/UniformImageCube.hpp>

namespace Desert::Graphic
{
    class TextureCubeProperty : public MaterialProperty
    {
    public:
        TextureCubeProperty( std::shared_ptr<ShaderResources::UniformImageCube> uniform ) : m_Uniform( uniform )
        {
        }

        void Apply( MaterialBackend* backend ) override
        {
            // NO `if ( m_Texture )` HERE ANY MORE — that guard was the Г14 defect, one link before the
            // descriptor. A slot told "nothing" marked itself dirty, skipped the write and then marked
            // itself clean, so the descriptor kept the last cube ANY scene had given it. Both ends looked
            // right (the producer restates absence every frame; the descriptor is always defined) and the
            // middle link dropped it: CornellDemo -> Clouds_Protocol -> CornellDemo came back lit by the
            // cloud scene's sky. `UniformImageCube::SetImageCube( nullptr )` is what "nothing" MEANS, and
            // it is a write like any other.
            // The uniform is shared by every view, so it is re-pointed once per write, not per view.
            if ( m_UniformVersion != GetVersion() )
            {
                m_Uniform->SetImageCube( m_Texture );
                m_UniformVersion = GetVersion();
            }
            // Asked every time: whether THIS view's set is behind is the set's record, not a flag here.
            backend->ApplyTextureCube( this );
        }

        /// Point this sampler at @p texture, or at NOTHING when it is null.
        ///
        /// WHY THIS ACCEPTS A NULL AND `Texture2DProperty::SetImage` REFUSES ONE. The two are not
        /// inconsistent; they answer different questions, and the difference is worth stating because the
        /// next reader will otherwise "fix" one of them.
        ///
        /// A 2D slot is a MATERIAL PARAMETER a person authored: `u_AlbedoTexture` empty means "the shader's
        /// own default for this slot", which only the shader schema knows, so М9 made clearing go through
        /// `Material::BindSchemaDefaultTexture` and made a raw null a loud refusal. There is no such
        /// authored default for a `samplerCube`: every cube binding in this engine is PER-FRAME SCENE
        /// STATE — the baked environment — and the question a null asks is "what does a scene with no sky
        /// look like", which the backend already answers with the fallback cube it seeds the binding with.
        void SetTexture( const ImageCube* texture )
        {
            m_Texture = texture;
            NoteWritten(); // INCLUDING the write that clears it
        }

        const auto& GetUniform() const
        {
            return m_Uniform;
        }

    private:
        std::shared_ptr<ShaderResources::UniformImageCube> m_Uniform;
        const ImageCube*                            m_Texture = nullptr;
        uint64_t                                           m_UniformVersion = PropertyVersion::kNeverWritten;
    };
} // namespace Desert::Graphic