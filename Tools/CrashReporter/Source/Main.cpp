// DesertCrashReporter - the window Common::Crash::LaunchReporter starts after a crash.
//
// WHY IT LINKS NO ENGINE. It is started BY a process that has just died, to explain why. Anything it
// depends on is a thing that can be broken at the moment it is needed, so its whole dependency set is
// GLFW, Dear ImGui and the Win32 shell - no Desert, no Vulkan, no asset system.
//
// This file is UI only. Every line that reads or interprets the report lives in CrashReport.cpp, so
// the move to our own UI framework replaces this file and keeps that one. The one thing that is not
// portable - making a strip of client area behave like a caption - lives in NativeFrame.cpp.
//
// THE LOOK IS DESERT'S OWN: warm, sand-toned dark, not a copy of anyone else's crash dialog. The
// palette below is the single source of those colours; nothing in this file hard-codes one inline.
//
// THE CALL STACK IS NOT SHOWN IN THIS WINDOW, BY DECISION. It is still collected, still written to
// crash.txt and the minidump, and still carried out of the UI by "Copy report", which puts the whole
// report file - [stack] section included - on the clipboard.

#include "../Resources/Icon/AppIcon.gen.hpp"
#include "CrashReport.hpp"
#include "NativeFrame.hpp"

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
    constexpr const char* kBarTitle    = "DesertEngine \xe2\x80\x94 Crash Report";

    // ── The Desert palette: warm, low-chroma sand over a near-black brown ────────────────────────
    constexpr ImU32 kColBase        = IM_COL32( 0x1B, 0x19, 0x16, 0xFF );
    constexpr ImU32 kColSurface     = IM_COL32( 0x24, 0x21, 0x1D, 0xFF );
    constexpr ImU32 kColSurfaceEdge = IM_COL32( 0x33, 0x2E, 0x28, 0xFF );
    constexpr ImU32 kColSunk        = IM_COL32( 0x14, 0x12, 0x10, 0xFF );
    constexpr ImU32 kColTitleBar    = IM_COL32( 0x15, 0x13, 0x11, 0xFF );
    constexpr ImU32 kColHeaderTop   = IM_COL32( 0x2A, 0x24, 0x1D, 0xFF );
    constexpr ImU32 kColText        = IM_COL32( 0xE8, 0xE2, 0xD8, 0xFF );
    constexpr ImU32 kColMuted       = IM_COL32( 0x9A, 0x91, 0x87, 0xFF );
    constexpr ImU32 kColAccent      = IM_COL32( 0xE0, 0xA4, 0x58, 0xFF );
    constexpr ImU32 kColAccentHover = IM_COL32( 0xED, 0xB8, 0x72, 0xFF );
    constexpr ImU32 kColDanger      = IM_COL32( 0xD9, 0x54, 0x4D, 0xFF );
    constexpr ImU32 kColBadgeBg     = IM_COL32( 0x3A, 0x1F, 0x1C, 0xFF );
    constexpr ImU32 kColBadgeText   = IM_COL32( 0xF0, 0x8A, 0x80, 0xFF );
    constexpr ImU32 kColGhostEdge   = IM_COL32( 0x3A, 0x34, 0x2C, 0xFF );
    constexpr ImU32 kColHover       = IM_COL32( 0x2E, 0x2A, 0x24, 0xFF );

    // ── Metrics, authored at 100% and multiplied by the monitor's content scale ──────────────────
    constexpr float kTitleBarHeight   = 36.0f;
    constexpr float kWindowButtonSize = 46.0f;
    constexpr float kHeaderHeight     = 106.0f;
    constexpr float kFooterHeight     = 64.0f;
    constexpr float kPadX             = 24.0f;
    constexpr float kPadY             = 16.0f;
    constexpr float kCardPad          = 16.0f;
    constexpr float kRowGap           = 10.0f;
    constexpr float kCardRounding     = 8.0f;
    constexpr float kResizeBorder     = 6.0f;

    // FontAwesome 4 codepoints, spelled as UTF-8 so no extra header is needed.
    // fontawesome-webfont.ttf is the FA4 face in Editor/Resources/Fonts.
    constexpr const char* kIconBug      = "\xef\x86\x88"; // f188 bug
    constexpr const char* kIconInfo     = "\xef\x81\x9a"; // f05a info-circle
    constexpr const char* kIconComment  = "\xef\x81\xb5"; // f075 comment
    constexpr const char* kIconLog      = "\xef\x83\xb6"; // f0f6 file-text-o
    constexpr const char* kIconCopy     = "\xef\x83\x85"; // f0c5 copy
    constexpr const char* kIconFolder   = "\xef\x81\xbb"; // f07b folder
    constexpr const char* kIconRedo     = "\xef\x80\x9e"; // f01e repeat
    constexpr const char* kIconMinimise = "\xef\x81\xa8"; // f068 minus
    constexpr const char* kIconClose    = "\xef\x80\x8d"; // f00d times

    struct IconGlyph
    {
        const char* utf8      = nullptr;
        ImWchar     codepoint = 0;
        const char* name      = nullptr;
    };

    // EVERY icon this window draws, paired with the codepoint it is spelled as. The face beside the
    // exe is one specific file; a codepoint it does not carry draws as tofu, and tofu is a defect
    // this tool names rather than ships. BuildFonts checks the list against the built atlas.
    constexpr IconGlyph kIcons[] = {
         { kIconBug, 0xF188, "bug" },         { kIconInfo, 0xF05A, "info-circle" },
         { kIconComment, 0xF075, "comment" }, { kIconLog, 0xF0F6, "file-text-o" },
         { kIconCopy, 0xF0C5, "files-o" },    { kIconFolder, 0xF07B, "folder" },
         { kIconRedo, 0xF01E, "repeat" },     { kIconMinimise, 0xF068, "minus" },
         { kIconClose, 0xF00D, "times" },
    };

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

    ImVec2 Add( const ImVec2& inLeft, const ImVec2& inRight )
    {
        return ImVec2( inLeft.x + inRight.x, inLeft.y + inRight.y );
    }

    std::string Upper( const std::string& inText )
    {
        std::string result = inText;
        for ( char& character : result )
        {
            if ( character >= 'a' && character <= 'z' )
            {
                character = static_cast<char>( character - ( 'a' - 'A' ) );
            }
        }
        return result;
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
        ::MultiByteToWideChar( CP_UTF8, 0, inText.c_str(), static_cast<int>( inText.size() ), wide.data(),
                               needed );
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

    // Latin-1 plus the General Punctuation block, because the window's own title carries an em dash
    // and ImGui's default range stops at U+00FF - an unlisted glyph draws as a box, not as nothing.
    const ImWchar* TextRanges()
    {
        // ImGui keeps the pointer rather than copying it, so this must outlive the atlas build.
        static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0 };
        return ranges;
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
        ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF( path.string().c_str(), inSizePixels, nullptr,
                                                                 TextRanges() );
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
        fonts.title = LoadFont( inFontRoot, "Roboto-Bold.ttf", 22.0f * inScale, fonts );
        MergeIcons( inFontRoot, 19.0f * inScale, fonts );
        fonts.mono = LoadFont( inFontRoot, "RobotoMono-Regular.ttf", 13.0f * inScale, fonts );

        // The default face is added ONLY when nothing else loaded, so ImGui still has something to
        // draw the "fonts are missing" message with. The window names the missing files either way.
        if ( fonts.body == nullptr )
        {
            ImGui::GetIO().Fonts->AddFontDefault();
            return fonts;
        }

        // The atlas has to exist before a glyph can be looked up; ImGui builds it lazily on the
        // first texture fetch, so ask for it here rather than after the first frame has already
        // drawn a row of boxes.
        ImGui::GetIO().Fonts->Build();
        for ( const IconGlyph& icon : kIcons )
        {
            if ( fonts.body->FindGlyphNoFallback( icon.codepoint ) == nullptr )
            {
                char text[160] = {};
                std::snprintf( text, sizeof( text ), "icon %s (U+%04X) is not in fontawesome-webfont.ttf",
                               icon.name, static_cast<unsigned int>( icon.codepoint ) );
                fonts.missing.push_back( text );
            }
        }
        return fonts;
    }

    void ApplyStyle( float inScale )
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowRounding    = 0.0f;
        style.ChildRounding     = kCardRounding;
        style.FrameRounding     = 6.0f;
        style.GrabRounding      = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.TabRounding       = 6.0f;
        style.TabBorderSize     = 0.0f;
        style.ScrollbarSize     = 10.0f;
        style.WindowBorderSize  = 0.0f;
        style.ChildBorderSize   = 0.0f;
        style.FrameBorderSize   = 0.0f;
        style.WindowPadding     = ImVec2( 0.0f, 0.0f );
        style.FramePadding      = ImVec2( 12.0f, 7.0f );
        style.ItemSpacing       = ImVec2( 8.0f, kRowGap );
        style.ItemInnerSpacing  = ImVec2( 8.0f, 4.0f );
        style.CellPadding       = ImVec2( 0.0f, 2.0f );

        ImVec4* colors                        = style.Colors;
        colors[ImGuiCol_WindowBg]             = ToVec4( kColBase );
        colors[ImGuiCol_ChildBg]              = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );
        colors[ImGuiCol_PopupBg]              = ToVec4( kColSurface );
        colors[ImGuiCol_Border]               = ToVec4( kColSurfaceEdge );
        colors[ImGuiCol_Text]                 = ToVec4( kColText );
        colors[ImGuiCol_TextDisabled]         = ToVec4( kColMuted );
        colors[ImGuiCol_FrameBg]              = ToVec4( kColSunk );
        colors[ImGuiCol_FrameBgHovered]       = ToVec4( kColSunk );
        colors[ImGuiCol_FrameBgActive]        = ToVec4( kColSunk );
        colors[ImGuiCol_Header]               = ToVec4( kColSunk );
        colors[ImGuiCol_HeaderHovered]        = ToVec4( kColHover );
        colors[ImGuiCol_HeaderActive]         = ToVec4( kColHover );
        colors[ImGuiCol_Button]               = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );
        colors[ImGuiCol_ButtonHovered]        = ToVec4( kColHover );
        colors[ImGuiCol_ButtonActive]         = ToVec4( kColSurfaceEdge );
        colors[ImGuiCol_ScrollbarBg]          = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );
        colors[ImGuiCol_ScrollbarGrab]        = ToVec4( kColGhostEdge );
        colors[ImGuiCol_ScrollbarGrabHovered] = ToVec4( kColSurfaceEdge );
        colors[ImGuiCol_ScrollbarGrabActive]  = ToVec4( kColAccent );
        colors[ImGuiCol_Tab]                  = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );
        colors[ImGuiCol_TabHovered]           = ToVec4( kColHover );
        colors[ImGuiCol_TabActive]            = ToVec4( kColSurface );
        colors[ImGuiCol_TabUnfocused]         = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );
        colors[ImGuiCol_TabUnfocusedActive]   = ToVec4( kColSurface );
        colors[ImGuiCol_Separator]            = ToVec4( kColSurfaceEdge );
        colors[ImGuiCol_TableBorderStrong]    = ToVec4( kColSurfaceEdge );
        colors[ImGuiCol_TableBorderLight]     = ToVec4( kColSurfaceEdge );
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
                               ToWide( inExecutable.parent_path().string() ).c_str(), &startup,
                               &process ) == FALSE )
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

    // ── Primitives ───────────────────────────────────────────────────────────────────────────────

    // A band across the full width of the window. The fill goes on the PARENT draw list before the
    // child opens, which is what lets the header carry a gradient instead of a flat ChildBg.
    void BeginBand( const char* inId, float inHeight, ImU32 inTop, ImU32 inBottom, const ImVec2& inPadding )
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float  width = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor( start, ImVec2( start.x + width, start.y + inHeight ),
                                                             inTop, inTop, inBottom, inBottom );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 0.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, inPadding );
        ImGui::BeginChild( inId, ImVec2( 0.0f, inHeight ), false, ImGuiWindowFlags_AlwaysUseWindowPadding );
    }

    void EndBand()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );
        // Bands are edges of the window, not rows in a list: the item spacing ImGui adds after a
        // child would leave a base-coloured seam between the header and the body.
        ImGui::SetCursorPosY( ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y );
    }

    // A rounded surface that sizes itself to whatever is drawn between Begin and End. ImGui 1.89 has
    // no auto-height child, so the background is deferred to a second draw channel and emitted once
    // the content height is known.
    class Card
    {
    public:
        void Begin( float inScale )
        {
            mPad   = kCardPad * inScale;
            mStart = ImGui::GetCursorScreenPos();
            mWidth = ImGui::GetContentRegionAvail().x;
            mSplitter.Split( ImGui::GetWindowDrawList(), 2 );
            mSplitter.SetCurrentChannel( ImGui::GetWindowDrawList(), 1 );
            ImGui::Dummy( ImVec2( 0.0f, mPad - ImGui::GetStyle().ItemSpacing.y ) );
            ImGui::Indent( mPad );
            ImGui::BeginGroup();
        }

        void End()
        {
            ImGui::EndGroup();
            ImGui::Unindent( mPad );
            ImGui::Dummy( ImVec2( 0.0f, mPad - ImGui::GetStyle().ItemSpacing.y ) );

            ImDrawList*  drawList = ImGui::GetWindowDrawList();
            const float  height   = ImGui::GetCursorScreenPos().y - mStart.y;
            const ImVec2 end      = ImVec2( mStart.x + mWidth, mStart.y + height );
            mSplitter.SetCurrentChannel( drawList, 0 );
            drawList->AddRectFilled( mStart, end, kColSurface, kCardRounding );
            drawList->AddRect( mStart, end, kColSurfaceEdge, kCardRounding, 0, 1.0f );
            mSplitter.Merge( drawList );
        }

        // The width available to content: ImGui's own GetContentRegionAvail is short by one padding
        // after Indent, so full-width items take this instead of -1.
        float InnerWidth() const
        {
            return mWidth - ( mPad * 2.0f );
        }

    private:
        ImDrawListSplitter mSplitter;
        ImVec2             mStart{};
        float              mWidth = 0.0f;
        float              mPad   = 0.0f;
    };

    void SectionTitle( const char* inIcon, const char* inLabel )
    {
        ImGui::TextColored( ToVec4( kColAccent ), "%s", inIcon );
        ImGui::SameLine( 0.0f, 8.0f );
        ImGui::TextUnformatted( inLabel );
    }

    void Badge( const std::string& inText, float inScale )
    {
        ImDrawList*  drawList = ImGui::GetWindowDrawList();
        const ImVec2 start    = ImGui::GetCursorScreenPos();
        const ImVec2 text     = ImGui::CalcTextSize( inText.c_str() );
        const ImVec2 padding( 9.0f * inScale, 4.0f * inScale );
        const ImVec2 size( text.x + ( padding.x * 2.0f ), text.y + ( padding.y * 2.0f ) );
        drawList->AddRectFilled( start, Add( start, size ), kColBadgeBg, 6.0f * inScale );
        drawList->AddText( Add( start, padding ), kColBadgeText, inText.c_str() );
        ImGui::Dummy( size );
    }

    // A button with no fill and a hairline border - the secondary weight in the footer.
    bool GhostButton( const char* inLabel, const ImVec2& inSize )
    {
        ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.0f );
        ImGui::PushStyleColor( ImGuiCol_Border, ToVec4( kColGhostEdge ) );
        const bool pressed = ImGui::Button( inLabel, inSize );
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return pressed;
    }

    // Amber fill with the base colour for text: the one call to action in the window.
    bool PrimaryButton( const char* inLabel, const ImVec2& inSize )
    {
        ImGui::PushStyleColor( ImGuiCol_Button, ToVec4( kColAccent ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ToVec4( kColAccentHover ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ToVec4( kColAccent ) );
        ImGui::PushStyleColor( ImGuiCol_Text, ToVec4( kColBase ) );
        const bool pressed = ImGui::Button( inLabel, inSize );
        ImGui::PopStyleColor( 4 );
        return pressed;
    }

    // ── Sections ─────────────────────────────────────────────────────────────────────────────────

    // Our own title bar. The hit-test that makes it drag lives in NativeFrame; everything drawn here
    // is plain ImGui and stays as it is on any platform that gains that hit-test.
    void DrawTitleBar( float inScale, GLFWwindow* inWindow, bool& outClose )
    {
        const float height   = kTitleBarHeight * inScale;
        const float buttonW  = kWindowButtonSize * inScale;
        const float lineHigh = ImGui::GetTextLineHeight();

        BeginBand( "titlebar", height, kColTitleBar, kColTitleBar, ImVec2( 0.0f, 0.0f ) );
        ImDrawList*  drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin   = ImGui::GetCursorScreenPos();
        const float  width    = ImGui::GetWindowWidth();
        const float  textY    = origin.y + ( ( height - lineHigh ) * 0.5f );

        drawList->AddText( ImVec2( origin.x + ( 14.0f * inScale ), textY ), kColAccent, kIconBug );
        drawList->AddText( ImVec2( origin.x + ( 38.0f * inScale ), textY ), kColText, kBarTitle );

        struct WindowButton
        {
            const char* id;
            const char* icon;
            ImU32       hoverFill;
        };
        const WindowButton buttons[2] = { { "##minimise", kIconMinimise, kColHover },
                                          { "##close", kIconClose, kColDanger } };

        for ( int index = 0; index < 2; ++index )
        {
            const ImVec2 start( origin.x + width - ( buttonW * static_cast<float>( 2 - index ) ), origin.y );
            ImGui::SetCursorScreenPos( start );
            ImGui::InvisibleButton( buttons[index].id, ImVec2( buttonW, height ) );
            const bool hovered = ImGui::IsItemHovered();
            if ( hovered )
            {
                drawList->AddRectFilled( start, ImVec2( start.x + buttonW, start.y + height ),
                                         buttons[index].hoverFill );
            }
            const ImVec2 iconSize = ImGui::CalcTextSize( buttons[index].icon );
            drawList->AddText( ImVec2( start.x + ( ( buttonW - iconSize.x ) * 0.5f ),
                                       start.y + ( ( height - iconSize.y ) * 0.5f ) ),
                               hovered ? kColText : kColMuted, buttons[index].icon );
            if ( ImGui::IsItemClicked() )
            {
                if ( index == 0 )
                {
                    glfwIconifyWindow( inWindow );
                }
                else
                {
                    outClose = true;
                }
            }
        }

        drawList->AddLine( ImVec2( origin.x, origin.y + height - 1.0f ),
                           ImVec2( origin.x + width, origin.y + height - 1.0f ), kColSurfaceEdge, 1.0f );
        EndBand();
    }

    void DrawHeader( const CrashReporter::Report& inReport, const Fonts& inFonts, float inScale )
    {
        BeginBand( "header", kHeaderHeight * inScale, kColHeaderTop, kColBase,
                   ImVec2( kPadX * inScale, kPadY * inScale ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8.0f, 7.0f * inScale ) );

        const std::string host = inReport.host.empty() ? std::string( "Editor" ) : inReport.host;
        if ( inReport.valid )
        {
            Badge( Upper( inReport.codename.empty() ? inReport.kind : inReport.codename ), inScale );
        }
        else
        {
            Badge( "UNREADABLE REPORT", inScale );
        }

        if ( inFonts.title != nullptr )
        {
            ImGui::PushFont( inFonts.title );
        }
        const std::string headline = inReport.valid ? ( "The " + host + " stopped unexpectedly" )
                                                    : std::string( "This crash report could not be read" );
        ImGui::TextUnformatted( headline.c_str() );
        if ( inFonts.title != nullptr )
        {
            ImGui::PopFont();
        }

        if ( inReport.valid )
        {
            const std::string plain =
                 inReport.codename + " in " + host + ".exe \xe2\x80\x94 a report was saved" +
                 ( inReport.complete ? std::string() : std::string( " (truncated: the process died mid-write)" ) );
            ImGui::TextColored( ToVec4( inReport.complete ? kColMuted : kColDanger ), "%s", plain.c_str() );
        }
        else
        {
            ImGui::TextColored( ToVec4( kColDanger ), "%s", inReport.error.c_str() );
        }
        ImGui::PopStyleVar();
        EndBand();
    }

    void SummaryRow( const char* inLabel, const std::string& inValue, ImFont* inValueFont )
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex( 0 );
        ImGui::TextColored( ToVec4( kColMuted ), "%s", inLabel );
        ImGui::TableSetColumnIndex( 1 );
        if ( inValueFont != nullptr )
        {
            ImGui::PushFont( inValueFont );
        }
        ImGui::TextUnformatted( inValue.empty() ? "(absent)" : inValue.c_str() );
        if ( inValueFont != nullptr )
        {
            ImGui::PopFont();
        }
    }

    void DrawSummaryCard( const CrashReporter::Report& inReport, const Fonts& inFonts, float inScale )
    {
        Card card;
        card.Begin( inScale );
        SectionTitle( kIconInfo, "Summary" );
        ImGui::Dummy( ImVec2( 0.0f, 2.0f * inScale ) );

        if ( ImGui::BeginTable( "summary", 2, ImGuiTableFlags_SizingFixedFit, ImVec2( card.InnerWidth(), 0.0f ) ) )
        {
            ImGui::TableSetupColumn( "key", ImGuiTableColumnFlags_WidthFixed, 96.0f * inScale );
            ImGui::TableSetupColumn( "value", ImGuiTableColumnFlags_WidthStretch );

            SummaryRow( "Kind", inReport.codename + "  [" + inReport.kind + "]", nullptr );
            SummaryRow( "Code", inReport.code + " at " + inReport.address, inFonts.mono );
            SummaryRow( "Module", inReport.module + " + " + inReport.moduleOffset, inFonts.mono );
            SummaryRow( "Version",
                        inReport.version + "  sha " + inReport.sha + "  branch " + inReport.branch +
                             ( inReport.dirty == "1" ? "  (dirty tree)" : "" ),
                        inFonts.mono );
            SummaryRow( "Time",
                        CrashReporter::FormatCrashTime( inReport ) + "  (started " + inReport.started + ")",
                        nullptr );
            SummaryRow( "Machine", inReport.machine + "  -  " + inReport.os, nullptr );
            SummaryRow( "GPU", inReport.gpu, nullptr );
            SummaryRow( "Scene", inReport.scene, nullptr );
            ImGui::EndTable();
        }
        card.End();
    }

    // The log fills whatever the tab leaves it and scrolls inside that, so no line of it can ever
    // end up below the bottom of the window where nobody knows to look.
    void DrawLogPanel( const CrashReporter::Report& inReport, const Fonts& inFonts, float inScale,
                       int& ioScrollToEndFrames )
    {
        ImGui::PushStyleColor( ImGuiCol_ChildBg, ToVec4( kColSunk ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, kCardRounding );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildBorderSize, 1.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10.0f * inScale, 8.0f * inScale ) );
        if ( ImGui::BeginChild( "log", ImVec2( 0.0f, 0.0f ), true,
                                ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding ) )
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
            // The tail is the interesting end, so the panel opens on the LAST line. Two frames,
            // because the first one is where ImGui learns the content height the scroll is measured
            // against - one frame lands short on a log longer than the panel.
            if ( ioScrollToEndFrames > 0 && !inReport.log.empty() )
            {
                ImGui::SetScrollHereY( 1.0f );
                --ioScrollToEndFrames;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar( 3 );
        ImGui::PopStyleColor();
    }

    void DrawCommentCard( char* ioComment, std::size_t inCommentSize, float inScale )
    {
        Card card;
        card.Begin( inScale );
        SectionTitle( kIconComment, "What were you doing?" );
        ImGui::Dummy( ImVec2( 0.0f, 2.0f * inScale ) );

        const ImVec2 boxStart = ImGui::GetCursorScreenPos();
        ImGui::InputTextMultiline( "##comment", ioComment, inCommentSize,
                                   ImVec2( card.InnerWidth(), 42.0f * inScale ) );
        if ( ioComment[0] == '\0' )
        {
            // Drawn after the field, so it sits over the sunk fill; it is a hint, never a value - an
            // empty box still writes no comment.txt at all.
            const ImVec2 padding = ImGui::GetStyle().FramePadding;
            ImGui::GetWindowDrawList()->AddText( Add( boxStart, padding ), kColMuted,
                                                 "e.g. I pressed Play right after importing a mesh" );
        }
        card.End();
    }
} // namespace

