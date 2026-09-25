#include "TextureViewerDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace Desert::Editor
{
    namespace
    {
        constexpr float kMinZoom       = 1.0f / 64.0f;
        constexpr float kMaxZoom       = 64.0f;
        constexpr float kWheelZoomStep = 1.15f;

        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets )
            {
                if ( const auto* meta = assets->FindMetadataByHandle( subject ) )
                    return meta->Filepath.filename().string();
            }
            return "Texture";
        }

        // Exhaustive on purpose (no default): a format added to the enum is a -Wswitch warning here rather
        // than a viewer that silently prints "?" for it.
        const char* FormatName( ::Desert::Core::Formats::ImageFormat format )
        {
            using F = ::Desert::Core::Formats::ImageFormat;
            switch ( format )
            {
                case F::RGBA8F:
                    return "RGBA8";
                case F::RGBA16F:
                    return "RGBA16F";
                case F::RGBA32F:
                    return "RGBA32F";
                case F::BGRA8F:
                    return "BGRA8";
                case F::DEPTH24STENCIL8:
                    return "D24S8";
                case F::DEPTH32F:
                    return "D32F";
                case F::BC7_UNORM:
                    return "BC7";
                case F::BC6H_UFLOAT:
                    return "BC6H";
                case F::BC4_UNORM:
                    return "BC4";
                case F::BC5_UNORM:
                    return "BC5";
                case F::R16_UNORM:
                    return "R16";
                case F::R32F:
                    return "R32F";
                case F::Count:
                    return "Count (not a format)";
            }
            return "unknown";
        }
    } // namespace

    TextureViewerDocument::TextureViewerDocument( const Assets::AssetHandle& subject,
                                                  Assets::AssetManager*      assets )
         : ISubjectDocument( SubjectTitle( subject, assets ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::Texture2D ) ) ),
           m_Assets( assets )
    {
    }

    TextureViewerDocument::~TextureViewerDocument() = default;

    bool TextureViewerDocument::IsSubjectAlive() const
    {
        return m_Assets && m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void TextureViewerDocument::ZoomAbout( float newZoom, const ImVec2& pivot, const ImVec2& canvasCentre )
    {
        newZoom           = std::clamp( newZoom, kMinZoom, kMaxZoom );
        const float ratio = newZoom / m_Zoom;
        // The image centre sits at canvasCentre + m_Pan; scaling the offset from the pivot by `ratio` keeps
        // the texel under the pivot fixed on screen.
        const ImVec2 fromPivot( canvasCentre.x + m_Pan.x - pivot.x, canvasCentre.y + m_Pan.y - pivot.y );
        m_Pan  = ImVec2( pivot.x + fromPivot.x * ratio - canvasCentre.x,
                         pivot.y + fromPivot.y * ratio - canvasCentre.y );
        m_Zoom = newZoom;
        m_Fit  = false;
    }

    void TextureViewerDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End (CloudNoiseVolumePanel.cpp:169).
        const auto image = TextureDnD::ResolveImage( Assets::AssetHandle( Subject().Owner ) );
        if ( !image )
        {
            // Not silent: the opener registered the asset with the texture service, so a null here means the
            // GPU image could not be built — the texture service logged why.
            ImGui::TextColored( ImVec4( 0.95f, 0.45f, 0.40f, 1.0f ),
                                "Texture %016llx has no GPU image — the texture service could not build it "
                                "(see the log).",
                                static_cast<unsigned long long>( Subject().Owner ) );
            return;
        }
        if ( !m_UIHelper )
        {
            m_UIHelper = std::make_unique<UI::UIHelper>();
            m_UIHelper->Init();
        }

        auto&       spec   = image->GetImageSpecification();
        const float width  = static_cast<float>( std::max( spec.Width, 1u ) );
        const float height = static_cast<float>( std::max( spec.Height, 1u ) );

        // ── Toolbar + facts ─────────────────────────────────────────────────────────────────────────────
        const bool fitClicked = ImGui::Button( ICON_MDI_FIT_TO_SCREEN " Fit" );
        ImGui::SameLine();
        const bool oneToOne = ImGui::Button( "1:1" );
        ImGui::SameLine();
        ImGui::Text( "%u x %u  |  %s  |  %u mip%s  |  %s  |  %.0f%%", spec.Width, spec.Height,
                     FormatName( spec.Format ), spec.Mips, spec.Mips == 1 ? "" : "s",
                     ::Desert::Core::Formats::IsBlockCompressed( spec.Format ) ? "block-compressed"
                                                                               : "uncompressed",
                     static_cast<double>( m_Zoom * 100.0f ) );

        // ── Canvas ──────────────────────────────────────────────────────────────────────────────────────
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail  = ImGui::GetContentRegionAvail();
        const ImVec2 canvas( std::max( avail.x, 1.0f ), std::max( avail.y, 1.0f ) );
        const ImVec2 centre( origin.x + canvas.x * 0.5f, origin.y + canvas.y * 0.5f );

        if ( fitClicked )
            m_Fit = true;
        if ( m_Fit )
        {
            m_Zoom = std::clamp( std::min( canvas.x / width, canvas.y / height ), kMinZoom, kMaxZoom );
            m_Pan  = ImVec2( 0.0f, 0.0f );
        }
        if ( oneToOne )
            ZoomAbout( 1.0f, centre, centre );

        ImGui::InvisibleButton( "##texture_canvas", canvas,
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle );
        const ImGuiIO& io = ImGui::GetIO();
        if ( ImGui::IsItemHovered() && io.MouseWheel != 0.0f )
            ZoomAbout( m_Zoom * std::pow( kWheelZoomStep, io.MouseWheel ), io.MousePos, centre );
        if ( ImGui::IsItemActive() && ( io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ) )
        {
            m_Pan = ImVec2( m_Pan.x + io.MouseDelta.x, m_Pan.y + io.MouseDelta.y );
            m_Fit = false;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect( origin, ImVec2( origin.x + canvas.x, origin.y + canvas.y ), true );
        draw->AddRectFilled( origin, ImVec2( origin.x + canvas.x, origin.y + canvas.y ),
                             IM_COL32( 24, 24, 24, 255 ) );
        const ImVec2 size( width * m_Zoom, height * m_Zoom );
        ImGui::SetCursorScreenPos(
             ImVec2( centre.x + m_Pan.x - size.x * 0.5f, centre.y + m_Pan.y - size.y * 0.5f ) );
        m_UIHelper->Image( image, size );
        draw->PopClipRect();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestTextureDocument( Assets::AssetManager*        assets,
                                                                   const std::string&           path,
                                                                   const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( !assets || std::filesystem::path( path ).extension() != Assets::kTextureAssetExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::TextureAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::TextureAsset>( Assets::AssetPriority::Medium, path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a texture — no viewer was opened.", path );
            return Outcome::Failed;
        }
        if ( const auto loaded = asset->EnsureLoaded( *assets ); !loaded )
        {
            LOG_ERROR( "[Assets] '{}' would not load as a texture — no viewer was opened: {}", path,
                       loaded.GetError() );
            return Outcome::Failed;
        }

        // The texture service builds the GPU image on its first Get; the viewer only ever asks by handle.
        Runtime::ResourceRegistry::GetTextureService()->RegisterAsset( asset );

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
