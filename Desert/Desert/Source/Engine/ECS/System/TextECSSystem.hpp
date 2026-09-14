#pragma once

#include "System.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Graphic/Render/Commands/DrawGenericMeshCommand.hpp>

#include <limits>

namespace Desert::ECS
{
    // Lays each TextComponent's string into a quad mesh (one quad per glyph, in the entity's local
    // space) and draws it through the generic path with the TextSDF shader + the font's SDF atlas.
    // Render-data collector only (no structural changes, no shared writes) -> parallel-safe.
    class TextECSSystem : public System
    {
    public:
        bool CanRunParallel() const override { return true; }

        // Snapshot the active camera's view matrix once per frame (main thread) so the parallel Update
        // can orient billboarded text toward the viewer without touching shared state.
        void SetCameraSnapshot( const glm::mat4& view, const glm::vec3& /*position*/ ) override
        {
            m_CameraView = view;
        }

        // WHY THIS IS NOT INSIDE Update. It was, and adding the localisation resolve above pushed that
        // function's cognitive complexity to 24 against the gate's threshold of 19 — the analyser is
        // right about the shape rather than about the count: the walk's job is "for every visible text
        // entity, place a mesh in the frame", and "decide whether the mesh it has is the mesh it needs"
        // is a separate decision with its own four-way condition.
        //
        // The layout is built from the RESOLVED string, and the component keeps what the author typed —
        // nothing is ever written back into it, which is the same rule the canvas follows for a bound
        // label. @p drawn is also what the cache is keyed on, so a language change rebuilds the glyph
        // mesh for free: nothing else would have told it to.
        static void RebuildGlyphMeshIfStale( TextComponent& text, const std::string& drawn,
                                             const std::string& fontPath, const Text::BakedFont& baked )
        {
            if ( text.RuntimeMesh && text.BuiltText == drawn && text.BuiltFont == fontPath &&
                 text.BuiltSize == text.Size )
            {
                return;
            }

            TextComponent laidOut = text;
            laidOut.Text          = drawn;
            text.RuntimeMesh      = BuildTextMesh( laidOut, baked );
            text.BuiltText        = drawn;
            text.BuiltFont        = fontPath;
            text.BuiltSize        = text.Size;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            auto view = registry.view<TextComponent, TransformComponent>();
            view.each(
                 [&]( entt::entity entity, TextComponent& text, const TransformComponent& transform )
                 {
                     if ( registry.has<VisibilityComponent>( entity ) &&
                          !registry.get<VisibilityComponent>( entity ).Visible )
                         return;
                     if ( text.Text.empty() )
                         return;

                     // WORLD TEXT IS AUTHORED TEXT AND GOES THROUGH THE SAME RESOLVER AS A UI LABEL
                     // (Ю15): a leading '#' is a string-table key, anything else is a literal. Uniform on
                     // purpose — "every string drawn from a component goes through one function" is a
                     // property a reader can rely on, and "every string except the ones in the world" is
                     // not.
                     //
                     // The resolved string is also what the mesh cache is keyed on below, so a language
                     // change rebuilds the glyph mesh for free: nothing else would have told it to.
                     const std::string drawn = Localization::Localization::Get().Resolve( text.Text ).Text;

                     // Resolve the font asset handle -> baked atlas (unset falls back to the built-in default).
                     auto*          fontSvc    = Runtime::ResourceRegistry::GetFontService();
                     const uint64_t fontHandle = static_cast<uint64_t>( text.Font ) != 0
                                                      ? static_cast<uint64_t>( text.Font )
                                                      : fontSvc->DefaultFontHandle();
                     // Anything beyond ASCII has to be requested before the atlas is resolved (see
                     // FontService::RequestGlyphs) — otherwise Cyrillic/CJK text bakes to nothing.
                     fontSvc->RequestGlyphs( fontHandle, Text::Utf8Decode( drawn ) );

                     auto* font = fontSvc->Get( fontHandle );
                     if ( !font )
                         return;
                     const std::string fontPath = fontSvc->PathForHandle( fontHandle );

                     RebuildGlyphMeshIfStale( text, drawn, fontPath, font->Baked );
                     if ( !text.RuntimeMesh )
                         return;

                     // WORLD transform (walk the parent chain), mirroring the mesh systems.
                     glm::mat4    worldTransform = transform.GetTransform();
                     entt::entity current        = entity;
                     while ( registry.has<RelationshipComponent>( current ) )
                     {
                         const auto& rel = registry.get<RelationshipComponent>( current );
                         if ( rel.Parent == entt::null )
                             break;
                         current = rel.Parent;
                         if ( registry.has<TransformComponent>( current ) )
                             worldTransform =
                                  registry.get<TransformComponent>( current ).GetTransform() * worldTransform;
                     }

                     // Billboard: keep the world translation + scale, but swap the orientation for a
                     // camera-facing basis so the glyph plane squarely faces the viewer from any angle.
                     // The mesh was authored for the default camera (at +Z looking -Z), whose world basis
                     // is exactly the view-matrix rows: row0 = right, row1 = up, row2 = toward-viewer. So
                     // local +X→right, +Y→up, +Z→row2 reproduces the head-on layout at any camera pose.
                     if ( text.Billboard )
                     {
                         const glm::vec3 translation = glm::vec3( worldTransform[3] );
                         const glm::vec3 scale( glm::length( glm::vec3( worldTransform[0] ) ),
                                                glm::length( glm::vec3( worldTransform[1] ) ),
                                                glm::length( glm::vec3( worldTransform[2] ) ) );
                         const glm::vec3 right( m_CameraView[0][0], m_CameraView[1][0], m_CameraView[2][0] );
                         const glm::vec3 up( m_CameraView[0][1], m_CameraView[1][1], m_CameraView[2][1] );
                         const glm::vec3 toViewer( m_CameraView[0][2], m_CameraView[1][2], m_CameraView[2][2] );
                         worldTransform =
                              glm::mat4( glm::vec4( right * scale.x, 0.0f ), glm::vec4( up * scale.y, 0.0f ),
                                         glm::vec4( toViewer * scale.z, 0.0f ), glm::vec4( translation, 1.0f ) );
                     }

                     Graphic::MaterialOverrides overrides;
                     overrides.Params.emplace_back( "TextColor", text.Color );
                     overrides.Params.emplace_back( "EmissiveIntensity",
                                                    glm::vec4( text.EmissiveIntensity, 0, 0, 0 ) );

                     renderCommandBuffer.Emplace<Graphic::Render::DrawGenericMeshCommand>(
                          text.RuntimeMesh.get(), worldTransform, std::string( "TextSDF" ),
                          std::move( overrides ), /*outlined*/ false, font->Atlas.get(),
                          std::string( "u_SDFAtlas" ) );
                 } );
        }

