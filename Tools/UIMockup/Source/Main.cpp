// UIMockup - draws one editor-UI mockup with the editor's theme and fonts, either to a PNG (--out) or in an
// interactive window. See Tools/UIMockup/premake5.lua for the command line.

#include "Mockup.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#if defined( __APPLE__ )
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image/stb_image_write.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif defined( __APPLE__ )
#include <mach-o/dyld.h>
#endif

namespace
{
    using namespace Desert;

    // ── Framebuffer objects through GLFW's loader ───────────────────────────────────────────────────
    // The platform gl.h stops at 1.1 on Windows and at 2.1 on macOS; the capture needs the 3.0 FBO entry
    // points, so they are fetched from the live context rather than from a second GL loader.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#if defined( _WIN32 )
#define MOCKUP_GLAPI __stdcall
#else
#define MOCKUP_GLAPI
#endif

    struct FboApi
    {
        void( MOCKUP_GLAPI* genFramebuffers )( GLsizei, GLuint* )                       = nullptr;
        void( MOCKUP_GLAPI* deleteFramebuffers )( GLsizei, const GLuint* )              = nullptr;
        void( MOCKUP_GLAPI* bindFramebuffer )( GLenum, GLuint )                         = nullptr;
        GLenum( MOCKUP_GLAPI* checkFramebufferStatus )( GLenum )                        = nullptr;
        void( MOCKUP_GLAPI* genRenderbuffers )( GLsizei, GLuint* )                      = nullptr;
        void( MOCKUP_GLAPI* deleteRenderbuffers )( GLsizei, const GLuint* )             = nullptr;
        void( MOCKUP_GLAPI* bindRenderbuffer )( GLenum, GLuint )                        = nullptr;
        void( MOCKUP_GLAPI* renderbufferStorage )( GLenum, GLenum, GLsizei, GLsizei )   = nullptr;
        void( MOCKUP_GLAPI* framebufferRenderbuffer )( GLenum, GLenum, GLenum, GLuint ) = nullptr;

        bool Load()
        {
            auto get = []( auto& fn, const char* name )
            {
                fn = reinterpret_cast<std::remove_reference_t<decltype( fn )>>( glfwGetProcAddress( name ) );
                return fn != nullptr;
            };
            return get( genFramebuffers, "glGenFramebuffers" ) &&
                   get( deleteFramebuffers, "glDeleteFramebuffers" ) &&
                   get( bindFramebuffer, "glBindFramebuffer" ) &&
                   get( checkFramebufferStatus, "glCheckFramebufferStatus" ) &&
                   get( genRenderbuffers, "glGenRenderbuffers" ) &&
                   get( deleteRenderbuffers, "glDeleteRenderbuffers" ) &&
                   get( bindRenderbuffer, "glBindRenderbuffer" ) &&
                   get( renderbufferStorage, "glRenderbufferStorage" ) &&
                   get( framebufferRenderbuffer, "glFramebufferRenderbuffer" );
        }
    };

    struct Options
    {
        std::string           mockup;
        std::filesystem::path out;
        int                   width  = 1600;
        int                   height = 900;
        bool                  list   = false;
    };

    void PrintUsage()
    {
        std::fprintf( stderr, "usage: UIMockup --list\n"
                              "       UIMockup --mockup <name> [--out <file.png>] [--size WxH]\n"
                              "       (without --out an interactive window opens)\n" );
    }

