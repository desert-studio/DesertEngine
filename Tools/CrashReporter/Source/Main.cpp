// DesertCrashReporter - the window Common::Crash::LaunchReporter starts after a crash.
//
// WHY IT LINKS NO ENGINE. It is started BY a process that has just died, to explain why. Anything it
// depends on is a thing that can be broken at the moment it is needed, so its whole dependency set is
// GLFW, Dear ImGui and the Win32 shell - no Desert, no Vulkan, no asset system.
//
// This file is UI only. Every line that reads or interprets the report lives in CrashReport.cpp, so
// the move to our own UI framework replaces this file and keeps that one.
//
// The palette and metrics below are the editor's measured UE5 values; the layout follows UE5's
// CrashReportClient - header band, summary card, comment box, footer band.
//
// THE CALL STACK IS NOT SHOWN IN THIS WINDOW, BY DECISION. It is still collected, still written to
// crash.txt and the minidump, and still carried out of the UI by "Copy report", which puts the whole
// report file - [stack] section included - on the clipboard.

#include "CrashReport.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#if !defined( NOMINMAX )
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#endif

namespace
{
    constexpr const char* kWindowTitle = "DesertEngine crashed";

    // ── The UE5 palette, as measured in the editor ───────────────────────────────────────────────
    constexpr ImU32 kColWindow     = IM_COL32( 0x24, 0x24, 0x24, 0xFF );
    constexpr ImU32 kColBand       = IM_COL32( 0x15, 0x15, 0x15, 0xFF );
    constexpr ImU32 kColCardHeader = IM_COL32( 0x2F, 0x2F, 0x2F, 0xFF );
    constexpr ImU32 kColRule       = IM_COL32( 0x1A, 0x1A, 0x1A, 0xFF );
    constexpr ImU32 kColSunk       = IM_COL32( 0x0F, 0x0F, 0x0F, 0xFF );
    constexpr ImU32 kColText       = IM_COL32( 0xC8, 0xC8, 0xC8, 0xFF );
    constexpr ImU32 kColMuted      = IM_COL32( 0x8A, 0x8A, 0x8A, 0xFF );
    constexpr ImU32 kColAccent     = IM_COL32( 0x00, 0x70, 0xE0, 0xFF );
    constexpr ImU32 kColError      = IM_COL32( 0xCB, 0x26, 0x00, 0xFF );
    constexpr ImU32 kColSelected   = IM_COL32( 0x40, 0x57, 0x6F, 0xFF );
    constexpr ImU32 kColScrollGrab = IM_COL32( 0x50, 0x50, 0x50, 0xFF );

    // FontAwesome 4 codepoints, spelled as UTF-8 so no extra header is needed.
    // fontawesome-webfont.ttf is the FA4 face in Editor/Resources/Fonts.
    constexpr const char* kIconWarning = "\xef\x81\xb1"; // f071 exclamation-triangle
    constexpr const char* kIconBug     = "\xef\x86\x88"; // f188 bug
    constexpr const char* kIconCopy    = "\xef\x83\x85"; // f0c5 copy
    constexpr const char* kIconFolder  = "\xef\x81\xbb"; // f07b folder
    constexpr const char* kIconRedo    = "\xef\x80\x9e"; // f01e repeat

    struct Fonts
    {
        ImFont* body  = nullptr;
        ImFont* title = nullptr;
        ImFont* mono  = nullptr;
        // Every font file that could not be loaded, with the full path tried. Shown in the window:
        // a missing font is a visible defect here, never a silent swap for the ImGui default.
        std::vector<std::string> missing;
    };

    ImVec4 ToVec4( ImU32 inColor )
    {
        return ImGui::ColorConvertU32ToFloat4( inColor );
    }

#if defined( _WIN32 )
    std::wstring ToWide( const std::string& inText )
    {
        if ( inText.empty() )
        {
            return std::wstring();
        }
        const int needed =
             ::MultiByteToWideChar( CP_UTF8, 0, inText.c_str(), static_cast<int>( inText.size() ), nullptr, 0 );
        if ( needed <= 0 )
        {
            return std::wstring();
        }
        std::wstring wide( static_cast<std::size_t>( needed ), L'\0' );
        ::MultiByteToWideChar( CP_UTF8, 0, inText.c_str(), static_cast<int>( inText.size() ), wide.data(), needed );
        return wide;
    }
#endif

