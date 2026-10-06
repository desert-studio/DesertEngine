// MOCKUP: the "VFX System" document window for a .dfx asset (VFX port plan §4.1).
//
// Toolbar on top; System + emitter list on the left; the preview view in the centre (a placeholder here -
// the real one is its own SceneRenderer, ClaimsView=true); the selected emitter's module stack on the
// right, every input carrying a mode chip (Value / Curve / Random / Binding); the emitter timeline and the
// curve of the selected input along the bottom.

#include "Mockup.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <array>
#include <cmath>
#include <cstdio>

namespace Desert::UIMockup::Mockups
{
    namespace
    {
        enum class InputMode
        {
            Value,
            Curve,
            Random,
            Binding
        };

        // The chip at the end of a module row: which of the three modes (+ a binding) feeds this input.
        void ModeChip( const char* id, InputMode mode )
        {
            static constexpr std::array<const char*, 4> kLabel = { "Value", "Curve", "Random", "Bind" };
            static constexpr std::array<ImU32, 4>       kTint  = { Rgb( 170, 170, 170 ), Rgb( 110, 190, 120 ),
                                                                   Rgb( 200, 160, 90 ), Rgb( 110, 160, 230 ) };
            const int                                   index  = static_cast<int>( mode );
            char                                        label[64];
            std::snprintf( label, sizeof label, "%s " ICON_MDI_CHEVRON_DOWN "##%s", kLabel[index], id );
            ImGui::PushStyleColor( ImGuiCol_Text, kTint[index] );
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 5.0f, 1.0f ) );
            ImGui::SmallButton( label );
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        // A colour-over-life gradient strip, drawn inline in the value column.
        void GradientStrip( ImVec2 size )
        {
            static constexpr std::array<ImU32, 5> kStops = { Rgb( 255, 236, 180 ), Rgb( 250, 170, 60 ),
                                                             Rgb( 190, 80, 30 ), Rgb( 90, 80, 76 ),
                                                             Rgb( 50, 50, 50, 0 ) };
            ImDrawList*                           draw   = ImGui::GetWindowDrawList();
            const ImVec2                          start  = ImGui::GetCursorScreenPos() + ImVec2( 0.0f, 3.0f );
            const float                           step   = size.x / static_cast<float>( kStops.size() - 1 );
            for ( size_t i = 0; i + 1 < kStops.size(); ++i )
            {
                const ImVec2 a = start + ImVec2( step * static_cast<float>( i ), 0.0f );
                const ImVec2 b = start + ImVec2( step * static_cast<float>( i + 1 ), size.y - 6.0f );
                draw->AddRectFilledMultiColor( a, b, kStops[i], kStops[i + 1], kStops[i + 1], kStops[i] );
            }
            draw->AddRect( start, start + ImVec2( size.x, size.y - 6.0f ), ImGui::GetColorU32( ImGuiCol_Border ) );
            ImGui::Dummy( size );
        }

        // A size-over-life thumbnail: the same curve the bottom panel edits, at row height.
        void CurveThumb( ImVec2 size )
        {
            ImDrawList*  draw  = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2( 0.0f, 2.0f );
            const ImRect rect( start, start + ImVec2( size.x, size.y - 4.0f ) );
            draw->AddRectFilled( rect.Min, rect.Max, ImGui::GetColorU32( ImGuiCol_FrameBg ) );
            draw->AddBezierCubic( ImVec2( rect.Min.x, rect.Max.y - 2.0f ),
                                  ImVec2( rect.Min.x + rect.GetWidth() * 0.3f, rect.Min.y ),
                                  ImVec2( rect.Min.x + rect.GetWidth() * 0.6f, rect.Min.y ),
                                  ImVec2( rect.Max.x, rect.Max.y - 4.0f ), Rgb( 110, 190, 120 ), 1.5f );
            ImGui::Dummy( size );
        }

