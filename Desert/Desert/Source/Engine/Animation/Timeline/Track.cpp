#include "Track.hpp"

#include <type_traits>

namespace Desert::Animation::Timeline
{
    const char* ToString( const TrackKind kind )
    {
        switch ( kind )
        {
            case TrackKind::Float:
                return "Float";
            case TrackKind::Vector:
                return "Vector";
            case TrackKind::Rotation:
                return "Rotation";
            case TrackKind::Transform:
                return "Transform";
            case TrackKind::Bool:
                return "Bool";
            case TrackKind::Event:
                return "Event";
            case TrackKind::Animation:
                return "Animation";
            case TrackKind::CameraCut:
                return "CameraCut";
        }
        // A kind outside the enum is refused where it is READ; this names it for the log that says so.
        return "Unknown";
    }

    TrackKind TrackKindOf( const SectionContent& content )
    {
        return std::visit(
             []( const auto& held ) -> TrackKind
             {
                 using Held = std::decay_t<decltype( held )>;
                 if constexpr ( std::is_same_v<Held, Channel> )
                 {
                     // The first six track kinds ARE the channel kinds, value for value (Track.hpp).
                     return static_cast<TrackKind>( KindOf( held ) );
                 }
                 else if constexpr ( std::is_same_v<Held, AnimationSectionContent> )
                 {
                     return TrackKind::Animation;
                 }
                 else
                 {
                     static_assert( std::is_same_v<Held, CameraCutSectionContent> );
                     return TrackKind::CameraCut;
                 }
             },
             content );
    }

    namespace
    {
        SectionContent RestContentOf( const TrackKind kind )
        {
            switch ( kind )
            {
                case TrackKind::Animation:
                    return AnimationSectionContent{};
                case TrackKind::CameraCut:
                    return CameraCutSectionContent{};
                default:
                    return MakeChannel( static_cast<ChannelKind>( kind ) );
            }
        }
    } // namespace

    Section& AddSection( Track& track, const FrameNumber start, const FrameNumber end )
    {
        // The lowest row the new range overlaps nothing on (UE: FindNextAvailableRow). Overlap is kept to
        // what the author asks for by moving a section onto a row, never produced by adding one — and a
        // Camera Cut row must never hold two overlapping cuts (Section.hpp).
        int32_t row = 0;
        for ( bool clash = true; clash; )
        {
            clash = false;
            for ( const Section& existing : track.Sections )
            {
                if ( existing.Row == row && !( end < existing.Start ) && !( existing.End < start ) )
                {
                    clash = true;
                    ++row;
                    break;
                }
            }
        }

        Section section;
        section.Start   = start;
        section.End     = end;
        section.Row     = row;
        section.Content = RestContentOf( track.Kind );
        track.Sections.push_back( std::move( section ) );
        return track.Sections.back();
    }
} // namespace Desert::Animation::Timeline
