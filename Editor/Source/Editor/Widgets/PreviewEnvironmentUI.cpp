#include "PreviewEnvironmentUI.hpp"
#include "PreviewEnvironment.hpp"
#include "PreviewViewport.hpp"

#include <Editor/Core/EditorPreferences.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

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

        std::vector<Entry> SkyboxesIn( const Assets::AssetManager* assets )
        {
            std::vector<Entry> entries;
            if ( assets == nullptr )
                return entries;
            for ( const auto& [handle, asset] : assets->FindAllByType<Assets::SkyboxAsset>() )
            {
                const auto& path = asset->GetMetadata().Filepath;
                entries.push_back( { path.generic_string(), Common::Utils::FileSystem::GetFileName( path ) } );
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
                          if ( auto asset = assets->FindByPath<Assets::SkyboxAsset>( Common::Filepath( path ) ) )
                              return static_cast<uint64_t>( asset->GetMetadata().Handle );
                          return std::nullopt;
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

    void DrawEnvironmentRows( const Assets::AssetManager* assets )
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
            for ( const Entry& entry : SkyboxesIn( assets ) )
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

    void AppendActions( std::vector<ISubjectDocument::DocumentAction>& actions,
                        const Assets::AssetManager*                    assets )
    {
        actions.push_back(
             { "Preview environment: preset sky", [] { Edit( []( Settings& s ) { s.Skybox.clear(); } ); } } );
        for ( Entry& entry : SkyboxesIn( assets ) )
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
