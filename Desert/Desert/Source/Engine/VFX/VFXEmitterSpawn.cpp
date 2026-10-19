#include "VFXEmitterSpawn.hpp"

#include <Engine/VFX/VFXModuleCatalog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace Desert::VFX
{
    Assets::AssetGuidRef ModuleRef( const VFXEngineCpuModule& module )
    {
        return { Common::Content::AssetGuidToText( module.Guid ), std::string( module.Path ) };
    }

    bool HasCpuBehaviour( const Common::Content::AssetGuid& guid )
    {
        return guid == kVFXSpawnRateModule.Guid || guid == kVFXSpawnBurstModule.Guid ||
               guid == kVFXSpawnFromChannelModule.Guid;
    }

    bool UsesModule( const Assets::Serialization::VFXModuleUse& use, const VFXEngineCpuModule& module )
    {
        const auto guid = Common::Content::AssetGuidFromText( use.Module.Guid );
        return guid && guid.GetValue() == module.Guid;
    }

    namespace S = Assets::Serialization;

    namespace
    {
        // The inputs the C++ reads: the CPU contract (VFXModuleCatalog.hpp), which the stack compiler holds the
        // module's `.dfxmod` to, so a row checked here is checked against the file's declaration too.
        std::span<const VFXModuleDecl> CpuInputs( const VFXEngineCpuModule& module )
        {
            return CpuModuleContract( module.Guid );
        }

        const VFXModuleDecl* CpuDecl( const VFXEngineCpuModule& module, const std::string_view input )
        {
            for ( const VFXModuleDecl& d : CpuInputs( module ) )
                if ( d.Name == input )
                    return &d;
            return nullptr;
        }

        // The scalar an EmitterUpdate input carries: its Value, or the User.* row it is bound to - kept as the
        // row's index and read per instance at each step (the placement may override it), its default beside it.
        struct SpawnScalar
        {
            float                        Constant = 0.0f;
            std::optional<std::uint32_t> User;
        };

        Common::ResultStr<SpawnScalar> ReadScalar( const S::VFXSystemData& system, const S::VFXModuleInput& in,
                                                   const std::string& where )
        {
            switch ( in.Source )
            {
                case S::VFXInputSource::Value:
                    if ( !in.Value )
                        return Common::MakeFormattedError<SpawnScalar>( "{}: Source Value without a Value",
                                                                        where );
                    return Common::MakeSuccess( SpawnScalar{ in.Value->x, std::nullopt } );
                case S::VFXInputSource::Binding:
                    if ( in.Binding && in.Binding->starts_with( S::kVFXUserPrefix ) )
                    {
                        const std::string_view name =
                             std::string_view( *in.Binding ).substr( S::kVFXUserPrefix.size() );
                        for ( std::size_t r = 0; r < system.UserParams.size(); ++r )
                            if ( const S::VFXUserParam& u = system.UserParams[r]; u.Name == name )
                            {
                                if ( u.Type != in.Type )
                                    return Common::MakeFormattedError<SpawnScalar>(
                                         "{}: bound to {} of another type", where, *in.Binding );
                                return Common::MakeSuccess(
                                     SpawnScalar{ u.Default.x, static_cast<std::uint32_t>( r ) } );
                            }
                        return Common::MakeFormattedError<SpawnScalar>( "{}: {} names no UserParams row", where,
                                                                        *in.Binding );
                    }
                    return Common::MakeFormattedError<SpawnScalar>(
                         "{}: a Particles.* binding has no particle to read — EmitterUpdate runs once per "
                         "emitter on the CPU",
                         where );
                case S::VFXInputSource::Curve:
                    return Common::MakeFormattedError<SpawnScalar>(
                         "{}: a curve over emitter time is not read on the CPU side yet (the LUT atlas is "
                         "per-particle age, VFX-05)",
                         where );
                case S::VFXInputSource::Random:
                    return Common::MakeFormattedError<SpawnScalar>(
                         "{}: a random range is not read on the CPU side yet (no per-loop random stream)", where );
            }
            return Common::MakeFormattedError<SpawnScalar>( "{}: unknown source", where );
        }

        // The module's inputs, in @p declared order, each exactly once with its declared type.
        Common::ResultStr<std::vector<SpawnScalar>> ReadInputs( const S::VFXSystemData&        system,
                                                                const S::VFXModuleUse&         use,
                                                                std::span<const VFXModuleDecl> declared,
                                                                const std::string&             where )
        {
            using Result = std::vector<SpawnScalar>;
            for ( const S::VFXModuleInput& in : use.Inputs )
                if ( std::none_of( declared.begin(), declared.end(),
                                   [&]( const VFXModuleDecl& d ) { return in.Name == d.Name; } ) )
                    return Common::MakeFormattedError<Result>( "{} input '{}': the module declares no such input",
                                                               where, in.Name );
            Result values;
            for ( const VFXModuleDecl& d : declared )
            {
                const auto        in      = std::find_if( use.Inputs.begin(), use.Inputs.end(),
                                                          [&]( const S::VFXModuleInput& x )
                                                          { return x.Name == d.Name; } );
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

        // SpawnFromChannel's inputs (see the header): the channel and its fields are DataChannel.*
        // bindings, the counts and the filter bound are Values.
        Common::ResultStr<VFXChannelSpawnModule> ReadChannelSpawn( const S::VFXModuleUse& use,
                                                                   const std::string&     where )
        {
            using Result = VFXChannelSpawnModule;
            for ( const S::VFXModuleInput& in : use.Inputs )
            {
                const VFXModuleDecl* decl = CpuDecl( kVFXSpawnFromChannelModule, in.Name );
                if ( decl == nullptr )
                    return Common::MakeFormattedError<Result>( "{} input '{}': the module declares no such input",
                                                               where, in.Name );
                if ( in.Type != decl->Type )
                    return Common::MakeFormattedError<Result>(
                         "{} input '{}': the row's type disagrees with the module's", where, in.Name );
            }

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

            const auto value = [&]( const char* name, const bool required ) -> Common::ResultStr<float>
            {
                const S::VFXModuleInput* in = FindInput( use, name );
                if ( in == nullptr )
                {
                    if ( required )
                        return Common::MakeFormattedError<float>(
                             "{} input '{}': declared by the module and not given", where, name );
                    return Common::MakeSuccess( 0.0f );
                }
                if ( in->Source != S::VFXInputSource::Value || !in->Value || !std::isfinite( in->Value->x ) )
                    return Common::MakeFormattedError<float>( "{} input '{}': a finite Value is required", where,
                                                              name );
                return Common::MakeSuccess( in->Value->x );
            };
            const auto field = [&]( const char* name ) -> Common::ResultStr<std::string>
            {
                const S::VFXModuleInput* in = FindInput( use, name );
                if ( in == nullptr )
                    return Common::MakeSuccess( std::string() );
                const std::string prefix = std::format( "{}{}.", S::kVFXDataChannelPrefix, m.Channel );
                if ( in->Source != S::VFXInputSource::Binding || !in->Binding ||
                     !in->Binding->starts_with( prefix ) || in->Binding->size() == prefix.size() )
                    return Common::MakeFormattedError<std::string>(
                         "{} input '{}': a Binding {}<field> is required", where, name, prefix );
                return Common::MakeSuccess( in->Binding->substr( prefix.size() ) );
            };

            const auto perEntry   = value( "ParticlesPerEntry", true );
            const auto maxEntries = value( "MaxEntriesPerFrame", true );
            const auto distance   = value( "MaxDistance", false );
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

            const auto position  = field( "Position" );
            const auto direction = field( "Direction" );
            const auto filter    = field( "Filter" );
            const auto color     = field( "Color" );
            const auto lifetime  = field( "Lifetime" );
            const auto size      = field( "Size" );
            for ( const auto* r : { &position, &direction, &filter, &color, &lifetime, &size } )
                if ( !r->IsSuccess() )
                    return Common::MakeError<Result>( r->GetError() );
            m.PositionField  = position.GetValue();
            m.DirectionField = direction.GetValue();
            m.FilterField    = filter.GetValue();
            m.ColorField     = color.GetValue();
            m.LifetimeField  = lifetime.GetValue();
            m.SizeField      = size.GetValue();

            const bool hasOp    = FindInput( use, "FilterOp" ) != nullptr;
            const bool hasBound = FindInput( use, "FilterValue" ) != nullptr;
            if ( m.FilterField.empty() )
            {
                if ( hasOp || hasBound )
                    return Common::MakeFormattedError<Result>( "{}: FilterOp / FilterValue without a Filter field",
                                                               where );
                return Common::MakeSuccess( std::move( m ) );
            }
            const auto op    = value( "FilterOp", true );
            const auto bound = value( "FilterValue", true );
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
            const std::string where = std::format( "emitter '{}' EmitterUpdate module {} '{}' ({})", emitter.Name,
                                                   m, use.Module.Path, use.Module.Guid );
            if ( UsesModule( use, kVFXSpawnRateModule ) )
            {
                auto in = ReadInputs( system, use, CpuInputs( kVFXSpawnRateModule ), where );
                if ( !in.IsSuccess() )
                    return Common::MakeError<Result>( in.GetError() );
                const SpawnScalar rate = in.GetValue()[0];
                if ( rate.User )
                    plan.UserRates.push_back( *rate.User );
                else if ( rate.Constant < 0.0f )
                    return Common::MakeFormattedError<Result>( "{}: SpawnRate {} is negative", where,
                                                               rate.Constant );
                else
                    plan.Rate += rate.Constant;
            }
            else if ( UsesModule( use, kVFXSpawnBurstModule ) )
            {
                auto in = ReadInputs( system, use, CpuInputs( kVFXSpawnBurstModule ), where );
                if ( !in.IsSuccess() )
                    return Common::MakeError<Result>( in.GetError() );
                // A constant is checked here; a User-bound value is the placement's and is checked where it is
                // read (Step: a negative count is none, a time outside the loop never fires).
                const SpawnScalar count = in.GetValue()[0];
                const SpawnScalar time  = in.GetValue()[1];
                if ( !count.User && count.Constant < 0.0f )
                    return Common::MakeFormattedError<Result>( "{}: SpawnCount {} is negative", where,
                                                               count.Constant );
                if ( !time.User && ( time.Constant < 0.0f || time.Constant >= plan.Lifecycle.LoopDuration ) )
                    return Common::MakeFormattedError<Result>(
                         "{}: SpawnTime {} lies outside the loop [0, {}) — it would never fire", where,
                         time.Constant, plan.Lifecycle.LoopDuration );
                VFXSpawnBurst burst;
                burst.Time      = time.User ? 0.0f : time.Constant;
                burst.Count     = count.User ? 0u : static_cast<std::uint32_t>( count.Constant );
                burst.TimeUser  = time.User;
                burst.CountUser = count.User;
                plan.Bursts.push_back( burst );
            }
            else if ( UsesModule( use, kVFXSpawnFromChannelModule ) )
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
                     "{}: EmitterUpdate runs on the CPU and the engine has C++ for {}, {} and {} only", where,
                     kVFXSpawnRateModule.Name, kVFXSpawnBurstModule.Name, kVFXSpawnFromChannelModule.Name );
        }
        for ( const S::VFXUserParam& u : system.UserParams )
            plan.UserDefaults.push_back( u.Default );
        return Common::MakeSuccess( std::move( plan ) );
    }

    namespace
    {
        // User row @p row this step: the instance's resolved value, or the system default for a step given none.
        float UserScalar( const VFXSpawnPlan& plan, std::span<const glm::vec4> user, const std::uint32_t row )
        {
            const std::span<const glm::vec4> values =
                 user.empty() ? std::span<const glm::vec4>( plan.UserDefaults ) : user;
            return row < values.size() ? values[row].x : 0.0f;
        }
    } // namespace

    double VFXSpawnPlan::RateFor( std::span<const glm::vec4> user ) const
    {
        double rate = Rate;
        for ( const std::uint32_t row : UserRates )
            rate += std::max( 0.0, static_cast<double>( UserScalar( *this, user, row ) ) );
        return rate;
    }

    std::uint32_t SpawnState::Step( const VFXSpawnPlan& plan, double stepSeconds, std::span<const glm::vec4> user )
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

        std::uint32_t born = RateCarry.Step( plan.RateFor( user ), hi - lo );

        // A burst fires in the step whose window [lo, hi) holds k * LoopDuration + Time, for every loop k. The
        // windows of consecutive steps share their bounds exactly, so each firing lands in exactly one step.
        for ( double k = std::floor( lo / loop ); k * loop < hi; k += 1.0 )
            for ( const VFXSpawnBurst& burst : plan.Bursts )
            {
                const double time = burst.TimeUser ? UserScalar( plan, user, *burst.TimeUser ) : burst.Time;
                if ( time < 0.0 || time >= loop )
                    continue; // a User-bound time outside the loop never fires (a constant one is refused above)
                const std::uint32_t count =
                     burst.CountUser ? static_cast<std::uint32_t>( std::max(
                                            0.0f, std::round( UserScalar( plan, user, *burst.CountUser ) ) ) )
                                     : burst.Count;
                const double at = k * loop + time;
                if ( at >= lo && at < hi )
                    born += count;
            }
        return born;
    }
} // namespace Desert::VFX