    std::optional<Options> ParseArguments( int argc, char** argv )
    {
        Options options;
        for ( int i = 1; i < argc; ++i )
        {
            const std::string_view arg      = argv[i];
            const bool             hasValue = i + 1 < argc;
            if ( arg == "--list" )
                options.list = true;
            else if ( arg == "--mockup" && hasValue )
                options.mockup = argv[++i];
            else if ( arg == "--out" && hasValue )
                options.out = argv[++i];
            else if ( arg == "--size" && hasValue )
            {
                const std::string_view size = argv[++i];
                const size_t           x    = size.find( 'x' );
                if ( x == std::string_view::npos ||
                     std::from_chars( size.data(), size.data() + x, options.width ).ec != std::errc{} ||
                     std::from_chars( size.data() + x + 1, size.data() + size.size(), options.height ).ec !=
                          std::errc{} ||
                     options.width < 64 || options.height < 64 )
                {
                    std::fprintf( stderr, "[UIMockup] --size wants WxH (each at least 64), got '%s'\n", argv[i] );
                    return std::nullopt;
                }
            }
            else
            {
                std::fprintf( stderr, "[UIMockup] unknown or incomplete argument '%s'\n", argv[i] );
                return std::nullopt;
            }
        }
        if ( !options.list && options.mockup.empty() )
            return std::nullopt;
        return options;
    }

    std::filesystem::path ExecutableDirectory()
    {
#if defined( _WIN32 )
        wchar_t     buffer[MAX_PATH] = {};
        const DWORD length           = GetModuleFileNameW( nullptr, buffer, MAX_PATH );
        return length == 0 ? std::filesystem::path() : std::filesystem::path( buffer ).parent_path();
#elif defined( __APPLE__ )
        uint32_t size = 0;
        _NSGetExecutablePath( nullptr, &size );
        std::vector<char> buffer( size + 1, '\0' );
        if ( _NSGetExecutablePath( buffer.data(), &size ) != 0 )
            return {};
        return std::filesystem::weakly_canonical( buffer.data() ).parent_path();
#else
        std::error_code error;
        return std::filesystem::read_symlink( "/proc/self/exe", error ).parent_path();
#endif
    }

    // The editor's text glyph ranges, spelled exactly as EditorResources.cpp spells them (ImFontAtlas keeps
    // the pointer, so it must be static).
    const ImWchar kTextRanges[] = { 0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F,
                                    0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0 };
    const ImWchar kIconRanges[] = { ICON_MIN_MDI, ICON_MAX_MDI, 0 };

    // The editor's font set (EditorResources::Initialize): Bold 16, ExtraBold 22, Regular 18 with the MDI
    // icons merged at 16. Returns an empty optional - and says which file - when any font is missing.
    std::optional<UIMockup::Fonts> LoadEditorFonts( const std::filesystem::path& fontDirectory )
    {
        ImGuiIO& io = ImGui::GetIO();

        ImFontConfig text;
        text.OversampleH        = 3;
        text.OversampleV        = 1;
        text.PixelSnapH         = true;
        text.RasterizerMultiply = 1.10f;

        for ( const char* file : { "Roboto-Regular.ttf", "Roboto-Bold.ttf", "materialdesignicons-webfont.ttf" } )
        {
            if ( !std::filesystem::exists( fontDirectory / file ) )
            {
                std::fprintf( stderr, "[UIMockup] font missing: %s\n", ( fontDirectory / file ).string().c_str() );
                return std::nullopt;
            }
        }
        const std::string regular = ( fontDirectory / "Roboto-Regular.ttf" ).string();
        const std::string bold    = ( fontDirectory / "Roboto-Bold.ttf" ).string();
        const std::string icons   = ( fontDirectory / "materialdesignicons-webfont.ttf" ).string();

        UIMockup::Fonts fonts;
        fonts.bold      = io.Fonts->AddFontFromFileTTF( bold.c_str(), 16.0f, &text, kTextRanges );
        fonts.extraBold = io.Fonts->AddFontFromFileTTF( bold.c_str(), 22.0f, &text, kTextRanges );
        fonts.regular   = io.Fonts->AddFontFromFileTTF( regular.c_str(), 18.0f, &text, kTextRanges );

        ImFontConfig iconConfig;
        iconConfig.MergeMode        = true;
        iconConfig.PixelSnapH       = true;
        iconConfig.OversampleH      = 1;
        iconConfig.OversampleV      = 1;
        iconConfig.GlyphMinAdvanceX = 4.0f;
        iconConfig.SizePixels       = 16.0f;
        iconConfig.GlyphOffset.y    = 1.0f;
        if ( io.Fonts->AddFontFromFileTTF( icons.c_str(), 16.0f, &iconConfig, kIconRanges ) == nullptr ||
             fonts.regular == nullptr || fonts.bold == nullptr || fonts.extraBold == nullptr )
        {
            std::fprintf( stderr, "[UIMockup] a font in %s failed to load\n", fontDirectory.string().c_str() );
            return std::nullopt;
        }
        io.FontDefault = fonts.regular;
        return fonts;
    }

