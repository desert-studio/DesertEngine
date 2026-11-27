// The blend space's widgets and plot (UE BlendSpaceEditor's sample list + SBlendSpaceGridWidget), shared by the
// BlendSpace1D / BlendSpace2D node and a state that plays a blend space.
#include "BlendSpaceEdit.hpp"

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace Desert::Editor::Graph
{
    namespace G = Animation::Graph;

    namespace
    {
        const char* kSmoothingNames[] = { "Average",           "Linear",       "Cubic", "Ease In Out",
                                          "Exponential Decay", "Spring Damper" };
        const char* kAxisNames[]      = { "X", "Y" };

        bool DrawAxis( G::BlendSpaceAxis& axis, size_t index )
        {
            bool dirty = false;
            ImGui::PushID( static_cast<int>( index ) );
            ImGui::TextUnformatted( std::format( "Axis {}", kAxisNames[index] ).c_str() );
            ImGui::SetNextItemWidth( 80.0f );
            dirty |= ImGui::DragFloat( "Min", &axis.Min, 1.0f );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 80.0f );
            dirty |= ImGui::DragFloat( "Max", &axis.Max, 1.0f );
            ImGui::SameLine();
            dirty |= ImGui::Checkbox( "Wrap", &axis.Wrap );
            int smoothing = static_cast<int>( axis.Smoothing );
            ImGui::SetNextItemWidth( 130.0f );
            if ( ImGui::Combo( "Smoothing", &smoothing, kSmoothingNames, IM_ARRAYSIZE( kSmoothingNames ) ) )
            {
                axis.Smoothing = static_cast<G::BlendAxisSmoothing>( smoothing );
                dirty          = true;
            }
            ImGui::SetNextItemWidth( 80.0f );
            dirty |= ImGui::DragFloat( "Time (s)", &axis.SmoothingTime, 0.01f, 0.0f, 10.0f, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp );
            if ( axis.Smoothing == G::BlendAxisSmoothing::SpringDamper )
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 70.0f );
                dirty |= ImGui::DragFloat( "Damping", &axis.DampingRatio, 0.01f, 0.0f, 4.0f, "%.2f",
                                           ImGuiSliderFlags_AlwaysClamp );
            }
            ImGui::SetNextItemWidth( 80.0f );
            dirty |= ImGui::DragFloat( "Max Speed (/s, 0 = any)", &axis.MaxSpeed, 1.0f, 0.0f, 100000.0f, "%.1f",
                                       ImGuiSliderFlags_AlwaysClamp );
            ImGui::PopID();
            return dirty;
        }

        /// A sample's normalized position in the axis box, [0, 1] per axis (Y up).
        ImVec2 Normalized( const G::BlendSpaceSetup& setup, const G::BlendSample& sample )
        {
            const auto unit = []( const G::BlendSpaceAxis& axis, float v )
            {
                const float span = axis.Max - axis.Min;
                return span > 0.0f ? std::clamp( ( v - axis.Min ) / span, 0.0f, 1.0f ) : 0.0f;
            };
            const float x = setup.Axes.empty() ? 0.0f : unit( setup.Axes[0], sample.X );
            const float y = setup.Axes.size() < 2 ? 0.5f : unit( setup.Axes[1], sample.Y );
            return ImVec2( x, y );
        }
    } // namespace

    BlendSpaceEditResult DrawBlendSpaceSetup( G::BlendSpaceSetup& setup, std::span<const std::string> clipNames )
    {
        bool       dirty = false;
        const bool twoD  = setup.Axes.size() >= 2;
        for ( size_t a = 0; a < setup.Axes.size() && a < 2; ++a )
            dirty |= DrawAxis( setup.Axes[a], a );

        ImGui::Separator();
        ImGui::TextUnformatted( twoD ? "Samples (clip, X, Y, rate)" : "Samples (clip, X, rate)" );
        int removed = -1;
        for ( size_t s = 0; s < setup.Samples.size(); ++s )
        {
            G::BlendSample& sample = setup.Samples[s];
            ImGui::PushID( static_cast<int>( s ) );
            ImGui::SetNextItemWidth( twoD ? 100.0f : 140.0f );
            if ( ImGui::BeginCombo( "##sampleClip", sample.Clip.empty() ? "<no clip>" : sample.Clip.c_str() ) )
            {
                for ( const std::string& clip : clipNames )
                    if ( ImGui::Selectable( clip.c_str(), clip == sample.Clip ) )
                    {
                        sample.Clip = clip;
                        dirty       = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 55.0f );
            dirty |= ImGui::DragFloat( "##sampleX", &sample.X, 1.0f );
            if ( twoD )
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 55.0f );
                dirty |= ImGui::DragFloat( "##sampleY", &sample.Y, 0.01f );
            }
            ImGui::SameLine();
            // UE FBlendSample::RateScale: the clip's own rate times this, so its native speed meets its position.
            ImGui::SetNextItemWidth( 55.0f );
            dirty |= ImGui::DragFloat( "##sampleRate", &sample.RateScale, 0.01f, 0.05f, 4.0f, "x%.2f",
                                       ImGuiSliderFlags_AlwaysClamp );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Rate Scale: this sample's playback rate against its clip" );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x" ) )
                removed = static_cast<int>( s );
            ImGui::PopID();
        }
        if ( removed >= 0 )
        {
            setup.Samples.erase( setup.Samples.begin() + removed );
            dirty = true;
        }
        if ( ImGui::Button( "Add Sample" ) )
        {
            // At the box's centre: inside every axis range, a position no corner sample holds.
            G::BlendSample added;
            added.Clip = setup.Samples.empty() ? std::string() : setup.Samples.back().Clip;
            if ( !setup.Axes.empty() )
                added.X = 0.5f * ( setup.Axes[0].Min + setup.Axes[0].Max );
            if ( twoD )
                added.Y = 0.5f * ( setup.Axes[1].Min + setup.Axes[1].Max );
            setup.Samples.push_back( std::move( added ) );
            dirty = true;
        }

        ImGui::Separator();
        dirty |= ImGui::DragFloat( "Weight Speed (/s, 0 = none)", &setup.WeightSpeed, 0.1f, 0.0f, 100.0f );
        dirty |= ImGui::Checkbox( "Ease In Out##blendSpace", &setup.WeightEaseInOut );
        ImGui::SameLine();
        dirty |= ImGui::Checkbox( "Loop##blendSpace", &setup.Loop );
        // UE AxisToScaleAnimation: the axis whose input beyond the samples speeds the clips up.
        const char* scaleNames[] = { "None", "X", "Y" };
        int         scale        = std::clamp( setup.ScaleAxis + 1, 0, static_cast<int>( setup.Axes.size() ) );
        ImGui::SetNextItemWidth( 90.0f );
        if ( ImGui::Combo( "Scale Axis", &scale, scaleNames, static_cast<int>( setup.Axes.size() ) + 1 ) )
        {
            setup.ScaleAxis = scale - 1;
            dirty           = true;
        }
        return BlendSpaceEditResult{ .Changed = dirty };
    }

    void DrawBlendSpacePlot( const G::BlendSpaceSetup& setup, const Animation::Animator::BlendSpaceRun* run )
    {
        const float  width  = std::max( 120.0f, ImGui::GetContentRegionAvail().x );
        const bool   twoD   = setup.Axes.size() >= 2;
        const float  height = twoD ? width : 48.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy( ImVec2( width, height ) );
        ImDrawList* draw = ImGui::GetWindowDrawList();

        constexpr float pad = 12.0f;
        const ImVec2    lo( origin.x + pad, origin.y + pad );
        const ImVec2    hi( origin.x + width - pad, origin.y + height - pad );
        const auto      toScreen = [&]( ImVec2 unit )
        { return ImVec2( lo.x + unit.x * ( hi.x - lo.x ), hi.y - unit.y * ( hi.y - lo.y ) ); };

        draw->AddRectFilled( origin, ImVec2( origin.x + width, origin.y + height ), IM_COL32( 24, 26, 30, 255 ) );
        draw->AddRect( lo, hi, IM_COL32( 110, 110, 120, 255 ) );
        if ( !setup.Axes.empty() )
        {
            draw->AddText( ImVec2( lo.x, hi.y ), IM_COL32( 150, 150, 160, 255 ),
                           std::format( "{:g}", setup.Axes[0].Min ).c_str() );
            const std::string maxX = std::format( "{:g}", setup.Axes[0].Max );
            draw->AddText( ImVec2( hi.x - ImGui::CalcTextSize( maxX.c_str() ).x, hi.y ),
                           IM_COL32( 150, 150, 160, 255 ), maxX.c_str() );
        }
        if ( twoD )
            draw->AddText( ImVec2( lo.x, origin.y ), IM_COL32( 150, 150, 160, 255 ),
                           std::format( "Y {:g}", setup.Axes[1].Max ).c_str() );

        // The triangles (2D) and the row (1D) are the RUN's: the same triangulation that weighs the samples.
        if ( run && run->Triangulation.Points.size() == setup.Samples.size() )
        {
            for ( const auto& tri : run->Triangulation.Triangles )
                for ( size_t e = 0; e < 3; ++e )
                    draw->AddLine( toScreen( Normalized( setup, setup.Samples[tri[e]] ) ),
                                   toScreen( Normalized( setup, setup.Samples[tri[( e + 1 ) % 3]] ) ),
                                   IM_COL32( 80, 120, 170, 200 ) );
            for ( const auto& edge : run->Triangulation.Hull )
                draw->AddLine( toScreen( Normalized( setup, setup.Samples[edge[0]] ) ),
                               toScreen( Normalized( setup, setup.Samples[edge[1]] ) ),
                               IM_COL32( 120, 160, 210, 255 ), 1.5f );
        }

        const bool weighted = run && run->Weights.size() == setup.Samples.size();
        for ( size_t s = 0; s < setup.Samples.size(); ++s )
        {
            const ImVec2 at     = toScreen( Normalized( setup, setup.Samples[s] ) );
            const float  weight = weighted ? run->Weights[s] : 0.0f;
            draw->AddCircleFilled( at, 4.0f + 6.0f * weight,
                                   weight > G::kBlendSampleWeight ? IM_COL32( 240, 190, 70, 255 )
                                                                  : IM_COL32( 200, 200, 210, 255 ) );
            const std::string label =
                 weighted ? std::format( "{} {:.2f}", setup.Samples[s].Clip, weight ) : setup.Samples[s].Clip;
            draw->AddText( ImVec2( at.x + 6.0f, at.y - 14.0f ), IM_COL32( 220, 220, 225, 255 ), label.c_str() );
        }

        // The live point: the filtered axis input the weights were computed from.
        if ( run && run->Started && !setup.Axes.empty() )
        {
            G::BlendSample input;
            input.X        = run->Input[0];
            input.Y        = run->Input[1];
            const ImVec2 p = toScreen( Normalized( setup, input ) );
            draw->AddCircle( p, 7.0f, IM_COL32( 90, 230, 120, 255 ), 0, 2.0f );
            draw->AddLine( ImVec2( p.x - 9.0f, p.y ), ImVec2( p.x + 9.0f, p.y ), IM_COL32( 90, 230, 120, 255 ) );
            draw->AddLine( ImVec2( p.x, p.y - 9.0f ), ImVec2( p.x, p.y + 9.0f ), IM_COL32( 90, 230, 120, 255 ) );
        }
    }
} // namespace Desert::Editor::Graph
