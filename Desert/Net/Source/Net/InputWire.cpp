#include "InputWire.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Desert::Net
{
    static_assert( kMaxUnackedInputSteps <= 255, "a run's step count is one byte" );
    static_assert( kMaxInputAxes <= 255, "a step's axis count is one byte" );

    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        // One step on the lattice, as the integers the wire carries. Equality of two steps is equality of these.
        struct QuantizedStep
        {
            std::array<std::int8_t, 3>             Move{};
            std::int16_t                           Yaw       = 0;
            std::uint32_t                          Actions   = 0;
            std::uint8_t                           AxisCount = 0;
            std::array<std::int8_t, kMaxInputAxes> Axes{};
        };

        // A finite -1..1 intent on the int8 lattice (Quantize refuses a non-finite one).
        std::int8_t QuantizeIntent( float value )
        {
            const float clamped = std::clamp( value, -1.f, 1.f );
            return static_cast<std::int8_t>( std::lround( clamped * kMoveScale ) );
        }

        float DequantizeIntent( std::int8_t q )
        {
            return static_cast<float>( q ) / kMoveScale;
        }

        // A finite yaw in radians, wrapped to [-pi, pi), on the int16 lattice. +pi rounds to the same point as -pi.
        std::int16_t QuantizeYaw( float yaw )
        {
            float wrapped = std::fmod( yaw + kPi, 2.f * kPi );
            if ( wrapped < 0.f )
                wrapped += 2.f * kPi;
            wrapped -= kPi;
            long q = std::lround( wrapped * kYawScale );
            if ( q >= 32768 )
                q -= 65536;
            else if ( q < -32768 )
                q += 65536;
            return static_cast<std::int16_t>( q );
        }

        float DequantizeYaw( std::int16_t q )
        {
            return static_cast<float>( q ) / kYawScale;
        }

        // False when the step is not one the wire carries: more axes than it allows, or a non-finite field (a NaN
        // intent has no direction - it is refused, never sent as zero).
        bool Quantize( const InputStep& step, QuantizedStep& q )
        {
            if ( step.Axes.size() > kMaxInputAxes || !std::isfinite( step.Move.X ) || !std::isfinite( step.Move.Y ) ||
                 !std::isfinite( step.Move.Z ) || !std::isfinite( step.Yaw ) ||
                 !std::all_of( step.Axes.begin(), step.Axes.end(), []( float a ) { return std::isfinite( a ); } ) )
                return false;
            q.Move      = { QuantizeIntent( step.Move.X ), QuantizeIntent( step.Move.Y ),
                            QuantizeIntent( step.Move.Z ) };
            q.Yaw       = QuantizeYaw( step.Yaw );
            q.Actions   = step.Actions;
            q.AxisCount = static_cast<std::uint8_t>( step.Axes.size() );
            q.Axes      = {};
            for ( std::size_t a = 0; a < step.Axes.size(); ++a )
                q.Axes[a] = QuantizeIntent( step.Axes[a] );
            return true;
        }

        InputStep Dequantize( const QuantizedStep& q, std::uint32_t number )
        {
            InputStep step;
            step.Step    = number;
            step.Move    = { DequantizeIntent( q.Move[0] ), DequantizeIntent( q.Move[1] ),
                             DequantizeIntent( q.Move[2] ) };
            step.Yaw     = DequantizeYaw( q.Yaw );
            step.Actions = q.Actions;
            step.Axes.resize( q.AxisCount );
            for ( std::size_t a = 0; a < q.AxisCount; ++a )
                step.Axes[a] = DequantizeIntent( q.Axes[a] );
            return step;
        }

        bool SameAxes( const QuantizedStep& a, const QuantizedStep& b )
        {
            return a.AxisCount == b.AxisCount &&
                   std::equal( a.Axes.begin(), a.Axes.begin() + a.AxisCount, b.Axes.begin() );
        }

        std::uint8_t DeltaMask( const QuantizedStep& previous, const QuantizedStep& step )
        {
            std::uint8_t mask = 0;
            if ( step.Move != previous.Move )
                mask |= kDeltaMove;
            if ( step.Yaw != previous.Yaw )
                mask |= kDeltaYaw;
            if ( step.Actions != previous.Actions )
                mask |= kDeltaActions;
            if ( !SameAxes( previous, step ) )
                mask |= kDeltaAxes;
            return mask;
        }

        std::size_t FieldBytes( const QuantizedStep& q, std::uint8_t mask )
        {
            std::size_t bytes = 0;
            if ( mask & kDeltaMove )
                bytes += 3;
            if ( mask & kDeltaYaw )
                bytes += 2;
            if ( mask & kDeltaActions )
                bytes += 4;
            if ( mask & kDeltaAxes )
                bytes += 1 + q.AxisCount;
            return bytes;
        }

        // --- little-endian byte writers / readers over the caller's buffer ---

        void PutU8( std::vector<std::byte>& out, std::uint8_t v )
        {
            out.push_back( static_cast<std::byte>( v ) );
        }
        void PutU16( std::vector<std::byte>& out, std::uint16_t v )
        {
            PutU8( out, static_cast<std::uint8_t>( v & 0xFFu ) );
            PutU8( out, static_cast<std::uint8_t>( v >> 8 ) );
        }
        void PutU32( std::vector<std::byte>& out, std::uint32_t v )
        {
            for ( int i = 0; i < 4; ++i )
                PutU8( out, static_cast<std::uint8_t>( ( v >> ( 8 * i ) ) & 0xFFu ) );
        }
        void PutI8( std::vector<std::byte>& out, std::int8_t v )
        {
            PutU8( out, static_cast<std::uint8_t>( v ) );
        }

        bool TakeU8( std::span<const std::byte>& in, std::uint8_t& v )
        {
            if ( in.empty() )
                return false;
            v  = static_cast<std::uint8_t>( in[0] );
            in = in.subspan( 1 );
            return true;
        }
        bool TakeU16( std::span<const std::byte>& in, std::uint16_t& v )
        {
            if ( in.size() < 2 )
                return false;
            v  = static_cast<std::uint16_t>( static_cast<std::uint16_t>( in[0] ) |
                                             ( static_cast<std::uint16_t>( in[1] ) << 8 ) );
            in = in.subspan( 2 );
            return true;
        }
        bool TakeU32( std::span<const std::byte>& in, std::uint32_t& v )
        {
            if ( in.size() < 4 )
                return false;
            v = 0;
            for ( int i = 0; i < 4; ++i )
                v |= static_cast<std::uint32_t>( in[i] ) << ( 8 * i );
            in = in.subspan( 4 );
            return true;
        }
        bool TakeI8( std::span<const std::byte>& in, std::int8_t& v )
        {
            std::uint8_t raw = 0;
            if ( !TakeU8( in, raw ) )
                return false;
            v = static_cast<std::int8_t>( raw );
            return true;
        }

        // --- the fields, in the FULL order (InputWire.hpp RUN LAYOUT) ---

        void PutMove( std::vector<std::byte>& out, const QuantizedStep& q )
        {
            for ( const std::int8_t m : q.Move )
                PutI8( out, m );
        }
        void PutYaw( std::vector<std::byte>& out, const QuantizedStep& q )
        {
            PutU16( out, static_cast<std::uint16_t>( q.Yaw ) );
        }
        void PutAxes( std::vector<std::byte>& out, const QuantizedStep& q )
        {
            PutU8( out, q.AxisCount );
            for ( std::size_t a = 0; a < q.AxisCount; ++a )
                PutI8( out, q.Axes[a] );
        }
        void PutFields( std::vector<std::byte>& out, const QuantizedStep& q, std::uint8_t mask )
        {
            if ( mask & kDeltaMove )
                PutMove( out, q );
            if ( mask & kDeltaYaw )
                PutYaw( out, q );
            if ( mask & kDeltaActions )
                PutU32( out, q.Actions );
            if ( mask & kDeltaAxes )
                PutAxes( out, q );
        }

        bool TakeMove( std::span<const std::byte>& in, QuantizedStep& q )
        {
            return TakeI8( in, q.Move[0] ) && TakeI8( in, q.Move[1] ) && TakeI8( in, q.Move[2] );
        }
        bool TakeYaw( std::span<const std::byte>& in, QuantizedStep& q )
        {
            std::uint16_t raw = 0;
            if ( !TakeU16( in, raw ) )
                return false;
            q.Yaw = static_cast<std::int16_t>( raw );
            return true;
        }
        bool TakeAxes( std::span<const std::byte>& in, QuantizedStep& q )
        {
            if ( !TakeU8( in, q.AxisCount ) || q.AxisCount > kMaxInputAxes )
                return false;
            q.Axes = {};
            for ( std::size_t a = 0; a < q.AxisCount; ++a )
                if ( !TakeI8( in, q.Axes[a] ) )
                    return false;
            return true;
        }
        bool TakeFields( std::span<const std::byte>& in, QuantizedStep& q, std::uint8_t mask )
        {
            if ( ( mask & kDeltaMove ) && !TakeMove( in, q ) )
                return false;
            if ( ( mask & kDeltaYaw ) && !TakeYaw( in, q ) )
                return false;
            if ( ( mask & kDeltaActions ) && !TakeU32( in, q.Actions ) )
                return false;
            if ( ( mask & kDeltaAxes ) && !TakeAxes( in, q ) )
                return false;
            return true;
        }

        // What @p count unchanged steps in a row cost: one mask-0 byte alone, one repeat record from two on.
        std::size_t UnchangedRunBytes( std::size_t count )
        {
            return count >= kMinRepeatSteps ? kRepeatRecordBytes : count * kUnchangedStepBytes;
        }

        // Appends @p count unchanged steps in a row as UnchangedRunBytes prices them (count < 256: a run holds at
        // most kMaxUnackedInputSteps steps).
        void PutUnchangedRun( std::vector<std::byte>& out, std::size_t count )
        {
            if ( count >= kMinRepeatSteps )
            {
                PutU8( out, kDeltaRepeat );
                PutU8( out, static_cast<std::uint8_t>( count ) );
                return;
            }
            for ( std::size_t i = 0; i < count; ++i )
                PutU8( out, 0 );
        }

        // The size of the run of @p steps, 0 when the run is refused (InputWire.hpp AppendInputRun).
        std::size_t RunBytes( std::span<const InputStep> steps )
        {
            if ( steps.empty() || steps.size() > kMaxUnackedInputSteps )
                return 0;
            std::size_t   bytes     = kRunHeadBytes;
            std::size_t   unchanged = 0; // the open run of unchanged steps, not yet counted
            QuantizedStep previous;
            for ( std::size_t i = 0; i < steps.size(); ++i )
            {
                QuantizedStep q;
                if ( steps[i].Step != steps[0].Step + static_cast<std::uint32_t>( i ) || !Quantize( steps[i], q ) )
                    return 0;
                if ( i == 0 )
                {
                    bytes += kFullStepBytes + q.AxisCount;
                }
                else
                {
                    const std::uint8_t mask = DeltaMask( previous, q );
                    if ( mask == 0 )
                    {
                        ++unchanged;
                    }
                    else
                    {
                        bytes += UnchangedRunBytes( unchanged ) + kUnchangedStepBytes + FieldBytes( q, mask );
                        unchanged = 0;
                    }
                }
                previous = q;
            }
            return bytes + UnchangedRunBytes( unchanged );
        }
    } // namespace

    InputStep OnWireLattice( const InputStep& step )
    {
        // A step the wire does not carry (too many axes, a non-finite field) is refused by the encoder, not
        // repaired here: the step keeps what it has, so the refusal reaches the sender.
        QuantizedStep q;
        if ( !Quantize( step, q ) )
            return step;
        return Dequantize( q, step.Step );
    }

    bool AppendInputRun( std::vector<std::byte>& out, std::span<const InputStep> steps )
    {
        const std::size_t bytes = RunBytes( steps );
        if ( bytes == 0 )
            return false;
        out.reserve( out.size() + bytes );
        PutU32( out, steps[0].Step );
        PutU8( out, static_cast<std::uint8_t>( steps.size() ) );
        std::size_t   unchanged = 0; // the open run of unchanged steps, not yet written
        QuantizedStep previous;
        for ( std::size_t i = 0; i < steps.size(); ++i )
        {
            QuantizedStep q;
            Quantize( steps[i], q ); // RunBytes accepted every step
            if ( i == 0 )
            {
                PutFields( out, q, kDeltaKnown );
            }
            else
            {
                const std::uint8_t mask = DeltaMask( previous, q );
                if ( mask == 0 )
                {
                    ++unchanged;
                }
                else
                {
                    PutUnchangedRun( out, unchanged );
                    unchanged = 0;
                    PutU8( out, mask );
                    PutFields( out, q, mask );
                }
            }
            previous = q;
        }
        PutUnchangedRun( out, unchanged );
        return true;
    }

    bool ReadInputRun( std::span<const std::byte>& in, std::vector<InputStep>& out )
    {
        std::uint32_t first = 0;
        std::uint8_t  count = 0;
        if ( !TakeU32( in, first ) || !TakeU8( in, count ) || count == 0 || count > kMaxUnackedInputSteps )
            return false;
        out.clear();
        out.reserve( count );
        QuantizedStep q;
        if ( !TakeFields( in, q, kDeltaKnown ) )
            return false;
        out.push_back( Dequantize( q, first ) );
        for ( std::uint32_t i = 1; i < count; )
        {
            std::uint8_t mask = 0;
            if ( !TakeU8( in, mask ) )
                return false;
            if ( mask == kDeltaRepeat )
            {
                // A repeat record: 2 .. (steps still to come) copies of the step before it. A shorter record is
                // not canonical, a longer one runs past the run's count - both are malformed.
                std::uint8_t repeat = 0;
                if ( !TakeU8( in, repeat ) || repeat < kMinRepeatSteps || repeat > count - i )
                    return false;
                for ( std::uint32_t r = 0; r < repeat; ++r, ++i )
                    out.push_back( Dequantize( q, first + i ) );
                continue;
            }
            if ( ( mask & ~kDeltaKnown ) != 0 || !TakeFields( in, q, mask ) )
                return false;
            out.push_back( Dequantize( q, first + i ) );
            ++i;
        }
        return true;
    }

    void AppendInputStepFull( std::vector<std::byte>& out, const InputStep& step )
    {
        // A step the wire does not carry (too many axes, a non-finite field) appends NOTHING: the caller sees the
        // buffer unchanged and refuses the whole message.
        QuantizedStep q;
        if ( !Quantize( step, q ) )
            return;
        PutU32( out, step.Step );
        PutFields( out, q, kDeltaKnown );
    }

    bool ReadInputStepFull( std::span<const std::byte>& in, InputStep& step )
    {
        std::uint32_t number = 0;
        QuantizedStep q;
        if ( !TakeU32( in, number ) || !TakeFields( in, q, kDeltaKnown ) )
            return false;
        step = Dequantize( q, number );
        return true;
    }

    std::size_t InputRunBytes( std::span<const InputStep> steps )
    {
        return RunBytes( steps );
    }
} // namespace Desert::Net
