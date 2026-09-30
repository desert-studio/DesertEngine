#include "MaterialSMAA.hpp"

namespace Desert::Graphic
{
    MaterialSMAAEdges::MaterialSMAAEdges() : Material( "MaterialSMAAEdges", "SMAAEdges" )
    {
        m_Color = m_MaterialExecutor->GetTexture2DProperty( "u_ColorTex" ).get();
    }

    void MaterialSMAAEdges::BindInputs( const std::shared_ptr<Image2D>& color )
    {
        if ( m_Color && color )
            m_Color->SetImage( color.get(), RDG::Access::SampledGraphics );
    }

    MaterialSMAAWeights::MaterialSMAAWeights() : Material( "MaterialSMAAWeights", "SMAAWeights" )
    {
        m_Edges  = m_MaterialExecutor->GetTexture2DProperty( "u_EdgesTex" ).get();
        m_Area   = m_MaterialExecutor->GetTexture2DProperty( "u_AreaTex" ).get();
        m_Search = m_MaterialExecutor->GetTexture2DProperty( "u_SearchTex" ).get();
    }

    void MaterialSMAAWeights::BindInputs( Image2D* edges, Image2D* area, Image2D* search )
    {
        if ( m_Edges && edges )
            m_Edges->SetImage( edges, RDG::Access::SampledGraphics );
        if ( m_Area && area )
            m_Area->SetImage( area, RDG::Access::SampledGraphics );
        if ( m_Search && search )
            m_Search->SetImage( search, RDG::Access::SampledGraphics );
    }

    MaterialSMAABlend::MaterialSMAABlend() : Material( "MaterialSMAABlend", "SMAABlend" )
    {
        m_Color = m_MaterialExecutor->GetTexture2DProperty( "u_ColorTex" ).get();
        m_Blend = m_MaterialExecutor->GetTexture2DProperty( "u_BlendTex" ).get();
        m_Edges = m_MaterialExecutor->GetTexture2DProperty( "u_EdgesTex" ).get();
        m_Area  = m_MaterialExecutor->GetTexture2DProperty( "u_AreaTex" ).get();
    }

    void MaterialSMAABlend::BindInputs( const std::shared_ptr<Image2D>& color, Image2D* weights, Image2D* edges,
                                        Image2D* area )
    {
        if ( m_Color && color )
            m_Color->SetImage( color.get(), RDG::Access::SampledGraphics );
        if ( m_Blend && weights )
            m_Blend->SetImage( weights, RDG::Access::SampledGraphics );
        if ( m_Edges && edges )
            m_Edges->SetImage( edges, RDG::Access::SampledGraphics );
        if ( m_Area && area )
            m_Area->SetImage( area, RDG::Access::SampledGraphics );
    }
} // namespace Desert::Graphic