    // The directory the reporter itself sits in - which, by CrashHandler::Install, is the directory
    // the crashed host executable sits in too. That is how "Restart <host>" finds the exe and how the
    // fonts are found: from the layout the handler and the build guarantee, never from a working
    // directory the crashed process happened to have.
    std::filesystem::path ReporterDirectory()
    {
#if defined( _WIN32 )
        wchar_t     buffer[MAX_PATH] = {};
        const DWORD written          = ::GetModuleFileNameW( nullptr, buffer, MAX_PATH );
        if ( written == 0 || written >= MAX_PATH )
        {
            return std::filesystem::path();
        }
        return std::filesystem::path( buffer ).parent_path();
#else
        return std::filesystem::path();
#endif
    }

    ImFont* LoadFont( const std::filesystem::path& inFontRoot, const char* inFile, float inSizePixels,
                      Fonts& ioFonts )
    {
        const std::filesystem::path path = inFontRoot / inFile;
        std::error_code             existsError;
        if ( !std::filesystem::exists( path, existsError ) || existsError )
        {
            ioFonts.missing.push_back( path.string() );
            return nullptr;
        }
        ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF( path.string().c_str(), inSizePixels );
        if ( font == nullptr )
        {
            ioFonts.missing.push_back( path.string() + " (present, but ImGui could not build it)" );
        }
        return font;
    }

    void MergeIcons( const std::filesystem::path& inFontRoot, float inSizePixels, Fonts& ioFonts )
    {
        const std::filesystem::path path = inFontRoot / "fontawesome-webfont.ttf";
        std::error_code             existsError;
        if ( !std::filesystem::exists( path, existsError ) || existsError )
        {
            ioFonts.missing.push_back( path.string() );
            return;
        }
        // FA4 occupies the private-use block F000-F2FF. The range array must outlive the atlas build,
        // hence static: ImGui keeps the pointer rather than copying it.
        static const ImWchar ranges[] = { 0xF000, 0xF2FF, 0 };
        ImFontConfig         config;
        config.MergeMode  = true;
        config.PixelSnapH = true;
        if ( ImGui::GetIO().Fonts->AddFontFromFileTTF( path.string().c_str(), inSizePixels, &config, ranges ) ==
             nullptr )
        {
            ioFonts.missing.push_back( path.string() + " (present, but ImGui could not build it)" );
        }
    }

    Fonts BuildFonts( const std::filesystem::path& inFontRoot, float inScale )
    {
        Fonts fonts;

        fonts.body = LoadFont( inFontRoot, "Roboto-Regular.ttf", 15.0f * inScale, fonts );
        MergeIcons( inFontRoot, 14.0f * inScale, fonts );
        fonts.title = LoadFont( inFontRoot, "Roboto-Bold.ttf", 21.0f * inScale, fonts );
        MergeIcons( inFontRoot, 19.0f * inScale, fonts );
        fonts.mono = LoadFont( inFontRoot, "RobotoMono-Regular.ttf", 13.0f * inScale, fonts );

        // The default face is added ONLY when nothing else loaded, so ImGui still has something to
        // draw the "fonts are missing" message with. The window names the missing files either way.
        if ( fonts.body == nullptr )
        {
            ImGui::GetIO().Fonts->AddFontDefault();
        }
        return fonts;
    }

    void ApplyStyle( float inScale )
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowRounding    = 0.0f;
        style.ChildRounding     = 3.0f;
        style.FrameRounding     = 3.0f;
        style.GrabRounding      = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.WindowBorderSize  = 0.0f;
        style.ChildBorderSize   = 0.0f;
        style.FrameBorderSize   = 1.0f;
        style.WindowPadding     = ImVec2( 0.0f, 0.0f );
        style.FramePadding      = ImVec2( 8.0f, 5.0f );
        style.ItemSpacing       = ImVec2( 8.0f, 6.0f );
        style.CellPadding       = ImVec2( 10.0f, 5.0f );

