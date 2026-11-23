#include "StaticMeshComponent.hpp"
#include <ImGui/imgui.h>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>

#include "MaterialsPanelComponent.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Widgets/MeshSlotThumbnail.hpp>
#include <Editor/Core/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <array>
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>

#include <algorithm>
#include <cfloat>
#include <format>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/Scene.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    StaticMeshComponentWidget::StaticMeshComponentWidget( Assets::AssetManager*       assetManager,
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
        int currentType = staticMesh.GetPrimitive().has_value() ? 1 : 0;
        if ( ImGui::Combo( "##MeshType", &currentType, meshTypes, IM_ARRAYSIZE( meshTypes ) ) )
        {
            ECS::StaticMeshDesc desc = staticMesh.GetDesc();
            if ( currentType == 0 )
                desc.Primitive.reset();
            else
                desc.Primitive = Geometry::PrimitiveType::Cube;
            ECS::SetStaticMesh( *entity.GetRegistry(), entity.GetHandle(), desc );
        }

        Utils::ImGuiUtilities::EndPropertyRow();

        if ( !staticMesh.GetPrimitive().has_value() )
        {
            // THE MESH ROW IS THE REFLECTED FIELD (UE: UStaticMeshComponent::StaticMesh drawn by the generic
            // Details), and so is its picture: the builder draws the thumbnail INSIDE that row, beside the picker
            // (DrawMeshSlotThumbnail), as UE's SObjectPropertyEntryBox does - a separate Preview row above it was
            // two rows for one slot, and its thumbnail overlapped the Static Mesh row (DETAILS-SM-PREVIEW). A pick
            // here, a DesertCtl `set` and an undo all write the same member and re-state what depends on it
            // through the same PostEditStaticMesh.
            const ECS::StaticMeshDesc before = staticMesh.GetDesc();
            if ( PropertyEditorBuilder::Draw( &staticMesh, "StaticMeshComponent", m_AssetManager,
                                              m_Ctx ? m_Ctx->UIHelper : nullptr,
                                              m_Ctx ? m_Ctx->FieldFilter : nullptr ) )
                ECS::PostEditStaticMesh( *entity.GetRegistry(), entity.GetHandle(), before );
        }
        else
        {
            // A primitive has no mesh asset to picture, so its row shows slot 0's material - the one fallback
            // DrawMeshSlotThumbnail takes. The combo after it ends the row's line below the thumbnail.
            const float shapeRow =
                 std::max( kMeshSlotThumbnailSize, ImGui::GetFrameHeight() ) + ImGui::GetStyle().ItemSpacing.y;
            Utils::ImGuiUtilities::BeginPropertyRow( "Shape", nullptr, shapeRow );
            DrawMeshSlotThumbnail( m_AssetManager, m_Ctx ? m_Ctx->UIHelper : nullptr, staticMesh.GetMeshHandle(),
                                   staticMesh.GetMaterialSlots().empty() ? Assets::AssetHandle{}
                                                                         : staticMesh.GetMaterialSlots().front() );

            // Offered list and stored value both come from Geometry::kAuthorablePrimitives, so the row
            // a person picks IS the enumerator that gets stored. The hand-typed twin of this list in the
            // Instanced Static Mesh editor had two shapes transposed — see PrimitiveTypeName's header.
            std::array<const char*, Geometry::kAuthorablePrimitives.size()> shapes{};
            int                                                             currentShape = 0;
            for ( size_t i = 0; i < Geometry::kAuthorablePrimitives.size(); ++i )
            {
                shapes[i] = Geometry::PrimitiveTypeName( Geometry::kAuthorablePrimitives[i] );
                if ( Geometry::kAuthorablePrimitives[i] == staticMesh.GetPrimitive().value() )
                    currentShape = static_cast<int>( i );
            }
            if ( ImGui::Combo( "##Shape", &currentShape, shapes.data(), static_cast<int>( shapes.size() ) ) )
            {
                ECS::StaticMeshDesc desc = staticMesh.GetDesc();
                desc.Primitive           = Geometry::kAuthorablePrimitives[static_cast<size_t>( currentShape )];
                ECS::SetStaticMesh( *entity.GetRegistry(), entity.GetHandle(), desc );
                // MeshECSSystem will handle the dynamic mesh generation/update
            }

            Utils::ImGuiUtilities::EndPropertyRow();
        }

        // What the mesh IS (statistics, elements, Import Settings) is the ASSET's, shown by its editor
        // (MeshAssetDetails, UE's Static Mesh Editor); a component's Details is the slot, its materials and
        // the component's own properties.
        {
            static MaterialComponentWidget materialComponent( m_AssetManager );
            materialComponent.Render( entity, scene );
        }

        RenderRigging( entity, staticMesh );

        Utils::ImGuiUtilities::PopID();
    }

    void StaticMeshComponentWidget::RenderRigging( ECS::Entity& entity, ECS::StaticMeshComponent& staticMesh )
    {
        // Rigging needs asset-backed CPU geometry (primitives/procedural have no StaticMeshAsset vertices).
        if ( staticMesh.GetPrimitive().has_value() || !staticMesh.GetMeshHandle() )
            return;
        if ( !entity.HasComponent<ECS::UUIDComponent>() )
            return;

        const Common::UUID uuid = entity.GetComponent<ECS::UUIDComponent>().UUID;

        // Seed / default placement = the mesh's local AABB centre.
        glm::vec3 center( 0.0f );
        if ( auto asset = m_AssetManager->FindByHandle<Assets::StaticMeshAsset>( staticMesh.GetMeshHandle() ) )
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
            const std::string label =
                 std::format( ICON_MDI_BONE "  {}{}", bones[i].Name, bones[i].Parent < 0 ? "   (root)" : "" );
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

    std::string StaticMeshComponentWidget::GetPrimitiveName( const ECS::StaticMeshComponent& staticMesh ) const
    {
        // Was a three-case switch with a `default: return "Primitive"`, so a cylinder, a capsule and a
        // pyramid all read as "Primitive" in the one place that names the mesh being drawn.
        if ( !staticMesh.GetPrimitive() )
            return "None";
        return Geometry::PrimitiveTypeName( *staticMesh.GetPrimitive() );
    }

    namespace
    {
        // A custom Details layout (Mesh Type, the Materials list - UE customizes both) over a REFLECTED
        // component: DataPtr and ReflectedTypeName let the header summary, the search filter and DesertCtl
        // `set` reach StaticMesh.MeshHandle / StaticMesh.MaterialSlots[i], and EditData re-states what an edit
        // there changes (PostEditStaticMesh), exactly as a pick in the panel does.
        ComponentEditorEntry MakeStaticMeshEntry()
        {
            using C = ECS::StaticMeshComponent;
            ComponentEditorEntry e = MakeCustomComponentEntry<C>(
                 "3D Model",
                 []( ECS::Entity& en, ::Desert::Core::Scene* s, const ComponentEditContext& ctx )
                 { StaticMeshComponentWidget( ctx.AssetMgr(), &ctx ).Render( en, s ); },
                 false );
            e.ReflectedTypeName = "StaticMeshComponent";
            e.DataPtr           = []( ECS::Entity& en ) -> void* { return &en.GetComponent<C>(); };
            e.EditData          = []( ECS::Entity& en, const std::function<void( void* )>& edit )
            {
                // The edit lands on a COPY and reaches the entity through the writer (ApplyStaticMeshEdit ->
                // SetStaticMesh); a reflected write never touches the component in its EnTT storage.
                C edited = en.GetComponent<C>();
                edit( &edited );
                ECS::ApplyStaticMeshEdit( *en.GetRegistry(), en.GetHandle(), edited.GetDesc() );
            };
            return e;
        }

        [[maybe_unused]] const int s_StaticMeshEntry = ComponentWidgetRegistry::Get().Register( MakeStaticMeshEntry() );
    } // namespace

} // namespace Desert::Editor
