// VIEWPORT CAPTURE — see ViewportCapture.hpp. Moved out of EditorLayer.cpp unchanged (EDL-4).
#include "Editor/LevelEditor/ViewportCapture.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/Core/ProjectContext.hpp"
#include <Common/Core/Core.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp> // reading the PRESENTED frame back (shot.window)
#include <Engine/Graphic/SceneRenderer.hpp>              // the view's HDR scene colour (--shot-hdr)
#include <Engine/Graphic/DisplayOutput.hpp>
#include <Engine/Graphic/PixelPack.hpp>
#include <stb_image/stb_image_write.h>

#include <glm/gtc/packing.hpp>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace Desert::Editor
{
    namespace
    {
        // Every PNG this file writes: top row first, RGBA8, tightly packed. One spelling, so the flip and the
        // stride cannot differ between the thumbnail, the viewport shot and the window shot.
        bool WritePngRGBA8( const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba )
        {
            stbi_flip_vertically_on_write( 0 );
            return stbi_write_png( path.c_str(), static_cast<int>( width ), static_cast<int>( height ), 4, rgba,
                                   static_cast<int>( width ) * 4 ) != 0;
        }

        // The directory of a sequence is named on the command line and usually does not exist yet. Create it
        // rather than letting the writer fail on a path that is only missing a folder.
        bool CreateParentDirectory( const std::string& path )
        {
            const std::filesystem::path file = std::filesystem::path( path );
            if ( file.has_parent_path() && !file.parent_path().empty() )
            {
                std::error_code ec;
                std::filesystem::create_directories( file.parent_path(), ec );
                if ( ec && !std::filesystem::exists( file.parent_path() ) )
                {
                    LOG_ERROR( "[Shot] could not create '{}': {}", file.parent_path().string(), ec.message() );
                    return false;
                }
            }
            return true;
        }

        // A PFM (the Portable Float Map: "PF", the size, a negative scale for little endian, then float RGB rows
        // from the BOTTOM row up). @p rgb is top row first, tightly packed.
        bool WritePfmRGB32F( const std::string& path, uint32_t width, uint32_t height,
                             const std::vector<float>& rgb )
        {
            std::ofstream out( std::filesystem::path( path ), std::ios::binary );
            if ( !out )
                return false;
            out << std::format( "PF\n{} {}\n-1.0\n", width, height );
            const std::streamsize rowBytes = static_cast<std::streamsize>( width ) * 3 * sizeof( float );
            for ( uint32_t row = height; row-- > 0; )
                out.write( reinterpret_cast<const char*>( rgb.data() + static_cast<size_t>( row ) * width * 3 ),
                           rowBytes );
            return static_cast<bool>( out );
        }
    } // namespace

    // The one readback. Every picture the editor writes out of the viewport comes through here, so a
    // capture cannot quietly differ from a dump in flip, format or the device-idle wait that makes the
    // readback legal at all.
    Common::BoolResultStr ViewportCapture::ReadViewportRGBA8( std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                                                              uint32_t& outHeight )
    {
        if ( !m_Workspace.ActiveScene() )
            return Common::MakeError<bool>( "no scene to capture" );

        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        auto img = m_Workspace.ActiveScene()->GetFinalImage();
        if ( !img )
            return Common::MakeError<bool>( "scene has no final image" );

        // Г13: ReadPixelsRGBA8 answers instead of returning an empty vector for every kind of failure.
        // The size check below is kept and now means only what it says — this arm carries the reason.
        auto read = img->ReadPixelsRGBA8();
        if ( !read.IsSuccess() )
            return Common::MakeFormattedError<bool>( "readback refused: {}", read.GetError() );

        outPixels = read.ExtractValue();
        outWidth  = img->GetWidth();
        outHeight = img->GetHeight();
        if ( outPixels.size() != static_cast<size_t>( outWidth ) * outHeight * 4 )
            return Common::MakeFormattedError<bool>( "readback is {} bytes, expected {}x{}x4 = {}",
                                                     outPixels.size(), outWidth, outHeight,
                                                     static_cast<size_t>( outWidth ) * outHeight * 4 );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ViewportCapture::WriteProjectThumbnail()
    {
        // The picture belongs to a project, so with no project there is nowhere for it to go. Not an
        // error: the editor can be running a scene that was opened without one.
        const std::string projectDirectory = Desert::Project::ProjectContext::Directory();
        if ( projectDirectory.empty() )
            return BOOLSUCCESS;

        std::vector<uint8_t> px;
        uint32_t             w = 0;
        uint32_t             h = 0;
        if ( const auto read = ReadViewportRGBA8( px, w, h ); !read.IsSuccess() )
            return read;
        if ( w == 0 || h == 0 )
            return Common::MakeError<bool>( "the viewport has no size" );

        // 16:9 window out of the middle of whatever the viewport is, then a box downsample to the
        // fixed output size. Cropping rather than squashing, because a squashed frame is a picture
        // of the wrong world; centre rather than top, because the interesting part of a viewport is
        // where the camera is pointed.
        constexpr uint32_t kOutW = 512;
        constexpr uint32_t kOutH = 288; // 16:9 — the aspect the launcher's grid is built out of

        uint32_t cropW = w;
        uint32_t cropH = ( w * kOutH ) / kOutW;
        if ( cropH > h )
        {
            cropH = h;
            cropW = ( h * kOutW ) / kOutH;
        }
        const uint32_t cropX = ( w - cropW ) / 2;
        const uint32_t cropY = ( h - cropH ) / 2;

        std::vector<uint8_t> out( static_cast<size_t>( kOutW ) * kOutH * 4 );
        for ( uint32_t y = 0; y < kOutH; ++y )
        {
            // Source rows this output row averages over. Integer bounds on both ends so no source
            // pixel is counted twice and none is skipped.
            const uint32_t sy0 = cropY + ( y * cropH ) / kOutH;
            const uint32_t sy1 = std::max( sy0 + 1u, cropY + ( ( y + 1 ) * cropH ) / kOutH );
            for ( uint32_t x = 0; x < kOutW; ++x )
            {
                const uint32_t sx0 = cropX + ( x * cropW ) / kOutW;
                const uint32_t sx1 = std::max( sx0 + 1u, cropX + ( ( x + 1 ) * cropW ) / kOutW );

                uint32_t accum[4] = { 0, 0, 0, 0 };
                uint32_t samples  = 0;
                for ( uint32_t sy = sy0; sy < sy1 && sy < h; ++sy )
                    for ( uint32_t sx = sx0; sx < sx1 && sx < w; ++sx )
                    {
                        const size_t at = ( static_cast<size_t>( sy ) * w + sx ) * 4;
                        for ( int c = 0; c < 4; ++c )
                            accum[c] += px[at + static_cast<size_t>( c )];
                        ++samples;
                    }
                const size_t dst = ( static_cast<size_t>( y ) * kOutW + x ) * 4;
                for ( int c = 0; c < 4; ++c )
                    out[dst + static_cast<size_t>( c )] =
                         static_cast<uint8_t>( samples != 0 ? accum[c] / samples : 0u );
            }
        }

        // The name is a CONVENTION shared with the launcher, which looks for exactly this file
        // beside the .deproj. Leading dot so it does not show up as project content.
        const std::string file = ( std::filesystem::path( projectDirectory ) / ".thumbnail.png" ).string();
        if ( !WritePngRGBA8( file, kOutW, kOutH, out.data() ) )
            return Common::MakeFormattedError<bool>( "could not write {}", file );
        LOG_INFO( "[Project] thumbnail -> {} ({}x{})", file, kOutW, kOutH );
        return BOOLSUCCESS;
    }

    // Read the resolved viewport back off the GPU and write it as a PNG. The one place that does this: the
    // still capture, every frame of a `--shot-sequence`, and the F9 dump all go through here, so a capture
    // cannot quietly differ from a dump in flip, format or the device-idle wait that makes the readback
    // legal at all.
    bool ViewportCapture::WriteViewportPng( const std::string& path )
    {
        if ( !m_Workspace.ActiveScene() )
        {
            LOG_ERROR( "[Shot] no scene to capture ('{}')", path );
            return false;
        }

        if ( !CreateParentDirectory( path ) )
            return false;

        std::vector<uint8_t> px;
        uint32_t             w = 0;
        uint32_t             h = 0;
        if ( const auto read = ReadViewportRGBA8( px, w, h ); !read.IsSuccess() )
        {
            LOG_ERROR( "[Shot] {} ('{}')", read.GetError(), path );
            return false;
        }

        const bool written = WritePngRGBA8( path, w, h, px.data() );
        LOG_INFO( "[Shot] {} -> {} ({}x{})", written ? "wrote" : "FAILED to write", path, w, h );
        return written;
    }

    bool ViewportCapture::WriteSceneColorPfm( const std::string& path )
    {
        const auto* scene = m_Workspace.ActiveScene().get();
        if ( !scene )
        {
            LOG_ERROR( "[Shot] no scene to capture the scene colour of ('{}')", path );
            return false;
        }
        if ( !CreateParentDirectory( path ) )
            return false;
        const Graphic::SceneRenderer* renderer = scene->GetViewRenderer( 0 );
        const auto                    image    = renderer ? renderer->GetSceneColorImage() : nullptr;
        if ( !image )
        {
            LOG_ERROR( "[Shot] view 0 has no scene colour target ('{}')", path );
            return false;
        }
        const auto format = image->GetImageSpecification().Format;
        if ( format != Desert::Core::Formats::ImageFormat::RGBA16F )
        {
            LOG_ERROR( "[Shot] the scene colour is not RGBA16F (format {}); the PFM writer reads RGBA16F ('{}')",
                       static_cast<int>( format ), path );
            return false;
        }

        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        auto begun = image->BeginReadbackBytes();
        if ( !begun.IsSuccess() )
        {
            LOG_ERROR( "[Shot] scene colour readback refused: {} ('{}')", begun.GetError(), path );
            return false;
        }
        // The copy was submitted by BeginReadbackBytes; an idle device has finished it.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        const auto& readback = begun.GetValue();
        if ( !readback->IsComplete() )
        {
            LOG_ERROR( "[Shot] the scene colour copy did not complete on an idle device ('{}')", path );
            return false;
        }
        auto bytes = readback->ReadBytes();
        if ( !bytes.IsSuccess() )
        {
            LOG_ERROR( "[Shot] scene colour readback failed: {} ('{}')", bytes.GetError(), path );
            return false;
        }
        const std::vector<uint8_t>& texels      = bytes.GetValue();
        const uint32_t              w           = image->GetWidth();
        const uint32_t              h           = image->GetHeight();
        const size_t                count       = static_cast<size_t>( w ) * h;
        constexpr size_t            kTexelBytes = 4 * sizeof( uint16_t );
        if ( texels.size() != count * kTexelBytes )
        {
            LOG_ERROR( "[Shot] scene colour readback is {} bytes, expected {}x{}x{} = {} ('{}')", texels.size(), w,
                       h, kTexelBytes, count * kTexelBytes, path );
            return false;
        }
        std::vector<float> rgb( count * 3 );
        for ( size_t texel = 0; texel < count; ++texel )
        {
            uint16_t halves[3] = {};
            std::memcpy( halves, texels.data() + texel * kTexelBytes, sizeof( halves ) );
            for ( size_t c = 0; c < 3; ++c )
                rgb[texel * 3 + c] = glm::unpackHalf1x16( halves[c] );
        }
        const bool written = WritePfmRGB32F( path, w, h, rgb );
        LOG_INFO( "[Shot] {} -> {} ({}x{} HDR scene colour)", written ? "wrote" : "FAILED to write", path, w, h );
        return written;
    }

    // The record half of a window capture, on the frame the control channel's gate named (see
    // EditorLayer::RecordWindowCaptureIfDue): a swapchain image may only be touched between acquire and present.
    void ViewportCapture::RecordWindowCapture()
    {
        auto swapChain = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanSwapChain>(
             EngineContext::GetInstance().GetWindow()->GetWindowSwapChain() );
        if ( !swapChain )
        {
            m_CaptureError = "there is no Vulkan swapchain to capture the presented frame from.";
            return;
        }

        if ( const auto recorded = swapChain->RecordFrameCapture(); !recorded )
        {
            m_CaptureError = recorded.GetError();
            return;
        }
        m_CaptureError.clear();
    }

    bool ViewportCapture::WriteWindowPng( const std::string& path, std::string& outError )
    {
        // THE WHOLE EDITOR, INTERFACE INCLUDED — which is what WriteViewportPng next door cannot do and
        // never could. That one reads the scene's own final image; ImGui is recorded into the SWAPCHAIN
        // render pass, so no capture this engine took before this existed held a single pixel of a panel,
        // a menu or a dialog. It is why proving anything about the interface meant photographing the
        // window from outside the process, by PID.
        //
        // This is the second half of the capture: the copy was recorded into this frame's command buffer
        // by RecordWindowCaptureIfDue, and the bytes are collected here, once the present that carried it
        // has gone out.
        if ( !m_CaptureError.empty() )
        {
            outError = m_CaptureError;
            m_CaptureError.clear();
            return false;
        }

        auto swapChain = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanSwapChain>(
             EngineContext::GetInstance().GetWindow()->GetWindowSwapChain() );
        if ( !swapChain )
        {
            outError = "there is no Vulkan swapchain to collect the captured frame from.";
            return false;
        }

        const std::filesystem::path file = std::filesystem::path( path );
        if ( file.has_parent_path() && !file.parent_path().empty() )
        {
            std::error_code ec;
            std::filesystem::create_directories( file.parent_path(), ec );
            if ( ec && !std::filesystem::exists( file.parent_path() ) )
            {
                outError = std::format( "could not create '{}': {}", file.parent_path().string(), ec.message() );
                return false;
            }
        }

        // The copy was submitted with this frame; waiting is what makes the staging buffer readable.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        uint32_t   width  = 0;
        uint32_t   height = 0;
        const auto pixels = swapChain->TakeCapturedFrameRGBA8( width, height );
        if ( !pixels )
        {
            outError = pixels.GetError();
            return false;
        }

        const bool written = WritePngRGBA8( path, width, height, pixels.GetValue().data() );
        if ( !written )
        {
            outError = std::format( "stb_image_write refused to write '{}'.", path );
            return false;
        }

        LOG_INFO( "[Control] wrote the presented frame (editor and interface) -> {} ({}x{})", path, width,
                  height );
        LogHdrRenditionPeak();
        return true;
    }

    void ViewportCapture::LogHdrRenditionPeak()
    {
        // HDR-OUTPUT p3: on an HDR output the PNG above is the SDR rendition (Graphic::SetSdrCaptureRendition);
        // the HDR transform of the same frame is the tonemap's second target, and its peak is the evidence the
        // HDR picture carries above paper white. Read on the idle device the swapchain readback already waited.
        if ( !Graphic::SdrCaptureRenditionRequested() || !Graphic::CurrentDisplayOutput().IsHDR() )
            return;
        const auto*                   scene    = m_Workspace.ActiveScene().get();
        const Graphic::SceneRenderer* renderer = scene ? scene->GetViewRenderer( 0 ) : nullptr;
        const auto                    image    = renderer ? renderer->GetHdrRenditionImage() : nullptr;
        if ( !image )
        {
            LOG_ERROR( "[Display] the HDR output's capture has no HDR rendition (view 0 drew none): no peak" );
            return;
        }
        auto begun = image->BeginReadbackBytes();
        if ( !begun.IsSuccess() )
        {
            LOG_ERROR( "[Display] HDR rendition readback refused: {}", begun.GetError() );
            return;
        }
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        const auto bytes = begun.GetValue()->ReadBytes();
        if ( !bytes.IsSuccess() )
        {
            LOG_ERROR( "[Display] HDR rendition readback failed: {}", bytes.GetError() );
            return;
        }
        const std::size_t pixels = static_cast<std::size_t>( image->GetWidth() ) * image->GetHeight();
        // The rendition holds SceneComposite's extended-sRGB encoding: the brightest code is the brightest light.
        const Graphic::ScRGBPeak peak    = Graphic::FindScRGBPeak( bytes.GetValue().data(), bytes.GetValue().size(), pixels );
        const float              linear  = Graphic::SrgbToLinearExtended( peak.Value );
        const float              paper   = Graphic::CurrentDisplayOutput().PaperWhiteNits;
        LOG_INFO( "[Display] HDR rendition of the captured frame: peak {:.3f} x paper white ({:.0f} nits) at viewport "
                  "pixel ({}, {}) of {}x{}; headroom {:.3f}",
                  linear, linear * paper, peak.Pixel % image->GetWidth(), peak.Pixel / image->GetWidth(),
                  image->GetWidth(), image->GetHeight(), Graphic::CurrentDisplayOutput().Headroom );
    }
} // namespace Desert::Editor
