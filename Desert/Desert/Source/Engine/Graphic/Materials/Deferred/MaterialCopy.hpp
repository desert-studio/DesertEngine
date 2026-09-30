#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // Trivial full-screen copy material: binds one input image, driving Copy.glsl.frag. Header-only.
    class MaterialCopy final : public Material
    {
    public:
        MaterialCopy() : Material( "MaterialCopy", "Copy" )
        {
            m_Input = m_MaterialExecutor->GetTexture2DProperty( "u_Input" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& input )
        {
            if ( m_Input && input )
                m_Input->SetImage( input.get(), RDG::Access::SampledGraphics );
        }

    private:
        Texture2DProperty* m_Input = nullptr;
    };
} // namespace Desert::Graphic
