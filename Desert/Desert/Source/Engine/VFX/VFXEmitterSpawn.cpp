#include "VFXEmitterSpawn.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <string>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    namespace
    {
        struct CpuInput
        {
            const char*     Name;
            S::VFXValueType Type;
        };

        // The scalar an EmitterUpdate input carries: its Value, or the default of the User.* row it is bound to.
        Common::ResultStr<float> ReadScalar( const S::VFXSystemData& system, const S::VFXModuleInput& in,
                                             const std::string& where )
        {
            switch ( in.Source )
            {
                case S::VFXInputSource::Value:
                    if ( !in.Value )
                        return Common::MakeFormattedError<float>( "{}: Source Value without a Value", where );
                    return Common::MakeSuccess( in.Value->x );
                case S::VFXInputSource::Binding:
                    if ( in.Binding && in.Binding->starts_with( S::kVFXUserPrefix ) )
                    {
                        const std::string_view name =
                             std::string_view( *in.Binding ).substr( S::kVFXUserPrefix.size() );
                        for ( const S::VFXUserParam& u : system.UserParams )
                            if ( u.Name == name )
                            {
                                if ( u.Type != in.Type )
                                    return Common::MakeFormattedError<float>( "{}: bound to {} of another type",
                                                                              where, *in.Binding );
                                return Common::MakeSuccess( u.Default.x );
                            }
                        return Common::MakeFormattedError<float>( "{}: {} names no UserParams row", where,
                                                                  *in.Binding );
                    }
                    return Common::MakeFormattedError<float>(
                         "{}: a Particles.* binding has no particle to read — EmitterUpdate runs once per "
                         "emitter on the CPU",
                         where );
                case S::VFXInputSource::Curve:
                    return Common::MakeFormattedError<float>(
                         "{}: a curve over emitter time is not read on the CPU side yet (the LUT atlas is "
                         "per-particle age, VFX-05)",
                         where );
                case S::VFXInputSource::Random:
                    return Common::MakeFormattedError<float>(
                         "{}: a random range is not read on the CPU side yet (no per-loop random stream)", where );
            }
            return Common::MakeFormattedError<float>( "{}: unknown source", where );
        }

        // The module's inputs, in @p declared order, each exactly once with its declared type.
        Common::ResultStr<std::vector<float>> ReadInputs( const S::VFXSystemData&      system,
                                                          const S::VFXModuleUse&       use,
                                                          const std::vector<CpuInput>& declared,
                                                          const std::string&           where )
        {
            using Result = std::vector<float>;
            for ( const S::VFXModuleInput& in : use.Inputs )
                if ( std::none_of( declared.begin(), declared.end(),
                                   [&]( const CpuInput& d ) { return in.Name == d.Name; } ) )
                    return Common::MakeFormattedError<Result>( "{} input '{}': the module declares no such input",
                                                               where, in.Name );
            Result values;
            for ( const CpuInput& d : declared )
            {
                const auto        in      = std::find_if( use.Inputs.begin(), use.Inputs.end(),
                                                          [&]( const S::VFXModuleInput& x ) { return x.Name == d.Name; } );
                const std::string inWhere = std::format( "{} input '{}'", where, d.Name );
                if ( in == use.Inputs.end() )
                    return Common::MakeFormattedError<Result>( "{}: declared by the module and not given",
                                                               inWhere );
                if ( in->Type != d.Type )
                    return Common::MakeFormattedError<Result>( "{}: the row's type disagrees with the module's",
                                                               inWhere );
                auto value = ReadScalar( system, *in, inWhere );
                if ( !value.IsSuccess() )
                    return Common::MakeError<Result>( value.GetError() );
                values.push_back( value.GetValue() );
            }
            return Common::MakeSuccess( std::move( values ) );
        }
        const S::VFXModuleInput* FindInput( const S::VFXModuleUse& use, const std::string_view name )
        {
            for ( const S::VFXModuleInput& in : use.Inputs )
                if ( in.Name == name )
                    return &in;
            return nullptr;
        }

        // engine:SpawnFromChannel's inputs (see the header): the channel and its fields are DataChannel.*
        // bindings, the counts and the filter bound are Values.
        Common::ResultStr<VFXChannelSpawnModule> ReadChannelSpawn( const S::VFXModuleUse& use,
                                                                   const std::string&     where )
        {
            using Result                                             = VFXChannelSpawnModule;
            static constexpr std::array<std::string_view, 11> kInputs = {
                 "Channel",     "ParticlesPerEntry", "MaxEntriesPerFrame",
                 "MaxDistance", "Position",          "Direction",
                 "Color",       "Lifetime",          "Filter",
                 "FilterOp",    "FilterValue" };
            for ( const S::VFXModuleInput& in : use.Inputs )
                if ( std::find( kInputs.begin(), kInputs.end(), in.Name ) == kInputs.end() )
                    return Common::MakeFormattedError<Result>( "{} input '{}': the module declares no such input",
                                                               where, in.Name );

            Result                   m;
            const S::VFXModuleInput* channel = FindInput( use, "Channel" );
            if ( channel == nullptr || channel->Source != S::VFXInputSource::Binding || !channel->Binding ||
                 !channel->Binding->starts_with( S::kVFXDataChannelPrefix ) )
                return Common::MakeFormattedError<Result>(
                     "{} input 'Channel': a Binding DataChannel.<channel> is required", where );
            m.Channel = channel->Binding->substr( S::kVFXDataChannelPrefix.size() );
            if ( m.Channel.empty() || m.Channel.find( '.' ) != std::string::npos )
                return Common::MakeFormattedError<Result>( "{} input 'Channel': '{}' names no channel", where,
                                                           *channel->Binding );

            const auto value = [&]( const char* name, const S::VFXValueType type,
                                    const bool required ) -> Common::ResultStr<float>
            {
                const S::VFXModuleInput* in = FindInput( use, name );
                if ( in == nullptr )
                {
                    if ( required )
                        return Common::MakeFormattedError<float>(
                             "{} input '{}': declared by the module and not given", where, name );
                    return Common::MakeSuccess( 0.0f );
                }
                if ( in->Type != type )
                    return Common::MakeFormattedError<float>(
                         "{} input '{}': the row's type disagrees with the module's", where, name );
                if ( in->Source != S::VFXInputSource::Value || !in->Value || !std::isfinite( in->Value->x ) )
                    return Common::MakeFormattedError<float>( "{} input '{}': a finite Value is required", where,
                                                              name );
                return Common::MakeSuccess( in->Value->x );
            };
            const auto field = [&]( const char*           name,
                                    const S::VFXValueType type ) -> Common::ResultStr<std::string>
            {
                const S::VFXModuleInput* in = FindInput( use, name );
                if ( in == nullptr )
                    return Common::MakeSuccess( std::string() );
                const std::string prefix = std::string( S::kVFXDataChannelPrefix ) + m.Channel + ".";
                if ( in->Type != type || in->Source != S::VFXInputSource::Binding || !in->Binding ||
                     !in->Binding->starts_with( prefix ) || in->Binding->size() == prefix.size() )
                    return Common::MakeFormattedError<std::string>(
                         "{} input '{}': a Binding {}<field> of the module's type is required", where, name,
                         prefix );
                return Common::MakeSuccess( in->Binding->substr( prefix.size() ) );
            };

            const auto perEntry   = value( "ParticlesPerEntry", S::VFXValueType::Int, true );
            const auto maxEntries = value( "MaxEntriesPerFrame", S::VFXValueType::Int, true );
            const auto distance   = value( "MaxDistance", S::VFXValueType::Float, false );
            for ( const auto* r : { &perEntry, &maxEntries, &distance } )
                if ( !r->IsSuccess() )
                    return Common::MakeError<Result>( r->GetError() );
            if ( perEntry.GetValue() < 1.0f || maxEntries.GetValue() < 1.0f || distance.GetValue() < 0.0f )
                return Common::MakeFormattedError<Result>(
                     "{}: ParticlesPerEntry {} and MaxEntriesPerFrame {} must be >= 1, MaxDistance {} >= 0", where,
                     perEntry.GetValue(), maxEntries.GetValue(), distance.GetValue() );
            m.ParticlesPerEntry  = static_cast<std::uint32_t>( perEntry.GetValue() );
            m.MaxEntriesPerFrame = static_cast<std::uint32_t>( maxEntries.GetValue() );
            m.MaxDistance        = distance.GetValue();

            const auto position  = field( "Position", S::VFXValueType::Vec3 );
            const auto direction = field( "Direction", S::VFXValueType::Vec3 );
            const auto filter    = field( "Filter", S::VFXValueType::Float );
            const auto color     = field( "Color", S::VFXValueType::Vec4 );
            const auto lifetime  = field( "Lifetime", S::VFXValueType::Float );
            for ( const auto* r : { &position, &direction, &filter, &color, &lifetime } )
                if ( !r->IsSuccess() )
                    return Common::MakeError<Result>( r->GetError() );
            m.PositionField  = position.GetValue();
            m.DirectionField = direction.GetValue();
            m.FilterField    = filter.GetValue();
            m.ColorField     = color.GetValue();
            m.LifetimeField  = lifetime.GetValue();

            const bool hasOp    = FindInput( use, "FilterOp" ) != nullptr;
            const bool hasBound = FindInput( use, "FilterValue" ) != nullptr;
            if ( m.FilterField.empty() )
            {
                if ( hasOp || hasBound )
                    return Common::MakeFormattedError<Result>( "{}: FilterOp / FilterValue without a Filter field",
                                                               where );
                return Common::MakeSuccess( std::move( m ) );
            }
            const auto op    = value( "FilterOp", S::VFXValueType::Int, true );
            const auto bound = value( "FilterValue", S::VFXValueType::Float, true );
            if ( !op.IsSuccess() )
                return Common::MakeError<Result>( op.GetError() );
            if ( !bound.IsSuccess() )
                return Common::MakeError<Result>( bound.GetError() );
            if ( op.GetValue() < 0.0f || op.GetValue() >= static_cast<float>( kVFXChannelFilterOpCount ) )
                return Common::MakeFormattedError<Result>( "{}: FilterOp {} is not an operator (0..{})", where,
                                                           op.GetValue(), kVFXChannelFilterOpCount - 1 );
            m.FilterOp    = static_cast<VFXChannelFilterOp>( static_cast<std::uint32_t>( op.GetValue() ) );
            m.FilterValue = bound.GetValue();
            return Common::MakeSuccess( std::move( m ) );
        }
    } // namespace

    Common::ResultStr<VFXSpawnPlan> CompileSpawnPlan( const S::VFXSystemData& system, std::size_t emitterIndex )
    {
        using Result = VFXSpawnPlan;
        if ( emitterIndex >= system.Emitters.size() )
            return Common::MakeFormattedError<Result>( "emitter {} of {}", emitterIndex, system.Emitters.size() );
        const S::VFXEmitterData& emitter = system.Emitters[emitterIndex];

        VFXSpawnPlan plan;
        plan.Lifecycle = emitter.Lifecycle;
        if ( !( plan.Lifecycle.LoopDuration > 0.0f ) )
            return Common::MakeFormattedError<Result>( "emitter '{}': LoopDuration {} is not positive",
                                                       emitter.Name, plan.Lifecycle.LoopDuration );

        const auto& group = emitter.Stack.EmitterUpdate;
        for ( std::size_t m = 0; m < group.size(); ++m )
        {
            const S::VFXModuleUse& use = group[m];
            if ( !use.Enabled )
                continue;
            const std::string where =
                 std::format( "emitter '{}' EmitterUpdate module {} '{}'", emitter.Name, m, use.Module );
            if ( use.Module == kVFXSpawnRateModule )
            {
                auto in = ReadInputs( system, use, { { "SpawnRate", S::VFXValueType::Float } }, where );
                if ( !in.IsSuccess() )
                    return Common::MakeError<Result>( in.GetError() );
                if ( in.GetValue()[0] < 0.0f )
                    return Common::MakeFormattedError<Result>( "{}: SpawnRate {} is negative", where,
                                                               in.GetValue()[0] );
                plan.Rate += in.GetValue()[0];
            }
            else if ( use.Module == kVFXSpawnBurstModule )
            {
                auto in = ReadInputs(
                     system, use,
                     { { "SpawnCount", S::VFXValueType::Int }, { "SpawnTime", S::VFXValueType::Float } }, where );
                if ( !in.IsSuccess() )
                    return Common::MakeError<Result>( in.GetError() );
                const float count = in.GetValue()[0];
                const float time  = in.GetValue()[1];
                if ( count < 0.0f )
                    return Common::MakeFormattedError<Result>( "{}: SpawnCount {} is negative", where, count );
                if ( time < 0.0f || time >= plan.Lifecycle.LoopDuration )
                    return Common::MakeFormattedError<Result>(
                         "{}: SpawnTime {} lies outside the loop [0, {}) — it would never fire", where, time,
                         plan.Lifecycle.LoopDuration );
                plan.Bursts.push_back( { time, static_cast<std::uint32_t>( count ) } );
            }
            else if ( use.Module == kVFXSpawnFromChannelModule )
            {
                if ( plan.Channel )
                    return Common::MakeFormattedError<Result>( "{}: an emitter spawns from one channel", where );
                auto channel = ReadChannelSpawn( use, where );
                if ( !channel.IsSuccess() )
                    return Common::MakeError<Result>( channel.GetError() );
                plan.Channel = channel.GetValue();
            }
            else
                return Common::MakeFormattedError<Result>(
                     "{}: EmitterUpdate runs on the CPU and knows only {}, {} and {}", where, kVFXSpawnRateModule,
                     kVFXSpawnBurstModule, kVFXSpawnFromChannelModule );
        }
        return Common::MakeSuccess( std::move( plan ) );
    }

    std::uint32_t SpawnState::Step( const VFXSpawnPlan& plan, double stepSeconds )
    {
        const double t0 = Age;
        const double t1 = Age + stepSeconds;
        Age             = t1;

        // The step in the emitter's own time (after the Delay), clipped to the loops it runs.
        const auto&  life = plan.Lifecycle;
        const double loop = life.LoopDuration;
        double       end  = std::numeric_limits<double>::infinity();
        if ( life.Loop == S::VFXLoopBehavior::Once )
            end = loop;
        else if ( life.Loop == S::VFXLoopBehavior::Multiple )
            end = loop * static_cast<double>( life.LoopCount );
        const double lo = std::max( t0 - life.Delay, 0.0 );
        const double hi = std::min( t1 - life.Delay, end );
        if ( !( hi > lo ) )
            return 0;

        std::uint32_t born = RateCarry.Step( plan.Rate, hi - lo );

        // A burst fires in the step whose window [lo, hi) holds k * LoopDuration + Time, for every loop k. The
        // windows of consecutive steps share their bounds exactly, so each firing lands in exactly one step.
        for ( double k = std::floor( lo / loop ); k * loop < hi; k += 1.0 )
            for ( const VFXSpawnBurst& burst : plan.Bursts )
            {
                const double at = k * loop + burst.Time;
                if ( at >= lo && at < hi )
                    born += burst.Count;
            }
        return born;
    }
} // namespace Desert::VFX
