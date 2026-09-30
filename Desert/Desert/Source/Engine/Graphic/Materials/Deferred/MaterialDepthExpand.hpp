#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>

namespace Desert::Graphic
{
    // "Deferred: DepthExpand": binds the single-sample G-buffer depth that DepthExpand.shader writes into every
    // sample of the multisampled scene depth. The node reads it as SampledGraphics; the descriptor names that
    // declared access's layout. Header-only.
    class MaterialDepthExpand final : public Material
    {
    public:
        MaterialDepthExpand() : Material( "MaterialDepthExpand", "DepthExpand" )
        {
            m_Depth = m_MaterialExecutor->GetTexture2DProperty( "u_Depth" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& depth )
        {
            if ( m_Depth && depth )
                m_Depth->SetImage( depth.get(), RDG::Access::SampledGraphics );
        }

    private:
        Texture2DProperty* m_Depth = nullptr;
    };
} // namespace Desert::Graphic
