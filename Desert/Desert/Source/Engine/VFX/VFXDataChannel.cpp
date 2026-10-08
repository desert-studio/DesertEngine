#include "VFXDataChannel.hpp"

#include <Common/Core/Logger.hpp>

#include <cmath>
#include <format>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    namespace
    {
        // The Int field's spelling in the float payload is exact below 2^24.
        constexpr float kExactIntLimit = 16777216.0f;

        const char* TypeName( const S::VFXDataChannelFieldType type )
        {
            switch ( type )
            {
                case S::VFXDataChannelFieldType::Position:
                    return "Position";
                case S::VFXDataChannelFieldType::Direction:
                    return "Direction";
                case S::VFXDataChannelFieldType::Color:
                    return "Color";
                case S::VFXDataChannelFieldType::Float:
                    return "Float";
                case S::VFXDataChannelFieldType::Int:
                    return "Int";
            }
            return "?";
        }

        bool Passes( const VFXChannelFilterOp op, const float value, const float bound )
        {
            switch ( op )
            {
                case VFXChannelFilterOp::Less:
                    return value < bound;
                case VFXChannelFilterOp::LessEqual:
                    return value <= bound;
                case VFXChannelFilterOp::Greater:
                    return value > bound;
                case VFXChannelFilterOp::GreaterEqual:
                    return value >= bound;
                case VFXChannelFilterOp::Equal:
                    return value == bound;
                case VFXChannelFilterOp::NotEqual:
                    return value != bound;
            }
            return false;
        }

        Common::BoolResultStr CheckField( const VFXDataChannelLayout& layout, const std::string& field,
                                          const char* role, const bool numeric,
                                          const S::VFXDataChannelFieldType type )
        {
            if ( field.empty() )
                return BOOLSUCCESS;
            const int i = layout.Find( field );
            if ( i < 0 )
                return Common::MakeFormattedError<bool>( "{} field '{}' is not in the channel", role, field );
            const S::VFXDataChannelFieldType has = layout.Fields[static_cast<std::size_t>( i )].Type;
            const bool                       ok =
                 numeric ? ( has == S::VFXDataChannelFieldType::Float || has == S::VFXDataChannelFieldType::Int )
                                               : has == type;
            if ( !ok )
                return Common::MakeFormattedError<bool>( "{} field '{}' is {}, the role takes {}", role, field,
                                                         TypeName( has ),
                                                         numeric ? "Float or Int" : TypeName( type ) );
            return BOOLSUCCESS;
        }
    } // namespace

    VFXDataChannelLayout VFXDataChannelLayout::From( const S::VFXDataChannelData& data )
    {
        VFXDataChannelLayout layout;
        layout.Fields = data.Fields;
        for ( const S::VFXDataChannelField& f : data.Fields )
        {
            layout.Offsets.push_back( layout.Stride );
            layout.Stride += S::FloatCount( f.Type );
        }
        return layout;
    }

    int VFXDataChannelLayout::Find( const std::string_view name ) const
    {
        for ( std::size_t i = 0; i < Fields.size(); ++i )
            if ( Fields[i].Name == name )
                return static_cast<int>( i );
        return -1;
    }

    std::size_t VFXDataChannel::Append( const std::size_t count )
    {
        const std::size_t first = EntryCount();
        m_Entries.resize( m_Entries.size() + count * m_Layout.Stride, 0.0f );
        return first;
    }

    Common::BoolResultStr VFXDataChannel::Set( const std::size_t entry, const std::string_view field,
                                               const S::VFXDataChannelFieldType type, const glm::vec4& value )
    {
        if ( entry >= EntryCount() )
            return Common::MakeFormattedError<bool>( "entry {} of {}", entry, EntryCount() );
        const int i = m_Layout.Find( field );
        if ( i < 0 )
            return Common::MakeFormattedError<bool>( "the channel has no field '{}'", field );
        const auto at = static_cast<std::size_t>( i );
        if ( m_Layout.Fields[at].Type != type )
            return Common::MakeFormattedError<bool>( "field '{}' is {}, written as {}", field,
                                                     TypeName( m_Layout.Fields[at].Type ), TypeName( type ) );
        float* out = m_Entries.data() + entry * m_Layout.Stride + m_Layout.Offsets[at];
        for ( uint32_t c = 0; c < S::FloatCount( type ); ++c )
            out[c] = value[static_cast<glm::length_t>( c )];
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXDataChannelWriter::Write( const std::size_t index, const std::string_view field,
                                                       const S::VFXDataChannelFieldType type,
                                                       const glm::vec4&                 value )
    {
        if ( index >= m_Count )
            return Common::MakeFormattedError<bool>( "writer entry {} of {}", index, m_Count );
        return m_Channel->Set( m_First + index, field, type, value );
    }

    Common::BoolResultStr VFXDataChannelWriter::WritePosition( const std::size_t      index,
                                                               const std::string_view field, const glm::vec3& cm )
    {
        return Write( index, field, S::VFXDataChannelFieldType::Position, glm::vec4( cm, 0.0f ) );
    }

    Common::BoolResultStr VFXDataChannelWriter::WriteDirection( const std::size_t      index,
                                                                const std::string_view field,
                                                                const glm::vec3&       direction )
    {
        return Write( index, field, S::VFXDataChannelFieldType::Direction, glm::vec4( direction, 0.0f ) );
    }

    Common::BoolResultStr VFXDataChannelWriter::WriteColor( const std::size_t index, const std::string_view field,
                                                            const glm::vec4& rgba )
    {
        return Write( index, field, S::VFXDataChannelFieldType::Color, rgba );
    }

    Common::BoolResultStr VFXDataChannelWriter::WriteFloat( const std::size_t index, const std::string_view field,
                                                            const float value )
    {
        return Write( index, field, S::VFXDataChannelFieldType::Float, glm::vec4( value, 0.0f, 0.0f, 0.0f ) );
    }

    Common::BoolResultStr VFXDataChannelWriter::WriteInt( const std::size_t index, const std::string_view field,
                                                          const std::int32_t value )
    {
        const auto asFloat = static_cast<float>( value );
        if ( !( std::fabs( asFloat ) < kExactIntLimit ) )
            return Common::MakeFormattedError<bool>( "Int field '{}': {} is not exact in the payload (|v| < 2^24)",
                                                     field, value );
        return Write( index, field, S::VFXDataChannelFieldType::Int, glm::vec4( asFloat, 0.0f, 0.0f, 0.0f ) );
    }

    Common::BoolResultStr VFXDataChannels::Register( const std::string& name, const S::VFXDataChannelData& data )
    {
        if ( name.empty() )
            return Common::MakeFormattedError<bool>( "a data channel needs a name" );
        if ( auto valid = S::ValidateVFXDataChannelData( data ); !valid )
            return Common::MakeFormattedError<bool>( "data channel '{}': {}", name, valid.GetError() );
        const auto it = m_Channels.find( name );
        if ( it != m_Channels.end() )
        {
            if ( it->second.Layout().Fields == data.Fields )
                return BOOLSUCCESS;
            if ( it->second.EntryCount() > 0 )
                return Common::MakeFormattedError<bool>(
                     "data channel '{}' changes its layout while {} entries of the old one are pending", name,
                     it->second.EntryCount() );
            m_Channels.erase( it );
        }
        m_Channels.emplace( name, VFXDataChannel( VFXDataChannelLayout::From( data ) ) );
        return BOOLSUCCESS;
    }

    const VFXDataChannel* VFXDataChannels::Find( const std::string_view name ) const
    {
        const auto it = m_Channels.find( name );
        return it != m_Channels.end() ? &it->second : nullptr;
    }

    Common::ResultStr<VFXDataChannelWriter> VFXDataChannels::Write( const std::string_view name,
                                                                    const std::size_t      count )
    {
        const auto it = m_Channels.find( name );
        if ( it == m_Channels.end() )
            return Common::MakeFormattedError<VFXDataChannelWriter>( "no data channel '{}' is registered", name );
        const std::size_t first = it->second.Append( count );
        return Common::MakeSuccess( VFXDataChannelWriter( it->second, first, count ) );
    }

    void VFXDataChannels::ClearEntries()
    {
        for ( auto& entry : m_Channels )
            entry.second.ClearEntries();
    }

    std::uint32_t VFXChannelSpawnBatch::ParticleCount() const
    {
        std::uint32_t n = 0;
        for ( const VFXChannelSpawnRequest& r : Requests )
            n += r.Count;
        return n;
    }

    Common::BoolResultStr BindChannelSpawn( const VFXChannelSpawnModule& module,
                                            const VFXDataChannelLayout&  layout )
    {
        if ( auto r = CheckField( layout, module.PositionField, "Position", false,
                                  S::VFXDataChannelFieldType::Position );
             !r )
            return r;
        if ( auto r = CheckField( layout, module.DirectionField, "Direction", false,
                                  S::VFXDataChannelFieldType::Direction );
             !r )
            return r;
        if ( auto r = CheckField( layout, module.FilterField, "Filter", true, S::VFXDataChannelFieldType::Float );
             !r )
            return r;
        if ( module.MaxDistance > 0.0f && module.PositionField.empty() )
            return Common::MakeFormattedError<bool>( "MaxDistance {} cm has no Position field to measure",
                                                     module.MaxDistance );
        return BOOLSUCCESS;
    }

    VFXChannelSpawnBatch GatherChannelSpawns( const VFXChannelSpawnModule& module, const VFXDataChannel* channel,
                                              const glm::vec3& emitterCm )
    {
        VFXChannelSpawnBatch batch;
        if ( channel == nullptr )
        {
            batch.Report.ChannelMissing = true;
            return batch;
        }
        const VFXDataChannelLayout& layout = channel->Layout();
        if ( auto bound = BindChannelSpawn( module, layout ); !bound )
        {
            LOG_ERROR( "[VFX] Spawn from Channel '{}' spawns nothing: {}", module.Channel, bound.GetError() );
            batch.Report.ChannelMissing = true;
            return batch;
        }
        const auto offsetOf = [&]( const std::string& field ) -> int
        {
            const int i = layout.Find( field );
            return i < 0 ? -1 : static_cast<int>( layout.Offsets[static_cast<std::size_t>( i )] );
        };
        const int   position      = offsetOf( module.PositionField );
        const int   direction     = offsetOf( module.DirectionField );
        const int   filter        = offsetOf( module.FilterField );
        const float maxDistanceSq = module.MaxDistance * module.MaxDistance;

        const std::size_t count = channel->EntryCount();
        batch.Report.Read       = static_cast<std::uint32_t>( count );
        for ( std::size_t e = 0; e < count; ++e )
        {
            const std::span<const float> entry = channel->Entry( e );
            VFXChannelSpawnRequest       request;
            request.Entry = static_cast<std::uint32_t>( e );
            request.Count = module.ParticlesPerEntry;
            if ( position >= 0 )
            {
                const auto P        = static_cast<std::size_t>( position );
                request.HasPosition = true;
                request.Position    = glm::vec3( entry[P], entry[P + 1], entry[P + 2] );
            }
            if ( direction >= 0 )
            {
                const auto D         = static_cast<std::size_t>( direction );
                request.HasDirection = true;
                request.Direction    = glm::vec3( entry[D], entry[D + 1], entry[D + 2] );
            }
            if ( module.MaxDistance > 0.0f )
            {
                const glm::vec3 d = request.Position - emitterCm;
                if ( glm::dot( d, d ) > maxDistanceSq )
                {
                    ++batch.Report.Filtered;
                    continue;
                }
            }
            if ( filter >= 0 &&
                 !Passes( module.FilterOp, entry[static_cast<std::size_t>( filter )], module.FilterValue ) )
            {
                ++batch.Report.Filtered;
                continue;
            }
            if ( batch.Report.Spawned >= module.MaxEntriesPerFrame )
            {
                ++batch.Report.Overflow;
                continue;
            }
            ++batch.Report.Spawned;
            batch.Requests.push_back( request );
        }
        return batch;
    }
} // namespace Desert::VFX
