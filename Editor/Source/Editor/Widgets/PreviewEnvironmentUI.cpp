#include "PreviewEnvironmentUI.hpp"
#include "PreviewEnvironment.hpp"
#include "PreviewViewport.hpp"

#include <Editor/Core/EditorPreferences.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Runtime/Services/Skybox/SkyboxService.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <functional>
#include <string>
#include <utility>

namespace Desert::Editor::PreviewEnvironment
{
    namespace
    {
        struct Entry
        {
            std::string Path; // the key stored in editor.json and answered by FindByPath
            std::string Name; // what a person reads in the picker and the palette
        };

        // The registry's rows, not the loaded skyboxes (AL1-1): a skybox nobody has opened yet is still a choice.
        std::vector<Entry> Skyboxes()
        {
            std::vector<Entry> entries;
            for ( const Assets::ContentRegistry::PickerRow& row :
                  Assets::ContentRegistry::Rows( Common::Content::ContentKind::Skybox ) )
            {
                entries.push_back( { row.Path.generic_string(),
                                     row.DisplayName.empty() ? Common::Utils::FileSystem::GetFileName( row.Path )
                                                             : row.DisplayName } );
            }
            return entries;
        }

        void Commit()
        {
            // Save names its own failure; the edit stays live for this session either way.
            (void)EditorPreferences::Save();
        }

        void Edit( const std::function<void( Settings& )>& change )
        {
            change( EditorPreferences::Get().PreviewScene );
            Commit();
        }

        void CommitAfterEdit()
        {
            // On release, not per frame of a drag (EditorPreferences::Save explains why).
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                Commit();
        }
    } // namespace

    void ApplyTo( PreviewViewport& preview, const Assets::AssetManager* assets )
    {
        const PreviewViewport::Fill fill = preview.GetFill();
        if ( fill == PreviewViewport::Fill::SkyDome || fill == PreviewViewport::Fill::Cubemap )
            return;

        const Settings& settings = EditorPreferences::Get().PreviewScene;
        const Resolved  resolved =
             Resolve( settings,
                      [assets]( const std::string& path ) -> std::optional<uint64_t>
                      {
                          if ( assets == nullptr )
                              return std::nullopt;
                          const Assets::AssetHandle handle = Runtime::SkyboxHandleAtPath( path );
                          if ( handle == 0 )
                              return std::nullopt;
                          // Finding the row is not having the sky: an HDR no loaded scene uses has no
                          // MaterialSkybox, SkyboxECSSystem sends an empty SkyboxCommand and the preview is lit
                          // by the black EMPTY environment. So it is required here — created from its registry
                          // row and requested once; the loader delivers it and the preview lights up then.
                          const auto* service = Runtime::ResourceRegistry::GetSkyboxService();
                          if ( service != nullptr && !service->Get( handle ) && !service->IsPending( handle ) &&
                               !Runtime::RequireSkybox( handle ) )
                              return std::nullopt;
                          return static_cast<uint64_t>( handle );
                      } );

        // Named ONCE per path: this runs every frame in every preview, and the refusal does not change
        // until the person picks another HDR.
        static std::string s_RefusedPath;
        if ( !resolved.Error.empty() && s_RefusedPath != settings.Skybox )
        {
            LOG_ERROR( "{}", resolved.Error );
            s_RefusedPath = settings.Skybox;
        }

        PreviewViewport::SceneSetup& setup = preview.Setup();
        setup.EnvironmentSkybox            = resolved.Skybox;
        setup.EnvironmentLook              = LookOf( settings );
        setup.ShowEnvironment              = settings.ShowEnvironment;
        setup.ShowFloor                    = settings.ShowFloor;
    }

    void DrawEnvironmentRows()
    {
        Settings&         settings = EditorPreferences::Get().PreviewScene;
        const std::string current  = settings.Skybox.empty()
                                          ? std::string( "Preset sky" )
                                          : Common::Utils::FileSystem::GetFileName( settings.Skybox );

        ImGui::SetNextItemWidth( -FLT_MIN );
        if ( ImGui::BeginCombo( "##preview_hdr", current.c_str() ) )
        {
            if ( ImGui::Selectable( "Preset sky", settings.Skybox.empty() ) )
                Edit( []( Settings& s ) { s.Skybox.clear(); } );
            for ( const Entry& entry : Skyboxes() )
            {
                ImGui::PushID( entry.Path.c_str() );
                if ( ImGui::Selectable( entry.Name.c_str(), entry.Path == settings.Skybox ) )
                    Edit( [&entry]( Settings& s ) { s.Skybox = entry.Path; } );
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "HDR environment the preview is lit by and shown against. Shared by every "
                               "preview window and remembered between runs (editor.json)." );

        // Rotation and EV act where an HDR is SAMPLED; the preset sky has its own intensity and its
        // rotation is the sun bearing below, so the rows are disabled rather than silently doing nothing.
        ImGui::BeginDisabled( settings.Skybox.empty() );
        ImGui::SetNextItemWidth( -FLT_MIN );
        ImGui::SliderFloat( "##hdr_rotation", &settings.RotationDegrees, -180.0f, 180.0f, "Rotation %.0f deg" );
        CommitAfterEdit();
        ImGui::SetNextItemWidth( -FLT_MIN );
        ImGui::SliderFloat( "##hdr_ev", &settings.ExposureEV, kMinEV, kMaxEV, "EV %+.1f" );
        CommitAfterEdit();
        ImGui::EndDisabled();

        if ( ImGui::Checkbox( "Show Environment", &settings.ShowEnvironment ) )
            Commit();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Draw the environment behind the subject. Off keeps its lighting." );
    }

    bool DrawShowFloor( const char* label )
    {
        if ( !ImGui::Checkbox( label, &EditorPreferences::Get().PreviewScene.ShowFloor ) )
            return false;
        Commit();
        return true;
    }

    void AppendActions( std::vector<ISubjectDocument::DocumentAction>& actions )
    {
        actions.push_back(
             { "Preview environment: preset sky", [] { Edit( []( Settings& s ) { s.Skybox.clear(); } ); } } );
        for ( Entry& entry : Skyboxes() )
            actions.push_back( { "Preview environment: " + entry.Name, [path = std::move( entry.Path )]
                                 { Edit( [&path]( Settings& s ) { s.Skybox = path; } ); } } );

        actions.push_back( { "Preview EV +1", []
                             { Edit( []( Settings& s ) { s.ExposureEV = ClampEV( s.ExposureEV + 1.0f ); } ); } } );
        actions.push_back( { "Preview EV -1", []
                             { Edit( []( Settings& s ) { s.ExposureEV = ClampEV( s.ExposureEV - 1.0f ); } ); } } );
        actions.push_back( { "Preview EV 0", [] { Edit( []( Settings& s ) { s.ExposureEV = 0.0f; } ); } } );
        actions.push_back( { "Preview rotation +90", [] {
                                Edit( []( Settings& s )
                                      { s.RotationDegrees = WrapDegrees( s.RotationDegrees + 90.0f ); } );
                            } } );
        actions.push_back(
             { "Preview environment: show", [] { Edit( []( Settings& s ) { s.ShowEnvironment = true; } ); } } );
        actions.push_back(
             { "Preview environment: hide", [] { Edit( []( Settings& s ) { s.ShowEnvironment = false; } ); } } );
        actions.push_back( { "Preview floor: show", [] { Edit( []( Settings& s ) { s.ShowFloor = true; } ); } } );
        actions.push_back( { "Preview floor: hide", [] { Edit( []( Settings& s ) { s.ShowFloor = false; } ); } } );
    }
} // namespace Desert::Editor::PreviewEnvironment
