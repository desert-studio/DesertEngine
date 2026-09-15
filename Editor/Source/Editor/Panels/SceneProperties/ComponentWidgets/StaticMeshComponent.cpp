#include "StaticMeshComponent.hpp"
#include <ImGui/imgui.h>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Engine/Geometry/Mesh.hpp>

#include "MaterialsPanelComponent.hpp"

#include "Helper/MeshDetailsWidget.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/MeshResolve.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <filesystem>
#include <system_error>
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>

#include <algorithm>
#include <cfloat>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    StaticMeshComponentWidget::StaticMeshComponentWidget( const Assets::AssetManager* assetManager,
                                                          const ComponentEditContext* ctx )
         : IComponentWidget( "3D Model" ), m_AssetManager( assetManager ), m_Ctx( ctx )
    {
    }

    void StaticMeshComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* scene )
    {
        auto& staticMesh = entity.GetComponent<ECS::StaticMeshComponent>();

        Utils::ImGuiUtilities::PushID();

        // No local FramePadding override: the row height is a PANEL metric (ThemeManager), and a widget
        // that shrinks its own controls is exactly how the Details grid ends up with rows of three
        // different heights.
        Utils::ImGuiUtilities::ResetPropertyRows();
        Utils::ImGuiUtilities::BeginPropertyRow( "Mesh Type" );

        const char* meshTypes[] = { "Asset", "Primitive" };
        int currentType = staticMesh.Primitive.has_value() ? 1 : 0;
        if ( ImGui::Combo( "##MeshType", &currentType, meshTypes, IM_ARRAYSIZE( meshTypes ) ) )
        {
            if ( currentType == 0 )
                staticMesh.Primitive.reset();
            else
                staticMesh.Primitive = Geometry::PrimitiveType::Cube;
        }

        Utils::ImGuiUtilities::EndPropertyRow();

        // The preview is a THUMBNAIL on this row, not a section of its own: what the mesh looks like
        // belongs beside the slot that chooses it, the way UE draws an asset row.
        constexpr float kThumb   = 64.0f;
        const float     assetRow = std::max( kThumb, ImGui::GetFrameHeight() ) + ImGui::GetStyle().ItemSpacing.y;

        if ( !staticMesh.Primitive.has_value() )
        {
            Utils::ImGuiUtilities::BeginPropertyRow( "Asset", nullptr, assetRow );
            DrawMeshThumbnail( staticMesh, kThumb );

            std::string currentSelectionName = "Select Mesh";
            bool        emptySlot            = true;
            if ( staticMesh.MeshHandle )
            {
                auto meshAsset = m_AssetManager->FindByHandle<Assets::MeshAsset>( staticMesh.MeshHandle );
                if ( meshAsset )
                {
                    currentSelectionName = Common::Utils::FileSystem::GetFileName( meshAsset->GetMetadata().Filepath );
                    emptySlot = false;
                }
            }

            // A sunk asset slot, not a raised button: this row HOLDS a value (UE draws it the same way).
            if ( Utils::ImGuiUtilities::AssetSlot( "MeshSlot", currentSelectionName.c_str(), emptySlot ) )
            {
                ImGui::OpenPopup( "mesh_selector" );
            }

            if ( ImGui::BeginPopup( "mesh_selector" ) )
            {
                auto meshAssets = m_AssetManager->FindAllByType<Assets::MeshAsset>();
                static ImGuiTextFilter meshFilter;
                meshFilter.Draw( "##Search", 200 );
                ImGui::Separator();

                for ( const auto& [handle, meshAsset] : meshAssets )
                {
                    const auto isSkinnedOpt = Runtime::ResourceRegistry::GetMeshService()->IsSkinned( handle );
                    if ( isSkinnedOpt.has_value() && isSkinnedOpt.value() )
                    {
                        continue;
                    }

                    const std::string& meshName = Common::Utils::FileSystem::GetFileName( meshAsset->GetMetadata().Filepath );
                    if ( meshFilter.PassFilter( meshName.c_str() ) )
                    {
                        if ( ImGui::Selectable( meshName.c_str(), staticMesh.MeshHandle == handle ) )
                        {
                            SetMeshAsset( staticMesh, handle );
                        }
                    }
                }
                ImGui::EndPopup();
            }
            Utils::ImGuiUtilities::EndPropertyRow();
        }
        else
        {
            Utils::ImGuiUtilities::BeginPropertyRow( "Shape", nullptr, assetRow );
            DrawMeshThumbnail( staticMesh, kThumb );

            const char* shapes[] = { "Cube", "Sphere", "Pyramid", "Plane", "Cylinder", "Capsule" };
            int currentShape = (int)staticMesh.Primitive.value();
            if ( ImGui::Combo( "##Shape", &currentShape, shapes, IM_ARRAYSIZE( shapes ) ) )
            {
                staticMesh.Primitive = (Geometry::PrimitiveType)currentShape;
                // MeshECSSystem will handle the dynamic mesh generation/update
            }

            Utils::ImGuiUtilities::EndPropertyRow();
        }

        ShowMeshDetails( entity, scene, staticMesh );

        {
            static MaterialComponentWidget materialComponent( m_AssetManager );
            materialComponent.Render( entity, scene );
        }

        RenderRigging( entity, staticMesh );

        Utils::ImGuiUtilities::PopID();
    }

    void StaticMeshComponentWidget::DrawMeshThumbnail( const ECS::StaticMeshComponent& staticMesh,
                                                       float                           size ) const
    {
        // THE REQUEST COMES FIRST, BEFORE THE LIVE PREVIEW IS EVEN CONSIDERED, and the ordering is the
        // whole lesson of this function.
        //
        // The obvious arrangement — show the live preview, and ask for a cached picture only on the frames
        // there is no live one — asks for the fallback at exactly the moment it cannot be produced. What
        // takes the live preview away is a shortage of renderer slots, and a capture needs a renderer slot
        // too; the service refuses to take the last one (Engine/Core/RendererSlotBudget.hpp), and
        // rightly, so the row would sit on "queued" for as long as the shortage lasted. Measured, not
        // reasoned: with the request placed after the branch, a selected mesh produced no capture at all
        // and the log showed the queue draining a material nobody had asked this row for.
        //
        // So the cache is warmed WHILE there is room to warm it. It costs one capture per mesh asset, ever
        // — 360-385 ms over seven runs on this machine, then a PNG that survives restarts — and the service
        // drops the request outright when the picture on disk is still fresh, which after the first time
        // it is.
        //
        // Asked through the service, never by reading `ThumbnailCache::DiskPath` and hoping. That hope was
        // the defect: the file existed only if the asset browser had happened to walk past this asset, and
        // if it had not, the row showed a grey cube glyph for the life of the project with nothing anywhere
        // saying why — the very wart ThumbnailService's header declares removed for the material slot, and
        // did not keep for this one. Desert/Tests/Editor/ThumbnailRequesters is the census that keeps every
        // showing slot a requesting slot.
        //
        // THE MESH first: this row is the mesh slot, and showing a material sphere where the model belongs
        // answers a question nobody asked. The material is the last resort, for an entity whose mesh slot
        // is empty (a primitive).
        static ThumbnailCache s_Thumbnails;

        std::shared_ptr<Graphic::Image2D> thumb;
        std::string                       png;
        std::string                       source;
        if ( m_AssetManager )
        {
            if ( staticMesh.MeshHandle )
            {
                if ( auto mesh = m_AssetManager->FindByHandle<Assets::MeshAsset>( staticMesh.MeshHandle ) )
                {
                    source = mesh->GetMetadata().Filepath.generic_string();
                    // NO material override, and that is a decision rather than an omission. The cache is
                    // keyed on the ASSET (Editor/Widgets/ThumbnailKey.hpp), so what it holds has to be a
                    // picture of the asset: handing over THIS entity's slot materials would put two
                    // entities that share one mesh in a fight over one file, and the second one selected
                    // would be shown the first one's paint with nothing able to tell them apart. The
                    // per-entity answer is the live preview BELOW; this one is per-asset by construction.
                    png = ThumbnailService::Get().RequestMesh( staticMesh.MeshHandle, source );
                }
            }
            if ( png.empty() && !staticMesh.MaterialSlots.empty() && staticMesh.MaterialSlots.front() )
            {
                if ( auto mat = m_AssetManager->FindByHandle<Assets::SurfaceMaterialAsset>(
                          staticMesh.MaterialSlots.front() ) )
                {
                    source = mat->GetMetadata().Filepath.generic_string();
                    // WHICH PICTURE, from the one place that decides — this file used to hold its own
                    // copy of the cutout rule and to ask nothing at all about the domain. A refusal leaves
                    // `png` empty, which this row already reads as "no rendered thumbnail".
                    if ( const auto route = ThumbnailSubject::PreviewRouteFor( *mat ) )
                    {
                        png = ThumbnailService::Get().RequestMaterial( mat->GetMetadata().Handle, source,
                                                                       route.GetValue() );
                    }
                }
            }

            if ( !png.empty() )
            {
                // Through the shared rule, not a bare exists(): a picture whose asset has moved on is not
                // the asset's picture (Editor/Widgets/ThumbnailFreshness.hpp). When it says Capture the
                // request above has already queued the replacement, so the decoded copy is dropped here —
                // otherwise this cache would keep handing back the OLD render after the new one lands.
                if ( ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( png, source ) ) ==
                     ThumbnailFreshness::Verdict::Show )
                    thumb = s_Thumbnails.Get( png );
                else
                    s_Thumbnails.Invalidate( png );
            }
        }

        // NOW the live one, if there is a live one with something in it. It is the better picture —
        // orbitable, wearing this entity's own materials, and it follows a material edit while you drag the
        // slider — and it is safe because per-frame GPU state is stored per (frame x renderer slot)
        // (Docs/RENDERER_FRAME_STATE.md).
        //
        // HasContent() IS PART OF THE CONDITION, and it is what makes the cached picture above reachable at
        // all. ScenePropertiesPanel builds the viewport as soon as a mesh entity is selected and only points
        // it at the mesh on the NEXT OnPreUpdate, and it declines to build one when every renderer slot is
        // taken. Asking DrawPreview alone answers "was a widget lent", which was true in every state this
        // row is ever drawn in — so the fallback underneath was unreachable code wearing a fallback's
        // clothes. Asking whether the preview has anything to SHOW is the question the row actually has.
        if ( m_Ctx && m_Ctx->Preview && m_Ctx->Preview->HasContent() &&
             m_Ctx->DrawPreview( ImVec2( size, size ) ) )
        {
            Utils::ImGuiUtilities::Tooltip( "Live preview — drag to orbit, wheel to zoom" );
            ImGui::SameLine();
            return;
        }

        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 br( at.x + size, at.y + size );
        ImDrawList*  dl = ImGui::GetWindowDrawList();

        ImGui::Dummy( ImVec2( size, size ) );
        dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );

        if ( thumb && m_Ctx && m_Ctx->UIHelper )
        {
            if ( const void* tex = m_Ctx->UIHelper->GetTextureID( thumb ) )
                dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ),
                                     ImVec2( at.x + 1.0f, at.y + 1.0f ), ImVec2( br.x - 1.0f, br.y - 1.0f ),
                                     ImVec2( 0, 0 ), ImVec2( 1, 1 ), IM_COL32_WHITE, 2.0f );
        }
        else
        {
            const char*  icon = ICON_MDI_CUBE_OUTLINE;
            const ImVec2 ts   = ImGui::CalcTextSize( icon );
            dl->AddText( ImVec2( at.x + ( size - ts.x ) * 0.5f, at.y + ( size - ts.y ) * 0.5f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), icon );
        }
        dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );
        // Not "rendered by the asset browser" any more, and the old wording was the defect written down:
        // this row now asks for its own picture, so the browser is not what it is waiting for.
        // Three states, not two: "here it is", "it is coming" and "there is nothing to draw one of" are
        // different answers, and collapsing the last two would let an empty slot look like a slow one.
        Utils::ImGuiUtilities::Tooltip( thumb         ? "Cached preview of this asset"
                                        : png.empty() ? "Nothing in this slot to preview"
                                                      : "Preview queued — it will appear in a moment" );

        ImGui::SameLine();
    }

    void StaticMeshComponentWidget::ShowMeshDetails( const ECS::Entity& entity, ::Desert::Core::Scene* scene,
                                                     const ECS::StaticMeshComponent& staticMesh ) const
    {
        MeshDetailsWidget::Context ctx;
        ctx.Entity    = &entity;
        ctx.Scene     = scene;
        ctx.ForcedLOD = staticMesh.ForcedLOD;
        ctx.LODBias   = staticMesh.LODBias;

        // The mesh that is ACTUALLY drawn — one shared resolver, so the panel, the viewport overlay and
        // the collider fit can never disagree about which mesh an entity shows.
        ctx.RuntimeMesh = ResolveDrawnMesh( entity );
        if ( staticMesh.MeshHandle )
            ctx.Asset = m_AssetManager->FindByHandle<Assets::MeshAsset>( staticMesh.MeshHandle );

        MeshDetailsWidget::Show( ctx );
    }

    void StaticMeshComponentWidget::RenderRigging( ECS::Entity& entity, ECS::StaticMeshComponent& staticMesh )
    {
        // Rigging needs asset-backed CPU geometry (primitives/procedural have no StaticMeshAsset vertices).
        if ( staticMesh.Primitive.has_value() || !staticMesh.MeshHandle )
            return;
        if ( !entity.HasComponent<ECS::UUIDComponent>() )
            return;

        const Common::UUID uuid = entity.GetComponent<ECS::UUIDComponent>().UUID;

        // Seed / default placement = the mesh's local AABB centre.
        glm::vec3 center( 0.0f );
        if ( auto asset = m_AssetManager->FindByHandle<Assets::StaticMeshAsset>( staticMesh.MeshHandle ) )
        {
            const auto& verts = asset->GetVertices();
            if ( !verts.empty() )
            {
                glm::vec3 mn( FLT_MAX ), mx( -FLT_MAX );
                for ( const auto& v : verts )
                {
                    mn = glm::min( mn, v.Position );
                    mx = glm::max( mx, v.Position );
                }
                center = 0.5f * ( mn + mx );
            }
        }

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_BONE "  Rigging (Skeleton)" ) )
            return;

        const bool riggingThis = RigBuilder::IsActive() && RigBuilder::Target() == uuid;

        ImGui::Indent( 6.0f );
        ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );

        if ( !riggingThis )
        {
            if ( RigBuilder::IsActive() )
            {
                ImGui::TextColored( ThemeManager::GetWarningColor(),
                                    ICON_MDI_ALERT " Another mesh is being rigged." );
                ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );
            }
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextDisabled( "Place bones on this static mesh, then convert it to a skinned mesh with "
                                 "automatic vertex weights. Pose the bones afterwards in Skeleton Edit mode." );
            ImGui::PopTextWrapPos();
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_BONE "  Add Skeleton / Rig this Mesh", 28.0f ) )
                RigBuilder::Begin( uuid, center );
            ImGui::Unindent( 6.0f );
            return;
        }

        const auto& bones = RigBuilder::Bones();
        const int   sel   = RigBuilder::SelectedBone();

        ImGui::TextDisabled( "BONES  (%d)", static_cast<int>( bones.size() ) );
        ImGui::BeginChild( "##rigBones", ImVec2( 0.0f, std::min( 140.0f, 8.0f + bones.size() * 20.0f ) ), true );
        for ( int i = 0; i < static_cast<int>( bones.size() ); ++i )
        {
            ImGui::PushID( i );
            std::string label = std::string( ICON_MDI_BONE "  " ) + bones[i].Name;
            if ( bones[i].Parent < 0 )
                label += "   (root)";
            if ( ImGui::Selectable( label.c_str(), i == sel ) )
                RigBuilder::SelectBone( i );
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );
        if ( sel >= 0 && sel < static_cast<int>( bones.size() ) )
        {
            glm::vec3 head = bones[sel].Head;
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::DragFloat3( "##head", &head.x, 0.01f, 0.0f, 0.0f, "%.3f" ) )
                RigBuilder::SetHead( sel, head );
            ImGui::SameLine( 0.0f, 0.0f );
        }

        if ( ImGui::Button( ICON_MDI_PLUS "  Add Child", ImVec2( ImGui::GetContentRegionAvail().x * 0.5f, 0 ) ) )
        {
            const glm::vec3 head = ( sel >= 0 ) ? bones[sel].Head + glm::vec3( 0.0f, 0.5f, 0.0f ) : center;
            RigBuilder::AddBone( sel, head );
        }
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_DELETE "  Delete", ImVec2( ImGui::GetContentRegionAvail().x, 0 ) ) )
            RigBuilder::DeleteBone( sel );

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_RUN_FAST "  Convert to Skinned", 30.0f ) )
            RigBuilder::RequestConvert();
        if ( ImGui::Button( "Cancel", ImVec2( ImGui::GetContentRegionAvail().x, 0 ) ) )
            RigBuilder::Cancel();

        ImGui::Unindent( 6.0f );
    }

    void StaticMeshComponentWidget::SetMeshAsset( ECS::StaticMeshComponent& staticMesh, const Assets::AssetHandle& handle )
    {
        staticMesh.MeshHandle = handle;
        staticMesh.Primitive.reset();
        // Load default materials from asset...
    }

    std::string StaticMeshComponentWidget::GetPrimitiveName( const ECS::StaticMeshComponent& staticMesh ) const
    {
        if ( !staticMesh.Primitive ) return "None";
        switch ( *staticMesh.Primitive )
        {
            case Geometry::PrimitiveType::Cube: return "Cube";
            case Geometry::PrimitiveType::Sphere: return "Sphere";
            case Geometry::PrimitiveType::Plane: return "Plane";
            default: return "Primitive";
        }
    }

    DESERT_REGISTER_CUSTOM_COMPONENT( ECS::StaticMeshComponent, "3D Model", false,
                                      ( []( ECS::Entity& e, ::Desert::Core::Scene* s,
                                            const ComponentEditContext& ctx )
                                        { StaticMeshComponentWidget( ctx.AssetMgr(), &ctx ).Render( e, s ); } ) )

} // namespace Desert::Editor
