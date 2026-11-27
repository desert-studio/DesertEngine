// BuildPool wire codec: the binary form of Protocol.hpp's Message. One field list per message type, walked by the
// same function for both directions, so encoding and decoding cannot drift apart.
#include "BuildPool/Protocol.hpp"

#include <cstring>
#include <type_traits>
#include <utility>

namespace buildpool
{
    namespace
    {
        static_assert( std::variant_size_v<Message> == 15, "a new message needs its field list in Fields()" );
        static_assert( std::variant_size_v<Message> <= 256, "the variant index is one byte" );

        class Writer
        {
        public:
            explicit Writer( std::vector<std::byte>& out ) noexcept : m_out( out )
            {
            }

            template <typename T>
            void Field( const T& value )
            {
                if constexpr ( std::is_same_v<T, std::string> )
                {
                    Unsigned( static_cast<uint32_t>( value.size() ), 4 );
                    const auto* bytes = reinterpret_cast<const std::byte*>( value.data() );
                    m_out.insert( m_out.end(), bytes, bytes + value.size() );
                }
                else if constexpr ( std::is_same_v<T, bool> )
                {
                    m_out.push_back( static_cast<std::byte>( value ? 1 : 0 ) );
                }
                else if constexpr ( std::is_enum_v<T> )
                {
                    using U = std::underlying_type_t<T>;
                    Unsigned( static_cast<std::make_unsigned_t<U>>( static_cast<U>( value ) ), sizeof( U ) );
                }
                else if constexpr ( std::is_same_v<T, int> )
                {
                    Unsigned( static_cast<uint32_t>( static_cast<int32_t>( value ) ), 4 );
                }
                else
                {
                    static_assert( std::is_unsigned_v<T>, "a field type the wire form does not know" );
                    Unsigned( value, sizeof( T ) );
                }
            }

        private:
            void Unsigned( uint64_t value, size_t width )
            {
                for ( size_t i = 0; i < width; ++i )
                {
                    m_out.push_back( static_cast<std::byte>( ( value >> ( 8 * i ) ) & 0xFFu ) );
                }
            }

            std::vector<std::byte>& m_out;
        };

        class Reader
        {
        public:
            explicit Reader( std::span<const std::byte> bytes ) noexcept : m_bytes( bytes )
            {
            }

            template <typename T>
            void Field( T& value )
            {
                if ( !m_error.empty() )
                {
                    return;
                }
                if constexpr ( std::is_same_v<T, std::string> )
                {
                    uint64_t length = 0;
                    if ( !Unsigned( length, 4 ) )
                    {
                        return;
                    }
                    if ( length > m_bytes.size() - m_at )
                    {
                        m_error = "a string of " + std::to_string( length ) + " bytes is cut short";
                        return;
                    }
                    value.assign( reinterpret_cast<const char*>( m_bytes.data() + m_at ),
                                  static_cast<size_t>( length ) );
                    m_at += static_cast<size_t>( length );
                }
                else if constexpr ( std::is_same_v<T, bool> )
                {
                    uint64_t raw = 0;
                    if ( !Unsigned( raw, 1 ) )
                    {
                        return;
                    }
                    if ( raw > 1 )
                    {
                        m_error = "a bool field holds " + std::to_string( raw );
                        return;
                    }
                    value = raw == 1;
                }
                else if constexpr ( std::is_enum_v<T> )
                {
                    using U      = std::underlying_type_t<T>;
                    uint64_t raw = 0;
                    if ( !Unsigned( raw, sizeof( U ) ) )
                    {
                        return;
                    }
                    if ( raw > static_cast<uint64_t>( MaxOf<T>() ) )
                    {
                        m_error = "an enum field holds " + std::to_string( raw ) + ", past its last value " +
                                  std::to_string( static_cast<uint64_t>( MaxOf<T>() ) );
                        return;
                    }
                    value = static_cast<T>( static_cast<U>( raw ) );
                }
                else if constexpr ( std::is_same_v<T, int> )
                {
                    uint64_t raw = 0;
                    if ( Unsigned( raw, 4 ) )
                    {
                        value = static_cast<int>( static_cast<int32_t>( static_cast<uint32_t>( raw ) ) );
                    }
                }
                else
                {
                    static_assert( std::is_unsigned_v<T>, "a field type the wire form does not know" );
                    uint64_t raw = 0;
                    if ( Unsigned( raw, sizeof( T ) ) )
                    {
                        value = static_cast<T>( raw );
                    }
                }
            }

            bool Unsigned( uint64_t& value, size_t width )
            {
                if ( m_bytes.size() - m_at < width )
                {
                    m_error = "the message is cut short at byte " + std::to_string( m_at );
                    return false;
                }
                value = 0;
                for ( size_t i = 0; i < width; ++i )
                {
                    value |= static_cast<uint64_t>( std::to_integer<uint8_t>( m_bytes[m_at + i] ) ) << ( 8 * i );
                }
                m_at += width;
                return true;
            }

