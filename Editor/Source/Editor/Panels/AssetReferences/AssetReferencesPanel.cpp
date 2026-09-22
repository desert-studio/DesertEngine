#include "AssetReferencesPanel.hpp"

#include <ImGui/imgui.h> // the panel interface no longer hands the toolkit over (IPanel.hpp)

#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <Common/Core/Constants.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <algorithm>
#include <filesystem>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // Leaf asset types worth flagging as unused (roots like scenes/prefabs are never "orphans").
        const std::vector<std::string> kLeafExts = { ".demat", ".png", ".jpg", ".jpeg",
                                                     ".tga",   ".hdr", ".bmp" };
    } // namespace

    void AssetReferencesPanel::Rebuild()
    {
        BuildProjectAssetReferenceIndex( m_Index );

        m_AllPaths.clear();
        for ( const auto& e : m_Index.Entries() )
            m_AllPaths.push_back( e.Path );
        std::sort( m_AllPaths.begin(), m_AllPaths.end() );

        m_Orphans  = m_Index.Orphans( kLeafExts );
        m_Selected = -1;
        m_Built    = true;
    }

    void AssetReferencesPanel::OnUIRender()
    {
        if ( ImGui::Button( ICON_MDI_REFRESH "  Rebuild index" ) )
            Rebuild();
        ImGui::SameLine();
        if ( m_Built )
            ImGui::TextDisabled( "%zu assets indexed", m_Index.Entries().size() );
        else
            ImGui::TextDisabled( "not scanned yet" );

        if ( !m_Built )
        {
            ImGui::Spacing();
            ImGui::TextWrapped( "Scans the project's Assets for cross-references (handle- and "
                                "path-based). It is a usage search, not a proof — use it to find "
                                "references and unused assets before deleting." );
            return;
        }

        ImGui::Separator();
        ImGui::TextUnformatted( "Referenced by" );

        const char* preview = m_Selected >= 0 && m_Selected < static_cast<int>( m_AllPaths.size() )
                                   ? m_AllPaths[m_Selected].c_str()
                                   : "select an asset...";
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##refAsset", preview ) )
        {
            for ( int i = 0; i < static_cast<int>( m_AllPaths.size() ); ++i )
                if ( ImGui::Selectable( m_AllPaths[i].c_str(), i == m_Selected ) )
                    m_Selected = i;
            ImGui::EndCombo();
        }

        if ( m_Selected >= 0 && m_Selected < static_cast<int>( m_AllPaths.size() ) )
        {
            const auto refs = m_Index.ReferencersOf( m_AllPaths[m_Selected] );
            ImGui::BeginChild( "##referencers", ImVec2( 0.0f, 140.0f ), true );
            if ( refs.empty() )
                ImGui::TextDisabled( ICON_MDI_ALERT_OUTLINE "  No FILE references this asset." );
            else
                for ( const auto& r : refs )
                    ImGui::BulletText( "%s", r.c_str() );
            ImGui::EndChild();

            DrawRuntimeUsage( m_AllPaths[m_Selected] );
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text( ICON_MDI_DELETE_SWEEP "  Unused leaf assets (%zu)", m_Orphans.size() );
        ImGui::BeginChild( "##orphans", ImVec2( 0.0f, 0.0f ), true );
        if ( m_Orphans.empty() )
            ImGui::TextDisabled( "None — every texture/material is referenced." );
        else
            for ( const auto& o : m_Orphans )
                ImGui::BulletText( "%s", o.c_str() );
        ImGui::EndChild();
    }

    void AssetReferencesPanel::DrawRuntimeUsage( const std::string& relPath )
    {
        // Runtime edges the file/token scan can't see. Currently material-centric: mesh assets
        // reference materials from BINARY cooked data, and open-scene entities reference them by
        // runtime handle in their slots — both invisible to a text scan by construction.
        if ( !m_AssetManager ||
             std::filesystem::path( relPath ).extension().string() !=
                  Common::Constants::Extensions::MATERIAL_EXTENSION )
            return;

        const std::string fullPath =
             ( std::filesystem::path( Common::Constants::Path::ASSETS_PATH ) / relPath ).generic_string();
        const auto material = m_AssetManager->FindByPath<Assets::SurfaceMaterialAsset>( fullPath );
        if ( !material )
            return; // not loaded this session — the file-level section above still covers it

        const auto handle     = material->GetMetadata().Handle;
        const auto externalId = material->GetMaterialUUID();

        // Mesh ASSETS whose embedded (imported) material list carries this material.
        ImGui::Spacing();
        ImGui::TextUnformatted( "Used by mesh assets" );
        ImGui::BeginChild( "##meshUsers", ImVec2( 0.0f, 90.0f ), true );
        {
            size_t found = 0;
            for ( const auto& [meshHandle, meshAsset] : m_AssetManager->FindAllByType<Assets::MeshAsset>() )
            {
                if ( !meshAsset )
                    continue;
                const auto& handles = meshAsset->GetMaterialHandles();
                if ( std::find( handles.begin(), handles.end(), externalId ) == handles.end() )
                    continue;
                ImGui::BulletText(
                     "%s", meshAsset->GetMetadata().Filepath.filename().generic_string().c_str() );
                ++found;
            }
            if ( found == 0 )
                ImGui::TextDisabled( "No loaded mesh asset embeds this material." );
        }
        ImGui::EndChild();

        // Entities of the OPEN scene rendering with this material right now (slots by handle).
        if ( !m_Scene )
            return;
        ImGui::Spacing();
        ImGui::TextUnformatted( "Used by entities (open scene)" );
        ImGui::BeginChild( "##entityUsers", ImVec2( 0.0f, 110.0f ), true );
        {
            size_t     found    = 0;
            auto&      registry = m_Scene->GetRegistry();
            const auto usesIt   = [&]( const std::vector<Assets::AssetHandle>& slots )
            { return std::find( slots.begin(), slots.end(), handle ) != slots.end(); };
            const auto tagOf = [&]( entt::entity e ) -> const char*
            {
                return registry.has<ECS::TagComponent>( e )
                            ? registry.get<ECS::TagComponent>( e ).Tag.c_str()
                            : "<unnamed>";
            };

            for ( auto e : registry.view<ECS::StaticMeshComponent>() )
                if ( usesIt( registry.get<ECS::StaticMeshComponent>( e ).MaterialSlots ) )
                {
                    ImGui::BulletText( "%s", tagOf( e ) );
                    ++found;
                }
            for ( auto e : registry.view<ECS::SkinnedMeshComponent>() )
                if ( usesIt( registry.get<ECS::SkinnedMeshComponent>( e ).MaterialSlots ) )
                {
                    ImGui::BulletText( "%s  (skinned)", tagOf( e ) );
                    ++found;
                }
            for ( auto e : registry.view<ECS::InstancedStaticMeshComponent>() )
                if ( usesIt( registry.get<ECS::InstancedStaticMeshComponent>( e ).MaterialSlots ) )
                {
                    ImGui::BulletText( "%s  (instanced)", tagOf( e ) );
                    ++found;
                }
            if ( found == 0 )
                ImGui::TextDisabled( "No entity in the open scene uses this material." );
        }
        ImGui::EndChild();
    }
} // namespace Desert::Editor
