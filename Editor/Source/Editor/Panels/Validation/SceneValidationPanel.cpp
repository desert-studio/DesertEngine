#include "SceneValidationPanel.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    SceneValidationPanel::SceneValidationPanel( std::shared_ptr<::Desert::Core::Scene> scene,
                                                Assets::AssetManager* assets )
         : IPanel( "Scene Validation", /*showPanel=*/false ), m_Scene( std::move( scene ) ), m_Assets( assets )
    {
    }

    void SceneValidationPanel::Validate()
    {
        m_Issues.clear();
        m_Ran = true;
        if ( !m_Scene )
            return;

        auto* meshService = Runtime::ResourceRegistry::GetMeshService();

        auto nameOf = []( ECS::Entity& e ) -> std::string
        {
            return e.HasComponent<ECS::TagComponent>() ? e.GetComponent<ECS::TagComponent>().Tag
                                                        : std::string( "Entity" );
        };
        auto idOf = []( ECS::Entity& e ) -> Common::UUID
        {
            return e.HasComponent<ECS::UUIDComponent>() ? e.GetComponent<ECS::UUIDComponent>().UUID
                                                        : Common::UUID::Null();
        };

        auto entities = m_Scene->GetAllEntities();
        for ( auto entity : entities )
        {
            const std::string name = nameOf( entity );

            auto checkMesh = [&]( const Assets::AssetHandle& handle, const char* what )
            {
                if ( handle && meshService && !meshService->GetAsset( handle ) )
                    m_Issues.push_back( { idOf( entity ), name + " -> missing " + what + " mesh asset" } );
            };

            if ( entity.HasComponent<ECS::StaticMeshComponent>() )
                checkMesh( entity.GetComponent<ECS::StaticMeshComponent>().MeshHandle, "static" );
            if ( entity.HasComponent<ECS::SkinnedMeshComponent>() )
                checkMesh( entity.GetComponent<ECS::SkinnedMeshComponent>().MeshHandle, "skinned" );

            if ( entity.HasComponent<ECS::SkyboxComponent>() )
            {
                const auto& sh = entity.GetComponent<ECS::SkyboxComponent>().SkyboxHandle;
                if ( sh && !Assets::ContentRegistry::RowOf( Common::Content::ContentKind::Skybox,
                                                            static_cast<uint64_t>( sh ) ) )
                    m_Issues.push_back( { idOf( entity ), name + " -> missing skybox asset" } );
            }
        }

        ToastManager::Push( m_Issues.empty() ? "Scene validation: no broken references"
                                             : "Scene validation: " + std::to_string( m_Issues.size() ) +
                                                    " issue(s) found",
                            m_Issues.empty() ? ToastLevel::Success : ToastLevel::Warning );
    }

    void SceneValidationPanel::OnUIRender()
    {
        if ( !m_Scene )
        {
            ImGui::TextDisabled( "No active scene." );
            return;
        }

        if ( ImGui::Button( "Validate Scene" ) )
            Validate();
        ImGui::SameLine();
        if ( !m_Ran )
            ImGui::TextDisabled( "Not run yet." );
        else if ( m_Issues.empty() )
            ImGui::TextColored( ImVec4( 0.4f, 0.85f, 0.45f, 1.0f ), "No broken references." );
        else
            ImGui::TextColored( ImVec4( 0.95f, 0.75f, 0.30f, 1.0f ), "%zu issue(s).", m_Issues.size() );

        ImGui::Separator();
        ImGui::BeginChild( "##valList" );
        for ( size_t i = 0; i < m_Issues.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            if ( ImGui::Selectable( m_Issues[i].Text.c_str() ) && !m_Issues[i].Entity.IsNull() )
                Core::SelectionManager::SetSelected( m_Issues[i].Entity );
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
} // namespace Desert::Editor