        ImVec4* colors                        = style.Colors;
        colors[ImGuiCol_WindowBg]             = ToVec4( kColWindow );
        colors[ImGuiCol_ChildBg]              = ToVec4( kColWindow );
        colors[ImGuiCol_PopupBg]              = ToVec4( kColBand );
        colors[ImGuiCol_Border]               = ToVec4( kColRule );
        colors[ImGuiCol_Text]                 = ToVec4( kColText );
        colors[ImGuiCol_TextDisabled]         = ToVec4( kColMuted );
        colors[ImGuiCol_FrameBg]              = ToVec4( kColSunk );
        colors[ImGuiCol_FrameBgHovered]       = ToVec4( kColRule );
        colors[ImGuiCol_FrameBgActive]        = ToVec4( kColRule );
        colors[ImGuiCol_Header]               = ToVec4( kColCardHeader );
        colors[ImGuiCol_HeaderHovered]        = ToVec4( kColSelected );
        colors[ImGuiCol_HeaderActive]         = ToVec4( kColSelected );
        colors[ImGuiCol_Button]               = ToVec4( kColCardHeader );
        colors[ImGuiCol_ButtonHovered]        = ImVec4( 0.26f, 0.26f, 0.26f, 1.0f );
        colors[ImGuiCol_ButtonActive]         = ImVec4( 0.32f, 0.32f, 0.32f, 1.0f );
        colors[ImGuiCol_ScrollbarBg]          = ToVec4( kColSunk );
        colors[ImGuiCol_ScrollbarGrab]        = ToVec4( kColScrollGrab );
        colors[ImGuiCol_ScrollbarGrabHovered] = ToVec4( kColScrollGrab );
        colors[ImGuiCol_ScrollbarGrabActive]  = ToVec4( kColAccent );
        colors[ImGuiCol_Separator]            = ToVec4( kColRule );
        colors[ImGuiCol_TableBorderStrong]    = ToVec4( kColRule );
        colors[ImGuiCol_TableBorderLight]     = ToVec4( kColRule );
        colors[ImGuiCol_NavHighlight]         = ToVec4( kColAccent );

