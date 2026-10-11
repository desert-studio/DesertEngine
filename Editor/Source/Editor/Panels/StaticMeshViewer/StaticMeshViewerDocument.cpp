#include "StaticMeshViewerDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Widgets/MeshAssetDetails.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
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
    StaticMeshViewerDocument::StaticMeshViewerDocument( const Assets::AssetHandle& mesh,
                                                        Assets::AssetManager*      assets )
         : StaticMeshViewerBase( AssetSubjectTitle( mesh, assets, "Static Mesh" ), mesh ), m_Assets( assets )
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
        PreviewEnvironment::AppendActions( actions );
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
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
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

    void StaticMeshViewerDocument::ExtendToolbar( AssetEditorToolbar& toolbar )
    {
        const std::size_t                       lods = m_Stats ? m_Stats->LODs() : 1u;
        std::vector<AssetEditorToolbar::Choice> choices;
        choices.push_back( { "LOD Auto", [this]() { m_ForcedLOD = -1; } } );
        for ( std::size_t lod = 0; lod < lods; ++lod )
            choices.push_back(
                 { std::format( "LOD {}", lod ), [this, lod]() { m_ForcedLOD = static_cast<int>( lod ); } } );
        const std::string current =
             m_ForcedLOD < 0 ? std::string( "LOD Auto" ) : std::format( "LOD {}", m_ForcedLOD );
        toolbar.AddCombo( ICON_MDI_LAYERS_TRIPLE_OUTLINE, current, "Which LOD the preview draws (viewing only)",
                          std::move( choices ), static_cast<std::size_t>( m_ForcedLOD ) + 1 );
        toolbar.AddButton(
             ICON_MDI_CHART_BOX_OUTLINE, "Stats", "Show the mesh statistics over the viewport",
             [this]() { m_ShowStats = !m_ShowStats; }, [this]() { return m_ShowStats; } );
    }

    std::string StaticMeshViewerDocument::StatusText() const
    {
        if ( !m_Stats )
            return {};
        return std::format( "{} tris \xc2\xb7 {} verts \xc2\xb7 {} sections", m_Stats->Triangles,
                            m_Stats->Vertices, m_Stats->Sections );
    }

    void StaticMeshViewerDocument::DrawViewportStats( const ImVec2& origin ) const
    {
        if ( !m_Stats || !m_ShowStats )
            return;
        const StaticMeshStats& stats = *m_Stats;
        // AUTO DOES NOT NAME A LEVEL: the preview does not report which LOD distance picked, so the counts are
        // LOD 0's and the line says so rather than presenting them as the drawn level's.
        const std::size_t shown =
             m_ForcedLOD < 0 ? 0u : std::min( static_cast<std::size_t>( m_ForcedLOD ), stats.LODs() - 1u );
        std::vector<std::string> lines;
        lines.push_back( m_ForcedLOD < 0 ? std::format( "LOD:  Auto (counts are LOD 0 of {})", stats.LODs() )
                                         : std::format( "LOD:  {} of {}", shown, stats.LODs() ) );
        lines.push_back( std::format(
             "Triangles:  {}", stats.TrianglesPerLOD.empty() ? stats.Triangles : stats.TrianglesPerLOD[shown] ) );
        lines.push_back( std::format( "Vertices:  {}", stats.Vertices ) );
        lines.push_back( std::format( "UV Channels:  {}", stats.UVChannels ) );
        lines.push_back( std::format( "Sections:  {}", stats.Sections ) );
        if ( stats.Bounds )
        {
            const glm::vec3 size = stats.Bounds->Max - stats.Bounds->Min;
            lines.push_back( std::format( "Approx Size:  {:.0f} x {:.0f} x {:.0f} cm", size.x, size.y, size.z ) );
        }
        else
            lines.emplace_back( "Approx Size:  no sections" );

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float step = ImGui::GetTextLineHeight() + 2.0f;
        ImVec2      at( origin.x + 10.0f, origin.y + 8.0f );
        const ImU32 text = ImGui::GetColorU32( ImVec4( 0.784f, 0.784f, 0.784f, 1.0f ) ); // #C8C8C8
        const ImU32 drop = IM_COL32( 0, 0, 0, 200 );
        for ( const std::string& line : lines )
        {
            draw->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), drop, line.c_str() );
            draw->AddText( at, text, line.c_str() );
            at.y += step;
        }
    }

    void StaticMeshViewerDocument::DrawLight()
    {
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

        // The editor-wide Preview Scene Settings, the same rows the Material Editor draws.
        ImGui::Separator();
        ImGui::TextUnformatted( "Environment" );
        PreviewEnvironment::DrawEnvironmentRows();
        PreviewEnvironment::DrawShowFloor( "Show Floor" );
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
            {
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
                DrawViewportStats( origin );
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##meshstats", ImVec2( kSidePanelWidth, view.y ) ) )
        {
            DrawStats();
            // The asset's Details (UE's Static Mesh Editor): its elements and the source's Import Settings with
            // Reimport — the one body the Animation Editor's Mesh mode draws too.
            if ( const auto asset =
                      m_Assets->FindByHandle<Assets::StaticMeshAsset>( Assets::AssetHandle( Subject().Owner ) ) )
                MeshAssetDetails::Draw( *asset );
            DrawLight();
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
            asset = assets->CreateAsset<Assets::StaticMeshAsset>( path );
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