    // Reads the bound framebuffer (bottom-up rows) and writes it top-down as an RGB PNG.
    bool WritePng( const std::filesystem::path& path, int width, int height )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( width ) * height * 4 );
        glPixelStorei( GL_PACK_ALIGNMENT, 1 );
        glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

        std::vector<unsigned char> rgb( static_cast<size_t>( width ) * height * 3 );
        for ( int y = 0; y < height; ++y )
        {
            const unsigned char* source = pixels.data() + static_cast<size_t>( height - 1 - y ) * width * 4;
            unsigned char*       target = rgb.data() + static_cast<size_t>( y ) * width * 3;
            for ( int x = 0; x < width; ++x )
            {
                target[x * 3 + 0] = source[x * 4 + 0];
                target[x * 3 + 1] = source[x * 4 + 1];
                target[x * 3 + 2] = source[x * 4 + 2];
            }
        }
        if ( path.has_parent_path() )
        {
            std::error_code error;
            std::filesystem::create_directories( path.parent_path(), error );
        }
        return stbi_write_png( path.string().c_str(), width, height, 3, rgb.data(), width * 3 ) != 0;
    }

    void ClearToWindowBackground()
    {
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glClearColor( bg.x, bg.y, bg.z, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
    }

    // Headless: the window is never shown and the frame goes into an offscreen W×H framebuffer at scale 1,
    // so the PNG has exactly the requested pixels whatever the display's content scale is. ImGui sizes
    // auto-fit content over the first frames, so several frames are run before the one that is captured.
    int RunCapture( GLFWwindow* window, const UIMockup::Mockup& mockup, const UIMockup::Fonts& fonts,
                    const Options& options )
    {
        FboApi fbo;
        if ( !fbo.Load() )
        {
            std::fprintf( stderr, "[UIMockup] the GL context has no framebuffer objects\n" );
            return 4;
        }
        GLuint framebuffer = 0, color = 0;
        fbo.genFramebuffers( 1, &framebuffer );
        fbo.bindFramebuffer( GL_FRAMEBUFFER, framebuffer );
        fbo.genRenderbuffers( 1, &color );
        fbo.bindRenderbuffer( GL_RENDERBUFFER, color );
        fbo.renderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, options.width, options.height );
        fbo.framebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color );
        if ( fbo.checkFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
        {
            std::fprintf( stderr, "[UIMockup] the offscreen framebuffer is incomplete\n" );
            return 4;
        }

        ImGuiIO&      io            = ImGui::GetIO();
        constexpr int kSettleFrames = 4;
        for ( int frame = 0; frame < kSettleFrames; ++frame )
        {
            io.DisplaySize = ImVec2( static_cast<float>( options.width ), static_cast<float>( options.height ) );
            io.DisplayFramebufferScale = ImVec2( 1.0f, 1.0f );
            io.DeltaTime               = 1.0f / 60.0f;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            mockup.draw( fonts );
            ImGui::Render();

            fbo.bindFramebuffer( GL_FRAMEBUFFER, framebuffer );
            glViewport( 0, 0, options.width, options.height );
            ClearToWindowBackground();
            ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
        }
        glFinish();
        const bool written = WritePng( options.out, options.width, options.height );

        fbo.bindFramebuffer( GL_FRAMEBUFFER, 0 );
        fbo.deleteRenderbuffers( 1, &color );
        fbo.deleteFramebuffers( 1, &framebuffer );
        (void)window;

        if ( !written )
        {
            std::fprintf( stderr, "[UIMockup] could not write %s\n", options.out.string().c_str() );
            return 5;
        }
        std::printf( "[UIMockup] %s -> %s (%dx%d)\n", std::string( mockup.name ).c_str(),
                     options.out.string().c_str(), options.width, options.height );
        return 0;
    }

    int RunInteractive( GLFWwindow* window, const UIMockup::Mockup& mockup, const UIMockup::Fonts& fonts )
    {
        ImGui_ImplGlfw_InitForOpenGL( window, true );
        glfwShowWindow( window );
        while ( !glfwWindowShouldClose( window ) )
        {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            mockup.draw( fonts );
            ImGui::Render();

            int width = 0, height = 0;
            glfwGetFramebufferSize( window, &width, &height );
            glViewport( 0, 0, width, height );
            ClearToWindowBackground();
            ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
            glfwSwapBuffers( window );
        }
        ImGui_ImplGlfw_Shutdown();
        return 0;
    }

    void GlfwErrorCallback( int code, const char* description )
    {
        std::fprintf( stderr, "[UIMockup] GLFW error %d: %s\n", code, description );
    }
} // namespace

