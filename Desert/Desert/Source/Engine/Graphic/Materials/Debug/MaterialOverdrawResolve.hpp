#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::Graphic
{
    // Fullscreen resolve material for the Overdraw view: binds the additive accumulation image and drives
    // OverdrawResolve.shader (heat-maps the overdraw count over the scene). Header-only (no new .cpp).
    class MaterialOverdrawResolve final : public Material
    {
    public:
        MaterialOverdrawResolve() : Material( "MaterialOverdrawResolve", "OverdrawResolve" )
        {
            m_Overdraw = m_MaterialExecutor->GetTexture2DProperty( "u_Overdraw" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& accum )
        {
            if ( m_Overdraw && accum )
                m_Overdraw->SetImage( accum.get(), RDG::Access::SampledGraphics );
        }

    private:
        Texture2DProperty* m_Overdraw = nullptr;
    };
} // namespace Desert::Graphic
