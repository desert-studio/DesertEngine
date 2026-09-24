#include "MaterialsPanelComponent.hpp"
#include <Common/Content/CanonicalText.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <ImGui/imgui.h>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

// rfl serialization environment (same as SurfaceMaterialAsset.cpp) — used to write a fresh
// material file with its stable GUID before the asset is created/registered.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    MaterialComponentWidget::MaterialComponentWidget( const Assets::AssetManager* assetManager )
         : m_AssetManager( assetManager )
    {
        m_UIHelper = std::make_unique<Editor::UI::UIHelper>();
        m_UIHelper->Init();
    }

    MaterialComponentWidget::MaterialHost MaterialComponentWidget::HostOf( ECS::Entity& entity )
    {
        MaterialHost host;
        if ( entity.HasComponent<ECS::StaticMeshComponent>() )
        {
            auto& c               = entity.GetComponent<ECS::StaticMeshComponent>();
            host.Slots            = &c.MaterialSlots;
            host.RuntimeInstances = &c.RuntimeMaterialInstances;
            host.MeshHandle       = c.MeshHandle;
            host.Mesh             = c.RuntimeMesh ? static_cast<::Desert::Mesh*>( c.RuntimeMesh.get() )
                                                  : Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
        }
        else if ( entity.HasComponent<ECS::SkinnedMeshComponent>() )
        {
            auto& c               = entity.GetComponent<ECS::SkinnedMeshComponent>();
            host.Slots            = &c.MaterialSlots;
            host.RuntimeInstances = &c.RuntimeMaterialInstances;
            host.MeshHandle       = c.MeshHandle;
            host.Mesh             = c.RuntimeMesh ? static_cast<::Desert::Mesh*>( c.RuntimeMesh.get() )
                                                  : Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
        }
        else if ( entity.HasComponent<ECS::InstancedStaticMeshComponent>() )
        {
            // THE THIRD MESH KIND, and it carries the same pair for the same reason: MeshECSSystem
            // builds its RuntimeMaterialInstances from MaterialSlots exactly as the static path does.
            // Until this branch existed an ISM had NO material UI at all — its slots could only be
            // written by editing the scene file — which made "one draw for five hundred props" a
            // feature you could not give a look to.
            //
            // A primitive ISM has no asset mesh, so `Mesh` resolves through the generated RuntimeMesh
            // the same way the static twin's does.
            auto& c               = entity.GetComponent<ECS::InstancedStaticMeshComponent>();
            host.Slots            = &c.MaterialSlots;
            host.RuntimeInstances = &c.RuntimeMaterialInstances;
            host.MeshHandle       = c.MeshHandle;
            host.Mesh             = c.RuntimeMesh ? static_cast<::Desert::Mesh*>( c.RuntimeMesh.get() )
                                                  : Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
        }
        return host;
    }

    void MaterialComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* scene )
    {
        const MaterialHost host = HostOf( entity );
        if ( !host.Slots )
            return;

        // A Shader Override component with a custom (non-PBR) shader takes this mesh off the PBR
        // path entirely — surface that here so the slots below don't look mysteriously dead.
        std::string overriddenBy;
        if ( entity.HasComponent<ECS::MaterialComponent>() )
        {
            const auto& matc = entity.GetComponent<ECS::MaterialComponent>();
            if ( !matc.ShaderName.empty() && matc.ShaderName != "StaticMeshPBR" &&
                 matc.ShaderName != "SkinnedMeshPBR" )
                overriddenBy = matc.ShaderName;
        }

        // NOTE: the per-entity "Runtime Override" param editor was removed — meshes get their look ONLY
        // from material-asset SLOTS now (UE-style). The MaterialComponent param channel survives purely as
        // the scripting (Lua setMaterialParam) + legacy-scene compat path; it is no longer editor-authored.

        RenderMaterialProperties( entity, host, overriddenBy );

        // Quick way back to the PBR path without hunting for the Shader Override section.
        if ( !overriddenBy.empty() )
        {
            if ( ImGui::Button( "Clear runtime override (use material slots)" ) )
            {
                auto& matc = entity.GetComponent<ECS::MaterialComponent>();
                matc.ShaderName.clear();
                matc.Params.clear();
                matc.Textures.clear();
            }
        }

        // Level of Detail + the static-only rendering flags (outline, per-submesh visibility) live on the
        // STATIC mesh component; a skinned entity gets the one rendering control its component carries —
        // Cast Shadows, in the same section and wording as the static twin, so the toggle is found in the
        // same place on both kinds of mesh.
        if ( !entity.HasComponent<ECS::StaticMeshComponent>() )
        {
            if ( entity.HasComponent<ECS::SkinnedMeshComponent>() &&
                 Utils::ImGuiUtilities::SectionHeader( ICON_MDI_EYE "  Rendering", false ) )
            {
                auto& skinnedComp = entity.GetComponent<ECS::SkinnedMeshComponent>();
                Utils::ImGuiUtilities::ResetPropertyRows();
                Utils::ImGuiUtilities::BeginPropertyRow( "Cast Shadows",
                                                         "Skip this mesh in the shadow (depth) passes" );
                ImGui::Checkbox( "##castshadows", &skinnedComp.CastShadows );
                Utils::ImGuiUtilities::EndPropertyRow();
            }
            return;
        }

        auto&           materialComp = entity.GetComponent<ECS::StaticMeshComponent>();
        ::Desert::Mesh* lodMesh      = host.Mesh;

        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_LAYERS_TRIPLE "  Level of Detail", false ) )
        {
            size_t levels = 0;
            if ( lodMesh )
                for ( const auto& sm : lodMesh->GetSubmeshes() )
                    if ( sm.LODs.size() > levels )
                        levels = sm.LODs.size();

            if ( levels <= 1 )
            {
                ImGui::TextDisabled( lodMesh ? "No LOD chain (mesh too small / not generated)."
                                             : "No LOD chain (primitive meshes have a single level)." );
            }
            else
            {
                // Which level the current view resolves to — the renderer's own policy, so the marker
                // can't disagree with what is on screen.
                size_t active     = 0;
                bool   haveActive = false;
                if ( scene )
                {
                    if ( const auto& camera = scene->GetActiveCamera() )
                    {
                        active =
                             std::min<size_t>( Geometry::SelectLOD( entity.GetWorldTransform(),
                                                                    lodMesh->GetSubmeshes(), camera->GetPosition(),
                                                                    materialComp.ForcedLOD, materialComp.LODBias ),
                                               levels - 1 );
                        haveActive = true;
                    }
                }

                const auto& subs = lodMesh->GetSubmeshes();
                for ( size_t l = 0; l < levels; ++l )
                {
                    uint32_t tris = 0;
                    for ( const auto& sm : subs )
                    {
                        const size_t li = l < sm.LODs.size() ? l : sm.LODs.size() - 1;
                        tris += sm.LODs[li].IndexCount / 3;
                    }
                    if ( haveActive && l == active )
                        ImGui::BulletText( "LOD %zu \xE2\x80\x94 %u tris   (drawing)", l, tris );
                    else
                        ImGui::BulletText( "LOD %zu \xE2\x80\x94 %u tris", l, tris );
                }
            }

            // Shared property rows: label column + full-width control, same stripe and hover as every
            // other row in Details.
            Utils::ImGuiUtilities::ResetPropertyRows();

            const char* items[] = { "Auto (by distance)", "LOD 0", "LOD 1", "LOD 2", "LOD 3" };
            int         cur     = materialComp.ForcedLOD < 0 ? 0 : materialComp.ForcedLOD + 1;
            Utils::ImGuiUtilities::BeginPropertyRow( "Force LOD" );
            if ( ImGui::Combo( "##forcelod", &cur, items, 5 ) )
                materialComp.ForcedLOD = ( cur == 0 ) ? -1 : cur - 1;
            Utils::ImGuiUtilities::EndPropertyRow();

            Utils::ImGuiUtilities::BeginPropertyRow(
                 "LOD Bias",
                 "Shifts the automatic LOD pick (+coarser, -finer). No effect while a LOD is forced." );
            ImGui::SliderInt( "##lodbias", &materialComp.LODBias, -3, 3 );
            Utils::ImGuiUtilities::EndPropertyRow();
        }

        // Rendering: persistent outline + per-submesh visibility. Both write component fields the
        // renderer honours (MeshECSSystem ORs OutlineDraw into the outline flag / the HiddenSubmeshes
        // bitmask), so they take effect live — the LOD-style inline-control pattern applied to the mesh.
        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_EYE "  Rendering", false ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();

            Utils::ImGuiUtilities::BeginPropertyRow(
                 "Draw Outline", "Always draw the outline for this mesh, even when it is not selected" );
            ImGui::Checkbox( "##outline", &materialComp.OutlineDraw );
            Utils::ImGuiUtilities::EndPropertyRow();

            Utils::ImGuiUtilities::BeginPropertyRow( "Cast Shadows",
                                                     "Skip this mesh in the shadow (depth) passes" );
            ImGui::Checkbox( "##castshadows", &materialComp.CastShadows );
            Utils::ImGuiUtilities::EndPropertyRow();

            Utils::ImGuiUtilities::BeginPropertyRow(
                 "Receive Shadows", "Sun (directional) shadows are not applied to this mesh when off" );
            ImGui::Checkbox( "##recvshadows", &materialComp.ReceiveShadows );
            Utils::ImGuiUtilities::EndPropertyRow();

            const size_t subCount = lodMesh ? lodMesh->GetSubmeshes().size() : 0;
            if ( subCount > 1 )
            {
                ImGui::Spacing();
                ImGui::TextDisabled( "Submesh visibility" );
                for ( size_t i = 0; i < subCount && i < 64; ++i )
                {
                    const uint64_t bit     = ( 1ull << i );
                    bool           visible = ( materialComp.HiddenSubmeshes & bit ) == 0;
                    const auto&    sm      = lodMesh->GetSubmeshes()[i];
                    const std::string label =
                         sm.Name.empty() ? ( "Submesh " + std::to_string( i ) ) : sm.Name;
                    ImGui::PushID( static_cast<int>( i ) );
                    if ( ImGui::Checkbox( label.c_str(), &visible ) )
                        materialComp.HiddenSubmeshes =
                             visible ? ( materialComp.HiddenSubmeshes & ~bit ) : ( materialComp.HiddenSubmeshes | bit );
                    ImGui::PopID();
                }
            }
        }
    }

    size_t MaterialComponentWidget::GetSubmeshCount( const MaterialHost& host ) const
    {
        // The ACTUAL submesh count of the mesh being rendered is the truth (the renderer maps
        // submesh i -> slot min(i, slots-1)).
        if ( host.Mesh && !host.Mesh->GetSubmeshes().empty() )
            return host.Mesh->GetSubmeshes().size();

        // Not built yet — fall back to the asset's imported material list.
        if ( host.MeshHandle )
        {
            if ( auto* meshAsset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( host.MeshHandle ) )
            {
                const size_t count = meshAsset->GetMaterialHandles().size();
                if ( count > 0 )
                    return count;
            }
        }
        return 1; // single implicit slot when the mesh exposes none (e.g. primitives)
    }

    Assets::AssetHandle MaterialComponentWidget::CreateAndRegisterMaterial( const std::string& baseName )
    {
        if ( !m_AssetManager )
            return {};

        // Meaningful, human-readable filename: "M_<Entity>" (sanitized), then "M_<Entity>_1", ... —
        // first free index in the material folder. The name is ONLY a label: the stable identity
        // lives INSIDE the file (MaterialId), so renaming the entity or the file later breaks nothing.
        std::string base;
        base.reserve( baseName.size() );
        for ( const char c : baseName )
            base += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_';
        if ( base.empty() )
            base = "Material";

        const std::string           ext( Common::Constants::Extensions::MATERIAL_EXTENSION );
        const std::filesystem::path dir = Common::Constants::Path::MATERIAL_PATH;
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );

        std::filesystem::path path = dir / ( base + ext );
        for ( int n = 1; std::filesystem::exists( path, ec ); ++n )
            path = dir / ( base + "_" + std::to_string( n ) + ext );

        // Write the file FIRST (defaults + a freshly minted header GUID), then create-with-load: the
        // asset adopts its stable in-file GUID as the internal handle during Load, so the handle the
        // AssetManager/MaterialService register under is the same one every future editor run gets.
        {
            Assets::MaterialData defaults;
            // Checked because the create-with-load below DEPENDS on the file: without it the asset
            // adopts no in-file GUID, so the handle registered here is not the one a later run
            // resolves, and the mesh slot points at a material that will not come back.
            if ( const auto written = Assets::WriteMaterialFile( path, defaults ); !written )
            {
                LOG_ERROR( "[Material] '{}' was not created: {}", path.generic_string(), written.GetError() );
                return {};
            }
        }

        auto asset = const_cast<Assets::AssetManager&>( *m_AssetManager )
                          .CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High,
                                                                  path.generic_string() );
        if ( !asset )
            return {};

        Runtime::ResourceRegistry::GetMaterialService()->Register( asset );
        return asset->GetMetadata().Handle;
    }

    Assets::AssetHandle
    MaterialComponentWidget::CreateAndRegisterMaterialInstance( const Assets::SurfaceMaterialAsset& parent )
    {
        if ( !m_AssetManager )
            return Common::UUID::Null();

        const std::string           ext( Common::Constants::Extensions::MATERIAL_EXTENSION );
        const std::filesystem::path dir = Common::Constants::Path::MATERIAL_PATH;
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );

        const std::string     base = std::filesystem::path( parent.GetMetadata().Filepath ).stem().string() + "_Inst";
        std::filesystem::path path = dir / ( base + ext );
        for ( int n = 1; std::filesystem::exists( path, ec ); ++n )
            path = dir / ( base + "_" + std::to_string( n ) + ext );

        {
            Assets::MaterialData data;
            // The parent is named by its header GUID, the one identity a material has. A parent that was
            // never written has none, and an instance naming it could not be resolved after a restart.
            const auto parentGuid = parent.Data().Guid();
            if ( parentGuid.IsNull() )
            {
                LOG_ERROR( "[Material] instance '{}' was not created: its parent '{}' has no GUID (never saved)",
                           path.generic_string(),
                           std::filesystem::path( parent.GetMetadata().Filepath ).generic_string() );
                return Common::UUID::Null();
            }
            data.SetParent( parentGuid );
            if ( const auto written = Assets::WriteMaterialFile( path, data );
                 !written ) // same reason as CreateAndRegisterMaterial above
            {
                LOG_ERROR( "[Material] instance '{}' was not created: {}", path.generic_string(),
                           written.GetError() );
                return Common::UUID::Null();
            }
        }

        auto asset = const_cast<Assets::AssetManager&>( *m_AssetManager )
                          .CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High,
                                                                      path.generic_string() );
        if ( !asset )
            return Common::UUID::Null();

        // LAZY registration: an instance asset must never build a runtime Material of its own
        // (Get() resolves it to the base; CreateRuntimeInstance applies the overrides).
        Runtime::ResourceRegistry::GetMaterialService()->RegisterAsset( asset );
        return asset->GetMetadata().Handle;
    }

    void MaterialComponentWidget::MakeSlotExplicit( const MaterialHost& host, size_t slot )
    {
        auto& meshComp = *host.Slots;
        // Extend the slot array up to `slot` by repeating the last handle — exactly the renderer's
        // min(i, slots-1) mapping — so materializing a row never changes the rendered look. With no
        // slots at all the fill is Null (the engine default), same as what those rows showed before.
        while ( meshComp.size() <= slot )
            meshComp.push_back( meshComp.empty() ? Common::UUID::Null() : meshComp.back() );
    }

    void MaterialComponentWidget::AssignMaterialFromPath( const MaterialHost& host, size_t slot,
                                                          const std::string& assetPath )
    {
        if ( !m_AssetManager )
            return;

        auto asset = m_AssetManager->FindByPath<Assets::SurfaceMaterialAsset>( assetPath );
        if ( !asset )
            asset = const_cast<Assets::AssetManager&>( *m_AssetManager )
                         .CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High, assetPath );
        if ( !asset )
            return;

        const auto handle = asset->GetMetadata().Handle;
        if ( !Runtime::ResourceRegistry::GetMaterialService()->Get( handle ) )
            Runtime::ResourceRegistry::GetMaterialService()->Register( asset );

        if ( slot < host.Slots->size() )
            ( *host.Slots )[slot] = handle;
        else
            host.Slots->push_back( handle );

        // Force MeshECSSystem to rebuild the runtime instances (it only rebuilds on slot-count change,
        // so an in-place handle swap needs an explicit reset).
        host.Invalidate();
    }

    void MaterialComponentWidget::OpenMaterialEditor( const Assets::SurfaceMaterialAsset& asset ) const
    {
        if ( !m_AssetManager )
            return;

        const std::string path = asset.GetMetadata().Filepath.generic_string();
        switch ( RequestMaterialDocument( &const_cast<Assets::AssetManager&>( *m_AssetManager ), path ) )
        {
            case MaterialDocumentRequest::Requested:
                break;
            case MaterialDocumentRequest::Failed:
                break; // already reported, with the path, by the opener
            case MaterialDocumentRequest::NotAMaterialPath:
                // A slot resolved to a material asset whose file is not on disk (deleted under the editor,
                // or an in-memory asset that was never written). Silence here would read as a dead button.
                LOG_ERROR( "[MaterialEditor] the material in this slot has no `.demat` on disk ('{}') — "
                           "no window was opened. Save it first, or reassign the slot.",
                           path );
                break;
        }
    }

    MaterialComponentWidget::SlotSwatch
    MaterialComponentWidget::BuildSlotSwatch( const Assets::SurfaceMaterialAsset& asset,
                                              const Assets::MaterialData*         parentData )
    {
        SlotSwatch swatch;

        auto*             shaderService = Runtime::ResourceRegistry::GetShaderService();
        const std::string shaderName =
             parentData ? parentData->EffectiveShaderName() : asset.Data().EffectiveShaderName();
        auto shader = shaderService ? shaderService->GetByName( shaderName ) : nullptr;
        if ( !shader )
            return swatch;

        using W = ::Desert::Core::Formats::ShaderParamWidget;

        const auto& data = asset.Data();
        for ( const auto& p : shader->GetProgramMeta().Params )
        {
            if ( p.IsTexture || p.Widget != W::Color )
                continue;

            // Same value resolution as the parameter rows: child override -> parent's effective value
            // (instance mode) -> schema default. A slot must never advertise a colour it doesn't render.
            const glm::vec4 fallback = parentData ? parentData->GetParam( p.Name, p.Default ) : p.Default;
            swatch.HasColor          = true;
            swatch.Color             = data.GetParam( p.Name, fallback );
            break; // the FIRST colour is the material's identity; later ones are accents
        }
        return swatch;
    }

    void MaterialComponentWidget::DrawSlotPreview( const Assets::SurfaceMaterialAsset* asset,
                                                   const SlotSwatch& swatch, float size )
    {
        // The rendered preview comes from the SHARED thumbnail service. Details used to only READ the disk
        // cache the asset browser happened to fill, so a material the browser had never shown stayed a flat
        // colour swatch forever. It now REQUESTS the capture itself — the service owns the single renderer
        // for the whole editor, so asking costs nothing extra and the result is shared with every panel.
        std::shared_ptr<Graphic::Image2D> thumb;
        std::string                       path;
        if ( asset )
        {
            path = asset->GetMetadata().Filepath.generic_string();

            std::error_code ec;
            // HOW it is photographed comes from the ONE place that decides — the material's shader domain,
            // plus the cutout rule this file used to hold its own copy of. A refusal (a domain no producer
            // draws) leaves `png` empty, and the swatch below is then the true statement about it.
            const auto        route = ThumbnailSubject::PreviewRouteFor( *asset );
            const std::string png   = route ? ThumbnailService::Get().RequestMaterial( asset->GetMetadata().Handle,
                                                                                       path, route.GetValue() )
                                            : std::string();

            // THE SAME RULE THE SERVICE APPLIES, out of the same header, and that is the point. This used
            // to be a hand-written copy of the margin comparison while ThumbnailService::ShouldQueue asked
            // only whether the file existed. When the two disagreed — a source newer than its picture —
            // this panel refused to draw and the service refused to render, and the material showed a flat
            // colour swatch for the rest of the project's life.
            bool haveFresh = ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( png, path ) ) ==
                             ThumbnailFreshness::Verdict::Show;
            if ( !haveFresh )
            {
                // Drop the decoded copy of a picture we have just decided not to show; the RequestMaterial
                // above has already queued the replacement, because it asked this same question.
                m_Thumbnails.Invalidate( png );
            }
            if ( haveFresh )
            {
                // The PNG on disk moved since this panel decoded it — drop the decoded copy or the slot
                // keeps showing the OLD render. The regeneration is no longer ours to know about: the
                // Material Editor window edits and saves the material now, and it cannot reach this cache.
                const auto stamp = std::filesystem::last_write_time( png, ec );
                if ( !ec )
                {
                    const auto seen = m_ThumbnailStamps.find( png );
                    if ( seen != m_ThumbnailStamps.end() && seen->second != stamp )
                        m_Thumbnails.Invalidate( png );
                    m_ThumbnailStamps[png] = stamp;
                }
                thumb = m_Thumbnails.Get( png );
            }
        }

        // An InvisibleButton rather than a Dummy: the preview is the row's drag-drop target, and a drop
        // target needs a real item to hang on.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton( "##preview", ImVec2( size, size ) );

        const ImVec2 br( at.x + size, at.y + size );
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );

        if ( thumb && m_UIHelper )
        {
            if ( const void* tex = m_UIHelper->GetTextureID( thumb ) )
                dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ),
                                     ImVec2( at.x + 1.0f, at.y + 1.0f ), ImVec2( br.x - 1.0f, br.y - 1.0f ),
                                     ImVec2( 0, 0 ), ImVec2( 1, 1 ), IM_COL32_WHITE, 2.0f );
        }
        else if ( swatch.HasColor )
        {
            dl->AddRectFilled(
                 ImVec2( at.x + 1.0f, at.y + 1.0f ), ImVec2( br.x - 1.0f, br.y - 1.0f ),
                 ImGui::ColorConvertFloat4ToU32( ImVec4( swatch.Color.x, swatch.Color.y, swatch.Color.z, 1.0f ) ),
                 2.0f );
        }
        dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );

        // UE underlines a slot's preview with a colour bar. Ours carries the material's identity colour,
        // so two slots with the same (or no) thumbnail are still telling apart.
        if ( swatch.HasColor )
        {
            const ImU32 col =
                 ImGui::ColorConvertFloat4ToU32( ImVec4( swatch.Color.x, swatch.Color.y, swatch.Color.z, 1.0f ) );
            dl->AddRectFilled( ImVec2( at.x + 1.0f, br.y - 3.0f ), ImVec2( br.x - 1.0f, br.y ), col );
        }

        if ( !path.empty() )
            Utils::ImGuiUtilities::Tooltip( path.c_str() );
    }

    std::string MaterialComponentWidget::SlotNameOf( const MaterialHost& host, size_t index ) const
    {
        if ( !host.Mesh || index >= host.Mesh->GetSubmeshes().size() )
            return {};
        return host.Mesh->GetSubmeshes()[index].Name;
    }

    MaterialComponentWidget::SlotAction MaterialComponentWidget::DrawSlotRow( const SlotRow& row,
                                                                              std::string&   droppedPath )
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        // 64px: big enough to recognise a material by its render, and the SAME box the skeletal-mesh slot
        // uses — one preview size across Details.
        constexpr float   kPreview   = 64.0f;
        const auto*       asset      = row.Asset;
        const bool        hasOwnSlot = row.HasOwnSlot;

        // The row is as tall as its content — the preview beside a two-line stack (asset field + action
        // strip) — so the grid's rules frame it instead of cutting through it.
        const float stack = ImGui::GetFrameHeight() * 2.0f + style.ItemSpacing.y;
        const float rowH  = std::max( kPreview, stack ) + style.ItemSpacing.y;

        Utils::ImGuiUtilities::PropertyRowBackground( rowH );

        ImGui::Columns( 2 );
        ImGui::SetColumnWidth( 0, Utils::ImGuiUtilities::PropertyLabelWidth() );
        ImGui::AlignTextToFramePadding();

        const std::string label = "Element " + std::to_string( row.Index );
        ImGui::TextUnformatted( label.c_str() );
        // The mesh's own name for the element, beside the index — UE's "Slot" field. The INDEX is what
        // the renderer maps materials by, so it stays the identity and the name is the hint.
        if ( !row.SlotName.empty() )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "%s", row.SlotName.c_str() );
        }
        // An element without its own slot still renders SOMETHING — say where that look comes from
        // rather than leaving the row looking broken.
        if ( !hasOwnSlot )
            ImGui::TextDisabled( "%s", asset ? "inherited" : "default" );
        else if ( row.IsInstance && !row.ParentName.empty() )
            ImGui::TextDisabled( "instance of %s", row.ParentName.c_str() );

        ImGui::NextColumn();

        SlotAction action = SlotAction::None;

        const auto acceptDrop = [&droppedPath]()
        {
            if ( !ImGui::BeginDragDropTarget() )
                return;
            if ( const ImGuiPayload* p =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MaterialAsset ) )
            {
                droppedPath.assign( static_cast<const char*>( p->Data ), p->DataSize > 0 ? p->DataSize - 1 : 0 );
            }
            ImGui::EndDragDropTarget();
        };

        DrawSlotPreview( asset, row.Swatch, kPreview );
        acceptDrop();

        ImGui::SameLine();
        ImGui::BeginGroup();

        const std::string name = asset ? std::filesystem::path( asset->GetMetadata().Filepath ).stem().string()
                                       : std::string( "None" );
        if ( Utils::ImGuiUtilities::AssetSlot( "slot", name.c_str(), asset == nullptr ) )
            action = SlotAction::Pick;
        acceptDrop();

        // A strip of flat icon actions, UE's row of small buttons under the asset field. Text buttons
        // here would each be a full-width bar and the slot list would stop reading as a list.
        const auto iconButton = []( const char* icon, const char* tip, bool active )
        {
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, active ? ThemeManager::GetSelectedColor()
                                                         : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            const bool clicked = ImGui::Button( icon );
            ImGui::PopStyleColor( 2 );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", tip );
            ImGui::SameLine( 0.0f, 2.0f );
            return clicked;
        };

        if ( asset && hasOwnSlot )
        {
            if ( iconButton( ICON_MDI_PENCIL, "Open this material in the Material Editor", false ) )
                action = SlotAction::OpenEditor;
        }
        if ( !hasOwnSlot && asset )
        {
            if ( iconButton( ICON_MDI_LINK_VARIANT,
                             "Give this element its own slot (same material, look unchanged)", false ) )
                action = SlotAction::MakeExplicit;
        }
        if ( !asset )
        {
            if ( iconButton( ICON_MDI_PLUS_BOX, "Create a new material for this element", false ) )
                action = SlotAction::CreateMaterial;
        }
        if ( asset && hasOwnSlot && !row.IsInstance )
        {
            if ( iconButton( ICON_MDI_CONTENT_DUPLICATE,
                             "New child instance — override parameters without touching the parent", false ) )
                action = SlotAction::CreateInstance;
        }

        // Always submitted, so the trailing SameLine of the strip never dangles into the group's end.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled( "%s", row.ShaderName.c_str() );

        ImGui::EndGroup();

        Utils::ImGuiUtilities::PropertyColumnRule();
        ImGui::Columns( 1 );
        return action;
    }

    void MaterialComponentWidget::RenderMaterialProperties( ECS::Entity& entity, const MaterialHost& host,
                                                            const std::string& overriddenByShader )
    {
        Utils::ImGuiUtilities::PushID();

        // When a runtime override (script) bypasses the slots: say so loudly and
        // start the section collapsed. Slots are the only AUTHORED source of truth.
        if ( !overriddenByShader.empty() )
        {
            ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetWarningColor() );
            ImGui::TextWrapped( "%s Runtime shader override active ('%s', set by a script) — "
                                "the material slots below are NOT used until it is cleared.",
                                ICON_MDI_ALERT, overriddenByShader.c_str() );
            ImGui::PopStyleColor();
        }

        // NOTE: there is deliberately NO per-frame param-override channel over the slots anymore.
        // Script writes go straight to the runtime instance; pre-build/legacy params are consumed
        // ONCE by MeshECSSystem at instance build. Slots are the single authored source of truth.

        // One row per submesh (plus any extra explicit slot) — the count belongs on the header, UE-style,
        // so a collapsed section still says how many elements this mesh has.
        const size_t      submeshCount = GetSubmeshCount( host );
        const size_t      rowCount     = std::max( submeshCount, host.Slots->size() );
        const std::string slotDetail   = std::to_string( rowCount ) + ( rowCount == 1 ? " element" : " elements" );

        const bool materialsOpen = Utils::ImGuiUtilities::SectionHeader(
             ICON_MDI_PALETTE "  Material Slots", overriddenByShader.empty(), slotDetail.c_str() );

        // Drop a .mat onto the Materials header (works open or collapsed): create slots up to the submesh
        // count if there are none, then assign the dropped material to EVERY slot.
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MaterialAsset ) )
            {
                const std::string path( static_cast<const char*>( p->Data ),
                                        p->DataSize > 0 ? p->DataSize - 1 : 0 );
                const size_t      count = GetSubmeshCount( host );
                while ( host.Slots->size() < count )
                    host.Slots->push_back( Common::UUID::Null() );
                for ( size_t s = 0; s < host.Slots->size(); ++s )
                    AssignMaterialFromPath( host, s, path );
            }
            ImGui::EndDragDropTarget();
        }

        if ( materialsOpen )
        {
            // New materials are named after the entity ("M_<Tag>") — "Material_9" told nobody
            // anything. The filename is a label only (identity = in-file GUID), so a later entity
            // rename orphans nothing.
            const std::string matBaseName =
                 "M_" + ( entity.HasComponent<ECS::TagComponent>()
                               ? entity.GetComponent<ECS::TagComponent>().Tag
                               : std::string( "Material" ) );

            // UE-style element list: ALWAYS one row per submesh (plus any extra explicit slots).
            // A row without its own slot INHERITS the effective material the renderer actually uses
            // (submesh i -> slot min(i, slots-1), or the engine default when there are no slots) and
            // is shown greyed with a "Make Explicit" affordance — no slots are created behind the
            // user's back, and making a row explicit never changes the rendered look.
            Utils::ImGuiUtilities::ResetPropertyRows();

            for ( size_t i = 0; i < rowCount; ++i )
            {
                ImGui::PushID( static_cast<int>( i ) );

                const bool hasOwnSlot = i < host.Slots->size() && ( *host.Slots )[i];

                // The handle this row EFFECTIVELY renders with (mirrors the renderer's mapping).
                Assets::AssetHandle handle = Common::UUID::Null();
                if ( hasOwnSlot )
                    handle = ( *host.Slots )[i];
                else if ( !host.Slots->empty() )
                    handle = ( *host.Slots )[std::min( i, host.Slots->size() - 1 )];

                const auto asset = ( m_AssetManager && handle )
                                        ? m_AssetManager->FindByHandle<Assets::SurfaceMaterialAsset>( handle )
                                        : nullptr;

                // Instance parent, resolved BEFORE the header is drawn: the header's swatches must show
                // the values the slot actually renders with, which for an instance live in the parent.
                const auto parentId = asset ? asset->Data().InstanceParentId() : std::optional<Common::UUID>{};
                const bool isInstanceAsset = parentId.has_value();
                std::shared_ptr<Assets::SurfaceMaterialAsset> parentAsset;
                std::string                                   parentName;
                if ( isInstanceAsset && m_AssetManager )
                {
                    const auto parentHandle =
                         Runtime::ResourceRegistry::GetMaterialService()->GetAssetHandleByExternal( *parentId );
                    if ( !parentHandle.IsNull() )
                        parentAsset = m_AssetManager->FindByHandle<Assets::SurfaceMaterialAsset>( parentHandle );
                    parentName =
                         parentAsset ? std::filesystem::path( parentAsset->GetMetadata().Filepath ).stem().string()
                                     : std::string( "<missing parent>" );
                }

                SlotSwatch swatch;
                if ( asset )
                    swatch = BuildSlotSwatch( *asset, parentAsset ? &parentAsset->Data() : nullptr );

                // The shader the slot RENDERS with: an instance's own ShaderName is empty and would read
                // as the engine default here, so it comes from the parent chain.
                const std::string shaderName = asset ? ( parentAsset ? parentAsset->Data().EffectiveShaderName()
                                                                     : asset->Data().EffectiveShaderName() )
                                                     : std::string( "Engine default material" );

                SlotRow row;
                row.Index      = i;
                row.Asset      = asset.get();
                row.Swatch     = swatch;
                row.SlotName   = SlotNameOf( host, i );
                row.ShaderName = shaderName;
                row.ParentName = parentName;
                row.HasOwnSlot = hasOwnSlot;
                row.IsInstance = isInstanceAsset;

                std::string      dropped;
                const SlotAction action = DrawSlotRow( row, dropped );

                // Drop an existing material asset on the row to assign it (creates the slot if needed).
                if ( !dropped.empty() )
                {
                    MakeSlotExplicit( host, i );
                    AssignMaterialFromPath( host, i, dropped );
                }

                switch ( action )
                {
                    case SlotAction::Pick:
                    {
                        ImGui::OpenPopup( "material_picker" );
                        break;
                    }
                    case SlotAction::OpenEditor:
                    {
                        // Routed by PATH through the shared opener rather than by queueing the handle here.
                        // The handle is in hand and the request only carries a handle, so this looks like the
                        // longer way round — but the opener is also what guarantees the asset is LOADED and
                        // registered with the material service before a window binds to it, and a slot can
                        // legitimately hold a handle whose asset was only ever a record (a scene that named a
                        // material nothing has drawn yet). Re-deriving those three steps here is precisely the
                        // two-implementations-of-one-quantity shape this engine keeps paying for.
                        if ( asset )
                            OpenMaterialEditor( *asset );
                        break;
                    }
                    case SlotAction::MakeExplicit:
                    {
                        MakeSlotExplicit( host, i );
                        host.Invalidate();
                        break;
                    }
                    case SlotAction::CreateMaterial:
                    {
                        if ( const auto h = CreateAndRegisterMaterial( matBaseName ) )
                        {
                            MakeSlotExplicit( host, i );
                            ( *host.Slots )[i] = h;
                            host.Invalidate();
                        }
                        break;
                    }
                    case SlotAction::CreateInstance:
                    {
                        if ( asset )
                        {
                            if ( const auto h = CreateAndRegisterMaterialInstance( *asset ) )
                            {
                                ( *host.Slots )[i] = h;
                                host.Invalidate();
                            }
                        }
                        break;
                    }
                    case SlotAction::None:
                        break;
                }

                // The picker the slot field's chevron promises. Assigning here is the same operation as a
                // drag-drop, so it goes through the same MakeSlotExplicit + assign pair.
                if ( ImGui::BeginPopup( "material_picker" ) )
                {
                    static ImGuiTextFilter materialFilter;
                    materialFilter.Draw( "##search", 200.0f );
                    ImGui::Separator();

                    if ( hasOwnSlot && ImGui::Selectable( "None (use the engine default)" ) )
                    {
                        ( *host.Slots )[i] = Common::UUID::Null();
                        host.Invalidate();
                    }

                    if ( m_AssetManager )
                    {
                        for ( const auto& [candidate, matAsset] :
                              m_AssetManager->FindAllByType<Assets::SurfaceMaterialAsset>() )
                        {
                            const std::string matName =
                                 std::filesystem::path( matAsset->GetMetadata().Filepath ).stem().string();
                            if ( !materialFilter.PassFilter( matName.c_str() ) )
                                continue;
                            if ( ImGui::Selectable( matName.c_str(), candidate == handle ) )
                            {
                                MakeSlotExplicit( host, i );
                                ( *host.Slots )[i] = candidate;
                                host.Invalidate();
                            }
                        }
                    }
                    ImGui::EndPopup();
                }

                if ( hasOwnSlot && !asset )
                {
                    // Slot exists but its asset can't be resolved (deleted/missing file). Not a styling
                    // question: the row would otherwise look like an ordinary empty slot.
                    ImGui::Indent( 12.0f );
                    ImGui::TextDisabled( ICON_MDI_ALERT "  Material asset missing" );
                    ImGui::Unindent( 12.0f );
                }

                ImGui::PopID();
            }
        }

        Utils::ImGuiUtilities::PopID();
    }

} // namespace Desert::Editor
