#include "StaticMeshViewerDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets != nullptr )
            {
                if ( const auto* meta = assets->FindMetadataByHandle( subject ) )
                    return meta->Filepath.filename().string();
            }
            return "Static Mesh";
        }
    } // namespace

    StaticMeshViewerDocument::StaticMeshViewerDocument( const Assets::AssetHandle& mesh,
                                                        Assets::AssetManager*      assets )
         : StaticMeshViewerBase( SubjectTitle( mesh, assets ), mesh ), m_Assets( assets )
    {
    }

    StaticMeshViewerDocument::~StaticMeshViewerDocument() = default;

    bool StaticMeshViewerDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void StaticMeshViewerDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Unavailable.empty() )
            return;

        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets != nullptr ? m_Assets->FindMetadataByHandle( handle ) : nullptr;
        if ( meta == nullptr )
        {
            m_Unavailable = "This mesh is not registered with the asset manager — nothing to show.";
            return;
        }
        const std::string name = meta->Filepath.filename().string();
        if ( meta->Filepath.extension() != Common::Constants::Extensions::STATIC_MESH )
        {
            m_Unavailable = std::format( "'{}' is not a static mesh; skeletal meshes open in the skeletal mesh "
                                         "viewer (AV1g), which does not exist yet.",
                                         name );
            return;
        }

        const auto asset = m_Assets->FindByHandle<Assets::StaticMeshAsset>( handle );
        if ( !asset )
        {
            m_Unavailable = std::format( "'{}' is not a StaticMeshAsset in the asset manager.", name );
            return;
        }
        if ( const auto loaded = asset->EnsureLoaded( *m_Assets ); !loaded )
        {
            m_Unavailable = std::format( "'{}' would not load: {}", name, loaded.GetError() );
            return;
        }

        // THE SAME BYTES THE ASSET DREW FROM: the DDC answers with the platform data EnsureLoaded has just
        // derived (a hit), decoded by the one reader. Read once; the window never re-derives per frame.
        const auto raw = Assets::LoadMeshPlatformData( meta->Filepath );
        if ( !raw )
        {
            m_Unavailable = std::format( "'{}' has no platform data: {}", name, raw.GetError() );
            return;
        }
        const auto data = Assets::Serialization::ReadMeshAssetData( raw.GetValue(), meta->Filepath.string() );
        if ( !data )
        {
            m_Unavailable = std::format( "'{}': {}", name, data.GetError() );
            return;
        }
        m_Stats = DescribeStaticMesh( data.GetValue() );

        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;

        const auto& slots = asset->GetMaterialHandles();
        m_Preview->SetMesh( handle, std::vector<Assets::AssetHandle>( slots.begin(), slots.end() ) );

        if ( m_PendingOrbitDegrees )
        {
            m_Preview->SetOrbit( glm::radians( m_PendingOrbitDegrees->x ),
                                 glm::radians( m_PendingOrbitDegrees->y ) );
            m_PendingOrbitDegrees.reset();
        }
    }

    void StaticMeshViewerDocument::ReleaseView()
    {
        m_Preview.reset();
        m_UIHelper.reset();
        m_PreviewLive = false;
    }

    void StaticMeshViewerDocument::SetPreviewViewpoint( const PreviewViewpoint& viewpoint )
    {
        if ( !m_Preview )
        {
            m_PendingOrbitDegrees = glm::vec2( viewpoint.YawDegrees, viewpoint.PitchDegrees );
            return;
        }
        m_Preview->SetOrbit( glm::radians( viewpoint.YawDegrees ), glm::radians( viewpoint.PitchDegrees ) );
    }

    std::vector<ISubjectDocument::DocumentAction> StaticMeshViewerDocument::Actions()
    {
        std::vector<DocumentAction> actions;
        actions.push_back( { "LOD Auto", [this]() { m_ForcedLOD = -1; } } );
        const std::size_t lods = m_Stats ? m_Stats->LODs() : 1u;
        for ( std::size_t lod = 0; lod < lods; ++lod )
            actions.push_back(
                 { std::format( "LOD {}", lod ), [this, lod]() { m_ForcedLOD = static_cast<int>( lod ); } } );
        return actions;
    }

    void StaticMeshViewerDocument::OnPreUpdate()
    {
        // THE SLOT IS NOT CLAIMED UNTIL THE WINDOW HAS BEEN DRAWN (MaterialEditorPanel::OnPreUpdate has the
        // argument): the document exists a frame before its window does.
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_RenderSize.x == 0u || m_RenderSize.y == 0u )
            return;

        m_Preview->SetForcedLOD( m_ForcedLOD );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    void StaticMeshViewerDocument::DrawStats() const
    {
        if ( !m_Stats )
            return;
        const StaticMeshStats& stats = *m_Stats;
        ImGui::Separator();
        ImGui::TextUnformatted( "Mesh" );
        ImGui::Text( "Vertices   %zu", stats.Vertices );
        ImGui::Text( "Triangles  %zu", stats.Triangles );
        ImGui::Text( "Sections   %zu", stats.Sections );
        ImGui::Text( "LODs       %zu", stats.LODs() );
        if ( stats.LODs() == 1u )
            ImGui::TextDisabled( "no baked LOD chain (generated at load)" );
        for ( std::size_t lod = 0; lod < stats.TrianglesPerLOD.size(); ++lod )
            ImGui::Text( "  LOD %zu: %zu tris", lod, stats.TrianglesPerLOD[lod] );

        ImGui::Separator();
        ImGui::TextUnformatted( "Bounds (cm)" );
        if ( !stats.Bounds )
        {
            ImGui::TextDisabled( "no sections" );
            return;
        }
        const glm::vec3 size = stats.Bounds->Max - stats.Bounds->Min;
        ImGui::Text( "Size  %.1f x %.1f x %.1f", size.x, size.y, size.z );
        ImGui::Text( "Min   %.1f %.1f %.1f", stats.Bounds->Min.x, stats.Bounds->Min.y, stats.Bounds->Min.z );
        ImGui::Text( "Max   %.1f %.1f %.1f", stats.Bounds->Max.x, stats.Bounds->Max.y, stats.Bounds->Max.z );
    }

    void StaticMeshViewerDocument::DrawLightAndLOD()
    {
        ImGui::Separator();
        ImGui::TextUnformatted( "LOD" );
        const std::size_t lods    = m_Stats ? m_Stats->LODs() : 1u;
        const std::string current = m_ForcedLOD < 0 ? std::string( "Auto" ) : std::format( "LOD {}", m_ForcedLOD );
        if ( ImGui::BeginCombo( "##lod", current.c_str() ) )
        {
            if ( ImGui::Selectable( "Auto", m_ForcedLOD < 0 ) )
                m_ForcedLOD = -1;
            for ( std::size_t lod = 0; lod < lods; ++lod )
            {
                const std::string label = std::format( "LOD {}", lod );
                if ( ImGui::Selectable( label.c_str(), m_ForcedLOD == static_cast<int>( lod ) ) )
                    m_ForcedLOD = static_cast<int>( lod );
            }
            ImGui::EndCombo();
        }

        if ( !m_Preview )
            return;
        // The preview scene's HDR sky and sun, which is the light the mesh is shown in; viewing only.
        ImGui::Separator();
        ImGui::TextUnformatted( "Light (viewing only)" );
        auto& setup = m_Preview->Setup();
        ImGui::SliderFloat( "Sky intensity", &setup.SkyIntensity, 0.0f, 8.0f, "%.2f" );
        ImGui::SliderFloat( "Exposure", &setup.Exposure, 0.01f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic );
        ImGui::SliderFloat( "Sun yaw", &setup.SunYawDegrees, -180.0f, 180.0f, "%.0f deg" );
        ImGui::SliderFloat( "Sun pitch", &setup.SunPitchDegrees, -89.0f, 89.0f, "%.0f deg" );
        ImGui::SliderFloat( "Sun intensity", &setup.LightIntensity, 0.0f, 20.0f, "%.2f" );
    }

    void StaticMeshViewerDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;

        if ( !m_Unavailable.empty() )
        {
            ImGui::TextWrapped( "%s", m_Unavailable.c_str() );
            return;
        }

        constexpr float kSidePanelWidth = 280.0f;
        const ImVec2    avail           = ImGui::GetContentRegionAvail();
        const ImVec2    view( std::max( avail.x - kSidePanelWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f ),
                              std::max( avail.y, 1.0f ) );
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        if ( ImGui::BeginChild( "##meshview", view ) )
        {
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##meshstats", ImVec2( kSidePanelWidth, view.y ) ) )
        {
            DrawStats();
            DrawLightAndLOD();
        }
        ImGui::EndChild();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestStaticMeshDocument( Assets::AssetManager*        assets,
                                                                      const std::string&           path,
                                                                      const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension() != Common::Constants::Extensions::STATIC_MESH ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::StaticMeshAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::StaticMeshAsset>( Assets::AssetPriority::Medium, path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a static mesh — no viewer was opened.", path );
            return Outcome::Failed;
        }
        if ( const auto loaded = asset->EnsureLoaded( *assets ); !loaded )
        {
            LOG_ERROR( "[Assets] '{}' would not load as a static mesh — no viewer was opened: {}", path,
                       loaded.GetError() );
            return Outcome::Failed;
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[Assets] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