int main( int inArgc, char** inArgv )
{
    const std::filesystem::path reportDirectory =
         ( inArgc > 1 ) ? std::filesystem::path( inArgv[1] ) : std::filesystem::path();
    const CrashReporter::Report report = CrashReporter::LoadReport( reportDirectory );

    const std::filesystem::path besideUs       = ReporterDirectory();
    const std::filesystem::path hostExecutable = ( report.valid && !report.host.empty() && !besideUs.empty() )
                                                      ? besideUs / ( report.host + ".exe" )
                                                      : std::filesystem::path();
    std::error_code             hostExistsError;
    const bool                  canRestartHost =
         !hostExecutable.empty() && std::filesystem::exists( hostExecutable, hostExistsError ) && !hostExistsError;

    glfwSetErrorCallback( &GlfwErrorCallback );
    if ( glfwInit() != GLFW_TRUE )
    {
        std::fprintf( stderr, "[CrashReporter] glfwInit failed; the report is at %s\n",
                      report.sourcePath.c_str() );
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

    // The window wears its own title bar, so the OS must not add one on top of ours.
    glfwWindowHint( GLFW_DECORATED, GLFW_FALSE );
    GLFWwindow* window = glfwCreateWindow( static_cast<int>( 900.0f * dpiScale ),
                                           static_cast<int>( 620.0f * dpiScale ), kWindowTitle, nullptr, nullptr );
    if ( window == nullptr )
    {
        std::fprintf( stderr, "[CrashReporter] could not create a window; the report is at %s\n",
                      report.sourcePath.c_str() );
        glfwTerminate();
        return 3;
    }
    glfwSetWindowSizeLimits( window, static_cast<int>( 640.0f * dpiScale ), static_cast<int>( 420.0f * dpiScale ),
                             GLFW_DONT_CARE, GLFW_DONT_CARE );
    // The window and taskbar icon comes from the SAME pixels as the .exe icon and the title bar:
    // one source image, one generator, so no copy of it can drift from the others.
    const GLFWimage windowIcons[3] = { { 16, 16, CrashReporter::AppIcon::kPixels16 },
                                       { 32, 32, CrashReporter::AppIcon::kPixels32 },
                                       { 48, 48, CrashReporter::AppIcon::kPixels48 } };
    glfwSetWindowIcon( window, 3, windowIcons );

    glfwMakeContextCurrent( window );
    glfwSwapInterval( 1 );

    const std::string frameProblems = CrashReporter::NativeFrame::Install( window );

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
    char        comment[2048]   = {};
    bool        commentSaved    = false;
    bool        logWasOpen      = false;
    int         logScrollFrames = 0;

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

        bool closeRequested = false;
        DrawTitleBar( dpiScale, window, closeRequested );

        // The OS learns where the caption is from the same numbers that drew it, every frame, so a
        // resize can never leave the drag strip behind the buttons.
        CrashReporter::NativeFrame::CaptionLayout caption;
        caption.captionHeight = kTitleBarHeight * dpiScale;
        caption.buttonsWidth  = kWindowButtonSize * 2.0f * dpiScale;
        caption.borderWidth   = kResizeBorder * dpiScale;
        CrashReporter::NativeFrame::SetLayout( caption );

        DrawHeader( report, fonts, dpiScale );

        // A missing font, or a title bar the OS refused to honour, is a defect of this tool: it is
        // named here rather than hidden behind a window that merely behaves oddly.
        if ( !fonts.missing.empty() || !frameProblems.empty() )
        {
            const float lines =
                 static_cast<float>( fonts.missing.size() ) + ( frameProblems.empty() ? 1.0f : 2.0f );
            BeginBand( "defects", ( 14.0f * dpiScale ) + ( lines * ImGui::GetTextLineHeightWithSpacing() ),
                       kColBadgeBg, kColBadgeBg, ImVec2( kPadX * dpiScale, 8.0f * dpiScale ) );
            if ( !fonts.missing.empty() )
            {
                ImGui::TextColored( ToVec4( kColBadgeText ), "%s  fonts could not be loaded:", kIconBug );
                for ( const std::string& missing : fonts.missing )
                {
                    ImGui::TextColored( ToVec4( kColMuted ), "%s", missing.c_str() );
                }
            }
            if ( !frameProblems.empty() )
            {
                ImGui::TextColored( ToVec4( kColBadgeText ), "%s  %s", kIconBug, frameProblems.c_str() );
            }
            EndBand();
        }

        // ── Body ────────────────────────────────────────────────────────────────────────────────
        const float footerHeight = kFooterHeight * dpiScale;
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( kPadX * dpiScale, 12.0f * dpiScale ) );
        ImGui::BeginChild( "body", ImVec2( 0.0f, ImGui::GetContentRegionAvail().y - footerHeight ), false,
                           ImGuiWindowFlags_AlwaysUseWindowPadding );
        if ( report.valid )
        {
            if ( ImGui::BeginTabBar( "sections" ) )
            {
                const std::string detailsLabel = std::string( kIconInfo ) + "  Details";
                if ( ImGui::BeginTabItem( detailsLabel.c_str() ) )
                {
                    DrawSummaryCard( report, fonts, dpiScale );
                    DrawCommentCard( comment, sizeof( comment ), dpiScale );
                    if ( !report.error.empty() )
                    {
                        ImGui::TextColored( ToVec4( kColDanger ), "%s", report.error.c_str() );
                    }
                    ImGui::EndTabItem();
                }

                const std::string logLabel =
                     std::string( kIconLog ) + "  Log tail (" + std::to_string( report.log.size() ) + ")";
                if ( ImGui::BeginTabItem( logLabel.c_str() ) )
                {
                    if ( !logWasOpen )
                    {
                        logWasOpen      = true;
                        logScrollFrames = 2;
                    }
                    DrawLogPanel( report, fonts, dpiScale, logScrollFrames );
                    ImGui::EndTabItem();
                }
                else
                {
                    logWasOpen = false;
                }
                ImGui::EndTabBar();
            }
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
        {
            const ImVec2 footerStart = ImGui::GetCursorScreenPos();
            const float  footerWidth = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddLine( footerStart, ImVec2( footerStart.x + footerWidth, footerStart.y ),
                                                 kColSurfaceEdge, 1.0f );
        }
        BeginBand( "footer", footerHeight, kColTitleBar, kColTitleBar, ImVec2( kPadX * dpiScale, 0.0f ) );
        {
            const float lineHeight   = ImGui::GetTextLineHeightWithSpacing();
            const float textBlock    = status.empty() ? lineHeight : ( lineHeight * 2.0f );
            const float buttonHeight = ImGui::GetFrameHeight();

            ImGui::SetCursorPosY( ( footerHeight - textBlock ) * 0.5f );
            ImGui::BeginGroup();
            ImGui::TextColored( ToVec4( kColMuted ), "%s",
                                report.sourcePath.empty() ? "(no report path)" : report.sourcePath.c_str() );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !reportDirectory.empty() )
                {
                    OpenReportFolder( reportDirectory, status );
                }
            }
            if ( !status.empty() )
            {
                ImGui::TextColored( ToVec4( kColAccent ), "%s", status.c_str() );
            }
            ImGui::EndGroup();

            const std::string copyLabel    = std::string( kIconCopy ) + "  Copy report";
            const std::string folderLabel  = std::string( kIconFolder ) + "  Open folder";
            const std::string restartLabel = std::string( kIconRedo ) + "  Restart " +
                                             ( report.host.empty() ? std::string( "Editor" ) : report.host );
            const float padding   = ImGui::GetStyle().FramePadding.x * 2.0f;
            const float spacing   = ImGui::GetStyle().ItemSpacing.x;
            const float widths[3] = { ImGui::CalcTextSize( copyLabel.c_str() ).x + padding,
                                      ImGui::CalcTextSize( folderLabel.c_str() ).x + padding,
                                      ImGui::CalcTextSize( restartLabel.c_str() ).x + padding };
            const float total     = widths[0] + widths[1] + widths[2] + ( spacing * 2.0f );

            ImGui::SetCursorPos( ImVec2( ImGui::GetWindowWidth() - ( kPadX * dpiScale ) - total,
                                         ( footerHeight - buttonHeight ) * 0.5f ) );
            if ( GhostButton( copyLabel.c_str(), ImVec2( widths[0], buttonHeight ) ) )
            {
                // The WHOLE report file, [stack] section included - this is the only route the stack
                // has out of the UI now that the stack card is gone.
                ImGui::SetClipboardText( report.valid ? report.rawText.c_str() : report.error.c_str() );
                status = "copied the full report (" + std::to_string( report.rawText.size() ) +
                         " bytes, stack included) to the clipboard";
            }
            ImGui::SameLine();
            ImGui::BeginDisabled( reportDirectory.empty() );
            if ( GhostButton( folderLabel.c_str(), ImVec2( widths[1], buttonHeight ) ) )
            {
                OpenReportFolder( reportDirectory, status );
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            ImGui::BeginDisabled( !canRestartHost );
            if ( PrimaryButton( restartLabel.c_str(), ImVec2( widths[2], buttonHeight ) ) )
            {
                WriteComment( reportDirectory, comment, status );
                commentSaved = true;
                RestartHost( hostExecutable, status );
                closeRequested = true;
            }
            ImGui::EndDisabled();
            if ( !canRestartHost && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            {
                ImGui::SetTooltip( "%s", hostExecutable.empty()
                                              ? "the report does not name a host executable"
                                              : ( "not found: " + hostExecutable.string() ).c_str() );
            }
        }
        EndBand();

        if ( closeRequested )
        {
            WriteComment( reportDirectory, comment, status );
            commentSaved = true;
            glfwSetWindowShouldClose( window, GLFW_TRUE );
        }

        ImGui::End();

        // The 1px outline that replaces the OS frame. On the foreground list so no band can cover it.
        ImGui::GetForegroundDrawList()->AddRect(
             viewport->WorkPos,
             ImVec2( viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y ),
             kColSurfaceEdge, 0.0f, 0, 1.0f );

        ImGui::Render();

        int width  = 0;
        int height = 0;
        glfwGetFramebufferSize( window, &width, &height );
        glViewport( 0, 0, width, height );
        const ImVec4 clear = ToVec4( kColBase );
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
