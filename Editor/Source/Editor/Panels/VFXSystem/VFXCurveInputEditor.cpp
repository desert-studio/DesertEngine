#include "VFXCurveInputEditor.hpp"

#include <Engine/VFX/VFXCurveLUT.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>

namespace Desert::Editor::VFXCurveEdit
{
    namespace S = Assets::Serialization;

    namespace
    {
        bool Near( const float a, const float b )
        {
            return std::abs( a - b ) < kStopEpsilon;
        }

        // The key of @p keys at @p time, or end.
        std::vector<S::VFXCurveKey>::iterator KeyAt( std::vector<S::VFXCurveKey>& keys, const float time )
        {
            return std::find_if( keys.begin(), keys.end(),
                                 [time]( const S::VFXCurveKey& k ) { return Near( k.Time, time ); } );
        }

        void SortKeys( std::vector<S::VFXCurveKey>& keys )
        {
            std::sort( keys.begin(), keys.end(),
                       []( const S::VFXCurveKey& a, const S::VFXCurveKey& b ) { return a.Time < b.Time; } );
        }

        // The time axis a widget spans: the normalised age 0..1, widened to the keys when they lie outside it
        // (an emitter-stage input's axis is the emitter's time, UE's curve editor frames the keys the same way).
        std::pair<float, float> TimeRange( const std::vector<float>& stops )
        {
            float lo = 0.0f, hi = 1.0f;
            if ( !stops.empty() )
            {
                lo = std::min( lo, stops.front() );
                hi = std::max( hi, stops.back() );
            }
            return { lo, hi };
        }

        constexpr std::array<ImU32, 4> kChannelColours = {
             IM_COL32( 230, 80, 80, 255 ), IM_COL32( 90, 200, 90, 255 ), IM_COL32( 90, 140, 240, 255 ),
             IM_COL32( 200, 200, 200, 255 ) };

        ImU32 Swatch( const glm::vec4& v )
        {
            const glm::vec4 c = glm::clamp( v, glm::vec4( 0.0f ), glm::vec4( 1.0f ) );
            return ImGui::ColorConvertFloat4ToU32( ImVec4( c.r, c.g, c.b, c.a ) );
        }
    } // namespace

    std::vector<float> StopTimes( const Channels& channels )
    {
        std::vector<float> out;
        for ( const auto& keys : channels )
            for ( const auto& key : keys )
                out.push_back( key.Time );
        std::sort( out.begin(), out.end() );
        out.erase( std::unique( out.begin(), out.end(), Near ), out.end() );
        return out;
    }

    glm::vec4 ValueAt( const Channels& channels, const float time )
    {
        glm::vec4 out( 0.0f );
        for ( std::size_t c = 0; c < channels.size() && c < 4; ++c )
            if ( !channels[c].empty() )
                out[static_cast<glm::length_t>( c )] = VFX::EvaluateCurve( channels[c], time );
        return out;
    }

    void SetStop( Channels& channels, const float time, const glm::vec4& value )
    {
        for ( std::size_t c = 0; c < channels.size() && c < 4; ++c )
        {
            auto&       keys = channels[c];
            const float v    = value[static_cast<glm::length_t>( c )];
            if ( auto key = KeyAt( keys, time ); key != keys.end() )
                key->Value = v;
            else
            {
                S::VFXCurveKey added;
                added.Time  = time;
                added.Value = v;
                keys.push_back( added );
                SortKeys( keys );
            }
        }
    }

    bool MoveStop( Channels& channels, const float from, const float to )
    {
        if ( Near( from, to ) )
            return true;
        for ( auto& keys : channels )
        {
            const bool moving = KeyAt( keys, from ) != keys.end();
            if ( moving && KeyAt( keys, to ) != keys.end() )
                return false;
        }
        for ( auto& keys : channels )
        {
            if ( auto key = KeyAt( keys, from ); key != keys.end() )
            {
                key->Time = to;
                SortKeys( keys );
            }
        }
        return true;
    }

