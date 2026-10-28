#include <Engine/Graphic/TextureStreamingDemand.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Desert::Graphic
{
    TextureStreamingDemand& TextureStreamingDemand::Get()
    {
        static TextureStreamingDemand demand;
        return demand;
    }

    float TextureStreamingDemand::ScreenPixels( const glm::vec3& center, const float radius,
                                                const Geometry::LODView& view, const uint32_t viewHeight )
    {
        // Geometry::BoundsScreenRadiusSquared's measure (UE ComputeBoundsScreenRadiusSquared), with the distance
        // taken to the sphere's surface as UE's texture streaming does (FRenderAssetInstanceView: the distance
        // from the view to the bounds, not to their centre) -- a wall the camera stands at is a wall at distance
        // 0.
        const float perspective = std::abs( view.Projection[2][3] );
        const float toSurface   = std::max( 0.0f, glm::length( center - view.Origin ) - radius ) * perspective;
        const float multiple =
             std::max( 0.5f * std::abs( view.Projection[0][0] ), 0.5f * std::abs( view.Projection[1][1] ) );
        const float screenRadius = multiple * radius / std::max( 1.0f, toSurface );
        return 2.0f * screenRadius * static_cast<float>( viewHeight );
    }

    void TextureStreamingDemand::Note( const Material* material, const float screenPixels )
    {
        if ( material == nullptr || !( screenPixels > 0.0f ) )
            return;
        auto [it, first] = m_MaterialTextures.try_emplace( material );
        if ( first )
        {
            // The cell's 2D slots live on its executor (MaterialExecutor::GetTexture2DProperties); the
            // registered properties are the reflected MPROPERTY fields only and never name a texture (p1 found
            // nothing through them: 3.9 MB wanted of 992 MB).
            const MaterialExecutor* executor = material->GetMaterialExecutor();
            if ( executor == nullptr )
                return;
            for ( const auto& [name, index] : executor->GetTexture2DProperties() )
            {
                const auto slot = executor->GetTexture2DProperty( name );
                if ( slot == nullptr || slot->GetImage() == nullptr )
                    continue;
                const Common::AssetHandle asset = slot->GetImage()->GetOwningAsset();
                if ( static_cast<uint64_t>( asset ) != 0 )
                    it->second.push_back( asset );
            }
        }
        for ( const Common::AssetHandle& asset : it->second )
            NoteAsset( asset, screenPixels );
    }

    void TextureStreamingDemand::NoteAsset( const Common::AssetHandle& asset, const float screenPixels )
    {
        if ( static_cast<uint64_t>( asset ) == 0 || !( screenPixels > 0.0f ) )
            return;
        float& wanted = m_Demand[asset];
        wanted        = std::max( wanted, screenPixels );
    }

    std::unordered_map<Common::AssetHandle, float> TextureStreamingDemand::TakeFrame()
    {
        // A material may be rebuilt (new texture bound) between frames: its texture list is resolved anew.
        m_MaterialTextures.clear();
        return std::exchange( m_Demand, {} );
    }
} // namespace Desert::Graphic
