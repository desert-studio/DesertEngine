#include "TerrainPaintTool.hpp"

#include <ImGui/imgui.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Editor::Tools
{
    namespace ImGui = ::ImGui; // engine headers introduce a Desert::ImGui that would otherwise shadow ::ImGui

    void TerrainPaintTool::DrawOverlay( const ECS::Entity& terrain )
    {
        auto& comp = terrain.GetComponent<ECS::TerrainComponent>();

        // Floating overlay anchored to the viewport's top-left corner.
        ImGui::SetCursorPos( ImVec2( ImGui::GetWindowContentRegionMin().x + 12.0f,
                                     ImGui::GetWindowContentRegionMin().y + 12.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12.0f, 12.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8.0f, 8.0f ) );
        ImGui::BeginChild( "##TerrainPaint", ImVec2( 320.0f, 260.0f ), true );

        ImGui::TextUnformatted( "Terrain Paint" );
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox( "Enable Brush", &m_Brush.Enabled );

        const char* layers[] = { "Grass (R)", "Rock (G)", "Snow (B)" };
        ImGui::SetNextItemWidth( 180.0f );
        ImGui::Combo( "Layer", &m_Brush.Layer, layers, 3 );
        ImGui::SetNextItemWidth( 180.0f );
        ImGui::SliderFloat( "Radius", &m_Brush.Radius, 50.0f, comp.Data.Size, "%.0f cm" );
        ImGui::SetNextItemWidth( 180.0f );
        ImGui::SliderFloat( "Strength", &m_Brush.Strength, 0.0f, 1.0f );
        ImGui::Checkbox( "Erase", &m_Brush.Erase );

        ImGui::Spacing();
        if ( ImGui::Button( "Clear Splat", ImVec2( 180.0f, 0.0f ) ) )
        {
            const uint32_t res = ECS::TerrainComponent::SplatResolution;
            comp.SplatPixels.assign( static_cast<size_t>( res ) * res * 4, 0 );
            comp.SplatDirty = true;
        }

        ImGui::Spacing();
        ImGui::TextDisabled( "Set the layer to 'Manual' in Details" );
        ImGui::TextDisabled( "to see painted weights. Hold LMB" );
        ImGui::TextDisabled( "and drag to paint." );

        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );
    }

    bool TerrainPaintTool::PickPoint( const Common::Math::Ray& ray, const ECS::Entity& terrain,
                                      glm::vec3& outHit ) const
    {
        const auto& tf = terrain.GetComponent<ECS::TransformComponent>();

        // Intersect with the horizontal plane at the terrain's base height. The splat map is XZ-indexed, so
        // the plane hit (ignoring displacement) is a good-enough position for v1.
        if ( std::abs( ray.Direction.y ) < 1e-5f )
            return false;
        const float t = ( tf.Translation.y - ray.Origin.y ) / ray.Direction.y;
        if ( t <= 0.0f )
            return false;
        outHit = ray.GetPoint( t );
        return true;
    }

    void TerrainPaintTool::Paint( const Common::Math::Ray& ray, const ECS::Entity& terrain )
    {
        auto& comp = terrain.GetComponent<ECS::TerrainComponent>();
        auto& tf   = terrain.GetComponent<ECS::TransformComponent>();

        const float    size = comp.Data.Size;
        const uint32_t res  = ECS::TerrainComponent::SplatResolution;
        if ( size <= 0.0f )
            return;
        if ( comp.SplatPixels.empty() )
            comp.SplatPixels.assign( static_cast<size_t>( res ) * res * 4, 0 );

        glm::vec3 hit;
        if ( !PickPoint( ray, terrain, hit ) )
            return;

        // Terrain-local UV (matches the shader's splat UV: (worldXZ - modelTranslationXZ)/Size + 0.5).
        const float u = ( hit.x - tf.Translation.x ) / size + 0.5f;
        const float v = ( hit.z - tf.Translation.z ) / size + 0.5f;
        if ( u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f )
            return;

        const int   cx       = static_cast<int>( u * res );
        const int   cy       = static_cast<int>( v * res );
        const float radiusPx = ( m_Brush.Radius / size ) * res;
        const int   channel  = std::clamp( m_Brush.Layer, 0, 2 ); // R=grass, G=rock, B=snow
        const int   r        = static_cast<int>( std::ceil( radiusPx ) );
        const float sign     = m_Brush.Erase ? -1.0f : 1.0f;

        for ( int dy = -r; dy <= r; ++dy )
        {
            for ( int dx = -r; dx <= r; ++dx )
            {
                const int px = cx + dx;
                const int py = cy + dy;
                if ( px < 0 || py < 0 || px >= (int)res || py >= (int)res )
                    continue;
                const float dist = std::sqrt( static_cast<float>( dx * dx + dy * dy ) );
                if ( dist > radiusPx )
                    continue;
                const float  falloff = 1.0f - dist / glm::max( radiusPx, 0.0001f );
                const size_t idx     = ( static_cast<size_t>( py ) * res + px ) * 4 + channel;
                const float  cur     = static_cast<float>( comp.SplatPixels[idx] );
                // Builds up over the frames the button is held (not instant).
                const float delta = sign * m_Brush.Strength * falloff * 60.0f;
                comp.SplatPixels[idx] = static_cast<unsigned char>( glm::clamp( cur + delta, 0.0f, 255.0f ) );
            }
        }
        comp.SplatDirty = true;
    }

    void TerrainPaintTool::DrawRing( const Common::Math::Ray& ray, const ECS::Entity& terrain,
                                     const glm::mat4& viewProj, const glm::vec2& viewportSize,
                                     const glm::vec2& viewportPos )
    {
        glm::vec3 center;
        if ( !PickPoint( ray, terrain, center ) )
            return;

        const float w = viewportSize.x;
        const float h = viewportSize.y;

        const auto toScreen = [&]( const glm::vec3& world, ImVec2& out ) -> bool
        {
            const glm::vec4 clip = viewProj * glm::vec4( world, 1.0f );
            if ( clip.w <= 1e-4f )
                return false;
            const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
            out = ImVec2( viewportPos.x + ( ndc.x * 0.5f + 0.5f ) * w,
                          viewportPos.y + ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * h );
            return true;
        };

        constexpr int N      = 48;
        const float   radius = m_Brush.Radius;
        ImVec2        pts[N];
        for ( int i = 0; i < N; ++i )
        {
            const float     a  = static_cast<float>( i ) / N * 2.0f * 3.14159265f;
            const glm::vec3 wp = center + glm::vec3( std::cos( a ) * radius, 0.0f, std::sin( a ) * radius );
            if ( !toScreen( wp, pts[i] ) )
                return; // part of the ring is behind the camera — skip this frame
        }

        auto*       dl  = ImGui::GetWindowDrawList();
        const ImU32 col = IM_COL32( 255, 220, 60, 230 );
        for ( int i = 0; i < N; ++i )
            dl->AddLine( pts[i], pts[( i + 1 ) % N], col, 2.0f );

        ImVec2 centerScreen;
        if ( toScreen( center, centerScreen ) )
            dl->AddCircleFilled( centerScreen, 3.0f, col );
    }

    void TerrainPaintTool::UploadDirtySplatMaps( ::Desert::Core::Scene& scene )
    {
        auto& registry = scene.GetRegistry();
        auto  view     = registry.view<ECS::TerrainComponent>();

        bool any = false;
        for ( auto e : view )
        {
            const auto& c = view.get<ECS::TerrainComponent>( e );
            if ( c.SplatDirty && !c.SplatPixels.empty() )
            {
                any = true;
                break;
            }
        }
        if ( !any )
            return;

        // Releasing/recreating a sampled image must not race in-flight GPU work.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        const uint32_t res = ECS::TerrainComponent::SplatResolution;
        for ( auto e : view )
        {
            auto& c = view.get<ECS::TerrainComponent>( e );
            if ( !c.SplatDirty || c.SplatPixels.empty() )
                continue;

            if ( !c.SplatMap )
            {
                ::Desert::Core::Formats::Image2DSpecification spec = {
                     .Tag        = "TerrainSplatMap",
                     .Width      = res,
                     .Height     = res,
                     .Format     = ::Desert::Core::Formats::ImageFormat::RGBA8F,
                     .Mips       = 1,
                     .Data       = c.SplatPixels,
                     .Usage      = ::Desert::Core::Formats::Image2DUsage::Image2D,
                     .Properties = ::Desert::Core::Formats::ImageProperties::Sample,
                };
                c.SplatMap = Graphic::Image2D::Create( spec );
            }
            else
            {
                c.SplatMap->GetImageSpecification().Data = c.SplatPixels;
                // `SplatDirty` is only cleared on success, which is the point of reading this result: the
                // flag used to be cleared regardless, so a re-upload that failed made the brush stroke
                // disappear permanently — the CPU pixels were correct, the GPU image was stale, and
                // nothing would ever try again.
                const auto uploaded = c.SplatMap->Invalidate();
                if ( !uploaded.IsSuccess() )
                {
                    LOG_ERROR( "[TerrainPaintTool] splat map re-upload failed: {}", uploaded.GetError() );
                    continue;
                }
            }
            c.SplatDirty = false;
        }
    }
} // namespace Desert::Editor::Tools
