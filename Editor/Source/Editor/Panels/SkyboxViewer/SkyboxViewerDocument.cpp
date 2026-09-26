#include "SkyboxViewerDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Skybox/SkyboxService.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <filesystem>

namespace Desert::Editor
{
    namespace
    {
        // The three elevations a sky is checked at (memory: "shoot three elevations, not one"). Pitch is the
        // orbit camera's ELEVATION, so looking UP at the sky behind the ball means standing BELOW it: negative.
        // -90 is clamped by SetOrbit to the same limit the mouse has.
        constexpr float kZenithPitchDegrees  = -90.0f;
        constexpr float kMidSkyPitchDegrees  = -35.0f;
        constexpr float kHorizonPitchDegrees = 0.0f;

        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets != nullptr )
            {
                if ( const auto* meta = assets->FindMetadataByHandle( subject ) )
                    return meta->Filepath.filename().string();
            }
            return "Skybox";
        }

        // The cache's cube a picker entry reads. All three come from the one bake (EnvironmentBake.hpp).
        const Runtime::ImageHandle& EnvironmentCube( const Graphic::Environment& environment,
                                                     SkyboxView::Cube            cube )
        {
            switch ( cube )
            {
                case SkyboxView::Cube::Prefiltered:
                    return environment.PreFilteredMap;
                case SkyboxView::Cube::Irradiance:
                    return environment.IrradianceMap;
                case SkyboxView::Cube::Radiance:
                    break;
            }
            return environment.RadianceMap;
        }

        uint32_t PrefilteredMipsOf( const Assets::AssetHandle& skybox )
        {
            const auto material = Runtime::ResourceRegistry::GetSkyboxService()->Get( skybox );
            if ( !material || !material->GetEnvironment().PreFilteredMap.IsValid() )
                return 0u;
            const auto* image = Runtime::ResourceRegistry::GetImageService()->Resolve(
                 material->GetEnvironment().PreFilteredMap );
            return image != nullptr ? image->GetMipmapLevels() : 0u;
        }
    } // namespace

    SkyboxViewerDocument::SkyboxViewerDocument( const Assets::AssetHandle& skybox, Assets::AssetManager* assets )
         : SkyboxViewerBase( SubjectTitle( skybox, assets ), skybox ), m_Assets( assets )
    {
    }

    SkyboxViewerDocument::~SkyboxViewerDocument() = default;

    bool SkyboxViewerDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void SkyboxViewerDocument::EnsurePreview()
    {
        if ( m_Preview )
            return;

        // THE SKYBOX SERVICE BUILDS THE CUBE, and it is asked here — before the preview exists and outside any
        // frame — rather than in the per-frame resolver: Register bakes on the GPU behind a device-idle wait,
        // which must not happen inside the render graph the resolver is called from. Same order as the Details
        // Skybox picker (SkyboxComponent.cpp, bindSkybox).
        const Assets::AssetHandle handle( Subject().Owner );
        auto&                     skyboxes = *Runtime::ResourceRegistry::GetSkyboxService();
        if ( !skyboxes.Get( handle ) )
        {
            const auto asset =
                 m_Assets != nullptr ? m_Assets->FindByHandle<Assets::SkyboxAsset>( handle ) : nullptr;
            if ( !asset )
            {
                m_Unavailable = "This skybox is not registered with the asset manager — nothing to show.";
                return;
            }
            Graphic::Renderer::GetInstance().WaitDeviceIdle();
            skyboxes.Register( asset );
        }

        m_Unavailable.clear();
        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;

        m_Preview->SetCubemapMaterial(
             [this, handle]() -> Graphic::SampledCube
             {
                 // RESOLVED EVERY FRAME, never held: a reload replaces the material and unregisters the old one.
                 const auto material = Runtime::ResourceRegistry::GetSkyboxService()->Get( handle );
                 if ( !material )
                     return {};
                 const auto& environment = material->GetEnvironment();
                 const auto  level       = SkyboxView::ResolveLevel( m_View.Level, m_PrefilteredMips );
                 if ( !level )
                     return {};
                 const Runtime::ImageHandle& key = EnvironmentCube( environment, level->Source );
                 if ( !key.IsValid() )
                     return {};

                 Graphic::SampledCube source;
                 // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): all three keys name cubes
                 source.Cube = static_cast<Graphic::ImageCube*>(
                      Runtime::ResourceRegistry::GetImageService()->Resolve( key ) );
                 source.Lod = level->Lod;
                 // The file as authored, turned by THIS WINDOW's rotation; gain and tint stay identity (the
                 // component's knobs belong to a scene, not to the asset).
                 source.Look.RotationDegrees = m_View.RotationDegrees;
                 return source;
             } );
        m_Preview->SetCubemapBackdrop( true );

        if ( m_PendingOrbitDegrees )
        {
            SetOrbitDegrees( m_PendingOrbitDegrees->x, m_PendingOrbitDegrees->y );
            m_PendingOrbitDegrees.reset();
        }
    }

    void SkyboxViewerDocument::ReleaseView()
    {
        m_Preview.reset();
        m_UIHelper.reset();
        m_PreviewLive = false;
    }

    void SkyboxViewerDocument::SetOrbitDegrees( float yawDegrees, float pitchDegrees )
    {
        if ( !m_Preview )
        {
            m_PendingOrbitDegrees = glm::vec2( yawDegrees, pitchDegrees );
            return;
        }
        m_Preview->SetOrbit( glm::radians( yawDegrees ), glm::radians( pitchDegrees ) );
    }

    void SkyboxViewerDocument::SetPreviewViewpoint( const PreviewViewpoint& viewpoint )
    {
        SetOrbitDegrees( viewpoint.YawDegrees, viewpoint.PitchDegrees );
    }

    std::vector<ISubjectDocument::DocumentAction> SkyboxViewerDocument::Actions()
    {
        std::vector<DocumentAction> actions = {
             { "Look at zenith", [this]() { SetOrbitDegrees( 0.0f, kZenithPitchDegrees ); } },
             { "Look at mid sky", [this]() { SetOrbitDegrees( 0.0f, kMidSkyPitchDegrees ); } },
             { "Look at horizon", [this]() { SetOrbitDegrees( 0.0f, kHorizonPitchDegrees ); } },
        };
        for ( auto& mode : SkyboxView::ViewActions( m_PrefilteredMips ) )
            actions.push_back(
                 { std::move( mode.Label ), [this, apply = std::move( mode.Apply )]() { apply( m_View ); } } );
        return actions;
    }

    void SkyboxViewerDocument::OnPreUpdate()
    {
        // THE SLOT IS NOT CLAIMED UNTIL THE WINDOW HAS BEEN DRAWN (MaterialEditorPanel::OnPreUpdate has the
        // argument): the document exists a frame before its window does, and a hidden tab gives its slot back
        // through ReleaseView.
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_RenderSize.x == 0u || m_RenderSize.y == 0u )
            return;

        m_PrefilteredMips = PrefilteredMipsOf( Assets::AssetHandle( Subject().Owner ) );
        m_Preview->SetCubemapProjection( m_View.View == SkyboxView::Projection::LongLat2D );
        // Level, projection and rotation are sampled inside the resolver, where the render gate cannot see
        // them; the fingerprint is how a change to any of them asks for a frame.
        m_Preview->SetContentFingerprint( SkyboxView::Fingerprint( m_View ) );
        m_Preview->Setup().Exposure = SkyboxView::ExposureFromEV( m_View.ExposureEV );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    void SkyboxViewerDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;

        // UE's TextureCube editor row: 3D/2D, the level picker, EV. All read the cached cubes at sampling time.
        int projection = static_cast<int>( m_View.View );
        ImGui::RadioButton( "3D", &projection, static_cast<int>( SkyboxView::Projection::Sphere3D ) );
        ImGui::SameLine();
        ImGui::RadioButton( "2D", &projection, static_cast<int>( SkyboxView::Projection::LongLat2D ) );
        m_View.View = static_cast<SkyboxView::Projection>( projection );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 210.0f );
        if ( ImGui::BeginCombo( "Level", SkyboxView::LevelLabel( m_View.Level, m_PrefilteredMips ).c_str() ) )
        {
            for ( int i = 0; i < SkyboxView::LevelCount( m_PrefilteredMips ); ++i )
                if ( ImGui::Selectable( SkyboxView::LevelLabel( i, m_PrefilteredMips ).c_str(),
                                        i == m_View.Level ) )
                    m_View.Level = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 140.0f );
        ImGui::SliderFloat( "EV", &m_View.ExposureEV, SkyboxView::kMinEV, SkyboxView::kMaxEV, "%+.1f" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 200.0f );
        ImGui::SliderFloat( "Rotation", &m_View.RotationDegrees, -180.0f, 180.0f, "%.0f deg" );
        ImGui::SameLine();
        ImGui::TextDisabled( "(viewing only — not saved)" );

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_RenderSize       = glm::uvec2( static_cast<uint32_t>( std::max( avail.x, 1.0f ) ),
                                         static_cast<uint32_t>( std::max( avail.y, 1.0f ) ) );

        if ( !m_Unavailable.empty() )
        {
            ImGui::TextWrapped( "%s", m_Unavailable.c_str() );
            return;
        }
        if ( !m_Preview || !m_UIHelper )
        {
            ImGui::TextDisabled( "Starting the preview..." );
            return;
        }
        (void)m_Preview->Draw( *m_UIHelper, avail, PreviewInteraction::Interactive );
    }

    SubjectEditorRegistry::PathOpenOutcome RequestSkyboxDocument( Assets::AssetManager*        assets,
                                                                  const std::string&           path,
                                                                  const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != Assets::kTextureAssetExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        // The header decides, not the folder: it is what the importer wrote from the folder at import time, and
        // what AssetPreloader sorts skyboxes from textures by.
        const auto key = Assets::ReadTextureAssetKey( path );
        if ( !key.IsSuccess() || key.GetValue().Kind != Common::Content::ContentKind::Skybox )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::SkyboxAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::SkyboxAsset>( Assets::AssetPriority::Medium, path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a skybox — no viewer was opened.", path );
            return Outcome::Failed;
        }
        if ( const auto loaded = asset->EnsureLoaded( *assets ); !loaded )
        {
            LOG_ERROR( "[Assets] '{}' would not load as a skybox — no viewer was opened: {}", path,
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