int main( int argc, char** argv )
{
    const std::optional<Options> options = ParseArguments( argc, argv );
    if ( !options )
    {
        PrintUsage();
        return 1;
    }
    if ( options->list )
    {
        for ( const UIMockup::Mockup& mockup : UIMockup::AllMockups() )
            std::printf( "%-20.*s %.*s\n", static_cast<int>( mockup.name.size() ), mockup.name.data(),
                         static_cast<int>( mockup.summary.size() ), mockup.summary.data() );
        return 0;
    }
    const UIMockup::Mockup* mockup = UIMockup::FindMockup( options->mockup );
    if ( mockup == nullptr )
    {
        std::fprintf( stderr, "[UIMockup] no mockup named '%s' (see --list)\n", options->mockup.c_str() );
        return 1;
    }

    glfwSetErrorCallback( &GlfwErrorCallback );
    glfwInitHint( GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE );
    if ( glfwInit() != GLFW_TRUE )
        return 2;

    // A 3.2 core context: the only kind macOS gives above 2.1, and what imgui_impl_opengl3 picks
    // "#version 150" for. The window starts hidden; only the interactive mode shows it.
    glfwWindowHint( GLFW_CONTEXT_VERSION_MAJOR, 3 );
    glfwWindowHint( GLFW_CONTEXT_VERSION_MINOR, 2 );
    glfwWindowHint( GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE );
    glfwWindowHint( GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE );
    glfwWindowHint( GLFW_VISIBLE, GLFW_FALSE );
    const std::string title = "UIMockup - " + options->mockup;
    GLFWwindow* window      = glfwCreateWindow( options->width, options->height, title.c_str(), nullptr, nullptr );
    if ( window == nullptr )
    {
        glfwTerminate();
        return 3;
    }
    glfwMakeContextCurrent( window );
    glfwSwapInterval( 1 );

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    Editor::ThemeManager::SetDarkTheme();

    int                                  result = 0;
    const std::optional<UIMockup::Fonts> fonts  = LoadEditorFonts( ExecutableDirectory() / "Resources" / "Fonts" );
    if ( !fonts )
        result = 6;
    else
    {
        ImGui_ImplOpenGL3_Init( nullptr );
        result = options->out.empty() ? RunInteractive( window, *mockup, *fonts )
                                      : RunCapture( window, *mockup, *fonts, *options );
        ImGui_ImplOpenGL3_Shutdown();
    }

    ImGui::DestroyContext();
    glfwDestroyWindow( window );
    glfwTerminate();
    return result;
}
