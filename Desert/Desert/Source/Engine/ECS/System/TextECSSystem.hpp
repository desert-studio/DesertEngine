#pragma once

#include "System.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Render2DCore/Text/Utf8.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Graphic/Render/Commands/DrawGenericMeshCommand.hpp>
#include <Engine/Graphic/Systems/RayTracing/TextRayTracing.hpp>

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

        // THE LABEL'S RESOLVE, REDONE ONLY WHEN ONE OF ITS INPUTS MOVED (UE: FSlateFontInfo resolved once to a
        // cached FFontData, the shaped text kept until the string or the style changes). It ran every frame for
        // every label: the localisation resolve copied the string, the codepoints were decoded into a vector, and
        // the font handle went handle -> path -> "path|size" key three times over (RequestGlyphs, Get and the
        // mesh's font guard), each a heap string. The inputs are the authored string and font, the language
        // (Localization::Generation) and the font cache (FontService::Generation): equal to what the cache was
        // made from, the frame compares and resolves nothing.
        //
        // The layout is built from the RESOLVED string, and the component keeps what the author typed - nothing
        // is ever written back into Text, the same rule the canvas follows for a bound label. A language change
        // moves the generation, so the glyph mesh rebuilds for free: nothing else would have told it to.
        static const Runtime::Font* ResolveLabelIfStale( TextComponent& text, Runtime::FontService& fontSvc )
        {
            auto&          localization     = Localization::Localization::Get();
            const uint32_t localeGeneration = localization.Generation();
            const uint64_t authoredFont     = static_cast<uint64_t>( text.Font );
            if ( text.ResolvedFont && text.ResolvedFontGeneration == fontSvc.Generation() &&
                 text.ResolvedLocaleGeneration == localeGeneration &&
                 static_cast<uint64_t>( text.ResolvedFontAsset ) == authoredFont &&
                 text.ResolvedSource == text.Text )
            {
                return text.ResolvedFont.get();
            }

            // WORLD TEXT IS AUTHORED TEXT AND GOES THROUGH THE SAME RESOLVER AS A UI LABEL (Ю15): a leading '#'
            // is a string-table key, anything else is a literal.
            text.ResolvedText = localization.Resolve( text.Text ).Text;

            // Unset falls back to the built-in default.
            const uint64_t fontHandle = authoredFont != 0 ? authoredFont : fontSvc.DefaultFontHandle();
            // Anything beyond ASCII has to be requested before the atlas is resolved (see
            // FontService::RequestGlyphs) - otherwise Cyrillic/CJK text bakes to nothing.
            fontSvc.RequestGlyphs( fontHandle, Text::Utf8Decode( text.ResolvedText ) );

            // Read BEFORE the share: a glyph request on another thread that drops this font after it moves the
            // generation past this value, and the next frame resolves again.
            const uint64_t fontGeneration = fontSvc.Generation();
            // The old font's atlas is the DirectTexture of draws still in flight: retired, never dropped here.
            RetireRenderCache( text.ResolvedFont );
            text.ResolvedFont             = fontSvc.Share( fontHandle );
            text.ResolvedSource           = text.Text;
            text.ResolvedFontAsset        = text.Font;
            text.ResolvedLocaleGeneration = localeGeneration;
            text.ResolvedFontGeneration   = fontGeneration;
            return text.ResolvedFont.get();
        }

        static void RebuildGlyphMeshIfStale( TextComponent& text, const Runtime::Font& font )
        {
            // The revision too: a glyph request re-bakes the atlas (FontService::RequestGlyphs) with the same
            // path and size but new UVs, and a mesh laid on the old bake would sample the wrong glyphs. A
            // revision is unique per bake, so it also stands for the font: two fonts never share one. No
            // "RuntimeMesh is set" term: a string with nothing to draw (all blanks) lays out to no mesh, and
            // re-laying it every frame to find that again was a heap round trip per frame.
            if ( text.BuiltText == text.ResolvedText && text.BuiltSize == text.Size &&
                 text.BuiltFontRevision == font.Revision )
            {
                return;
            }

            // THE OLD GLYPH MESH IS RETIRED, NEVER FREED HERE (UE: a game-thread release of a render resource goes
            // through the deferred-cleanup queue, BeginCleanup). This tick may run past the previous update's view
            // job, which replays a DrawGenericMeshCommand naming that mesh by raw pointer: the last owner dropping
            // it here freed it under the replay. Parked like SetStaticMesh's edited mesh (RetireRenderCache), it
            // dies once no frame in flight names it (FrameManager::RetireAfterFrames: any thread, locked).
            RetireRenderCache( text.RuntimeMesh );
            text.RuntimeMesh       = BuildTextMesh( text.ResolvedText, text.Size, font.Baked );
            text.BuiltText         = text.ResolvedText;
            text.BuiltSize         = text.Size;
            text.BuiltFontRevision = font.Revision;
        }

        // The draw's overrides and program hit, built when the colour, the emission or the atlas moves rather than
        // every frame (two vectors and two strings per label per frame): the command names them by pointer.
        static void RebuildDrawParamsIfStale( TextComponent& text, const Runtime::Font& font )
        {
            uint64_t atlasGeneration = 0;
            if ( font.Atlas != nullptr )
                atlasGeneration = font.Atlas->GetResourceGeneration();
            if ( text.RuntimeDraw && text.DrawColor == text.Color &&
                 text.DrawEmissiveIntensity == text.EmissiveIntensity && text.DrawFontRevision == font.Revision &&
                 text.DrawAtlasGeneration == atlasGeneration )
            {
                return;
            }

            auto params = std::make_shared<Graphic::Render::GenericMeshDrawParams>();
            params->Overrides.Params.emplace_back( "TextColor", text.Color );
            params->Overrides.Params.emplace_back( "EmissiveIntensity",
                                                   glm::vec4( text.EmissiveIntensity, 0, 0, 0 ) );
            // In the ray tracing scene as UE's text render component is (bVisibleInRayTracing): the glyph quads
            // with the label's own hit group - the SDF coverage as the any hit, the TextSDF colour as the emission
            // (TextRayTracing.hpp), from the values this draw carries. A font whose atlas is not made yet has no
            // coverage to test: the label is not traced (the hit stays empty).
            if ( font.Atlas != nullptr )
                params->RayHit =
                     Graphic::RayTracing::TextRayHit( text.Color, text.EmissiveIntensity, *font.Atlas );

            // Named by raw pointer by the draws in flight (DrawGenericMeshCommand::Params): retired, as the mesh.
            RetireRenderCache( text.RuntimeDraw );
            text.RuntimeDraw           = std::move( params );
            text.DrawColor             = text.Color;
            text.DrawEmissiveIntensity = text.EmissiveIntensity;
            text.DrawFontRevision      = font.Revision;
            text.DrawAtlasGeneration   = atlasGeneration;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            auto view = registry.view<TextComponent, TransformComponent>();
            view.each(
                 [&]( entt::entity entity, TextComponent& text, const TransformComponent& transform )
                 {
                     if ( ECS::IsHidden( registry, entity ) )
                         return;
                     if ( text.Text.empty() )
                         return;

                     auto* const font = ResolveLabelIfStale( text, *Runtime::ResourceRegistry::GetFontService() );
                     if ( !font )
                         return;
                     RebuildGlyphMeshIfStale( text, *font );
                     if ( !text.RuntimeMesh )
                         return;
                     RebuildDrawParamsIfStale( text, *font );

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

                     // No shadow caster: the raster's shadow pass has no alpha test, and the ray tracing shadow
                     // mask follows it. The two literals fit the small-string buffer: no heap.
                     const bool traced = font->Atlas != nullptr;
                     renderCommandBuffer.Emplace<Graphic::Render::DrawGenericMeshCommand>(
                          static_cast<uint32_t>( entity ), text.RuntimeMesh.get(), worldTransform,
                          std::string( "TextSDF" ), *text.RuntimeDraw, /*outlined*/ false, font->Atlas.get(),
                          std::string( "SdfAtlasTexture" ), /*castShadows*/ false, /*maxDrawDistance*/ 0.0f,
                          /*visibleInRayTracing*/ traced );
                 } );
        }

    private:
        glm::mat4 m_CameraView{ 1.0f }; // active-camera view matrix snapshot (see SetCameraSnapshot)

        static std::shared_ptr<DynamicMesh> BuildTextMesh( const std::string& drawn, const float size,
                                                           const Text::BakedFont& font )
        {
            const float worldPerPixel = ( font.PixelHeight > 0.0f ) ? size / font.PixelHeight : 0.0f;
            if ( worldPerPixel <= 0.0f )
                return nullptr;

            std::vector<Vertex> verts;
            std::vector<Index>  inds;
            verts.reserve( drawn.size() * 4 );
            inds.reserve( drawn.size() * 2 );

            glm::vec3 mn( std::numeric_limits<float>::max() );
            glm::vec3 mx( std::numeric_limits<float>::lowest() );

            float penX = 0.0f, penY = 0.0f; // pixels; baseline at penY, +X right, lines step -Y
            const float lineStep = font.LineHeight();

            for ( size_t ci = 0; ci < drawn.size(); )
            {
                const uint32_t ch = Text::Utf8Next( drawn, ci ); // codepoints, not bytes
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
                    // The pen advances along +X and so does the quad, because WORLD +X IS SCREEN-RIGHT.
                    // `MakePerspective` is glm::perspectiveRH_ZO and the view is glm::lookAt, both
                    // right-handed, and Vulkan's clip-space flip is in Y — so a camera at +Z looking -Z
                    // puts +X on the right. The comment that stood here claimed the opposite and the
                    // code obeyed it, so every label in the world was drawn MIRRORED.
                    auto push = [&]( float x, float y, float u, float v )
                    {
                        Vertex vtx{};
                        vtx.Position = { x, y, 0.0F };
                        vtx.Normal   = { 0.0f, 0.0f, 1.0f };
                        // The ONE mesh UV convention (ShaderProtocols/VertexInputs.hpp MeshVertexAccessGlsl):
                        // stored V up, so the mesh vertex factory's DesertMeshTextureUV hands the atlas its
                        // top-left V.
                        vtx.TexCoord = { u, 1.0F - v };
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

            // No upload here: this runs in the tick, which may overlap the previous update's views, and the pool
            // upload belongs to the fence. The constructor raised the mesh's pending flag; the draw's Prepare
            // (SceneRenderer::PrepareMesh, Scene::OnUpdate's intake) uploads it before any view replays it.
            return std::make_shared<DynamicMesh>( verts, inds, subs, /*generateLODs*/ false );
        }
    };
} // namespace Desert::ECS
