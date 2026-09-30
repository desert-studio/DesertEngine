#include "Track.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
    namespace
    {
        Common::BoolResultStr CheckIndex( const Track& track, const size_t index )
        {
            if ( index >= track.Sections.size() )
            {
                return Common::MakeFormattedError<bool>( "section {} of {}: there is no such section", index,
                                                         track.Sections.size() );
            }
            return Common::MakeSuccess( true );
        }

        /// The one spelling of the range rule; the edits below call it, none re-states it.
        Common::BoolResultStr CheckRange( const FrameNumber start, const FrameNumber end )
        {
            if ( end < start )
            {
                return Common::MakeFormattedError<bool>( "a section ends before it starts: [{}, {}]", start.Value,
                                                         end.Value );
            }
            return Common::MakeSuccess( true );
        }

        /// A Camera Cut row never holds two overlapping cuts (Section.hpp, `Validate`): the section at
        /// @p index, placed at [start, end] on @p row, against every OTHER section of the track.
        Common::BoolResultStr CheckCutRow( const Track& track, const size_t index, const FrameNumber start,
                                           const FrameNumber end, const int32_t row )
        {
            if ( track.Kind != TrackKind::CameraCut )
            {
                return Common::MakeSuccess( true );
            }
            for ( size_t other = 0; other < track.Sections.size(); ++other )
            {
                const Section& existing = track.Sections[other];
                if ( other != index && existing.Row == row && !( end < existing.Start ) &&
                     !( existing.End < start ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "[{}, {}] overlaps Camera Cut section {} on row {}", start.Value, end.Value, other, row );
                }
            }
            return Common::MakeSuccess( true );
        }

        template <typename Visit>
        void ForEachKeyTick( FloatChannel& channel, Visit&& visit )
        {
            for ( ScalarKey& key : channel.Keys )
            {
                visit( key.Tick );
            }
        }

        /// Every tick a section's content owns — the keys MoveSection carries with it.
        template <typename Visit>
        void ForEachKeyTick( SectionContent& content, Visit&& visit )
        {
            std::visit(
                 [&visit]( auto& held )
                 {
                     using Held = std::decay_t<decltype( held )>;
                     if constexpr ( std::is_same_v<Held, Channel> )
                     {
                         std::visit(
                              [&visit]( auto& channel )
                              {
                                  using Kind = std::decay_t<decltype( channel )>;
                                  if constexpr ( std::is_same_v<Kind, FloatChannel> )
                                  {
                                      ForEachKeyTick( channel, visit );
                                  }
                                  else if constexpr ( std::is_same_v<Kind, VectorChannel> )
                                  {
                                      ForEachKeyTick( channel.X, visit );
                                      ForEachKeyTick( channel.Y, visit );
                                      ForEachKeyTick( channel.Z, visit );
                                  }
                                  else if constexpr ( std::is_same_v<Kind, RotationChannel> )
                                  {
                                      ForEachKeyTick( channel.X, visit );
                                      ForEachKeyTick( channel.Y, visit );
                                      ForEachKeyTick( channel.Z, visit );
                                      ForEachKeyTick( channel.W, visit );
                                  }
                                  else if constexpr ( std::is_same_v<Kind, TransformChannel> )
                                  {
                                      for ( FloatChannel* component :
                                            { &channel.Translation.X, &channel.Translation.Y,
                                              &channel.Translation.Z, &channel.Rotation.X, &channel.Rotation.Y,
                                              &channel.Rotation.Z, &channel.Rotation.W, &channel.Scale.X,
                                              &channel.Scale.Y, &channel.Scale.Z } )
                                      {
                                          ForEachKeyTick( *component, visit );
                                      }
                                  }
                                  else if constexpr ( std::is_same_v<Kind, BoolChannel> )
                                  {
                                      ForEachKeyTick( channel.Bits, visit );
                                  }
                                  else
                                  {
                                      static_assert( std::is_same_v<Kind, EventChannel> );
                                      for ( EventKey& key : channel.Keys )
                                      {
                                          visit( key.Tick );
                                      }
                                  }
                              },
                              held );
                     }
                     // Animation: StartOffset is in the clip's ticks, relative to the section — it moves
                     // with the section by construction. Camera Cut: no keys.
                 },
                 content );
        }

        template <typename Visit>
        void ForEachSectionTick( Section& section, Visit&& visit )
        {
            visit( section.Start );
            visit( section.End );
            for ( ScalarKey& key : section.Weight )
            {
                visit( key.Tick );
            }
            ForEachKeyTick( section.Content, visit );
        }
    } // namespace

    Common::BoolResultStr MoveSection( Track& track, const size_t index, const int32_t deltaTicks )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        Section& section = track.Sections[index];

        // REFUSED WHOLE, never clamped: a move that pinned one end at the frame type's limit would silently
        // become a resize, the one thing a move exists not to do. Checked before anything is written.
        bool fits = true;
        ForEachSectionTick( section,
                            [&fits, deltaTicks]( const FrameNumber& tick )
                            {
                                const int64_t moved = static_cast<int64_t>( tick.Value ) + deltaTicks;
                                fits = fits && moved >= std::numeric_limits<int32_t>::min() &&
                                       moved <= std::numeric_limits<int32_t>::max();
                            } );
        if ( !fits )
        {
            return Common::MakeFormattedError<bool>( "moving section {} by {} ticks leaves the frame range", index,
                                                     deltaTicks );
        }
        const FrameNumber start{ section.Start.Value + deltaTicks };
        const FrameNumber end{ section.End.Value + deltaTicks };
        if ( auto cut = CheckCutRow( track, index, start, end, section.Row ); !cut.IsSuccess() )
        {
            return cut;
        }

        ForEachSectionTick( section, [deltaTicks]( FrameNumber& tick ) { tick.Value += deltaTicks; } );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetSectionRange( Track& track, const size_t index, const FrameNumber start,
                                           const FrameNumber end )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        if ( auto range = CheckRange( start, end ); !range.IsSuccess() )
        {
            return range;
        }
        Section& section = track.Sections[index];
        if ( auto cut = CheckCutRow( track, index, start, end, section.Row ); !cut.IsSuccess() )
        {
            return cut;
        }
        section.Start = start;
        section.End   = end;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetSectionRow( Track& track, const size_t index, const int32_t row )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        if ( row < 0 )
        {
            return Common::MakeFormattedError<bool>( "row {} is above the first row, which is 0", row );
        }
        Section& section = track.Sections[index];
        if ( auto cut = CheckCutRow( track, index, section.Start, section.End, row ); !cut.IsSuccess() )
        {
            return cut;
        }
        section.Row = row;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveSection( Track& track, const size_t index )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        track.Sections.erase( track.Sections.begin() + static_cast<std::ptrdiff_t>( index ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetSectionWeightKey( Track& track, const size_t index, const FrameNumber tick,
                                               const float value )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        if ( !std::isfinite( value ) )
        {
            return Common::MakeError<bool>( "a section weight has to be a number" );
        }
        if ( track.Kind == TrackKind::CameraCut )
        {
            return Common::MakeError<bool>( "a Camera Cut is Absolute at full weight: it has no weight to key" );
        }

        std::vector<ScalarKey>& weight = track.Sections[index].Weight;
        const auto              at = std::lower_bound( weight.begin(), weight.end(), tick,
                                                       []( const ScalarKey& key, const FrameNumber want )
                                                       { return key.Tick < want; } );
        if ( at != weight.end() && !( tick < at->Tick ) )
        {
            // An existing key keeps its shape: changing a value is not permission to discard the slope
            // somebody authored (TrackEditing::SetTransformKey's rule).
            at->Value = value;
            return Common::MakeSuccess( true );
        }

        ScalarKey key;
        key.Tick   = tick;
        key.Value  = value;
        key.Interp = KeyInterp::Linear;
        key.Mode   = TangentMode::Auto;
        weight.insert( at, key );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveSectionWeightKey( Track& track, const size_t index, const size_t keyIndex )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        std::vector<ScalarKey>& weight = track.Sections[index].Weight;
        if ( keyIndex >= weight.size() )
        {
            return Common::MakeFormattedError<bool>( "weight key {} of {}: there is no such key", keyIndex,
                                                     weight.size() );
        }
        weight.erase( weight.begin() + static_cast<std::ptrdiff_t>( keyIndex ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ClearSectionWeight( Track& track, const size_t index )
    {
        if ( auto valid = CheckIndex( track, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        track.Sections[index].Weight.clear();
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation::Timeline