        style.ScaleAllSizes( inScale );
    }

    void OpenReportFolder( const std::filesystem::path& inDirectory, std::string& outStatus )
    {
#if defined( _WIN32 )
        const std::wstring wide = ToWide( inDirectory.string() );
        const HINSTANCE    result =
             ::ShellExecuteW( nullptr, L"explore", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
        // ShellExecuteW returns a value <= 32 as its error code; it is not a handle.
        const INT_PTR code = reinterpret_cast<INT_PTR>( result );
        if ( code <= 32 )
        {
            outStatus = "could not open the folder (ShellExecute error " + std::to_string( code ) +
                        "): " + inDirectory.string();
        }
        else
        {
            outStatus = "opened " + inDirectory.string();
        }
#else
        outStatus = "opening a folder is implemented for Windows only; the report is at " + inDirectory.string();
#endif
    }

    void RestartHost( const std::filesystem::path& inExecutable, std::string& outStatus )
    {
#if defined( _WIN32 )
        std::wstring command = L"\"" + ToWide( inExecutable.string() ) + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof( startup );
        PROCESS_INFORMATION process{};
        if ( ::CreateProcessW( nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               ToWide( inExecutable.parent_path().string() ).c_str(), &startup, &process ) == FALSE )
        {
            outStatus = "could not restart " + inExecutable.filename().string() + " (CreateProcess error " +
                        std::to_string( ::GetLastError() ) + "): " + inExecutable.string();
            return;
        }
        ::CloseHandle( process.hThread );
        ::CloseHandle( process.hProcess );
        outStatus = "restarted " + inExecutable.filename().string();
#else
        outStatus = "restarting the host is implemented for Windows only: " + inExecutable.string();
#endif
    }

    // THE ONE FILE THIS TOOL WRITES, and it goes into the report directory beside the evidence it
    // belongs to. Nothing is written when the box is empty, so an untouched dialog leaves no trace.
    void WriteComment( const std::filesystem::path& inDirectory, const char* inText, std::string& outStatus )
    {
        if ( inDirectory.empty() || inText == nullptr || inText[0] == '\0' )
        {
            return;
        }
        const std::filesystem::path file = inDirectory / "comment.txt";
        std::ofstream               output( file, std::ios::binary | std::ios::trunc );
        if ( !output )
        {
            outStatus = "could not write the comment: " + file.string();
            return;
        }
        output << inText;
        outStatus = "saved " + file.string();
    }

    void GlfwErrorCallback( int inCode, const char* inDescription )
    {
        std::fprintf( stderr, "[CrashReporter] GLFW error %d: %s\n", inCode, inDescription );
    }

    // A filled band across the full width of the window, used for the header and the footer.
    void BeginBand( const char* inId, float inHeight, ImU32 inColor )
    {
        ImGui::PushStyleColor( ImGuiCol_ChildBg, ToVec4( inColor ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 0.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 16.0f, 10.0f ) );
        ImGui::BeginChild( inId, ImVec2( 0.0f, inHeight ), false );
    }

    void EndBand()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );
        ImGui::PopStyleColor();
    }

    void CardHeader( const char* inLabel )
    {
        const float  height = ImGui::GetTextLineHeight() + ( ImGui::GetStyle().FramePadding.y * 2.0f );
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddRectFilled( start, ImVec2( start.x + width, start.y + height ),
                                                   kColCardHeader, 3.0f );
        ImGui::SetCursorScreenPos( ImVec2( start.x + 10.0f, start.y + ImGui::GetStyle().FramePadding.y ) );
        ImGui::TextUnformatted( inLabel );
        ImGui::SetCursorScreenPos( ImVec2( start.x, start.y + height + 5.0f ) );
    }

    void SummaryRow( const char* inLabel, const std::string& inValue )
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex( 0 );
        ImGui::TextColored( ToVec4( kColMuted ), "%s", inLabel );
        ImGui::TableSetColumnIndex( 1 );
        ImGui::TextUnformatted( inValue.empty() ? "(absent)" : inValue.c_str() );
    }

    void DrawSummaryCard( const CrashReporter::Report& inReport, float inScale )
    {
        CardHeader( "Summary" );
        if ( !ImGui::BeginTable( "summary", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV ) )
        {
            return;
        }
        ImGui::TableSetupColumn( "key", ImGuiTableColumnFlags_WidthFixed, 120.0f * inScale );
        ImGui::TableSetupColumn( "value", ImGuiTableColumnFlags_WidthStretch );

        SummaryRow( "Kind", inReport.codename + "   [" + inReport.kind + "]" );
        SummaryRow( "Code", inReport.code + "   at " + inReport.address );
        SummaryRow( "Module", inReport.module + " + " + inReport.moduleOffset );
        SummaryRow( "Build", inReport.version + "   sha " + inReport.sha + "   branch " + inReport.branch +
                                  ( inReport.dirty == "1" ? "   (dirty tree)" : "" ) );
        SummaryRow( "Time", CrashReporter::FormatCrashTime( inReport ) + "   (started " + inReport.started + ")" );
        SummaryRow( "Machine", inReport.machine + "   -   " + inReport.os );
        SummaryRow( "GPU", inReport.gpu );
        SummaryRow( "Scene", inReport.scene );

        ImGui::EndTable();
    }

    void DrawLog( const CrashReporter::Report& inReport, const Fonts& inFonts, float inScale )
    {
        const std::string header = "Log tail (" + std::to_string( inReport.log.size() ) + " lines)";
        if ( !ImGui::CollapsingHeader( header.c_str() ) )
        {
            return;
        }
        ImGui::PushStyleColor( ImGuiCol_ChildBg, ToVec4( kColSunk ) );
        if ( ImGui::BeginChild( "log", ImVec2( 0.0f, 120.0f * inScale ), false,
                                ImGuiWindowFlags_HorizontalScrollbar ) )
        {
            if ( inFonts.mono != nullptr )
            {
                ImGui::PushFont( inFonts.mono );
            }
            for ( const std::string& line : inReport.log )
            {
                ImGui::TextUnformatted( line.c_str() );
            }
            if ( inFonts.mono != nullptr )
            {
                ImGui::PopFont();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
} // namespace

int main( int inArgc, char** inArgv )
{
    const std::filesystem::path reportDirectory =
         ( inArgc > 1 ) ? std::filesystem::path( inArgv[1] ) : std::filesystem::path();
    const CrashReporter::Report report = CrashReporter::LoadReport( reportDirectory );

    const std::filesystem::path besideUs = ReporterDirectory();
    const std::filesystem::path hostExecutable =
         ( report.valid && !report.host.empty() && !besideUs.empty() ) ? besideUs / ( report.host + ".exe" )
                                                                      : std::filesystem::path();
    std::error_code hostExistsError;
    const bool      canRestartHost =
         !hostExecutable.empty() && std::filesystem::exists( hostExecutable, hostExistsError ) && !hostExistsError;

    glfwSetErrorCallback( &GlfwErrorCallback );
    if ( glfwInit() != GLFW_TRUE )
    {
        std::fprintf( stderr, "[CrashReporter] glfwInit failed; the report is at %s\n", report.sourcePath.c_str() );
        return 2;
    }

    // DPI: the whole UI is authored at 1.0 and scaled once, so a 150% monitor gets bigger glyphs
    // rather than a blurry upscale of small ones.
    float        dpiScale = 1.0f;
    GLFWmonitor* monitor  = glfwGetPrimaryMonitor();
    if ( monitor != nullptr )
    {
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetMonitorContentScale( monitor, &scaleX, &scaleY );
        if ( scaleX > 0.0f )
        {
            dpiScale = scaleX;
        }
    }

    GLFWwindow* window = glfwCreateWindow( static_cast<int>( 900.0f * dpiScale ),
                                           static_cast<int>( 620.0f * dpiScale ), kWindowTitle, nullptr, nullptr );
    if ( window == nullptr )
    {
        std::fprintf( stderr, "[CrashReporter] could not create a window; the report is at %s\n",
                      report.sourcePath.c_str() );
        glfwTerminate();
        return 3;
    }
    glfwMakeContextCurrent( window );
    glfwSwapInterval( 1 );

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // NO imgui.ini, AND NO OTHER FILE EITHER except the comment the owner types. A crash handler that
    // leaves litter in the working directory is a second problem to debug.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ApplyStyle( dpiScale );
    const Fonts fonts = BuildFonts( besideUs / "Resources" / "Fonts", dpiScale );

    ImGui_ImplGlfw_InitForOpenGL( window, true );
    ImGui_ImplOpenGL3_Init( "#version 130" );

    std::string status;
    char        comment[2048] = {};
    bool        commentSaved  = false;

    while ( glfwWindowShouldClose( window ) == 0 )
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos( viewport->WorkPos );
        ImGui::SetNextWindowSize( viewport->WorkSize );
        ImGui::Begin( kWindowTitle, nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoBringToFrontOnFocus );

        // ── Header band ─────────────────────────────────────────────────────────────────────────
        BeginBand( "header", 76.0f * dpiScale, kColBand );
        {
            if ( fonts.title != nullptr )
            {
                ImGui::PushFont( fonts.title );
            }
            ImGui::TextColored( ToVec4( kColError ), "%s", kIconWarning );
            ImGui::SameLine();
            const std::string headline =
                 report.valid
                      ? ( "DesertEngine " + ( report.host.empty() ? std::string( "host" ) : report.host ) +
                          " crashed" )
                      : std::string( "This crash report could not be read" );
            ImGui::TextUnformatted( headline.c_str() );
            if ( fonts.title != nullptr )
            {
                ImGui::PopFont();
            }

            if ( report.valid )
            {
                const std::string subtitle =
                     report.codename + " - report saved" +
                     ( report.complete ? "" : "   (TRUNCATED - the process died before it finished writing)" );
                ImGui::TextColored( ToVec4( report.complete ? kColMuted : kColError ), "%s", subtitle.c_str() );
            }
            else
            {
                ImGui::TextColored( ToVec4( kColError ), "%s", report.error.c_str() );
            }
        }
        EndBand();

        // A missing font is a defect of this tool, and it is named here rather than hidden behind a
        // default face that merely looks wrong.
        if ( !fonts.missing.empty() )
        {
            const float lines = static_cast<float>( fonts.missing.size() ) + 1.0f;
            BeginBand( "fontwarning", ( 20.0f * dpiScale ) + ( lines * ImGui::GetTextLineHeightWithSpacing() ),
                       kColBand );
            ImGui::TextColored( ToVec4( kColError ),
                                "%s  fonts could not be loaded - this window is not styled as intended:",
                                kIconBug );
            for ( const std::string& missing : fonts.missing )
            {
                ImGui::TextColored( ToVec4( kColMuted ), "%s", missing.c_str() );
            }
            EndBand();
        }

        // ── Body ────────────────────────────────────────────────────────────────────────────────
        const float footerHeight = 54.0f * dpiScale;
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 16.0f, 12.0f ) );
        ImGui::BeginChild( "body", ImVec2( 0.0f, ImGui::GetContentRegionAvail().y - footerHeight ), false );
        if ( report.valid )
        {
            DrawSummaryCard( report, dpiScale );
            ImGui::Dummy( ImVec2( 0.0f, 8.0f ) );
            DrawLog( report, fonts, dpiScale );
            ImGui::Dummy( ImVec2( 0.0f, 8.0f ) );
            if ( !report.error.empty() )
            {
                ImGui::TextColored( ToVec4( kColError ), "%s", report.error.c_str() );
            }
            CardHeader( "What were you doing?" );
            ImGui::PushStyleColor( ImGuiCol_FrameBg, ToVec4( kColSunk ) );
            ImGui::InputTextMultiline( "##comment", comment, sizeof( comment ),
                                       ImVec2( -1.0f, 64.0f * dpiScale ) );
            ImGui::PopStyleColor();
            ImGui::TextColored( ToVec4( kColMuted ),
                                "Saved as comment.txt in the report folder when you close or restart. The full "
                                "report, call stack included, is on the clipboard after Copy report." );
        }
        else
        {
            ImGui::TextColored( ToVec4( kColMuted ), "Path tried:" );
            ImGui::TextWrapped( "%s", report.sourcePath.empty() ? "(none - no directory was given)"
                                                                : report.sourcePath.c_str() );
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // ── Footer band ─────────────────────────────────────────────────────────────────────────
        BeginBand( "footer", footerHeight, kColBand );
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored( ToVec4( kColAccent ), "%s",
                                report.sourcePath.empty() ? "(no report path)" : report.sourcePath.c_str() );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !reportDirectory.empty() )
                {
                    OpenReportFolder( reportDirectory, status );
                }
            }

            const std::string copyLabel   = std::string( kIconCopy ) + "  Copy report";
            const std::string folderLabel = std::string( kIconFolder ) + "  Open folder";
            const std::string restartLabel =
                 std::string( kIconRedo ) + "  Restart " +
                 ( report.host.empty() ? std::string( "host" ) : report.host );
            const float buttonHeight = ImGui::GetFrameHeight();
            const float widths[4]    = { ImGui::CalcTextSize( copyLabel.c_str() ).x + 22.0f,
                                         ImGui::CalcTextSize( folderLabel.c_str() ).x + 22.0f,
                                         ImGui::CalcTextSize( restartLabel.c_str() ).x + 22.0f,
                                         ImGui::CalcTextSize( "Close" ).x + 26.0f };
            float       total        = ImGui::GetStyle().ItemSpacing.x * 3.0f;
            for ( const float width : widths )
            {
                total += width;
            }
            ImGui::SameLine( ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total );

            if ( ImGui::Button( copyLabel.c_str(), ImVec2( widths[0], buttonHeight ) ) )
            {
                // The WHOLE report file, [stack] section included - this is the only route the stack
                // has out of the UI now that the stack card is gone.
                ImGui::SetClipboardText( report.valid ? report.rawText.c_str() : report.error.c_str() );
                status = "copied the full report (" + std::to_string( report.rawText.size() ) +
                         " bytes, stack included) to the clipboard";
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( reportDirectory.empty() );
            if ( ImGui::Button( folderLabel.c_str(), ImVec2( widths[1], buttonHeight ) ) )
            {
                OpenReportFolder( reportDirectory, status );
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            ImGui::PushStyleColor( ImGuiCol_Button, ToVec4( kColAccent ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.05f, 0.50f, 0.92f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.00f, 0.36f, 0.72f, 1.0f ) );
            ImGui::BeginDisabled( !canRestartHost );
            if ( ImGui::Button( restartLabel.c_str(), ImVec2( widths[2], buttonHeight ) ) )
            {
                WriteComment( reportDirectory, comment, status );
                commentSaved = true;
                RestartHost( hostExecutable, status );
                glfwSetWindowShouldClose( window, GLFW_TRUE );
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor( 3 );
            if ( !canRestartHost && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            {
                ImGui::SetTooltip( "%s", hostExecutable.empty()
                                              ? "the report does not name a host executable"
                                              : ( "not found: " + hostExecutable.string() ).c_str() );
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Close", ImVec2( widths[3], buttonHeight ) ) )
            {
                WriteComment( reportDirectory, comment, status );
                commentSaved = true;
                glfwSetWindowShouldClose( window, GLFW_TRUE );
            }

            if ( !status.empty() )
            {
                ImGui::TextColored( ToVec4( kColMuted ), "%s", status.c_str() );
            }
        }
        EndBand();

        ImGui::End();
        ImGui::Render();

        int width  = 0;
        int height = 0;
        glfwGetFramebufferSize( window, &width, &height );
        glViewport( 0, 0, width, height );
        const ImVec4 clear = ToVec4( kColWindow );
        glClearColor( clear.x, clear.y, clear.z, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
        glfwSwapBuffers( window );
    }

    // The window frame's own close button bypasses the footer, so the comment is still saved here.
    if ( !commentSaved )
    {
        WriteComment( reportDirectory, comment, status );
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow( window );
    glfwTerminate();
    return report.valid ? 0 : 1;
}