            bool AtEnd() const noexcept
            {
                return m_at == m_bytes.size();
            }
            size_t Offset() const noexcept
            {
                return m_at;
            }
            const std::string& ErrorText() const noexcept
            {
                return m_error;
            }

        private:
            // The last value of each enum the wire carries; an enum without an entry here does not compile.
            template <typename T>
            static constexpr T MaxOf()
            {
                if constexpr ( std::is_same_v<T, Stage> )
                {
                    return Stage::Control;
                }
                else if constexpr ( std::is_same_v<T, ActionKind> )
                {
                    return ActionKind::Other;
                }
                else if constexpr ( std::is_same_v<T, Query::What> )
                {
                    return Query::What::Stop;
                }
                else
                {
                    static_assert( std::is_same_v<T, OsPriority>, "an enum without its last value in MaxOf()" );
                    return OsPriority::Idle;
                }
            }

            std::span<const std::byte> m_bytes;
            size_t                     m_at = 0;
            std::string                m_error;
        };

        // The fields of each message, in declaration order. `M` is the message (const for Encode), `io` a Writer
        // or a Reader.
        template <typename M, typename IO>
        void Fields( M& m, IO& io )
        {
            using T = std::remove_const_t<M>;
            static_cast<void>( m ); // Retry has no field
            static_cast<void>( io );
            if constexpr ( std::is_same_v<T, Hello> )
            {
                io.Field( m.protocol );
                io.Field( m.stage );
                io.Field( m.process.pid );
                io.Field( m.process.startTime );
                io.Field( m.session );
                io.Field( m.buildRoot );
                io.Field( m.className );
                io.Field( m.runLabel );
            }
            else if constexpr ( std::is_same_v<T, Acquire> )
            {
                io.Field( m.kind );
                io.Field( m.costKey );
                io.Field( m.inflightKey );
                io.Field( m.restartable );
                io.Field( m.retried );
            }
            else if constexpr ( std::is_same_v<T, Note> || std::is_same_v<T, Text> )
            {
                io.Field( m.text );
            }
            else if constexpr ( std::is_same_v<T, Heartbeat> )
            {
                io.Field( m.currentBytes );
            }
            else if constexpr ( std::is_same_v<T, Released> )
            {
                io.Field( m.exitCode );
                io.Field( m.peakBytes );
                io.Field( m.wallMs );
                io.Field( m.cpuMs );
                io.Field( m.preempted );
            }
            else if constexpr ( std::is_same_v<T, CacheExited> )
            {
                io.Field( m.exitCode );
            }
            else if constexpr ( std::is_same_v<T, Query> )
            {
                io.Field( m.what );
            }
            else if constexpr ( std::is_same_v<T, Welcome> )
            {
                io.Field( m.session );
                io.Field( m.note );
            }
            else if constexpr ( std::is_same_v<T, Grant> )
            {
                io.Field( m.lease );
                io.Field( m.priority );
            }
            else if constexpr ( std::is_same_v<T, Await> )
            {
                io.Field( m.leaderBuildRoot );
            }
            else if constexpr ( std::is_same_v<T, Retry> )
            {
                // no fields
            }
            else if constexpr ( std::is_same_v<T, Preempt> )
            {
                io.Field( m.lease );
            }
            else if constexpr ( std::is_same_v<T, Verdict> )
            {
                io.Field( m.runCacheAgain );
            }
            else
            {
                static_assert( std::is_same_v<T, Refused>, "a message without its field list in Fields()" );
                io.Field( m.reason );
            }
        }

        template <size_t I>
        Result<Message> DecodeAlternative( size_t index, Reader& reader )
        {
            if constexpr ( I == std::variant_size_v<Message> )
            {
                return Fail( "unknown message index " + std::to_string( index ) );
            }
            else
            {
                if ( index != I )
                {
                    return DecodeAlternative<I + 1>( index, reader );
                }
                std::variant_alternative_t<I, Message> message;
                Fields( message, reader );
                if ( !reader.ErrorText().empty() )
                {
                    return Fail( reader.ErrorText() );
                }
                return Message( std::in_place_index<I>, std::move( message ) );
            }
        }
    } // namespace

    std::vector<std::byte> Encode( const Message& message )
    {
        std::vector<std::byte> out;
        out.push_back( static_cast<std::byte>( message.index() ) );
        Writer writer( out );
        std::visit( [&writer]( const auto& alternative ) { Fields( alternative, writer ); }, message );
        return out;
    }

    Result<Message> Decode( std::span<const std::byte> bytes ) noexcept
    {
        Reader   reader( bytes );
        uint64_t index = 0;
        if ( !reader.Unsigned( index, 1 ) )
        {
            return Fail( "an empty message" );
        }
        Result<Message> decoded = DecodeAlternative<0>( static_cast<size_t>( index ), reader );
        if ( !decoded )
        {
            return decoded;
        }
        if ( !reader.AtEnd() )
        {
            return Fail( std::to_string( bytes.size() - reader.Offset() ) +
                         " trailing bytes after message index " + std::to_string( index ) );
        }
        return decoded;
    }
} // namespace buildpool
