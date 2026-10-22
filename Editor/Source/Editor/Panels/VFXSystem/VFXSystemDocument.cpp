#include "VFXSystemDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Panels/AssetEditorToolbar.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        namespace S = Assets::Serialization;

        // Exhaustive on purpose (no default): a new enumerator is a -Wswitch warning here, not a blank label.
        const char* SourceLabel( const S::VFXInputSource source )
        {
            switch ( source )
            {
                case S::VFXInputSource::Value:
                    return "Value";
                case S::VFXInputSource::Curve:
                    return "Curve";
                case S::VFXInputSource::Binding:
                    return "Bind";
                case S::VFXInputSource::Random:
                    return "Random";
            }
            return "?";
        }

        const char* RendererLabel( const S::VFXRendererKind kind )
        {
            switch ( kind )
            {
                case S::VFXRendererKind::Sprite:
                    return "Sprite Renderer";
                case S::VFXRendererKind::Ribbon:
                    return "Ribbon Renderer";
                case S::VFXRendererKind::Mesh:
                    return "Mesh Renderer";
            }
            return "?";
        }

        std::string VecText( const glm::vec4& v )
        {
            return std::format( "{:.3g}, {:.3g}, {:.3g}, {:.3g}", v.x, v.y, v.z, v.w );
        }

        // What the input is set to, in the words of its source (the stack row's right-hand side).
        std::string InputSummary( const S::VFXModuleInput& input )
        {
            switch ( input.Source )
            {
                case S::VFXInputSource::Value:
                    return input.Value ? VecText( *input.Value ) : std::string( "(no value)" );
                case S::VFXInputSource::Curve:
                    return input.Curve ? std::format( "{} channel(s)", input.Curve->size() )
                                       : std::string( "(no curve)" );
                case S::VFXInputSource::Binding:
                    return input.Binding ? *input.Binding : std::string( "(unbound)" );
                case S::VFXInputSource::Random:
                    return input.Random ? std::format( "[{}] .. [{}]", VecText( input.Random->Min ),
                                                       VecText( input.Random->Max ) )
                                        : std::string( "(no range)" );
            }
            return {};
        }

        void DrawStage( const char* title, const std::vector<S::VFXModuleUse>& modules )
        {
            ImGui::PushID( title );
            if ( ImGui::CollapsingHeader( title, ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                if ( modules.empty() )
                    ImGui::TextDisabled( "  (no modules)" );
                for ( std::size_t m = 0; m < modules.size(); ++m )
                {
                    const auto& use = modules[m];
                    ImGui::PushID( static_cast<int>( m ) );
                    const std::string label =
                         std::format( "{}{}", use.Module, use.Enabled ? "" : "  (disabled)" );
                    if ( ImGui::TreeNodeEx( "##module", ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str() ) )
                    {
                        for ( const auto& input : use.Inputs )
                        {
                            const std::string summary = InputSummary( input );
                            ImGui::BulletText( "%s  [%s]  %s", input.Name.c_str(), SourceLabel( input.Source ),
                                               summary.c_str() );
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
    } // namespace

    VFXSystemDocument::VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( subject, assets, "VFX System" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXSystem ) ) ),
           m_Assets( assets )
    {
        const auto asset = m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXSystemAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the system asset is not held by the asset manager";
            return;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( auto loaded = asset->LoadFromFile(); !loaded )
            {
                m_LoadError = loaded.GetError();
                return;
            }
        }
        m_Data   = asset->GetData();
        m_Loaded = true;
    }

    VFXSystemDocument::~VFXSystemDocument() = default;

    bool VFXSystemDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void VFXSystemDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Loaded )
            return;
        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;
        // The preview scene's own VFXComponent plays the system the VFXSystemService holds for this handle.
        m_Preview->SetVFXSystem( Assets::AssetHandle( Subject().Owner ) );
    }

    void VFXSystemDocument::ReleaseView()
    {
        m_Preview.reset();
        m_UIHelper.reset();
        m_PreviewLive = false;
    }

    void VFXSystemDocument::OnPreUpdate()
    {
        // The slot is claimed only once the window has been drawn (StaticMeshViewerDocument::OnPreUpdate).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_ViewSize.x == 0u || m_ViewSize.y == 0u )
            return;
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_ViewSize.x, m_ViewSize.y );
    }

    void VFXSystemDocument::ExtendToolbar( AssetEditorToolbar& toolbar )
    {
        toolbar.AddButton(
             ICON_MDI_REFRESH, "Restart", "Start every emitter of the preview over (UE Reset Simulation)",
             [this]()
             {
                 if ( m_Preview )
                     m_Preview->RestartVFX();
             },
             []() { return false; } );
    }

    void VFXSystemDocument::DrawSystemColumn()
    {
        ImGui::TextUnformatted( "SYSTEM" );
        ImGui::Separator();
        ImGui::Text( "Category  %s", m_Data.Category.empty() ? "-" : m_Data.Category.c_str() );
        ImGui::Text( "Duration  %.2f s", m_Data.Duration );
        ImGui::Text( "Loop      %s", m_Data.Loop ? "yes" : "no" );
        ImGui::Text( "Seed      %u", m_Data.Seed );

        ImGui::Spacing();
        ImGui::TextUnformatted( "USER PARAMETERS" );
        ImGui::Separator();
        if ( m_Data.UserParams.empty() )
            ImGui::TextDisabled( "(none)" );
        for ( const auto& param : m_Data.UserParams )
            ImGui::BulletText( "User.%s", param.Name.c_str() );

        ImGui::Spacing();
        ImGui::TextUnformatted( "EMITTERS" );
        ImGui::Separator();
        for ( std::size_t e = 0; e < m_Data.Emitters.size(); ++e )
        {
            const auto&       emitter = m_Data.Emitters[e];
            const std::string label   = std::format( "{}{}  ({} max)##emitter{}", emitter.Name,
                                                     emitter.Enabled ? "" : " (disabled)", emitter.Capacity, e );
            if ( ImGui::Selectable( label.c_str(), m_SelectedEmitter == e ) )
                m_SelectedEmitter = e;
        }
    }

    void VFXSystemDocument::DrawStackColumn()
    {
        if ( m_Data.Emitters.empty() )
        {
            ImGui::TextDisabled( "The system has no emitters." );
            return;
        }
        m_SelectedEmitter         = std::min( m_SelectedEmitter, m_Data.Emitters.size() - 1 );
        const auto& emitter       = m_Data.Emitters[m_SelectedEmitter];
        ImGui::Text( "STACK  %s", emitter.Name.c_str() );
        ImGui::Separator();
        // UE's stack order: Emitter Update runs once per tick, Particle Spawn once per new particle, Particle
        // Update per live particle per tick, then the Render rows.
        DrawStage( "Emitter Update", emitter.Stack.EmitterUpdate );
        DrawStage( "Particle Spawn", emitter.Stack.ParticleSpawn );
        DrawStage( "Particle Update", emitter.Stack.ParticleUpdate );
        if ( ImGui::CollapsingHeader( "Render", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            if ( emitter.Renderers.empty() )
                ImGui::TextDisabled( "  (no renderers)" );
            for ( const auto& row : emitter.Renderers )
                ImGui::BulletText( "%s%s%s%s", RendererLabel( row.Kind ), row.Enabled ? "" : " (disabled)",
                                   row.Material ? "  material " : "",
                                   row.Material ? row.Material->Guid.c_str() : "" );
        }
    }

    void VFXSystemDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This system could not be read: %s", m_LoadError.c_str() );
            return;
        }

        constexpr float kSystemWidth = 260.0f;
        constexpr float kStackWidth  = 380.0f;
        const float     spacing      = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2    avail        = ImGui::GetContentRegionAvail();
        const ImVec2    view( std::max( avail.x - kSystemWidth - kStackWidth - 2.0f * spacing, 1.0f ),
                              std::max( avail.y, 1.0f ) );
        m_ViewSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        if ( ImGui::BeginChild( "##vfxsystem", ImVec2( kSystemWidth, view.y ), ImGuiChildFlags_Borders ) )
            DrawSystemColumn();
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##vfxpreview", view ) )
        {
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##vfxstack", ImVec2( kStackWidth, view.y ), ImGuiChildFlags_Borders ) )
            DrawStackColumn();
        ImGui::EndChild();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestVFXSystemDocument( Assets::AssetManager*        assets,
                                                                     const std::string&           path,
                                                                     const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != S::kVFXSystemExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::VFXSystemAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::VFXSystemAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[VFX] '{}' could not be registered as a VFX system - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[VFX] '{}' would not load as a VFX system - no editor was opened: {}", path,
                           loaded.GetError() );
                return Outcome::Failed;
            }
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[VFX] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