    bool RemoveStop( Channels& channels, const float time )
    {
        for ( auto& keys : channels )
            if ( keys.size() == 1 && KeyAt( keys, time ) != keys.end() )
                return false;
        for ( auto& keys : channels )
            if ( auto key = KeyAt( keys, time ); key != keys.end() )
                keys.erase( key );
        return true;
    }

    Common::ResultStr<Channels> FromPacked( const std::span<const float> packed, const std::size_t components )
    {
        const std::size_t stride = components + 1;
        if ( components == 0 || packed.empty() || packed.size() % stride != 0 )
            return Common::MakeFormattedError<Channels>(
                 "a curve of {} channel(s) takes its keys as time then {} value(s) each ({} numbers per key), "
                 "not {} numbers",
                 components, components, stride, packed.size() );
        Channels out( components );
        for ( std::size_t k = 0; k * stride < packed.size(); ++k )
        {
            const float time = packed[k * stride];
            if ( k > 0 && !( time > packed[( k - 1 ) * stride] + kStopEpsilon ) )
                return Common::MakeFormattedError<Channels>( "key {} is at time {}, not after key {} at {}", k,
                                                             time, k - 1, packed[( k - 1 ) * stride] );
            for ( std::size_t c = 0; c < components; ++c )
            {
                S::VFXCurveKey key;
                key.Time  = time;
                key.Value = packed[k * stride + 1 + c];
                out[c].push_back( key );
            }
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    std::vector<float> ToPacked( const Channels& channels )
    {
        std::vector<float> out;
        for ( const float t : StopTimes( channels ) )
        {
            out.push_back( t );
            const glm::vec4 v = ValueAt( channels, t );
            for ( std::size_t c = 0; c < channels.size() && c < 4; ++c )
                out.push_back( v[static_cast<glm::length_t>( c )] );
        }
        return out;
    }

    Gesture Draw( const char* id, Channels& channels, const bool colour )
    {
        Gesture    gesture;
        const auto track = [&gesture]
        {
            gesture.Activated   = gesture.Activated || ImGui::IsItemActivated();
            gesture.Deactivated = gesture.Deactivated || ImGui::IsItemDeactivated();
        };
        if ( channels.empty() )
            return gesture;

        ImGui::PushID( id );
        const std::vector<float>      stops  = StopTimes( channels );
        const std::pair<float, float> range  = TimeRange( stops );
        const float                   tMin   = range.first;
        const float                   tMax   = range.second;
        const float                   tSpan  = std::max( tMax - tMin, kStopEpsilon );
        ImDrawList*                   draw   = ImGui::GetWindowDrawList();
        const float                   width  = std::max( ImGui::GetContentRegionAvail().x, 64.0f );
        const ImVec2                  origin = ImGui::GetCursorScreenPos();
        const float                   height = colour ? 20.0f : 72.0f;
        const auto timeToX = [&]( const float t ) { return origin.x + ( t - tMin ) / tSpan * width; };
        const auto xToTime = [&]( const float x )
        { return std::clamp( tMin + ( x - origin.x ) / width * tSpan, tMin, tMax ); };
        const ImVec2  bottomRight( origin.x + width, origin.y + height );
        constexpr int kSamples = 64;

        if ( colour )
        {
            // UE's colour curve gradient: a checkerboard under the sampled colour, so the alpha reads.
            const float cell = height * 0.5f;
            for ( float x = origin.x; x < bottomRight.x; x += cell )
                for ( int row = 0; row < 2; ++row )
                {
                    const bool dark = ( static_cast<int>( ( x - origin.x ) / cell ) + row ) % 2 == 0;
                    draw->AddRectFilled(
                         ImVec2( x, origin.y + row * cell ),
                         ImVec2( std::min( x + cell, bottomRight.x ), origin.y + ( row + 1 ) * cell ),
                         dark ? IM_COL32( 90, 90, 90, 255 ) : IM_COL32( 150, 150, 150, 255 ) );
                }
            for ( int s = 0; s < kSamples; ++s )
            {
                const float t0 = tMin + tSpan * static_cast<float>( s ) / kSamples;
                const float t1 = tMin + tSpan * static_cast<float>( s + 1 ) / kSamples;
                const ImU32 a  = Swatch( ValueAt( channels, t0 ) );
                const ImU32 b  = Swatch( ValueAt( channels, t1 ) );
                draw->AddRectFilledMultiColor( ImVec2( timeToX( t0 ), origin.y ),
                                               ImVec2( timeToX( t1 ), bottomRight.y ), a, b, b, a );
            }
            draw->AddRect( origin, bottomRight, IM_COL32( 20, 20, 20, 255 ) );
        }
        else
        {
            float lo = 0.0f, hi = 1.0f;
            for ( const auto& keys : channels )
                for ( const auto& k : keys )
                {
                    lo = std::min( lo, k.Value );
                    hi = std::max( hi, k.Value );
                }
            const float pad = ( hi - lo ) * 0.1f;
            lo -= pad;
            hi += pad;
            const auto valueToY = [&]( const float v )
            { return bottomRight.y - ( v - lo ) / ( hi - lo ) * height; };
            draw->AddRectFilled( origin, bottomRight, IM_COL32( 30, 30, 30, 255 ) );
            draw->AddLine( ImVec2( origin.x, valueToY( 0.0f ) ), ImVec2( bottomRight.x, valueToY( 0.0f ) ),
                           IM_COL32( 70, 70, 70, 255 ) );
            draw->AddLine( ImVec2( origin.x, valueToY( 1.0f ) ), ImVec2( bottomRight.x, valueToY( 1.0f ) ),
                           IM_COL32( 70, 70, 70, 255 ) );
            for ( std::size_t c = 0; c < channels.size() && c < 4; ++c )
            {
                if ( channels[c].empty() )
                    continue;
                std::array<ImVec2, kSamples + 1> line{};
                for ( int s = 0; s <= kSamples; ++s )
                {
                    const float t = tMin + tSpan * static_cast<float>( s ) / kSamples;
                    line[static_cast<std::size_t>( s )] =
                         ImVec2( timeToX( t ), valueToY( VFX::EvaluateCurve( channels[c], t ) ) );
                }
                draw->AddPolyline( line.data(), static_cast<int>( line.size() ), kChannelColours[c], 0, 1.5f );
            }
            // A key is a handle: dragging it moves that channel's key in value, and in time between its
            // neighbours. Submitted before the background, so the handle - not the add-a-stop click - owns the
            // hover.
            const float pixelValue = ( hi - lo ) / height;
            for ( std::size_t c = 0; c < channels.size() && c < 4; ++c )
            {
                auto& keys = channels[c];
                for ( std::size_t k = 0; k < keys.size(); ++k )
                {
                    const ImVec2 at( timeToX( keys[k].Time ), valueToY( keys[k].Value ) );
                    ImGui::SetCursorScreenPos( ImVec2( at.x - 5.0f, at.y - 5.0f ) );
                    ImGui::PushID( static_cast<int>( c * 1024 + k ) );
                    ImGui::InvisibleButton( "##key", ImVec2( 10.0f, 10.0f ) );
                    track();
                    if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 0.0f ) )
                    {
                        const ImVec2 delta = ImGui::GetIO().MouseDelta;
                        keys[k].Value -= delta.y * pixelValue;
                        const float lowT  = k > 0 ? keys[k - 1].Time + kStopEpsilon * 2.0f : tMin;
                        const float highT = k + 1 < keys.size() ? keys[k + 1].Time - kStopEpsilon * 2.0f : tMax;
                        keys[k].Time      = std::clamp( keys[k].Time + delta.x / width * tSpan, lowT, highT );
                    }
                    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
                    ImGui::PopID();
                    draw->AddCircleFilled( at, hot ? 5.0f : 3.5f, kChannelColours[c] );
                }
            }
        }

        // Clicking the bar or the plot between handles adds a stop there, holding the value the curve has there.
        ImGui::SetCursorScreenPos( origin );
        ImGui::InvisibleButton( "##area", ImVec2( width, height ) );
        track();
        if ( ImGui::IsItemClicked( ImGuiMouseButton_Left ) )
        {
            const float t    = xToTime( ImGui::GetIO().MousePos.x );
            bool        free = true;
            for ( const float s : stops )
                free = free && std::abs( s - t ) > tSpan * 0.01f;
            if ( free )
                SetStop( channels, t, ValueAt( channels, t ) );
        }

        // The gradient's stop markers under the bar; dragging one moves the stop between its neighbours.
        if ( colour )
        {
            for ( std::size_t i = 0; i < stops.size(); ++i )
            {
                const float x = timeToX( stops[i] );
                ImGui::SetCursorScreenPos( ImVec2( x - 6.0f, bottomRight.y ) );
                ImGui::PushID( static_cast<int>( i ) );
                ImGui::InvisibleButton( "##stop", ImVec2( 12.0f, 10.0f ) );
                track();
                if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 0.0f ) )
                {
                    const float lowT  = i > 0 ? stops[i - 1] + kStopEpsilon * 2.0f : tMin;
                    const float highT = i + 1 < stops.size() ? stops[i + 1] - kStopEpsilon * 2.0f : tMax;
                    (void)MoveStop( channels, stops[i],
                                    std::clamp( xToTime( ImGui::GetIO().MousePos.x ), lowT, highT ) );
                }
                const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
                ImGui::PopID();
                draw->AddTriangleFilled( ImVec2( x, bottomRight.y + 1.0f ),
                                         ImVec2( x - 5.0f, bottomRight.y + 9.0f ),
                                         ImVec2( x + 5.0f, bottomRight.y + 9.0f ),
                                         hot ? IM_COL32( 255, 200, 60, 255 ) : IM_COL32( 220, 220, 220, 255 ) );
            }
        }
        ImGui::SetCursorScreenPos( ImVec2( origin.x, bottomRight.y + ( colour ? 12.0f : 4.0f ) ) );

        // One row per stop: its time, its value (every channel), remove.
        const std::vector<float> rows  = StopTimes( channels );
        const int                count = static_cast<int>( std::min<std::size_t>( channels.size(), 4 ) );
        for ( std::size_t i = 0; i < rows.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i + 4096 ) );
            float time = rows[i];
            ImGui::SetNextItemWidth( 56.0f );
            if ( ImGui::DragFloat( "##t", &time, 0.005f, tMin, tMax, "%.3f" ) )
            {
                const float lowT  = i > 0 ? rows[i - 1] + kStopEpsilon * 2.0f : -1e6f;
                const float highT = i + 1 < rows.size() ? rows[i + 1] - kStopEpsilon * 2.0f : 1e6f;
                (void)MoveStop( channels, rows[i], std::clamp( time, lowT, highT ) );
            }
            track();
            ImGui::SameLine();
            glm::vec4 value = ValueAt( channels, rows[i] );
            if ( colour )
            {
                (void)ImGui::ColorButton( "##swatch", ImVec4( value.r, value.g, value.b, value.a ),
                                          ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip,
                                          ImVec2( ImGui::GetFrameHeight(), ImGui::GetFrameHeight() ) );
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(
                 std::max( ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 6.0f, 40.0f ) );
            if ( ImGui::DragScalarN( "##v", ImGuiDataType_Float, &value.x, count, 0.01f, nullptr, nullptr,
                                     "%.3f" ) )
                SetStop( channels, rows[i], value );
            track();
            ImGui::SameLine();
            ImGui::BeginDisabled( rows.size() < 2 );
            if ( ImGui::Button( "x", ImVec2( ImGui::GetFrameHeight(), 0.0f ) ) )
                (void)RemoveStop( channels, rows[i] );
            track();
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::PopID();
        return gesture;
    }
} // namespace Desert::Editor::VFXCurveEdit
