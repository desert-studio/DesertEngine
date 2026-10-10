#define IMGUI_DEFINE_MATH_OPERATORS
#define NOMINMAX // engine headers below use std::min/max; keep the windows.h macros out

#include "ThumbnailEditMode.hpp"

#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailEdit.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailPose.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Core/Scene.hpp>       // GetFinalImage
#include <Engine/Graphic/Image.hpp>    // Image2D::ReadPixelsRGBA8
#include <Engine/Graphic/Renderer.hpp> // WaitDeviceIdle before readback
#include <Common/Core/Logger.hpp>
#include <ImGui/imgui_internal.h> // SetItemUsingMouseWheel

// STB_IMAGE_WRITE_IMPLEMENTATION lives in Desert.lib; just declare for the capture PNG write.
#include <stb_image/stb_image_write.h>

#include <ImGui/imgui.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    // Port pattern: UE Editor/ContentBrowser/Private/SThumbnailEditModeTools.cpp (the tile as the orbit control,
    // one transaction per gesture) and AssetContextMenu.cpp ExecuteCaptureThumbnail (the viewport as the picture).
    ThumbnailEditMode::ThumbnailEditMode( Assets::AssetManager*                assetManager,
                                          std::weak_ptr<::Desert::Core::Scene> viewportScene, Delegates delegates )
         : m_AssetManager( assetManager ), m_ViewportScene( std::move( viewportScene ) ),
           m_On( std::move( delegates ) )
    {
    }

    Common::BoolResultStr ThumbnailEditMode::Capture( const DirectoryInformation& entry )
    {
        // WHERE THE PICTURE IS FILED: the key the asset's tile reads and the hash its judge compares. A PNG
        // written under the asset path alone was not what a model's tile reads (its cooked mesh's key), and a
        // PNG with no recorded hash is Capture to the judge, so the service shot over it.
        const std::optional<ThumbnailProducers::CaptureKey> how =
             entry.IsFile ? ThumbnailProducers::CaptureKeyOf( entry.Type ) : std::nullopt;
        if ( !how )
            return Common::MakeError<bool>( "its picture is not shot through a camera (a model, skeletal mesh, "
                                            "skeleton, animation or material has one)" );
        ThumbnailService::PictureKey key;
        switch ( *how )
        {
            case ThumbnailProducers::CaptureKey::ImportedMesh:
            {
                const std::optional<std::string> cooked = m_On.CookedPictureOf( entry.AssetPath, entry.Type );
                if ( !cooked )
                    return Common::MakeError<bool>(
                         "not imported: there is no cooked mesh to file a picture under" );
                key = ThumbnailService::MeshPictureKey( *cooked );
                break;
            }
            case ThumbnailProducers::CaptureKey::PosedFile:
                key = ThumbnailService::MeshPictureKey( entry.AssetPath );
                break;
            case ThumbnailProducers::CaptureKey::MaterialFile:
                key = ThumbnailService::MaterialPictureKey( entry.AssetPath );
                break;
        }

        const auto scene = m_ViewportScene.lock();
        if ( !scene )
            return Common::MakeError<bool>( "there is no viewport scene to capture" );
        const auto img = scene->GetFinalImage(); // the main viewport's post-processed render
        if ( !img )
            return Common::MakeError<bool>( "the viewport has not rendered a frame yet" );

        Graphic::Renderer::GetInstance().WaitDeviceIdle(); // readback after the GPU finished the frame
        // A failed readback is refused by name, never answered with a blank picture.
        const auto read = img->ReadPixelsRGBA8();
        if ( !read.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the viewport readback failed: {}", read.GetError() );

        const std::vector<uint8_t>& src = read.GetValue();
        const uint32_t              W   = img->GetWidth();
        const uint32_t              H   = img->GetHeight();
        if ( W == 0 || H == 0 || src.size() != static_cast<size_t>( W ) * H * 4 )
            return Common::MakeFormattedError<bool>( "the viewport readback returned {} byte(s) for a {}x{} image",
                                                     src.size(), W, H );

        // Center-crop to a square, then downscale (nearest) to a square thumbnail — frame the asset in the
        // viewport so the centered square captures it.
        const uint32_t       side = ( W < H ) ? W : H; // (avoid std::min — windows.h min macro)
        const uint32_t       ox   = ( W - side ) / 2;
        const uint32_t       oy   = ( H - side ) / 2;
        constexpr uint32_t   kOut = 512; // NOTE: not 'OUT' — that's a windows.h SAL macro
        std::vector<uint8_t> out( static_cast<size_t>( kOut ) * kOut * 4 );
        for ( uint32_t y = 0; y < kOut; ++y )
            for ( uint32_t x = 0; x < kOut; ++x )
            {
                const uint32_t sx = ox + x * side / kOut;
                const uint32_t sy = oy + y * side / kOut;
                for ( uint32_t c = 0; c < 4; ++c )
                    out[( ( y * kOut + x ) * 4 ) + c] = src[( ( sy * W + sx ) * 4 ) + c];
            }

        const std::string& png = key.Png;
        std::error_code    ec;
        std::filesystem::create_directories( std::filesystem::path( png ).parent_path(), ec );
        stbi_flip_vertically_on_write( 0 ); // viewport readback is already upright (same as the offscreen path)
        if ( stbi_write_png( png.c_str(), kOut, kOut, 4, out.data(), kOut * 4 ) == 0 )
            return Common::MakeFormattedError<bool>( "the picture could not be written to '{}'", png );
        if ( key.Hash )
        {
            if ( const Common::BoolResultStr recorded = ThumbnailFreshness::Record( png, *key.Hash ); !recorded )
                return recorded;
        }
        m_On.OnCaptured( png, entry.AssetPath ); // the grid reloads the new image, a refused tile shows it
        LOG_INFO( "[Thumbnail] Captured from viewport -> {}", png );
        return Common::MakeSuccess( true );
    }

    std::optional<std::string> ThumbnailEditMode::OrbitFileOf( const DirectoryInformation& entry ) const
    {
        if ( !entry.IsFile || !ThumbnailProducers::HasThumbnailOrbit( entry.Type ) )
            return std::nullopt;
        const std::optional<ThumbnailProducers::Producer> how = ThumbnailProducers::ProducerOf( entry.Type );
        if ( how == ThumbnailProducers::Producer::RenderedMaterial ||
             how == ThumbnailProducers::Producer::RenderedPose )
            return entry.AssetPath; // filed under itself (a posed kind is its own cooked form)
        return m_On.CookedPictureOf( entry.AssetPath, entry.Type );
    }

    std::vector<ThumbnailEditMode::Subject>
    ThumbnailEditMode::SubjectsOf( const std::vector<DirectoryInformation*>& entries ) const
    {
        std::vector<Subject> subjects;
        for ( const DirectoryInformation* entry : entries )
            if ( std::optional<std::string> orbitFile = OrbitFileOf( *entry ) )
                subjects.push_back( { entry->AssetPath, std::move( *orbitFile ) } );
        return subjects;
    }

    Common::BoolResultStr ThumbnailEditMode::Enter( const DirectoryInformation& entry, std::string_view label )
    {
        if ( !entry.IsFile || !ThumbnailProducers::HasThumbnailOrbit( entry.Type ) )
            return Common::MakeFormattedError<bool>(
                 "'{}': '{}' has no thumbnail orbit (its picture is not shot through an orbit camera)", label,
                 entry.AssetPath );
        const std::optional<std::string> orbitFile = OrbitFileOf( entry );
        if ( !orbitFile )
            return Common::MakeFormattedError<bool>( "'{}': '{}' has no picture to edit (not imported)", label,
                                                     entry.AssetPath );
        if ( const auto stated = ThumbnailEdit::ReadOrbit( *orbitFile ); !stated )
            return Common::MakeFormattedError<bool>( "'{}': {}", label, stated.GetError() );
        if ( m_Gesture )
            CommitGesture();
        m_Path      = entry.AssetPath;
        m_OrbitFile = *orbitFile;
        return Common::MakeSuccess( true );
    }

    void ThumbnailEditMode::CommitGesture()
    {
        if ( !m_Gesture )
            return;
        const Assets::ThumbnailOrbit live = m_Gesture->Live;
        ThumbnailService::Get().EndPreview( m_Gesture->PreviewKey );
        m_Gesture.reset();
        if ( auto edited = ThumbnailEdit::EditOrbit( m_OrbitFile, live ); !edited )
            LOG_WARN( "[Thumbnail] Edit Thumbnail '{}': {}", m_Path, edited.GetError() );
    }

    void ThumbnailEditMode::Leave()
    {
        CommitGesture();
        m_Path.clear();
        m_OrbitFile.clear();
    }

    void ThumbnailEditMode::RequestPreview( const DirectoryInformation& entry, Gesture& gesture )
    {
        if ( m_AssetManager == nullptr )
            return;
        if ( entry.Type == FileType::Material )
        {
            const auto subject = ThumbnailSubject::ResolveMaterial(
                 *m_AssetManager, entry.AssetPath, []( const std::string&, const auto& ) {} ); // tile asks again
            if ( !subject )
                return;
            if ( const auto& material = subject.GetValue(); material )
                gesture.PreviewPng =
                     ThumbnailService::Get().RequestPreviewMaterial( *material, entry.AssetPath, gesture.Live );
            return;
        }
        // A posed picture (a .skmesh / .skeleton / .anim, or a skinned source's .skmesh: OrbitFileOf) is the
        // pose route's: the static route refuses a skinned mesh as "no drawable geometry".
        if ( CookPaths::IsSkinnedAssetFile( m_OrbitFile ) )
        {
            const auto pose = ThumbnailPose::ResolvePoseSubject( *m_AssetManager, m_OrbitFile );
            if ( pose && !pose.GetValue().Pending )
                gesture.PreviewPng = ThumbnailService::Get().RequestPreviewPose( pose.GetValue(), gesture.Live );
            return;
        }
        const std::optional<std::string> source = m_On.MeshSourceOf( entry ); // a model, or a foliage type's mesh
        if ( !source )
            return;
        const auto subject = ThumbnailSubject::ResolveMesh( *m_AssetManager, *source );
        if ( subject && !subject.GetValue().Pending )
            gesture.PreviewPng = ThumbnailService::Get().RequestPreviewMesh( subject.GetValue(), gesture.Live );
    }

    void ThumbnailEditMode::Draw( const DirectoryInformation& entry, const ImVec2& min, const ImVec2& max )
    {
        const ImGuiIO& io      = ImGui::GetIO();
        const bool     hovered = ImGui::IsItemHovered();
        const bool     active  = ImGui::IsItemActive();
        if ( hovered )
            ImGui::SetItemUsingMouseWheel(); // the wheel zooms the tile instead of scrolling the browser

        const bool dragging = active && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 0.0f );
        const bool wheeled  = hovered && io.MouseWheel != 0.0f;
        if ( ( dragging || wheeled ) && !m_Gesture )
        {
            // The gesture starts from what the home states NOW (an undo since the last gesture moved it).
            const auto stated = ThumbnailEdit::ReadOrbit( m_OrbitFile );
            if ( !stated )
            {
                LOG_WARN( "[Thumbnail] Edit Thumbnail '{}': {}", entry.AssetPath, stated.GetError() );
                Leave();
                return;
            }
            // The preview is keyed where the tile's picture is (OrbitFileOf): a skinned source's is its .skmesh,
            // not the .stmesh an extension swap would name.
            m_Gesture = Gesture{ .From       = stated.GetValue(),
                                 .Live       = stated.GetValue(),
                                 .Drag       = ImVec2( 0.0f, 0.0f ),
                                 .Wheel      = 0.0f,
                                 .LastWheel  = 0.0,
                                 .PreviewKey = m_OrbitFile,
                                 .PreviewPng = {} };
        }
        if ( m_Gesture )
        {
            Gesture& g = *m_Gesture;
            if ( dragging )
                g.Drag = ImGui::GetMouseDragDelta( ImGuiMouseButton_Left, 0.0f );
            if ( wheeled )
            {
                g.Wheel += io.MouseWheel;
                g.LastWheel = ImGui::GetTime();
            }
            g.Live = ThumbnailEdit::Orbited( g.From, g.Drag.x, g.Drag.y, g.Wheel );
            RequestPreview( entry, g ); // the service keeps only the newest orbit

            // THE LIVE PICTURE over the tile, once this gesture's first preview has landed.
            if ( !g.PreviewPng.empty() && ThumbnailService::Get().PreviewLanded( g.PreviewKey ) )
                if ( const auto img = m_On.Thumbnails().Get( g.PreviewPng ) )
                    if ( ImTextureID tex = m_On.TextureIdOf( img ); tex != nullptr )
                        ImGui::GetWindowDrawList()->AddImage( tex, min, max );

            const bool wheelRests = g.Wheel != 0.0f && ImGui::GetTime() - g.LastWheel > kWheelRestSeconds;
            if ( !active && ( ImGui::IsItemDeactivated() || !hovered || wheelRests ) )
                CommitGesture();
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect( ImVec2( min.x - 2.0f, min.y - 2.0f ), ImVec2( max.x + 2.0f, max.y + 2.0f ),
                     ImGui::GetColorU32( ImGuiCol_DragDropTarget ), 4.0f, 0, 2.0f );
        const std::string readout =
             m_Gesture ? std::format( "yaw {:.0f}  pitch {:.0f}  zoom {:.2f}", m_Gesture->Live.Yaw,
                                      m_Gesture->Live.Pitch, m_Gesture->Live.Zoom )
                       : std::string( "drag / wheel" );
        dl->AddText( ImVec2( min.x + 4.0f, min.y + 2.0f ), IM_COL32( 255, 255, 255, 230 ), readout.c_str() );

        // Leaving: Esc, or a click anywhere outside this tile. A running gesture is committed first.
        const bool clickedOutside = !hovered && ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) ||
                                                  ImGui::IsMouseClicked( ImGuiMouseButton_Right ) );
        if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) || clickedOutside )
            Leave();
    }
} // namespace Desert::Editor