    private:
        glm::mat4 m_CameraView{ 1.0f }; // active-camera view matrix snapshot (see SetCameraSnapshot)

        static std::shared_ptr<DynamicMesh> BuildTextMesh( const TextComponent& text,
                                                           const Text::BakedFont& font )
        {
            const float worldPerPixel = ( font.PixelHeight > 0.0f ) ? text.Size / font.PixelHeight : 0.0f;
            if ( worldPerPixel <= 0.0f )
                return nullptr;

            std::vector<Vertex> verts;
            std::vector<Index>  inds;
            verts.reserve( text.Text.size() * 4 );
            inds.reserve( text.Text.size() * 2 );

            glm::vec3 mn( std::numeric_limits<float>::max() );
            glm::vec3 mx( std::numeric_limits<float>::lowest() );

            float penX = 0.0f, penY = 0.0f; // pixels; baseline at penY, +X right, lines step -Y
            const float lineStep = font.LineHeight();

            for ( size_t ci = 0; ci < text.Text.size(); )
            {
                const uint32_t ch = Text::Utf8Next( text.Text, ci ); // codepoints, not bytes
                if ( ch == '\n' )
                {
                    penX = 0.0f;
                    penY -= lineStep;
                    continue;
                }
                const auto it = font.Glyphs.find( ch );
                if ( it == font.Glyphs.end() )
                    continue;
                const Text::Glyph& g = it->second;

                if ( g.Width > 0.0f && g.Height > 0.0f )
                {
                    // OffsetY is the top of the bitmap relative to the baseline (Y-down); flip to Y-up world.
                    const float x0   = ( penX + g.OffsetX ) * worldPerPixel;
                    const float x1   = x0 + g.Width * worldPerPixel;
                    const float yTop = ( penY - g.OffsetY ) * worldPerPixel;
                    const float yBot = yTop - g.Height * worldPerPixel;

                    const uint32_t base = static_cast<uint32_t>( verts.size() );
                    // The quad's local +Z faces the default camera (at +Z looking -Z); that view maps
                    // world +X to SCREEN-left, so a +X layout reads mirrored. Negate X to lay the text
                    // out so it reads left-to-right head-on (a flat label is mirrored from behind — as
                    // expected for any single plane of text).
                    auto push = [&]( float x, float y, float u, float v )
                    {
                        Vertex vtx{};
                        vtx.Position = { -x, y, 0.0f };
                        vtx.Normal   = { 0.0f, 0.0f, 1.0f };
                        vtx.TexCoord = { u, v };
                        verts.push_back( vtx );
                        mn = glm::min( mn, vtx.Position );
                        mx = glm::max( mx, vtx.Position );
                    };
                    push( x0, yTop, g.U0, g.V0 ); // TL
                    push( x1, yTop, g.U1, g.V0 ); // TR
                    push( x1, yBot, g.U1, g.V1 ); // BR
                    push( x0, yBot, g.U0, g.V1 ); // BL
                    inds.push_back( { base + 0, base + 3, base + 2 } );
                    inds.push_back( { base + 0, base + 2, base + 1 } );
                }
                penX += g.Advance;
            }

            if ( verts.empty() )
                return nullptr;

            Common::Math::AABB aabb;
            aabb.Min = mn;
            aabb.Max = mx;
            std::vector<Submesh> subs = { { "Text", 0, static_cast<uint32_t>( verts.size() ), 0,
                                           static_cast<uint32_t>( inds.size() ) * 3, glm::mat4( 1.0f ),
                                           aabb } };

            auto mesh = std::make_shared<DynamicMesh>( verts, inds, subs, /*generateLODs*/ false );
            mesh->Invalidate();
            return mesh;
        }
    };
} // namespace Desert::ECS
