#include <Editor/Core/DetailsNavigation.hpp>
#include "SkinnedMeshComponentWidget.hpp"
#include <Editor/Widgets/AssetFieldOpen.hpp>

#include <ImGui/imgui.h>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailPose.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>

#include <string>
#include <unordered_set>

#include "MaterialsPanelComponent.hpp"

#include <Engine/Geometry/SkinnedMesh.hpp>

#include <Editor/Core/AssetPickerRows.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // UE's asset-type colour for a skeletal mesh, sampled off the reference: the bar under the slot's
        // preview says WHAT KIND of asset the slot takes, before you have read a single word of the name.
        constexpr ImU32 kSkeletalMeshTint = IM_COL32( 241, 163, 241, 255 );

        // The framed preview box beside an asset slot: the asset's rendered thumbnail when @p picture is
        // given (DrawMeshThumbnail), its GLYPH until then — and the type bar underneath when the slot is filled.
        void DrawAssetBox( float size, const char* icon, bool filled, ImU32 tint, ImTextureID picture = nullptr )
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy( ImVec2( size, size ) );

            const ImVec2 br( at.x + size, at.y + size );
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );
            if ( picture != nullptr )
                dl->AddImageRounded( picture, ImVec2( at.x + 1.0f, at.y + 1.0f ),
                                     ImVec2( br.x - 1.0f, br.y - 1.0f ), ImVec2( 0, 0 ), ImVec2( 1, 1 ),
                                     IM_COL32_WHITE, 2.0f );
            dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );

            if ( picture == nullptr )
            {
                const ImVec2 ts = ImGui::CalcTextSize( icon );
                dl->AddText( ImVec2( at.x + ( size - ts.x ) * 0.5f, at.y + ( size - ts.y ) * 0.5f ),
                             ImGui::GetColorU32( filled ? ImGuiCol_Text : ImGuiCol_TextDisabled ), icon );
            }

            if ( filled )
                dl->AddRectFilled( ImVec2( at.x + 1.0f, br.y - 3.0f ), ImVec2( br.x - 1.0f, br.y ), tint );
        }

        // THE SLOT'S PICTURES OUTLIVE THE FRAME. The registration below builds this widget per draw (as every
        // component row is), so a cache it owned was born empty every frame: it asked the worker for the
        // .skmesh PNG, the decode landed in a cache already destroyed, and the next frame asked again — 417
        // decodes in 90 s, the slot and the browser tile (whose decode the loop kept stealing) on their
        // icons, 111 -> 59 FPS while the Fox was selected (THM-FIXD). Kept where the static-mesh slot keeps
        // its own (StaticMeshComponent.cpp `s_Thumbnails`), released with every live cache (ReleaseAll).
        ThumbnailCache& SlotPictures()
        {
            static ThumbnailCache s_Pictures;
            return s_Pictures;
        }

        // A refusal is logged once for the session, not once per frame of a widget rebuilt per frame.
        std::unordered_set<std::string>& RefusedSlots()
        {
            static std::unordered_set<std::string> s_Refused;
            return s_Refused;
        }
    } // namespace

    SkinnedMeshComponentWidget::SkinnedMeshComponentWidget(
         const std::weak_ptr<Assets::AssetManager>& assetManager, UI::UIHelper* ui )
         : IComponentWidget( "Skinned Mesh" ), m_AssetManager( assetManager ), m_UI( ui )
    {
    }

    void SkinnedMeshComponentWidget::DrawMeshThumbnail( Assets::AssetManager& manager, const std::string& meshPath,
                                                        const float size, const bool filled )
    {
        // The Content Browser tile's rule (FileExplorerPanel::DrawRenderedPoseThumbnail): the .skmesh is its own
        // cooked form, so the picture is filed under it and judged against it. A stale or missing picture is
        // asked for through the service — never a read of the disk cache hoping the browser walked past it
        // (Desert/Tests/Editor/ThumbnailRequesters keeps every showing slot a requesting slot).
        std::shared_ptr<Graphic::Image2D> thumb;
        if ( !meshPath.empty() && !RefusedSlots().contains( meshPath ) )
        {
            const std::string png = ThumbnailKey::DiskPath( meshPath );
            if ( ThumbnailService::JudgeMeshPicture( meshPath ) == ThumbnailFreshness::Verdict::Show )
                thumb = SlotPictures().Get( png );
            else
            {
                SlotPictures().Invalidate( png ); // the old render must not be handed back once the new one lands
                const auto subject = ThumbnailPose::ResolvePoseSubject( manager, meshPath );
                if ( !subject )
                {
                    LOG_WARN( "[Thumbnail] Skeletal Mesh slot '{}': {}", meshPath, subject.GetError() );
                    RefusedSlots().insert( meshPath );
                }
                else if ( !subject.GetValue().Pending ) // read in flight: asked again next frame
                    ThumbnailService::Get().RequestPose( subject.GetValue() );
            }
        }
        const ImTextureID picture = thumb && m_UI != nullptr ? m_UI->GetTextureID( thumb ) : nullptr;
        DrawAssetBox( size, ICON_MDI_HUMAN, filled, kSkeletalMeshTint, picture );
    }

    void SkinnedMeshComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* scene )
    {
        auto& skinnedMesh  = entity.GetComponent<ECS::SkinnedMeshComponent>();
        auto  assetManager = m_AssetManager.lock();
        if ( !assetManager )
            return;

        Utils::ImGuiUtilities::PushID();

        const auto meshAssets = Assets::ContentRegistry::MeshRows( true );

        // The mesh that is ACTUALLY drawn: an in-editor rig (Convert to Skinned) overrides the asset.
        ::Desert::Mesh* mesh = skinnedMesh.RuntimeMesh.get();
        if ( !mesh && skinnedMesh.MeshHandle )
            mesh = Runtime::ResourceRegistry::GetMeshService()->Get( skinnedMesh.MeshHandle );

        // What the slot says. A PROCEDURAL mesh (the built-in humanoid, or a mesh rigged in the editor)
        // is registered straight with the MeshService and has no MeshAsset at all, so looking it up in the
        // AssetManager returns nothing — the row used to call that "None", which reads as an empty slot on
        // a character that is plainly standing in the viewport. Say what it actually is instead.
        auto        asset = assetManager->FindByHandle<Assets::MeshAsset>( skinnedMesh.MeshHandle );
        std::string currentMeshName;
        if ( asset )
            currentMeshName = Common::Utils::FileSystem::GetFileName( asset->GetMetadata().Filepath );
        else if ( skinnedMesh.RuntimeMesh )
            currentMeshName = "Procedural (rigged in the editor)";
        else if ( mesh )
            currentMeshName = "Procedural (generated)";

        const bool emptySlot = currentMeshName.empty();
        if ( emptySlot )
            currentMeshName = "None";

        // UE's SkeletalMeshComponent leads with exactly this row — the asset the component renders, as a
        // preview box beside a sunk slot field — before any statistic about it.
        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_HUMAN "  Skeletal Mesh" ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();

            // Same 64px as a material slot's preview — one preview size across Details.
            constexpr float kBox = 64.0f;
            const float     rowH = std::max( kBox, ImGui::GetFrameHeight() ) + ImGui::GetStyle().ItemSpacing.y;

            Utils::ImGuiUtilities::BeginPropertyRow( "Skeletal Mesh Asset",
                                                     "The skinned mesh asset this component renders", rowH );

            DrawMeshThumbnail( *assetManager,
                               asset ? asset->GetMetadata().Filepath.generic_string() : std::string(), kBox,
                               !emptySlot );
            ImGui::SameLine();
            const bool clicked =
                 Utils::ImGuiUtilities::AssetSlot( "SkinnedMeshSlot", currentMeshName.c_str(), emptySlot );
            if ( TakeDetailsPickerRequest( "Skinned mesh" ) || clicked )
                ImGui::OpenPopup( "skinned_mesh_selector" );
            DrawAssetFieldOpen( emptySlot || !asset ? 0 : static_cast<uint64_t>( skinnedMesh.MeshHandle ) );
            DrawAssetFieldButtons( emptySlot || !asset ? 0 : static_cast<uint64_t>( skinnedMesh.MeshHandle ) );

            if ( ImGui::BeginPopup( "skinned_mesh_selector" ) )
            {
                static ImGuiTextFilter filter;
                filter.Draw( "##Search", 200 );
                ImGui::Separator();

                for ( const auto& row : meshAssets )
                {
                    const std::string name = ::Desert::Editor::PickerDisplayName( row );

                    if ( filter.PassFilter( name.c_str() ) )
                    {
                        const bool selected = skinnedMesh.MeshHandle == row.Handle;
                        if ( ImGui::Selectable( name.c_str(), selected ) )
                        {
                            skinnedMesh.MeshHandle = row.Handle;
                        }
                    }
                }

                ImGui::EndPopup();
            }

            Utils::ImGuiUtilities::EndPropertyRow();
        }

        Utils::ImGuiUtilities::PopID();

        // What the mesh IS — statistics, elements, the skeleton and its bone tree, the skinning audit, Import
        // Settings — is the ASSET's, shown by its editors (UE: the Skeletal Mesh / Skeleton Editor; here the
        // Animation Editor's Mesh and Skeleton modes, MeshAssetDetails). A component's Details is the slot,
        // its materials and the component's own properties.
        //
        // Material slots. A skinned mesh carries the same MaterialSlots the renderer maps per submesh, but
        // the slot editor used to be hard-wired to StaticMeshComponent — so a character's materials could
        // only be set by editing the scene file. Same widget, same rows, same drag-drop.
        {
            static MaterialComponentWidget materials( assetManager.get() );
            materials.Render( entity, scene );
        }
    }

    DESERT_REGISTER_CUSTOM_COMPONENT(
         ECS::SkinnedMeshComponent, "Skinned Mesh", false,
         ( []( ECS::Entity& e, ::Desert::Core::Scene* s, const ComponentEditContext& ctx )
           { SkinnedMeshComponentWidget( ctx.AssetManager, ctx.UIHelper ).Render( e, s ); } ) )
} // namespace Desert::Editor
