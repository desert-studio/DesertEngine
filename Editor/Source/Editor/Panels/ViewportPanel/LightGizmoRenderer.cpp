#include "LightGizmoRenderer.hpp"
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/SkeletonEditMode.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>

#include <Engine/Core/Projection.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "../../Core/EditorResources.hpp"

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // Light billboards draw with the big icon font at this pixel size — the default-font glyph
        // was a barely-visible, barely-clickable speck.
        constexpr float kLightIconSize = 30.0f;
        // Every viewport billboard (lights, camera, sun, spawn icons, text) uses this one size so the
        // markers read as a consistent set.
        constexpr float kIconSize = kLightIconSize;

        // Projects a world point to viewport-local screen coords, returning false when the point is
        // behind the camera (clip w <= 0). WorldToScreenSpace divides by w unconditionally, so behind
        // points flip to mirrored on-screen positions — that produced both the ghost bulb icon when
        // turning 180 degrees and the radius circle lines streaking across the whole screen.
        bool ProjectToScreen( const glm::vec3& world, const glm::mat4& mvp, float width, float height,
                              glm::vec2& outScreen )
        {
            const glm::vec4 clip = mvp * glm::vec4( world, 1.0f );
            if ( clip.w <= 1e-4f )
                return false;

            const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
            outScreen.x = ( ndc.x * 0.5f + 0.5f ) * width;
            outScreen.y = ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * height;
            return true;
        }
    } // namespace

    LightGizmoRenderer::LightGizmoRenderer( const std::shared_ptr<Desert::Core::Scene>& scene ) : m_Scene( scene )
    {
    }

    void LightGizmoRenderer::Render( float width, float height, float xpos, float ypos )
    {
        const auto camera = m_Scene->GetMainCamera().lock();
        if ( !camera )
            return;

        m_LightIconHovered = false; // recomputed by the icon renderers below (gates scene picking)

        RenderPointLights( camera, width, height, xpos, ypos );
        RenderSpotLights( camera, width, height, xpos, ypos );
        RenderDirectionLights( camera, width, height );
        RenderCameras( camera, width, height, xpos, ypos );
        RenderSpawnIcons( camera, width, height );
        RenderTextIcons( camera, width, height );

        // Collider wireframes moved to EditorColliderPass (true 3D, depth-tested) via the Editor Pass API.

        if ( Core::SkeletonEditMode::IsActive() )
        {
            RenderSkeleton( camera, width, height, xpos, ypos );
        }
        else
        {
            m_BoneScreenPositions.clear(); // outside Skeleton Edit -> no stale bone picks
        }

        // Rig placement overlay (Convert-to-Skinned) — draws the bones being placed on a static mesh.
        RenderRigBuilder( camera, width, height );
    }

    void LightGizmoRenderer::RenderPointLights( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                                float height, float /*xpos*/, float /*ypos*/ )
    {
        auto entities = m_Scene->GetAllEntities();

        ImVec2 windowPos = ImGui::GetWindowPos();

        for ( auto entity : entities )
        {
            if ( !entity.HasComponent<ECS::PointLightComponent>() )
            {
                continue;
            }

            auto&           light    = entity.GetComponent<ECS::PointLightComponent>().Data;
            const glm::vec3 worldPos = glm::vec3( entity.GetWorldTransform()[3] ); // parent-composed position

            const auto mvp = camera->GetProjectionMatrix() * camera->GetViewMatrix();

            glm::vec2 screenPos;
            if ( !ProjectToScreen( worldPos, mvp, width, height, screenPos ) )
                continue; // light is behind the camera — skip its icon, radius and tooltip entirely

            float absoluteX = windowPos.x + screenPos.x;
            float absoluteY = windowPos.y + screenPos.y;

            // Readable billboard: the big icon font at a fixed pixel size (the default-font glyph
            // was a barely-clickable speck). Tinted with the light's colour so lights are
            // distinguishable at a glance.
            const char* icon     = ICON_MDI_LIGHTBULB;
            ImFont*     iconFont = EditorResources::GetBigIconFont();
            ImVec2      iconSize = iconFont->CalcTextSizeA( kLightIconSize, FLT_MAX, 0.0f, icon );

            ImDrawList* drawList   = ImGui::GetWindowDrawList();
            const ImVec4 lightColor( light.Color.r, light.Color.g, light.Color.b, 1.0f );

            drawList->AddText( iconFont, kLightIconSize,
                               ImVec2( absoluteX - iconSize.x * 0.5f, absoluteY - iconSize.y * 0.5f ),
                               ImColor( lightColor ), icon );

            if ( light.ShowRadius )
            {
                DrawLightRadiusSphere( camera, worldPos, light.Radius, width, height, windowPos.x,
                                       windowPos.y, absoluteX, absoluteY );
            }

            // Radius handle on the SELECTED light: a dot on the sphere, placed along the camera's right
            // axis so it is always facing the viewer and never hides behind the light itself.
            if ( IsSelected( entity ) )
            {
                const glm::mat4 view  = camera->GetViewMatrix();
                const glm::vec3 right = glm::normalize( glm::vec3( view[0][0], view[1][0], view[2][0] ) );

                glm::vec2 handleScreen;
                if ( ProjectToScreen( worldPos + right * light.Radius, mvp, width, height, handleScreen ) )
                {
                    const ImVec2 center( absoluteX, absoluteY );
                    const ImVec2 handle( windowPos.x + handleScreen.x, windowPos.y + handleScreen.y );
                    if ( DragValueHandle( HandleKind::PointRadius, entity.GetComponent<ECS::UUIDComponent>().UUID,
                                          center, handle, light.Radius, 1.0f, 100000.0f, "Radius",
                                          &light.Radius ) &&
                         !light.ShowRadius )
                    {
                        // Dragging the radius while the sphere is hidden is editing blind — show it.
                        DrawLightRadiusSphere( camera, worldPos, light.Radius, width, height, windowPos.x,
                                               windowPos.y, absoluteX, absoluteY );
                    }
                }
            }
            ImVec2 mousePos = ImGui::GetMousePos();
            if ( mousePos.x >= absoluteX - iconSize.x * 0.5f && mousePos.x <= absoluteX + iconSize.x * 0.5f &&
                 mousePos.y >= absoluteY - iconSize.y * 0.5f && mousePos.y <= absoluteY + iconSize.y * 0.5f )
            {
                m_LightIconHovered = true;
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                    Core::SelectionManager::SetSelected( entity.GetComponent<ECS::UUIDComponent>().UUID );
                ImGui::PushStyleColor( ImGuiCol_PopupBg, IM_COL32( 0, 0, 0, 0 ) );
                ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 0, 0, 0, 0 ) );
                Utils::ImGuiUtilities::Tooltip(
                     std::format( "Point Light\nIntensity: {}\nRadius: {}\nPosition: ({}, {}, {})",
                                  light.Intensity, light.Radius, worldPos.x, worldPos.y, worldPos.z )
                          .c_str() );
                ImGui::PopStyleColor( 2 );
            }
        }
    }

    void LightGizmoRenderer::RenderDirectionLights( const std::shared_ptr<Desert::Core::Camera>& camera,
                                                    float width, float height )
    {
        // Sun billboard + direction arrow. The light's DIRECTION is its Translation and the toward-sun
        // vector is -normalize(T) — the one convention, through ECS::Rules::AtmosphereSunDirection. The
        // arrow shows where the light actually points, which a bare transform never made visible.
        auto entities = m_Scene->GetAllEntities();

        const ImVec2 windowPos = ImGui::GetWindowPos();
        const auto   mvp       = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        ImDrawList*  drawList  = ImGui::GetWindowDrawList();

        for ( auto entity : entities )
        {
            if ( !entity.HasComponent<ECS::DirectionLightComponent>() )
                continue;

            // The entity's Translation is a DIRECTION encoding, not a place — drawing the icon there
            // parked it next to the origin. Instead the sun billboards IN THE SKY: camera-relative along
            // the toward-sun direction, like a skybox element — same screen spot for a given direction,
            // from anywhere.
            //
            // The negation below is the engine's ONE negation, and it used to be missing here: the gizmo
            // read Translation as if it pointed AT the sun while every other consumer read it as the
            // direction the light TRAVELS. Both errors cancelled as long as the shipped scenes authored
            // their suns upside down; once those were corrected, the compensating inversion had to go with
            // them, or the icon would sit exactly opposite the sun the sky draws.
            const glm::vec3 t = glm::vec3( entity.GetWorldTransform()[3] );
            if ( !::Desert::ECS::Rules::IsSunDirectionValid( t ) )
                continue; // undefined direction — nothing meaningful to draw

            const glm::vec3 towardSun = ::Desert::ECS::Rules::AtmosphereSunDirection( t ); // scene -> sun
            const glm::vec3 lightDir  = -towardSun;                                        // sun -> scene
            const glm::vec3 iconWorld = camera->GetPosition() + towardSun * 50.0f;

            glm::vec2 screenPos;
            if ( !ProjectToScreen( iconWorld, mvp, width, height, screenPos ) )
                continue; // sun is behind the camera

            const float absoluteX = windowPos.x + screenPos.x;
            const float absoluteY = windowPos.y + screenPos.y;

            const char* icon     = ICON_MDI_WHITE_BALANCE_SUNNY;
            ImFont*     iconFont = EditorResources::GetBigIconFont();
            ImVec2      iconSize = iconFont->CalcTextSizeA( kLightIconSize, FLT_MAX, 0.0f, icon );
            const ImU32 sunCol   = IM_COL32( 255, 214, 90, 255 );

            drawList->AddText( iconFont, kLightIconSize,
                               ImVec2( absoluteX - iconSize.x * 0.5f, absoluteY - iconSize.y * 0.5f ),
                               sunCol, icon );

            // Direction arrow: from the sun into the scene (the direction the LIGHT travels).
            {
                glm::vec2 tip;
                if ( ProjectToScreen( iconWorld + lightDir * 8.0f, mvp, width, height, tip ) )
                {
                    const ImVec2 from( absoluteX, absoluteY );
                    const ImVec2 to( windowPos.x + tip.x, windowPos.y + tip.y );
                    drawList->AddLine( from, to, sunCol, 2.0f );
                    // Arrow head: two short flicks back from the tip.
                    const ImVec2 d( to.x - from.x, to.y - from.y );
                    const float  len = std::sqrt( d.x * d.x + d.y * d.y );
                    if ( len > 8.0f )
                    {
                        const ImVec2 n( d.x / len, d.y / len );
                        const ImVec2 p( -n.y, n.x );
                        drawList->AddLine( to, ImVec2( to.x - n.x * 10 + p.x * 5, to.y - n.y * 10 + p.y * 5 ),
                                           sunCol, 2.0f );
                        drawList->AddLine( to, ImVec2( to.x - n.x * 10 - p.x * 5, to.y - n.y * 10 - p.y * 5 ),
                                           sunCol, 2.0f );
                    }
                }
            }

            const auto&  light    = entity.GetComponent<ECS::DirectionLightComponent>().Data;
            const ImVec2 mousePos = ImGui::GetMousePos();
            if ( mousePos.x >= absoluteX - iconSize.x * 0.5f && mousePos.x <= absoluteX + iconSize.x * 0.5f &&
                 mousePos.y >= absoluteY - iconSize.y * 0.5f && mousePos.y <= absoluteY + iconSize.y * 0.5f )
            {
                m_LightIconHovered = true;
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                    Core::SelectionManager::SetSelected( entity.GetComponent<ECS::UUIDComponent>().UUID );
                ImGui::PushStyleColor( ImGuiCol_PopupBg, IM_COL32( 0, 0, 0, 0 ) );
                ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 0, 0, 0, 0 ) );
                Utils::ImGuiUtilities::Tooltip(
                     std::format( "Directional Light (sun)\nIntensity: {}\nDirection: ({:.2f}, {:.2f}, {:.2f})",
                                  light.Intensity, lightDir.x, lightDir.y, lightDir.z )
                          .c_str() );
                ImGui::PopStyleColor( 2 );
            }
        }
    }

    void LightGizmoRenderer::RenderSpotLights( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                               float height, float /*xpos*/, float /*ypos*/ )
    {
        auto entities = m_Scene->GetAllEntities();

        ImVec2 windowPos = ImGui::GetWindowPos();

        for ( auto entity : entities )
        {
            if ( !entity.HasComponent<ECS::SpotLightComponent>() )
                continue;

            auto&           light    = entity.GetComponent<ECS::SpotLightComponent>().Data;
            const glm::mat4 worldXf  = entity.GetWorldTransform(); // parent-composed
            const glm::vec3 worldPos = glm::vec3( worldXf[3] );

            const auto mvp = camera->GetProjectionMatrix() * camera->GetViewMatrix();

            glm::vec2 screenPos;
            if ( !ProjectToScreen( worldPos, mvp, width, height, screenPos ) )
                continue;

            const float absoluteX = windowPos.x + screenPos.x;
            const float absoluteY = windowPos.y + screenPos.y;

            const char* icon     = ICON_MDI_SPOTLIGHT;
            ImFont*     iconFont = EditorResources::GetBigIconFont();
            ImVec2      iconSize = iconFont->CalcTextSizeA( kLightIconSize, FLT_MAX, 0.0f, icon );
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            drawList->AddText( iconFont, kLightIconSize,
                               ImVec2( absoluteX - iconSize.x * 0.5f, absoluteY - iconSize.y * 0.5f ),
                               ImColor( ImVec4( 1.0f, 0.9f, 0.5f, 1.0f ) ), icon );

            // Forward = entity's -Z in world space (matches the SpotLightECSSystem direction).
            const glm::vec3 forward = glm::normalize( -glm::vec3( worldXf[2] ) );

            if ( light.ShowCone )
                DrawSpotCone( camera, worldPos, forward, light.OuterConeAngle, light.Range, width,
                              height, windowPos.x, windowPos.y );

            // Range + cone handles on the SELECTED spot light.
            if ( IsSelected( entity ) )
            {
                const Common::UUID owner = entity.GetComponent<ECS::UUIDComponent>().UUID;
                const glm::vec3    end   = worldPos + forward * light.Range;

                glm::vec2    endScreen;
                const bool   endVisible = ProjectToScreen( end, mvp, width, height, endScreen );
                const ImVec2 apex( absoluteX, absoluteY );

                // Range: drag the dot at the cone's far end along the aim axis.
                if ( endVisible )
                {
                    const ImVec2 handle( windowPos.x + endScreen.x, windowPos.y + endScreen.y );
                    DragValueHandle( HandleKind::SpotRange, owner, apex, handle, light.Range, 1.0f, 100000.0f,
                                     "Range", &light.Range );
                }

                // Outer cone: the handle rides the cone's rim, and the drag scales tan(angle) — the rim's
                // distance from the axis IS range*tan(angle), so a proportional pull maps to it exactly.
                if ( endVisible )
                {
                    const glm::vec3 up   = std::abs( forward.y ) > 0.95f ? glm::vec3( 1.0f, 0.0f, 0.0f )
                                                                         : glm::vec3( 0.0f, 1.0f, 0.0f );
                    const glm::vec3 perp = glm::normalize( glm::cross( forward, up ) );

                    const float outer = glm::clamp( light.OuterConeAngle, 0.5f, 89.0f );
                    const float rim   = light.Range * std::tan( glm::radians( outer ) );

                    glm::vec2 rimScreen;
                    if ( ProjectToScreen( end + perp * rim, mvp, width, height, rimScreen ) )
                    {
                        const ImVec2 axisEnd( windowPos.x + endScreen.x, windowPos.y + endScreen.y );
                        const ImVec2 handle( windowPos.x + rimScreen.x, windowPos.y + rimScreen.y );

                        float tangent = std::tan( glm::radians( outer ) );
                        // The drag scales `tangent`, a LOCAL; the undo entry addresses the authored
                        // degrees on the component. Passing `&tangent` here is what the defect was.
                        if ( DragValueHandle( HandleKind::SpotOuterAngle, owner, axisEnd, handle, tangent,
                                              std::tan( glm::radians( 0.5f ) ), std::tan( glm::radians( 89.0f ) ),
                                              nullptr, &light.OuterConeAngle ) )
                        {
                            light.OuterConeAngle = glm::degrees( std::atan( tangent ) );
                            // The inner cone can never overtake the outer one (that inverts the falloff).
                            light.InnerConeAngle = glm::min( light.InnerConeAngle, light.OuterConeAngle );
                        }
                        // The handle edits tan(angle), so the readout has to be written here in degrees.
                        if ( m_ActiveHandle == HandleKind::SpotOuterAngle && m_ActiveHandleOwner == owner )
                            ImGui::SetTooltip( "Outer cone: %.0f deg", light.OuterConeAngle );
                    }
                }
            }

            ImVec2 mousePos = ImGui::GetMousePos();
            if ( mousePos.x >= absoluteX - iconSize.x * 0.5f && mousePos.x <= absoluteX + iconSize.x * 0.5f &&
                 mousePos.y >= absoluteY - iconSize.y * 0.5f && mousePos.y <= absoluteY + iconSize.y * 0.5f )
            {
                m_LightIconHovered = true;
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                    Core::SelectionManager::SetSelected( entity.GetComponent<ECS::UUIDComponent>().UUID );
                ImGui::PushStyleColor( ImGuiCol_PopupBg, IM_COL32( 0, 0, 0, 0 ) );
                ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 0, 0, 0, 0 ) );
                Utils::ImGuiUtilities::Tooltip(
                     std::format( "Spot Light\nIntensity: {}\nRange: {}\nCone: {} / {}\nPosition: ({}, {}, {})",
                                  light.Intensity, light.Range, light.InnerConeAngle, light.OuterConeAngle,
                                  worldPos.x, worldPos.y, worldPos.z )
                          .c_str() );
                ImGui::PopStyleColor( 2 );
            }
        }
    }

    void LightGizmoRenderer::RenderCameras( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                            float height, float /*xpos*/, float /*ypos*/ )
    {
        auto         entities  = m_Scene->GetAllEntities();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const auto   mvp       = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        ImDrawList*  drawList  = ImGui::GetWindowDrawList();

        for ( auto entity : entities )
        {
            if ( !entity.HasComponent<ECS::CameraComponent>() )
                continue;

            const auto& cam = entity.GetComponent<ECS::CameraComponent>().Data;
            // WORLD transform (walks parents) — a camera parented to e.g. a character must draw its icon and
            // frustum at the parent-composed position, not its local offset.
            const glm::mat4 worldXf  = entity.GetWorldTransform();
            const glm::vec3 worldPos = glm::vec3( worldXf[3] );

            // Billboard icon at the camera position.
            glm::vec2 screenPos;
            if ( ProjectToScreen( worldPos, mvp, width, height, screenPos ) )
            {
                const char*  icon     = ICON_MDI_VIDEO; // video/movie camera (not a photo camera)
                const ImVec2 iconSize = ImGui::CalcTextSize( icon );
                const float  ax       = windowPos.x + screenPos.x;
                const float  ay       = windowPos.y + screenPos.y;
                drawList->AddText( ImVec2( ax - iconSize.x * 0.5f, ay - iconSize.y * 0.5f ),
                                   ImColor( ImVec4( 0.6f, 0.85f, 1.0f, 1.0f ) ), icon );

                const ImVec2 m = ImGui::GetMousePos();
                if ( m.x >= ax - iconSize.x * 0.5f && m.x <= ax + iconSize.x * 0.5f &&
                     m.y >= ay - iconSize.y * 0.5f && m.y <= ay + iconSize.y * 0.5f )
                {
                    ImGui::PushStyleColor( ImGuiCol_PopupBg, IM_COL32( 0, 0, 0, 0 ) );
                    ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 0, 0, 0, 0 ) );
                    Utils::ImGuiUtilities::Tooltip(
                         std::format( "Camera{}\nFOV: {}\nNear: {}  Far: {}", cam.IsMainCamera ? " (Main)" : "",
                                      cam.FOV, cam.Near, cam.Far )
                              .c_str() );
                    ImGui::PopStyleColor( 2 );
                }
            }

            // View frustum wireframe. The real Far is 50 km -> draw only a SHORT, compact frustum
            // (a couple units deep) so the FOV/aspect shape reads clearly near the icon instead of a few
            // diverging lines streaking off-screen.
            const glm::mat4 world    = worldXf;
            const glm::vec3 pos      = worldPos;
            const glm::vec3 forward  = glm::normalize( -glm::vec3( world[2] ) );
            const glm::vec3 up       = glm::normalize( glm::vec3( world[1] ) );
            const float     aspect   = height > 0.0f ? width / height : 1.7778f;
            const float     gizmoFar = glm::min( cam.Far, cam.Near + 2.5f );
            const glm::mat4 camView  = glm::lookAt( pos, pos + forward, up );
            const glm::mat4 camProj =
                 ::Desert::Core::MakePerspective( glm::radians( cam.FOV ), aspect, cam.Near, gizmoFar );
            const glm::mat4 invVP = glm::inverse( camProj * camView );

            // NDC cube corners: 0-3 near, 4-7 far. z = 1 is the NEAR plane and z = 0 the far one, because
            // the matrix above is built by the engine's own reversed-Z factory (Core/Projection.hpp) —
            // the previous GL-convention -1/+1 pair would place four corners outside the clip volume
            // entirely and draw a frustum with its narrow end at the wrong side. Keep them in WORLD space
            // so we can clip each edge to the editor camera's near plane before projecting (a wide FOV
            // puts corners near/behind the editor camera; an unclipped perspective divide then streaks
            // lines off-screen).
            static const glm::vec3 ndc[8] = { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 },
                                              { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 } };
            glm::vec3              corners[8];
            for ( int i = 0; i < 8; ++i )
            {
                glm::vec4 w = invVP * glm::vec4( ndc[i], 1.0f );
                corners[i]  = glm::vec3( w ) / w.w;
            }

            static const int edges[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
                                              { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            const ImU32      col = IM_COL32( 150, 220, 255, 200 );

            const auto toScreen = [&]( const glm::vec4& clip ) -> ImVec2
            {
                const glm::vec3 n = glm::vec3( clip ) / clip.w;
                return ImVec2( windowPos.x + ( n.x * 0.5f + 0.5f ) * width,
                               windowPos.y + ( 1.0f - ( n.y * 0.5f + 0.5f ) ) * height );
            };

            for ( const auto& e : edges )
            {
                glm::vec4   ca   = mvp * glm::vec4( corners[e[0]], 1.0f );
                glm::vec4   cb   = mvp * glm::vec4( corners[e[1]], 1.0f );
                const float kEps = 1e-3f;
                if ( ca.w <= kEps && cb.w <= kEps )
                    continue; // both behind the editor camera
                // Clip the endpoint that crosses the near plane (w = kEps) so the line never wraps.
                if ( ca.w <= kEps )
                    ca = ca + ( ( kEps - ca.w ) / ( cb.w - ca.w ) ) * ( cb - ca );
                else if ( cb.w <= kEps )
                    cb = cb + ( ( kEps - cb.w ) / ( ca.w - cb.w ) ) * ( ca - cb );
                drawList->AddLine( toScreen( ca ), toScreen( cb ), col, 1.5f );
            }
        }
    }

    void LightGizmoRenderer::DrawWorldLine( ImDrawList* drawList, const glm::vec3& a, const glm::vec3& b,
                                            const glm::mat4& mvp, float width, float height, float windowX,
                                            float windowY, ImU32 color, float thickness )
    {
        glm::vec4         ca   = mvp * glm::vec4( a, 1.0f );
        glm::vec4         cb   = mvp * glm::vec4( b, 1.0f );
        constexpr float   kEps = 1e-3f;
        if ( ca.w <= kEps && cb.w <= kEps )
            return; // both behind the editor camera

        if ( ca.w <= kEps )
            ca = ca + ( ( kEps - ca.w ) / ( cb.w - ca.w ) ) * ( cb - ca );
        else if ( cb.w <= kEps )
            cb = cb + ( ( kEps - cb.w ) / ( ca.w - cb.w ) ) * ( ca - cb );

        const auto toScreen = [&]( const glm::vec4& clip ) -> ImVec2
        {
            const glm::vec3 n = glm::vec3( clip ) / clip.w;
            return ImVec2( windowX + ( n.x * 0.5f + 0.5f ) * width,
                           windowY + ( 1.0f - ( n.y * 0.5f + 0.5f ) ) * height );
        };

        drawList->AddLine( toScreen( ca ), toScreen( cb ), color, thickness );
    }

    void LightGizmoRenderer::RenderSkeleton( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                             float height, float /*xpos*/, float /*ypos*/ )
    {
        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected )
            return;
        const auto& entOpt = m_Scene->FindEntityByID( *selected );
        if ( !entOpt )
            return;
        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::SkinnedMeshComponent>() )
            return;

        const auto&   smc  = entity.GetComponent<ECS::SkinnedMeshComponent>();
        Desert::Mesh* mesh = smc.RuntimeMesh ? static_cast<Desert::Mesh*>( smc.RuntimeMesh.get() )
                                             : Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle );
        if ( !mesh || !mesh->IsSkinned() )
            return;
        const auto& bones = static_cast<SkinnedMesh*>( mesh )->GetSkeleton().GetBones();
        if ( bones.empty() )
            return;

        const glm::mat4 entityWorld = entity.GetComponent<ECS::TransformComponent>().GetTransform();
        const glm::mat4 mvp         = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        const ImVec2    windowPos   = ImGui::GetWindowPos();
        ImDrawList*     drawList    = ImGui::GetWindowDrawList();

        const int selectedBone = Core::SkeletonEditMode::GetSelectedBone();

        // Bone head (world) = entityWorld * chainGlobal[3], where chainGlobal = the parent chain of
        // LocalBindTransform (= the Animator's bind global). This MATCHES the rendered mesh, which is skinned
        // with bind bone matrices = chainGlobal * OffsetMatrix (NOT identity). Using inverse(OffsetMatrix)
        // instead put the bones at the raw-vertex scale (thousands of units) while the mesh renders at the
        // chain scale — hence "bones much bigger than the mesh". Memoized so bone array order doesn't matter.
        std::vector<glm::mat4>             chainGlobal( bones.size(), glm::mat4( 1.0f ) );
        std::vector<bool>                  done( bones.size(), false );
        std::function<glm::mat4( size_t )> resolve = [&]( size_t i ) -> glm::mat4
        {
            if ( done[i] )
                return chainGlobal[i];
            glm::mat4 g = bones[i].LocalBindTransform;
            if ( bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < bones.size() )
                g = resolve( bones[i].ParentBoneID.value() ) * bones[i].LocalBindTransform;
            chainGlobal[i] = g;
            done[i]        = true;
            return g;
        };
        // Use the SAME pose the mesh is RENDERED with, so bones overlay the actual (possibly animated) mesh
        // — not the static bind pose. The render skins with BoneMatrices[i] = globalPosed_i * OffsetMatrix_i,
        // so the bone's posed global = BoneMatrices[i] * inverse(OffsetMatrix_i) and its head = that [3]. When
        // there is no Animator, fall back to the bind chain (== what MeshECSSystem feeds as the bind pose).
        // Skeleton Edit renders the mesh in BIND pose (SelectionContext bind-preview), so overlay the bones
        // in bind pose too — an animated overlay would sit off the (bind) mesh and the gizmo. Kept nullptr so
        // the head resolve below uses the bind chain.
        const std::vector<glm::mat4>* poseMatrices = nullptr;

        std::vector<glm::vec3> heads( bones.size() );
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            glm::mat4 global;
            if ( poseMatrices && i < poseMatrices->size() )
                global = ( *poseMatrices )[i] * glm::inverse( bones[i].OffsetMatrix );
            else
                global = resolve( i ); // bind pose (no animator)
            heads[i] = glm::vec3( entityWorld * glm::vec4( glm::vec3( global[3] ), 1.0f ) );
        }

        // Project every bone head to absolute-screen once (nullopt when behind the camera).
        std::vector<std::optional<ImVec2>> screen( bones.size() );
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            glm::vec2 s;
            if ( ProjectToScreen( heads[i], mvp, width, height, s ) )
                screen[i] = ImVec2( windowPos.x + s.x, windowPos.y + s.y );
        }

        std::vector<int>         parents( bones.size() );
        std::vector<std::string> names( bones.size() );
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            parents[i]             = -1;
            const auto& parentBone = bones[i].ParentBoneID;
            if ( parentBone.has_value() )
            {
                parents[i] = static_cast<int>( *parentBone );
            }
            names[i] = bones[i].Name;
        }

        DrawBoneGizmos( drawList, screen, parents, names, selectedBone, Core::SkeletonEditMode::ShowAllNames(),
                        /*recordForPick=*/true );
    }

    void LightGizmoRenderer::DrawBoneGizmos( ImDrawList*                               drawList,
                                             const std::vector<std::optional<ImVec2>>& screen,
                                             const std::vector<int>&                   parents,
                                             const std::vector<std::string>& names, int selectedBone,
                                             bool showAllNames, bool recordForPick )
    {
        const ImVec2 mouse = ImGui::GetMousePos();

        // UE-style bones: each parent->child link is a tapered octahedron (a 2D "kite" widest ~20% from the
        // parent). A translucent fill + bright edge reads as a solid bone rather than a bare line.
        const ImU32 boneFill    = IM_COL32( 200, 215, 240, 55 );
        const ImU32 boneEdge    = IM_COL32( 225, 235, 255, 190 );
        const ImU32 boneFillSel = IM_COL32( 255, 165, 60, 110 );
        const ImU32 boneEdgeSel = IM_COL32( 255, 190, 90, 255 );
        for ( size_t i = 0; i < screen.size(); ++i )
        {
            const int p = parents[i];
            if ( p < 0 || p >= static_cast<int>( screen.size() ) )
            {
                continue;
            }
            // Each projected point is read ONCE. Subscripting twice — once to test, once to unwrap — makes
            // the guard and the use two different objects to anything reasoning about this loop.
            const auto& parentPoint = screen[static_cast<size_t>( p )];
            const auto& childPoint  = screen[i];
            if ( !parentPoint.has_value() || !childPoint.has_value() )
            {
                continue;
            }

            const ImVec2 P  = *parentPoint;
            const ImVec2 C  = *childPoint;
            const float  dx = C.x - P.x, dy = C.y - P.y;
            const float  len = std::sqrt( dx * dx + dy * dy );
            if ( len < 1.0f )
                continue;
            const ImVec2 dir( dx / len, dy / len );
            const ImVec2 perp( -dir.y, dir.x );
            const float  w = std::clamp( len * 0.16f, 2.5f, 12.0f );                // octahedron half-width
            const ImVec2 mid( P.x + dir.x * len * 0.2f, P.y + dir.y * len * 0.2f ); // widest ring
            ImVec2       kite[4] = { P, ImVec2( mid.x + perp.x * w, mid.y + perp.y * w ), C,
                                     ImVec2( mid.x - perp.x * w, mid.y - perp.y * w ) };
            const bool   sel     = ( static_cast<int>( i ) == selectedBone || p == selectedBone );
            drawList->AddConvexPolyFilled( kite, 4, sel ? boneFillSel : boneFill );
            drawList->AddPolyline( kite, 4, sel ? boneEdgeSel : boneEdge, ImDrawFlags_Closed, sel ? 2.0f : 1.5f );
        }

        // UE-style joints: a filled "sphere" (disc + dark rim) at each bone head. Root is cyan, the
        // selected/hovered joint is accented + enlarged. Records absolute-screen positions for PickBone.
        const ImU32 jointCol    = IM_COL32( 240, 220, 120, 255 );
        const ImU32 jointRoot   = IM_COL32( 90, 220, 235, 255 );
        const ImU32 jointSel    = IM_COL32( 255, 140, 40, 255 );
        const ImU32 jointRim    = IM_COL32( 25, 25, 30, 220 );
        const ImU32 labelCol    = IM_COL32( 220, 220, 230, 220 );
        const ImU32 labelSelCol = IM_COL32( 255, 175, 95, 255 );
        if ( recordForPick )
            m_BoneScreenPositions.clear();
        for ( size_t i = 0; i < screen.size(); ++i )
        {
            const auto& point = screen[i];
            if ( !point.has_value() )
            {
                continue;
            }
            const ImVec2 c = *point;
            if ( recordForPick )
                m_BoneScreenPositions.emplace_back( static_cast<int>( i ), c ); // absolute-screen — for PickBone

            const bool  sel     = ( static_cast<int>( i ) == selectedBone );
            const bool  hovered = ( std::abs( mouse.x - c.x ) < 7.0f && std::abs( mouse.y - c.y ) < 7.0f );
            const bool  isRoot  = ( parents[i] < 0 );
            const float r       = sel ? 6.0f : ( hovered ? 5.5f : 4.0f );
            const ImU32 fill    = sel ? jointSel : ( isRoot ? jointRoot : jointCol );

            // Soft outer glow on the interactive handle so it reads as grabbable (accent when selected).
            if ( sel || hovered )
                drawList->AddCircleFilled(
                     c, r + 4.0f, sel ? IM_COL32( 255, 140, 40, 55 ) : IM_COL32( 255, 255, 255, 40 ), 24 );
            drawList->AddCircleFilled( c, r, fill, 24 );
            // Sphere shine: a small offset highlight so the disc reads as a 3D ball, not a flat dot.
            drawList->AddCircleFilled( ImVec2( c.x - r * 0.3f, c.y - r * 0.3f ), r * 0.35f,
                                       IM_COL32( 255, 255, 255, 150 ), 12 );
            drawList->AddCircle( c, r, jointRim, 24, 1.5f ); // dark rim -> depth
            if ( sel )
                drawList->AddCircle( c, r + 2.5f, IM_COL32( 255, 255, 255, 220 ), 24, 1.5f ); // selected ring
            else if ( hovered )
                drawList->AddCircle( c, r + 2.5f, IM_COL32( 255, 255, 255, 140 ), 24, 1.0f ); // hover ring

            // Label the selected/hovered bone by default; "Names" toggle shows them all (dense rigs blob).
            if ( i < names.size() && !names[i].empty() && ( sel || hovered || showAllNames ) )
                drawList->AddText( ImVec2( c.x + r + 4.0f, c.y - 7.0f ), sel ? labelSelCol : labelCol,
                                   names[i].c_str() );
        }
    }

    void LightGizmoRenderer::RenderRigBuilder( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                               float height )
    {
        if ( !RigBuilder::IsActive() )
            return;
        const auto& entOpt = m_Scene->FindEntityByID( RigBuilder::Target() );
        if ( !entOpt )
            return;
        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::TransformComponent>() )
            return;

        const auto& bones = RigBuilder::Bones();
        if ( bones.empty() )
            return;

        // Heads live in mesh-LOCAL space; lift to world with the entity transform (mirrors RenderSkeleton).
        const glm::mat4 entityWorld = entity.GetComponent<ECS::TransformComponent>().GetTransform();
        const glm::mat4 mvp         = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        const ImVec2    windowPos   = ImGui::GetWindowPos();
        ImDrawList*     drawList    = ImGui::GetWindowDrawList();

        std::vector<std::optional<ImVec2>> screen( bones.size() );
        std::vector<int>                   parents( bones.size() );
        std::vector<std::string>           names( bones.size() );
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            parents[i]                = bones[i].Parent;
            names[i]                  = bones[i].Name;
            const glm::vec3 headWorld = glm::vec3( entityWorld * glm::vec4( bones[i].Head, 1.0f ) );
            glm::vec2       s;
            if ( ProjectToScreen( headWorld, mvp, width, height, s ) )
                screen[i] = ImVec2( windowPos.x + s.x, windowPos.y + s.y );
        }

        DrawBoneGizmos( drawList, screen, parents, names, RigBuilder::SelectedBone(), /*showAllNames=*/true,
                        /*recordForPick=*/false );
    }

    int LightGizmoRenderer::PickBone( const ImVec2& absMouse, float radiusPx ) const
    {
        int   best      = -1;
        float bestDist2 = radiusPx * radiusPx;
        for ( const auto& [idx, pos] : m_BoneScreenPositions )
        {
            const float dx = pos.x - absMouse.x;
            const float dy = pos.y - absMouse.y;
            const float d2 = dx * dx + dy * dy;
            if ( d2 <= bestDist2 )
            {
                bestDist2 = d2;
                best      = idx;
            }
        }
        return best;
    }

    void LightGizmoRenderer::RenderSpawnIcons( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                               float height )
    {
        auto         entities  = m_Scene->GetAllEntities();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const auto   mvp       = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        ImDrawList*  drawList  = ImGui::GetWindowDrawList();
        const ImVec2 mouse     = ImGui::GetMousePos();

        for ( auto entity : entities )
        {
            // Only entities WITHOUT a rendered/gizmo'd representation get a billboard — anything already
            // visible (mesh/terrain/foliage/light/camera/skybox) is skipped so we don't double-mark it.
            if ( entity.HasComponent<ECS::StaticMeshComponent>() ||
                 entity.HasComponent<ECS::SkinnedMeshComponent>() ||
                 entity.HasComponent<ECS::InstancedStaticMeshComponent>() ||
                 entity.HasComponent<ECS::TerrainComponent>() || entity.HasComponent<ECS::FoliageComponent>() ||
                 entity.HasComponent<ECS::PointLightComponent>() ||
                 entity.HasComponent<ECS::SpotLightComponent>() ||
                 entity.HasComponent<ECS::DirectionLightComponent>() ||
                 entity.HasComponent<ECS::CameraComponent>() || entity.HasComponent<ECS::SkyboxComponent>() ||
                 entity.HasComponent<ECS::TextComponent>() || // Text has its own big billboard (RenderTextIcons)
                 entity.HasComponent<ECS::UICanvasComponent>() ||
                 entity.HasComponent<ECS::UILayoutComponent>() ) // screen-space UI: edited via the canvas
                                                                 // overlay, a world billboard just confuses
                continue;

            // Pick the icon by the most specific "invisible" role the entity plays.
            const char* icon  = ICON_MDI_AXIS_ARROW; // generic empty / transform helper
            ImVec4      color = ImVec4( 0.75f, 0.78f, 0.85f, 1.0f );
            const char* label = "Empty";
            if ( entity.HasComponent<ECS::AudioSourceComponent>() )
            {
                icon  = ICON_MDI_VOLUME_HIGH;
                color = ImVec4( 0.60f, 0.90f, 0.70f, 1.0f );
                label = "Audio Source";
            }
            else if ( entity.HasComponent<ECS::CharacterControllerComponent>() ||
                      entity.HasComponent<ECS::ProjectileComponent>() )
            {
                icon  = ICON_MDI_MAP_MARKER;
                color = ImVec4( 1.00f, 0.80f, 0.40f, 1.0f );
                label = "Spawn Point";
            }
            else if ( entity.HasComponent<ECS::ColliderComponent>() &&
                      !entity.HasComponent<ECS::RigidBodyComponent>() )
            {
                icon  = ICON_MDI_SHAPE_OUTLINE;
                color = ImVec4( 0.50f, 0.85f, 1.00f, 1.0f );
                label = "Trigger / Volume";
            }
            else if ( entity.HasComponent<ECS::ScriptComponent>() )
            {
                icon  = ICON_MDI_SCRIPT;
                color = ImVec4( 0.85f, 0.70f, 1.00f, 1.0f );
                label = "Script";
            }

            const glm::vec3 worldPos = glm::vec3( entity.GetWorldTransform()[3] );
            glm::vec2       screenPos;
            if ( !ProjectToScreen( worldPos, mvp, width, height, screenPos ) )
                continue;

            const float  ax       = windowPos.x + screenPos.x;
            const float  ay       = windowPos.y + screenPos.y;
            ImFont*      iconFont = EditorResources::GetBigIconFont();
            const ImVec2 sz       = iconFont->CalcTextSizeA( kIconSize, FLT_MAX, 0.0f, icon );
            drawList->AddText( iconFont, kIconSize, ImVec2( ax - sz.x * 0.5f, ay - sz.y * 0.5f ), ImColor( color ),
                               icon );

            if ( mouse.x >= ax - sz.x * 0.5f && mouse.x <= ax + sz.x * 0.5f && mouse.y >= ay - sz.y * 0.5f &&
                 mouse.y <= ay + sz.y * 0.5f )
            {
                const std::string name = entity.HasComponent<ECS::TagComponent>()
                                              ? entity.GetComponent<ECS::TagComponent>().Tag
                                              : std::string( "Actor" );
                ImGui::PushStyleColor( ImGuiCol_PopupBg, IM_COL32( 0, 0, 0, 0 ) );
                ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 0, 0, 0, 0 ) );
                Utils::ImGuiUtilities::Tooltip( std::format( "{}\n{}\nPosition: ({:.2f}, {:.2f}, {:.2f})", name,
                                                             label, worldPos.x, worldPos.y, worldPos.z )
                                                     .c_str() );
                ImGui::PopStyleColor( 2 );
            }
        }
    }

    void LightGizmoRenderer::RenderTextIcons( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                              float height )
    {
        auto         entities  = m_Scene->GetAllEntities();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const auto   mvp       = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        ImDrawList*  drawList  = ImGui::GetWindowDrawList();
        const ImVec2 mouse     = ImGui::GetMousePos();

        ImFont* iconFont = EditorResources::GetBigIconFont();

        for ( auto entity : entities )
        {
            if ( !entity.HasComponent<ECS::TextComponent>() )
                continue;

            const glm::vec3 worldPos = glm::vec3( entity.GetWorldTransform()[3] );
            glm::vec2       screenPos;
            if ( !ProjectToScreen( worldPos, mvp, width, height, screenPos ) )
                continue;

            const float  ax   = windowPos.x + screenPos.x;
            const float  ay   = windowPos.y + screenPos.y;
            const char*  icon = ICON_MDI_FORMAT_TEXT;
            const ImVec2 sz   = iconFont->CalcTextSizeA( kIconSize, FLT_MAX, 0.0f, icon );

            // Text colour tint so the marker reads as "this is the label" at a glance.
            const auto&  tc = entity.GetComponent<ECS::TextComponent>();
            const ImVec4 col( tc.Color.r, tc.Color.g, tc.Color.b, 1.0f );
            drawList->AddText( iconFont, kIconSize, ImVec2( ax - sz.x * 0.5f, ay - sz.y * 0.5f ), ImColor( col ),
                               icon );

            if ( mouse.x >= ax - sz.x * 0.5f && mouse.x <= ax + sz.x * 0.5f && mouse.y >= ay - sz.y * 0.5f &&
                 mouse.y <= ay + sz.y * 0.5f )
            {
                m_LightIconHovered = true; // shares the icon-hover gate so scene ray-pick doesn't fire under it
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                    Core::SelectionManager::SetSelected( entity.GetComponent<ECS::UUIDComponent>().UUID );

                const std::string name = entity.HasComponent<ECS::TagComponent>()
                                              ? entity.GetComponent<ECS::TagComponent>().Tag
                                              : std::string( "Text" );
                ImGui::PushStyleColor( ImGuiCol_PopupBg, IM_COL32( 0, 0, 0, 0 ) );
                ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 0, 0, 0, 0 ) );
                Utils::ImGuiUtilities::Tooltip(
                     std::format( "{}\nText: \"{}\"\nPosition: ({:.2f}, {:.2f}, {:.2f})", name, tc.Text,
                                  worldPos.x, worldPos.y, worldPos.z )
                          .c_str() );
                ImGui::PopStyleColor( 2 );
            }
        }
    }

    void LightGizmoRenderer::DrawSpotCone( const std::shared_ptr<Desert::Core::Camera>& camera,
                                           const glm::vec3& apex, const glm::vec3& dir, float outerAngleDeg,
                                           float range, float width, float height, float windowX, float windowY )
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const auto  mvp      = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        const ImU32 color    = ImColor( 1.0f, 0.9f, 0.5f, 0.85f );

        // Build an orthonormal basis around the cone axis.
        glm::vec3 up = ( glm::abs( dir.y ) > 0.99f ) ? glm::vec3( 1.0f, 0.0f, 0.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
        glm::vec3 right = glm::normalize( glm::cross( dir, up ) );
        up             = glm::normalize( glm::cross( right, dir ) );

        const float  outer       = glm::radians( outerAngleDeg );
        const float  capRadius   = range * glm::tan( outer );
        const glm::vec3 capCenter = apex + dir * range;

        const int    segments = 48;
        glm::vec2    prev;
        bool         prevValid = false;
        glm::vec2    apexS;
        const bool   apexValid = ProjectToScreen( apex, mvp, width, height, apexS );

        for ( int i = 0; i <= segments; ++i )
        {
            const float a   = 2.0f * glm::pi<float>() * i / segments;
            glm::vec3   p   = capCenter + capRadius * ( glm::cos( a ) * right + glm::sin( a ) * up );
            glm::vec2   s;
            const bool  ok  = ProjectToScreen( p, mvp, width, height, s );
            const ImVec2 sp = ImVec2( windowX + s.x, windowY + s.y );

            if ( ok && prevValid )
                drawList->AddLine( ImVec2( windowX + prev.x, windowY + prev.y ), sp, color, 1.5f );
            // A few rib lines from the apex to the cap edge.
            if ( ok && apexValid && ( i % 12 == 0 ) )
                drawList->AddLine( ImVec2( windowX + apexS.x, windowY + apexS.y ), sp, color, 1.5f );

            prev      = s;
            prevValid = ok;
        }
    }

    bool LightGizmoRenderer::IsSelected( const ECS::Entity& entity ) const
    {
        const auto selected = Core::SelectionManager::GetSelected();
        return selected.has_value() && entity.HasComponent<ECS::UUIDComponent>() &&
               entity.GetComponent<ECS::UUIDComponent>().UUID == *selected;
    }

    bool LightGizmoRenderer::DragValueHandle( HandleKind kind, const Common::UUID& owner, const ImVec2& center,
                                              const ImVec2& handle, float& value, float minValue, float maxValue,
                                              const char* tooltip, float* undoTarget )
    {
        constexpr float kHandleRadius = 5.0f;
        constexpr float kGrabRadius   = 9.0f; // forgiving hit area; the dot itself stays small

        ImDrawList*  drawList = ImGui::GetWindowDrawList();
        const ImVec2 mouse    = ImGui::GetMousePos();

        const auto distance = []( const ImVec2& a, const ImVec2& b )
        { return std::sqrt( ( a.x - b.x ) * ( a.x - b.x ) + ( a.y - b.y ) * ( a.y - b.y ) ); };

        const bool active  = m_ActiveHandle == kind && m_ActiveHandleOwner == owner;
        const bool hovered = distance( mouse, handle ) <= kGrabRadius;

        // A drag starts only on a fresh press over the dot, and only when nothing else is grabbed.
        if ( hovered && m_ActiveHandle == HandleKind::None && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            m_ActiveHandle      = kind;
            m_ActiveHandleOwner = owner;
            m_DragStartValue    = value;
            m_DragStartAuthored = undoTarget ? *undoTarget : value;
            m_DragStartDistance = distance( center, handle );
        }

        bool changed = false;
        if ( active )
        {
            if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                // One undo entry for the whole drag, not one per frame. Addressed through THIS frame's
                // undoTarget, never a pointer kept since mouse-down.
                if ( undoTarget && m_DragStartAuthored != *undoTarget )
                {
                    const float oldValue = m_DragStartAuthored;
                    CommandHistory::Get().Push( undoTarget, &oldValue, undoTarget, sizeof( float ) );
                }
                m_ActiveHandle = HandleKind::None;
            }
            else if ( m_DragStartDistance > 2.0f )
            {
                const float now = distance( center, mouse );
                value   = glm::clamp( m_DragStartValue * ( now / m_DragStartDistance ), minValue, maxValue );
                changed = true;
            }
        }

        const ImU32 fill = active    ? IM_COL32( 255, 190, 60, 255 )
                           : hovered ? IM_COL32( 255, 230, 150, 255 )
                                     : IM_COL32( 240, 240, 240, 210 );
        drawList->AddLine( center, handle, IM_COL32( 255, 255, 255, 90 ), 1.0f );
        drawList->AddCircleFilled( handle, kHandleRadius, fill, 16 );
        drawList->AddCircle( handle, kHandleRadius, IM_COL32( 25, 25, 25, 220 ), 16 );

        if ( hovered || active )
        {
            // Shares the gate that stops the scene ray-pick firing under a gizmo.
            m_LightIconHovered = true;
            if ( tooltip )
                ImGui::SetTooltip( "%s: %.0f", tooltip, value );
        }

        return changed;
    }

    void LightGizmoRenderer::DrawLightRadiusSphere( const std::shared_ptr<Desert::Core::Camera>& camera,
                                                    const glm::vec3& worldPos, float radius, float width,
                                                    float height, float windowX, float windowY,
                                                    float /*iconCenterX*/, float /*iconCenterY*/ )
    {
        ImDrawList* drawList         = ImGui::GetWindowDrawList();
        const auto  viewMatrix       = camera->GetViewMatrix();
        const auto  projectionMatrix = camera->GetProjectionMatrix();
        const auto  mvp              = projectionMatrix * viewMatrix;

        const int segments = 64;

        ImU32 colorWhite = ImColor( 1.0f, 1.0f, 1.0f, 1.0f );

        DrawAxisAlignedCircle( drawList, worldPos, radius, segments, glm::vec3( 1.0f, 0.0f, 0.0f ),
                               glm::vec3( 0.0f, 0.0f, 1.0f ), mvp, width, height, windowX, windowY, colorWhite );

        DrawAxisAlignedCircle( drawList, worldPos, radius, segments, glm::vec3( 1.0f, 0.0f, 0.0f ),
                               glm::vec3( 0.0f, 1.0f, 0.0f ), mvp, width, height, windowX, windowY, colorWhite );

        DrawAxisAlignedCircle( drawList, worldPos, radius, segments, glm::vec3( 0.0f, 1.0f, 0.0f ),
                               glm::vec3( 0.0f, 0.0f, 1.0f ), mvp, width, height, windowX, windowY, colorWhite );
    }

    void LightGizmoRenderer::DrawAxisAlignedCircle( ImDrawList* drawList, const glm::vec3& center, float radius,
                                                    int segments, const glm::vec3& axis1, const glm::vec3& axis2,
                                                    const glm::mat4& mvp, float width, float height, float windowX,
                                                    float windowY, ImU32 color )
    {
        std::vector<ImVec2> screenPoints( segments + 1 );
        std::vector<bool>   valid( segments + 1, false );

        for ( int i = 0; i <= segments; ++i )
        {
            float     angle = 2.0f * glm::pi<float>() * i / segments;
            glm::vec3 point = center + radius * ( cos( angle ) * axis1 + sin( angle ) * axis2 );

            glm::vec2 screenPoint;
            valid[i] = ProjectToScreen( point, mvp, width, height, screenPoint );
            if ( valid[i] )
                screenPoints[i] = ImVec2( windowX + screenPoint.x, windowY + screenPoint.y );
        }

        // Only connect neighbouring points that are BOTH in front of the camera — otherwise a segment
        // crossing behind the near plane would draw a line streaking across the whole viewport.
        for ( int i = 1; i <= segments; ++i )
        {
            if ( valid[i - 1] && valid[i] )
                drawList->AddLine( screenPoints[i - 1], screenPoints[i], color, 1.5f );
        }
    }

} // namespace Desert::Editor