#include "AnimLayersPanel.hpp"
#include <Editor/Panels/PanelContext.hpp>

#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>

#include <ImGui/imgui.h>

#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    AnimLayersPanel::AnimLayersPanel( std::shared_ptr<::Desert::Core::Scene> scene,
                                      Animation::AnimationLibrary* library )
         : IPanel( "Anim Layers", /*showPanel=*/false ), m_Scene( std::move( scene ) ), m_Library( library )
    {
    }

    void AnimLayersPanel::OnUIRender()
    {
        if ( !m_Scene )
        {
            ImGui::TextDisabled( "No active scene." );
            return;
        }
        const auto& sel = Core::SelectionManager::GetSelected();
        if ( !sel )
        {
            ImGui::TextDisabled( "Select a skinned-mesh entity." );
            return;
        }
        const auto entOpt = m_Scene->FindEntityByID( *sel );
        if ( !entOpt )
            return;
        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::AnimationComponent>() ||
             !entity.HasComponent<ECS::SkinnedMeshComponent>() )
        {
            ImGui::TextDisabled( "Selected entity has no Animation + Skinned Mesh." );
            return;
        }

        auto& anim = entity.GetComponent<ECS::AnimationComponent>();
        if ( !anim.Animator )
        {
            ImGui::TextDisabled( "Animator not ready (press Play / pick a clip)." );
            return;
        }
        Animation::Animator* animator = anim.Animator.get();

        auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get(
             entity.GetComponent<ECS::SkinnedMeshComponent>().MeshHandle );
        if ( !mesh || !mesh->IsSkinned() )
            return;
        const Animation::Skeleton& skeleton = static_cast<SkinnedMesh*>( mesh )->GetSkeleton();
        const auto                 clips    = m_Library ? m_Library->GetForSkeleton( skeleton )
                                                        : std::vector<Assets::Asset<Assets::AnimationAsset>>{};
        std::vector<const char*> clipNames;
        clipNames.reserve( clips.size() );
        for ( const auto& c : clips )
            clipNames.push_back( c->GetClip().AnimationName.c_str() );

        // ---- The pipeline, in the order it runs ----
        // Worth showing because it is now a LIST rather than three branches inside Update(): what an artist
        // sees on screen is the product of these stages in this order, and "my layer does nothing" has a
        // different answer depending on whether the stage is in the list at all.
        std::string pipeline;
        for ( const Animation::PoseStage stage : animator->GetStages() )
        {
            pipeline += pipeline.empty() ? "" : "  ->  ";
            pipeline += Animation::ToString( stage );
        }
        ImGui::TextDisabled( "Pipeline:  %s  ->  skinning", pipeline.c_str() );

        // ---- Active layers ----
        ImGui::TextDisabled( "%zu active layer(s) over the base clip.", animator->GetLayerCount() );
        ImGui::Separator();

        int removeIdx = -1;
        for ( int i = 0; i < static_cast<int>( animator->GetLayerCount() ); ++i )
        {
            ImGui::PushID( i );
            const Animation::AnimationClip* clip = animator->GetLayerClip( i );
            ImGui::Text( "%d: %s%s", i, clip ? clip->AnimationName.c_str() : "<none>",
                         animator->GetLayerAdditive( i ) ? "  [additive]" : "  [override]" );

            float w = animator->GetLayerWeight( i );
            ImGui::SetNextItemWidth( 160.0f );
            if ( ImGui::SliderFloat( "Weight", &w, 0.0f, 1.0f ) )
                animator->SetLayerWeight( i, w );
            ImGui::SameLine();
            bool additive = animator->GetLayerAdditive( i );
            if ( ImGui::Checkbox( "Additive", &additive ) )
                animator->SetLayerAdditive( i, additive );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Remove" ) )
                removeIdx = i;

            ImGui::Separator();
            ImGui::PopID();
        }
        if ( removeIdx >= 0 )
            animator->RemoveLayer( removeIdx );

        // ---- Add a layer ----
        ImGui::Spacing();
        ImGui::TextDisabled( "Add layer" );
        ImGui::SetNextItemWidth( 200.0f );
        ImGui::Combo( "Clip", &m_AddClip, clipNames.empty() ? nullptr : clipNames.data(),
                      static_cast<int>( clipNames.size() ) );
        ImGui::SetNextItemWidth( 160.0f );
        ImGui::SliderFloat( "Weight##add", &m_AddWeight, 0.0f, 1.0f );
        ImGui::SameLine();
        ImGui::Checkbox( "Additive##add", &m_AddAdditive );
        ImGui::SetNextItemWidth( 200.0f );
        ImGui::InputText( "Mask from bone", m_AddMaskBone, sizeof( m_AddMaskBone ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Restrict the layer to this bone + its children (empty = whole body)." );

        const bool canAdd = m_AddClip >= 0 && m_AddClip < static_cast<int>( clips.size() );
        ImGui::BeginDisabled( !canAdd );
        if ( ImGui::Button( "Add Layer" ) && canAdd )
        {
            const int idx = animator->AddLayer( clips[m_AddClip]->GetClip(), m_AddWeight, m_AddAdditive, true );
            if ( m_AddMaskBone[0] != '\0' )
                animator->SetLayerMaskByNames( idx, { std::string( m_AddMaskBone ) } );
        }
        ImGui::EndDisabled();
    }

    bool AnimLayersPanel::IsRelevant() const
    {
        return SelectionHas<ECS::AnimationComponent>( m_Scene ) &&
               SelectionHas<ECS::SkinnedMeshComponent>( m_Scene );
    }

} // namespace Desert::Editor