        // One module row: drag handle, enable box, name, value, mode chip.
        template <typename ValueFn>
        void ModuleRow( const char* id, const char* name, bool* enabled, InputMode mode, ValueFn&& value )
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            ImGui::PushID( id );
            ImGui::TextDisabled( ICON_MDI_DRAG_VERTICAL );
            ImGui::SameLine( 0.0f, 2.0f );
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 2.0f, 2.0f ) );
            ImGui::Checkbox( "##on", enabled );
            ImGui::PopStyleVar();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            if ( *enabled )
                ImGui::TextUnformatted( name );
            else
                ImGui::TextDisabled( "%s", name );

            ImGui::TableSetColumnIndex( 1 );
            ImGui::SetNextItemWidth( -FLT_MIN );
            value();

            ImGui::TableSetColumnIndex( 2 );
            ModeChip( id, mode );
            ImGui::PopID();
        }

        void Toolbar( const Fonts& fonts )
        {
            static bool loop    = true;
            static int  seed    = 7;
            static int  speed   = 2;
            static int  quality = 3;
            static bool bounds  = true;

            ImGui::SetCursorPos( ImGui::GetCursorPos() + ImVec2( 8.0f, 5.0f ) );
            ToolButton( ICON_MDI_PLAY, true, ImVec2( 32, 0 ) );
            ImGui::SameLine( 0, 4 );
            ToolButton( ICON_MDI_PAUSE, false, ImVec2( 32, 0 ) );
            ImGui::SameLine( 0, 4 );
            ToolButton( ICON_MDI_RESTART, false, ImVec2( 32, 0 ) );
            ImGui::SameLine( 0, 12 );
            ImGui::SetNextItemWidth( 70 );
            static constexpr const char* kSpeeds[] = { "0.1×", "0.5×", "1×", "2×" };
            ImGui::Combo( "##speed", &speed, kSpeeds, 4 );
            ImGui::SameLine( 0, 12 );
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled( "Seed" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 60 );
            ImGui::DragInt( "##seed", &seed );
            ImGui::SameLine( 0, 12 );
            ImGui::Checkbox( "Loop", &loop );

            ImGui::SameLine( 0, 28 );
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont( fonts.bold );
            ImGui::TextUnformatted( "Particles: 3 412" );
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled( "(GPU readback, frame -1)" );

            // Right-aligned group: quality level and the bounds overlay.
            const float rightWidth = 330.0f;
            ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - rightWidth );
            ImGui::TextDisabled( "Quality" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 110 );
            static constexpr const char* kQuality[] = { "Low", "Medium", "High", "Epic", "Cinematic" };
            ImGui::Combo( "##quality", &quality, kQuality, 5 );
            ImGui::SameLine( 0, 12 );
            ToolButton( ICON_MDI_CUBE_OUTLINE " Bounds", bounds );
            ImGui::SameLine( 0, 4 );
            ToolButton( ICON_MDI_COG, false, ImVec2( 32, 0 ) );
        }

        void SystemPanel( const Fonts& fonts )
        {
            static float                 intensity = 1.0f;
            static bool                  enabled[] = { true, true, true, false };
            static constexpr const char* kNames[]  = { "Flash", "Smoke", "Sparks", "Debris" };
            // Live particle share per emitter, drawn as the activity bar next to the name.
            static constexpr float kShare[] = { 0.15f, 0.85f, 0.4f, 0.0f };

            SectionHeader( fonts, "SYSTEM", "Fireball" );
            ImGui::Indent( 6.0f );
            ImGui::Selectable( ICON_MDI_FIRE "  System", false );
            ImGui::Indent( 14.0f );
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled( "User." );
            ImGui::SameLine( 0, 0 );
            ImGui::TextUnformatted( "Intensity" );
            ImGui::SameLine( 150 );
            ImGui::SetNextItemWidth( -8 );
            ImGui::DragFloat( "##intensity", &intensity, 0.01f, 0.0f, 4.0f, "%.2f" );
            ImGui::Unindent( 14.0f );
            ImGui::Unindent( 6.0f );
            ImGui::Spacing();

            SectionHeader( fonts, "EMITTERS", "4" );
            for ( int i = 0; i < 4; ++i )
            {
                ImGui::PushID( i );
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 6.0f );
                ImGui::Checkbox( "##on", &enabled[i] );
                ImGui::SameLine();
                const bool   selected = ( i == 1 );
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                ImGui::Selectable( kNames[i], selected, 0, ImVec2( 0, ImGui::GetFrameHeight() ) );
                if ( kShare[i] > 0.0f )
                {
                    const float  barX  = rowStart.x + 90.0f;
                    const float  width = ( ImGui::GetContentRegionMax().x - 110.0f ) * kShare[i];
                    const ImVec2 a( barX, rowStart.y + 8.0f );
                    ImGui::GetWindowDrawList()->AddRectFilled(
                         a, a + ImVec2( width, ImGui::GetFrameHeight() - 16.0f ),
                         ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetSelectedColor() ), 1.0f );
                }
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 6.0f );
            ImGui::Button( ICON_MDI_PLUS " Emitter " ICON_MDI_CHEVRON_DOWN );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 6.0f );
            ImGui::TextDisabled( "Templates: Empty, Burst, Fountain," );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 6.0f );
            ImGui::TextDisabled( "Smoke, Sparks, Trail, Beam" );
        }

        void PreviewPanel()
        {
            const ImVec2 size = ImGui::GetContentRegionAvail();
            const ImRect rect = ViewportPlaceholder(
                 "PREVIEW VIEW - own SceneRenderer: floor grid, sky, light, orbit camera", size );

            // Overlays the real view will carry: the system bounds, the emitter axes and the emission shape.
            ImDrawList*  draw   = ImGui::GetWindowDrawList();
            const ImVec2 centre = rect.GetCenter() + ImVec2( 0.0f, 40.0f );
            draw->AddRect( centre - ImVec2( 150, 170 ), centre + ImVec2( 150, 80 ), Rgb( 240, 200, 80, 200 ), 0.0f,
                           0, 1.0f );
            draw->AddCircle( centre, 50.0f, Rgb( 120, 200, 255, 200 ), 48, 1.5f );
            draw->AddLine( centre, centre + ImVec2( 60, 0 ),
                           ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetAxisColor( 0 ) ), 2.0f );
            draw->AddLine( centre, centre + ImVec2( -34, 26 ),
                           ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetAxisColor( 1 ) ), 2.0f );
            draw->AddLine( centre, centre + ImVec2( 0, -60 ),
                           ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetAxisColor( 2 ) ), 2.0f );

            // A deterministic cloud of "particles" so the bounds have something inside them.
            unsigned state = 7u;
            auto     next  = [&state]()
            {
                state = state * 1664525u + 1013904223u;
                return static_cast<float>( ( state >> 8 ) & 0xFFFF ) / 65535.0f;
            };
            for ( int i = 0; i < 260; ++i )
            {
                const float  angle  = next() * 6.2831853f;
                const float  height = next();
                const float  radius = 20.0f + 110.0f * height * next();
                const ImVec2 p = centre + ImVec2( std::cos( angle ) * radius, -height * 160.0f + 30.0f * next() );
                const int    shade = 120 + static_cast<int>( 100.0f * ( 1.0f - height ) );
                draw->AddCircleFilled( p, 2.0f + 3.0f * height, Rgb( shade + 30, shade - 10, shade - 60, 150 ) );
            }

            const char* legend = "Bounds   Emission shape   Emitter axes";
            draw->AddText( ImVec2( rect.Min.x + 12.0f, rect.Max.y - 26.0f ),
                           ImGui::GetColorU32( ImGuiCol_TextDisabled ), legend );
        }

        void StackPanel( const Fonts& fonts )
        {
            static bool  on[16]     = { true, true, true, true, true, true, true, true, true, true, true, true };
            static float rate       = 120.0f;
            static int   burst      = 40;
            static float radius     = 50.0f;
            static float cone[2]    = { 30.0f, 400.0f };
            static float life[2]    = { 2.5f, 0.4f };
            static float gravity[3] = { 0.0f, 0.0f, -980.0f };

            SectionHeader( fonts, "STACK", "Emitter \"Smoke\"" );

            constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
            auto                      group  = [&]( const char* title, auto&& rows )
            {
                ImGui::PushStyleColor( ImGuiCol_Header, ImGui::GetStyleColorVec4( ImGuiCol_FrameBg ) );
                const bool open = ImGui::CollapsingHeader( title, ImGuiTreeNodeFlags_DefaultOpen );
                ImGui::PopStyleColor();
                if ( !open )
                    return;
                if ( ImGui::BeginTable( title, 3, kTable ) )
                {
                    ImGui::TableSetupColumn( "name", ImGuiTableColumnFlags_WidthStretch, 1.0f );
                    ImGui::TableSetupColumn( "value", ImGuiTableColumnFlags_WidthFixed, 150.0f );
                    ImGui::TableSetupColumn( "mode", ImGuiTableColumnFlags_WidthFixed, 88.0f );
                    rows();
                    ImGui::EndTable();
                }
            };

            group( "Emitter Update",
                   [&]
                   {
                       ModuleRow( "rate", "Spawn Rate", &on[0], InputMode::Value,
                                  [] { ImGui::DragFloat( "##v", &rate, 1.0f, 0.0f, 0.0f, "%.0f /s" ); } );
                       ModuleRow( "burst", "Spawn Burst", &on[1], InputMode::Value,
                                  [] { ImGui::DragInt( "##v", &burst, 1.0f, 0, 0, "x%d @ 0.0 s" ); } );
                   } );
            group( "Particle Spawn",
                   [&]
                   {
                       ModuleRow( "shape", "Shape: Sphere", &on[2], InputMode::Value,
                                  [] { ImGui::DragFloat( "##v", &radius, 1.0f, 0.0f, 0.0f, "r = %.0f" ); } );
                       ModuleRow( "velocity", "Velocity: Cone", &on[3], InputMode::Random,
                                  [] { ImGui::DragFloat2( "##v", cone, 1.0f, 0.0f, 0.0f, "%.0f" ); } );
                       ModuleRow( "lifetime", "Lifetime", &on[4], InputMode::Random,
                                  [] { ImGui::DragFloat2( "##v", life, 0.05f, 0.0f, 0.0f, "%.1f" ); } );
                   } );
            group( "Particle Update",
                   [&]
                   {
                       ModuleRow( "gravity", "Gravity", &on[5], InputMode::Value,
                                  [] { ImGui::DragFloat3( "##v", gravity, 1.0f, 0.0f, 0.0f, "%.0f" ); } );
                       ModuleRow(
                            "drag", "Drag", &on[6], InputMode::Binding,
                            [] { ImGui::TextColored( ImVec4( 0.43f, 0.63f, 0.9f, 1.0f ), "User.Intensity" ); } );
                       ModuleRow( "curl", "Curl Noise", &on[7], InputMode::Value,
                                  [] { ImGui::Button( ICON_MDI_TUNE " 3 inputs", ImVec2( -FLT_MIN, 0 ) ); } );
                       ModuleRow( "color", "Color over Life", &on[8], InputMode::Curve,
                                  [] { GradientStrip( ImVec2( 150.0f, ImGui::GetFrameHeight() ) ); } );
                       ModuleRow( "size", "Size over Life", &on[9], InputMode::Curve,
                                  [] { CurveThumb( ImVec2( 150.0f, ImGui::GetFrameHeight() ) ); } );
                   } );
            group( "Render",
                   [&]
                   {
                       ModuleRow( "sprite", "Sprite Renderer", &on[10], InputMode::Value,
                                  [] { ImGui::TextUnformatted( "M_Smoke  4×4 SubUV" ); } );
                   } );

            ImGui::Spacing();
            ImGui::Button( ICON_MDI_PLUS " Module " ICON_MDI_CHEVRON_DOWN );
            ImGui::SameLine();
            ImGui::TextDisabled( "Library  ·  New Local (Scratch Pad)" );
        }

        void TimelinePanel( const Fonts& fonts )
        {
            SectionHeader( fonts, "TIMELINE", "Delay / Duration / Loops" );

            struct Section
            {
                const char* name;
                float       delay;
                float       duration;
                bool        loops;
            };
            static constexpr std::array<Section, 4> kRows      = { { { "Flash", 0.0f, 0.15f, false },
                                                                     { "Smoke", 0.1f, 2.9f, true },
                                                                     { "Sparks", 0.0f, 0.6f, false },
                                                                     { "Debris", 0.05f, 1.2f, false } } };
            constexpr float                         kSeconds   = 3.0f;
            constexpr float                         kNameWidth = 90.0f;

            ImDrawList*  draw  = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float  width = ImGui::GetContentRegionAvail().x - kNameWidth - 28.0f;
            const float  x0    = start.x + kNameWidth;
            auto         at    = [&]( float seconds ) { return x0 + width * seconds / kSeconds; };

            // Ruler.
            for ( int tick = 0; tick <= 30; ++tick )
            {
                const float x     = at( static_cast<float>( tick ) * 0.1f );
                const bool  major = ( tick % 10 ) == 0;
                draw->AddLine( ImVec2( x, start.y + ( major ? 2.0f : 12.0f ) ), ImVec2( x, start.y + 20.0f ),
                               ImGui::GetColorU32( ImGuiCol_TextDisabled ) );
                if ( major )
                {
                    char label[8];
                    std::snprintf( label, sizeof label, "%ds", tick / 10 );
                    draw->AddText( ImVec2( x + 3.0f, start.y ), ImGui::GetColorU32( ImGuiCol_TextDisabled ),
                                   label );
                }
            }

            const float rowHeight = 30.0f;
            for ( size_t i = 0; i < kRows.size(); ++i )
            {
                const Section& row = kRows[i];
                const float    y   = start.y + 26.0f + rowHeight * static_cast<float>( i );
                if ( i % 2 == 1 )
                    draw->AddRectFilled( ImVec2( start.x, y ), ImVec2( at( kSeconds ), y + rowHeight ),
                                         Rgb( 255, 255, 255, 8 ) );
                draw->AddText( ImVec2( start.x + 6.0f, y + 6.0f ),
                               ImGui::GetColorU32( i == 3 ? ImGuiCol_TextDisabled : ImGuiCol_Text ), row.name );

                const ImVec2 a( at( row.delay ), y + 5.0f );
                const ImVec2 b( at( row.delay + row.duration ), y + rowHeight - 5.0f );
                const ImU32  fill =
                     i == 1 ? ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetSelectedColor() )
                             : Rgb( 90, 100, 115, i == 3 ? 110 : 255 );
                draw->AddRectFilled( a, b, fill, 2.0f );
                // Edge grips: dragging an edge is Delay (left) or Duration (right).
                draw->AddRectFilled( a, ImVec2( a.x + 4.0f, b.y ), Rgb( 255, 255, 255, 70 ), 2.0f );
                draw->AddRectFilled( ImVec2( b.x - 4.0f, a.y ), b, Rgb( 255, 255, 255, 70 ), 2.0f );
                if ( row.loops )
                    draw->AddText( ImVec2( b.x - 26.0f, a.y + 1.0f ), Rgb( 255, 255, 255, 200 ), ICON_MDI_REPEAT );
            }

            // Playhead.
            const float playX = at( 1.25f );
            draw->AddLine( ImVec2( playX, start.y ), ImVec2( playX, start.y + 26.0f + rowHeight * 4.0f ),
                           ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetHighlightColor() ), 2.0f );
            draw->AddTriangleFilled( ImVec2( playX - 6.0f, start.y ), ImVec2( playX + 6.0f, start.y ),
                                     ImVec2( playX, start.y + 8.0f ),
                                     ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetHighlightColor() ) );

            ImGui::Dummy( ImVec2( width + kNameWidth, 26.0f + rowHeight * 4.0f + 6.0f ) );
            ImGui::TextDisabled(
                 "Drag a section edge: Delay / Duration.   1.25 s   Scrub is deterministic (seed 7)." );
        }

        void CurvesPanel( const Fonts& fonts )
        {
            SectionHeader( fonts, "CURVES", "Smoke · Size over Life" );

            static int tangent = 0;
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 6.0f );
            ToolButton( "Cubic", tangent == 0 );
            ImGui::SameLine( 0, 2 );
            ToolButton( "Auto", tangent == 1 );
            ImGui::SameLine( 0, 2 );
            ToolButton( "Break", tangent == 2 );
            ImGui::SameLine( 0, 2 );
            ToolButton( "Linear", false );
            ImGui::SameLine( 0, 16 );
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled( "X axis: particle age 0..1   (system time for emitter inputs)" );
            ImGui::Spacing();

            ImDrawList*  draw  = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2( 34.0f, 4.0f );
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const ImRect graph( start, start + ImVec2( avail.x - 46.0f, avail.y - 34.0f ) );
            draw->AddRectFilled( graph.Min, graph.Max, ImGui::GetColorU32( ImGuiCol_FrameBg ) );
            for ( int i = 1; i < 10; ++i )
            {
                const float x = graph.Min.x + graph.GetWidth() * static_cast<float>( i ) / 10.0f;
                draw->AddLine( ImVec2( x, graph.Min.y ), ImVec2( x, graph.Max.y ), Rgb( 255, 255, 255, 14 ) );
            }
            for ( int i = 1; i < 4; ++i )
            {
                const float y = graph.Min.y + graph.GetHeight() * static_cast<float>( i ) / 4.0f;
                draw->AddLine( ImVec2( graph.Min.x, y ), ImVec2( graph.Max.x, y ), Rgb( 255, 255, 255, 14 ) );
            }
            auto point = [&]( float age, float value )
            { return ImVec2( graph.Min.x + graph.GetWidth() * age, graph.Max.y - graph.GetHeight() * value ); };
            draw->AddText( ImVec2( graph.Min.x - 22.0f, graph.Min.y - 2.0f ),
                           ImGui::GetColorU32( ImGuiCol_TextDisabled ), "1" );
            draw->AddText( ImVec2( graph.Min.x - 22.0f, graph.Max.y - 16.0f ),
                           ImGui::GetColorU32( ImGuiCol_TextDisabled ), "0" );
            draw->AddText( ImVec2( graph.Max.x - 30.0f, graph.Max.y + 4.0f ),
                           ImGui::GetColorU32( ImGuiCol_TextDisabled ), "age" );

            const ImU32  curve = Rgb( 110, 190, 120 );
            const ImVec2 k0 = point( 0.0f, 0.15f ), k1 = point( 0.35f, 0.92f ), k2 = point( 1.0f, 0.55f );
            draw->AddBezierCubic( k0, point( 0.12f, 0.6f ), point( 0.22f, 0.92f ), k1, curve, 2.0f );
            draw->AddBezierCubic( k1, point( 0.55f, 0.92f ), point( 0.8f, 0.6f ), k2, curve, 2.0f );
            // Tangent handles of the selected key.
            const ImU32 handle = Rgb( 200, 200, 200, 160 );
            draw->AddLine( point( 0.22f, 0.92f ), point( 0.48f, 0.92f ), handle );
            draw->AddCircleFilled( point( 0.22f, 0.92f ), 3.0f, handle );
            draw->AddCircleFilled( point( 0.48f, 0.92f ), 3.0f, handle );
            for ( const ImVec2& key : { k0, k1, k2 } )
            {
                const bool  selected = ( key.x == k1.x );
                const ImU32 fill =
                     selected ? ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetHighlightColor() )
                              : Rgb( 230, 230, 230 );
                draw->AddQuadFilled( key + ImVec2( 0, -5 ), key + ImVec2( 5, 0 ), key + ImVec2( 0, 5 ),
                                     key + ImVec2( -5, 0 ), fill );
            }
            draw->AddText( k1 + ImVec2( 8.0f, -22.0f ), ImGui::GetColorU32( ImGuiCol_Text ), "0.35, 0.92" );
        }
    } // namespace

    void VfxSystemWindow( const Fonts& fonts )
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos( viewport->WorkPos );
        ImGui::SetNextWindowSize( viewport->WorkSize );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        ImGui::Begin( "##VfxSystemWindow", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus );
        ImGui::PopStyleVar( 2 );

        const ImVec2 full   = ImGui::GetContentRegionAvail();
        ImDrawList*  draw   = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // Document tab strip: the .dfx opens as a document tab in the editor's document well.
        constexpr float kTabStrip = 32.0f;
        draw->AddRectFilled( origin, origin + ImVec2( full.x, kTabStrip ),
                             ImGui::GetColorU32( ImGuiCol_TitleBg ) );
        draw->AddRectFilled( origin + ImVec2( 8, 4 ), origin + ImVec2( 190, kTabStrip ),
                             ImGui::GetColorU32( ImGuiCol_TabActive ), 2.0f, ImDrawFlags_RoundCornersTop );
        draw->AddRectFilled( origin + ImVec2( 8, 4 ), origin + ImVec2( 190, 6 ),
                             ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetSelectedColor() ) );
        draw->AddText( origin + ImVec2( 18, 9 ), ImGui::GetColorU32( ImGuiCol_Text ),
                       ICON_MDI_FIRE " Fireball.dfx" );
        draw->AddText( origin + ImVec2( 206, 9 ), ImGui::GetColorU32( ImGuiCol_TextDisabled ),
                       "Content/VFX/Fireball.dfx" );
        ImGui::Dummy( ImVec2( full.x, kTabStrip ) );

        // Toolbar.
        ImGui::BeginChild( "##toolbar", ImVec2( full.x, 40.0f ), false, ImGuiWindowFlags_NoScrollbar );
        Toolbar( fonts );
        ImGui::EndChild();

        const float bottomHeight = std::floor( full.y * 0.30f );
        const float topHeight    = full.y - kTabStrip - 40.0f - bottomHeight - 2.0f;
        const float leftWidth    = 300.0f;
        const float rightWidth   = 470.0f;
        const float centreWidth  = full.x - leftWidth - rightWidth - 4.0f;

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 4, 4 ) );
        ImGui::BeginChild( "##system", ImVec2( leftWidth, topHeight ), true );
        SystemPanel( fonts );
        ImGui::EndChild();
        ImGui::SameLine( 0, 2 );
        ImGui::BeginChild( "##preview", ImVec2( centreWidth, topHeight ), false, ImGuiWindowFlags_NoScrollbar );
        PreviewPanel();
        ImGui::EndChild();
        ImGui::SameLine( 0, 2 );
        ImGui::BeginChild( "##stack", ImVec2( rightWidth, topHeight ), true );
        StackPanel( fonts );
        ImGui::EndChild();

        ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 2.0f - ImGui::GetStyle().ItemSpacing.y );
        const float timelineWidth = leftWidth + centreWidth * 0.45f;
        ImGui::BeginChild( "##timeline", ImVec2( timelineWidth, 0 ), true, ImGuiWindowFlags_NoScrollbar );
        TimelinePanel( fonts );
        ImGui::EndChild();
        ImGui::SameLine( 0, 2 );
        ImGui::BeginChild( "##curves", ImVec2( 0, 0 ), true, ImGuiWindowFlags_NoScrollbar );
        CurvesPanel( fonts );
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::End();
    }
} // namespace Desert::UIMockup::Mockups
