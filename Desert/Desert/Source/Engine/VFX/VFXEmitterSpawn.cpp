#include "VFXEmitterSpawn.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
        // What the C++ below reads; the defaults (UE SpawnRate 10 / s, SpawnBurstInstantaneous 10 at time 0,
        // Spawn From Data Channel one particle per entry) are the `.dfxmod`s' alone.
        constexpr std::array<VFXCpuModuleInput, 1> kSpawnRateInputs  = { { { "SpawnRate", S::VFXValueType::Float } } };
        constexpr std::array<VFXCpuModuleInput, 2> kSpawnBurstInputs = {
             { { "SpawnCount", S::VFXValueType::Int }, { "SpawnTime", S::VFXValueType::Float } } };
        // Channel is a DataChannel.* binding (`= Binding` in the file); its type is the file's `int`.
        constexpr std::array<VFXCpuModuleInput, 4> kSpawnFromChannelInputs = {
             { { "Channel", S::VFXValueType::Int },
               { "ParticlesPerEntry", S::VFXValueType::Int },
               { "MaxEntriesPerFrame", S::VFXValueType::Int },
               { "MaxDistance", S::VFXValueType::Float } } };

        // The scalar an EmitterUpdate input carries: its Value, or the default of the User.* row it is bound to.
        Common::ResultStr<float> ReadScalar( const S::VFXSystemData& system, const std::vector<ECS::VFXUserOverride>& overrides,
                                             const S::VFXModuleInput& in,
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
                                return Common::MakeSuccess( S::UserParamValue( u, overrides ).x );
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
                                                          const std::vector<ECS::VFXUserOverride>& overrides,
                                                          const S::VFXModuleUse&       use,
                                                          std::span<const VFXCpuModuleInput> declared,
                                                          const std::string&           where )
        {
            using Result = std::vector<float>;
            for ( const S::VFXModuleInput& in : use.Inputs )
                if ( std::none_of( declared.begin(), declared.end(),
                                   [&]( const VFXCpuModuleInput& d ) { return in.Name == d.Name; } ) )
                    return Common::MakeFormattedError<Result>( "{} input '{}': the module declares no such input",
                                                               where, in.Name );
            Result values;
            for ( const VFXCpuModuleInput& d : declared )
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
                auto value = ReadScalar( system, overrides, *in, inWhere );
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
            using Result                                              = VFXChannelSpawnModule;
            static constexpr std::array<std::string_view, 12> kInputs = {
                 "Channel",     "ParticlesPerEntry", "MaxEntriesPerFrame",
                 "MaxDistance", "Position",          "Direction",
                 "Color",       "Lifetime",          "Filter",
                 "FilterOp",    "FilterValue",       "Size" };
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
                const std::string prefix = std::format( "{}{}.", S::kVFXDataChannelPrefix, m.Channel );
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
            const auto size      = field( "Size", S::VFXValueType::Float );
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

    uint8_t CpuModuleUsage( const Common::Content::AssetGuid& module )
    {
        if ( module == kVFXSpawnRateModule.Guid || module == kVFXSpawnBurstModule.Guid ||
             module == kVFXSpawnFromChannelModule.Guid )
            return UsageBit( VFXStage::EmitterUpdate );
        return 0;
    }

    std::span<const VFXCpuModuleInput> CpuModuleInputs( const Common::Content::AssetGuid& module )
    {
        if ( module == kVFXSpawnRateModule.Guid )
            return kSpawnRateInputs;
        if ( module == kVFXSpawnBurstModule.Guid )
            return kSpawnBurstInputs;
        if ( module == kVFXSpawnFromChannelModule.Guid )
            return kSpawnFromChannelInputs;
        return {};
    }

    Common::BoolResultStr CheckCpuModuleContract( const VFXModuleDefinition& module )
    {
        const std::string where = std::format( "module '{}' ({})", module.DisplayName,
                                               Common::Content::AssetGuidToText( module.Guid ) );
        if ( !HasCpuBehaviour( module.Guid ) )
            return Common::MakeFormattedError<bool>(
                 "{}: its Usage names an Emitter group, and the engine has no C++ registered under its GUID", where );
        const uint8_t cpu = static_cast<uint8_t>( module.Block.Usage & kVFXCpuUsage );
        if ( ( cpu & ~CpuModuleUsage( module.Guid ) ) != 0 )
            return Common::MakeFormattedError<bool>(
                 "{}: Usage EmitterSpawn - the engine's C++ for this module runs in EmitterUpdate only", where );
        const std::span<const VFXCpuModuleInput>     reads = CpuModuleInputs( module.Guid );
        const std::vector<VFXModuleInputDecl>&       file  = module.Block.Inputs;
        for ( std::size_t i = 0; i < std::max( reads.size(), file.size() ); ++i )
        {
            if ( i >= file.size() )
                return Common::MakeFormattedError<bool>( "{}: declares no input '{}' - the engine's C++ reads it",
                                                         where, reads[i].Name );
            if ( i >= reads.size() || file[i].Name != reads[i].Name )
                return Common::MakeFormattedError<bool>(
                     "{}: input {} '{}' - the engine's C++ reads {}", where, i, file[i].Name,
                     i < reads.size() ? std::format( "'{}' there", reads[i].Name ) : std::string( "no more inputs" ) );
            if ( file[i].Type != reads[i].Type )
                return Common::MakeFormattedError<bool>(
                     "{}: input '{}' is {} - the engine's C++ reads it as {}", where, file[i].Name,
                     GlslTypeName( file[i].Type ), GlslTypeName( reads[i].Type ) );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<VFXSpawnPlan> CompileSpawnPlan( const S::VFXSystemData& system, std::size_t emitterIndex,
                                                      const std::vector<ECS::VFXUserOverride>& overrides )
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

        // EmitterSpawn takes no spawning module: births are Emitter Update's (UE SpawnBurst_Instantaneous and
        // SpawnRate are Emitter Update modules - a burst fires once per loop, and Loop Once makes a one-shot).
        const auto& spawnGroup = emitter.Stack.EmitterSpawn;
        for ( std::size_t m = 0; m < spawnGroup.size(); ++m )
        {
            const S::VFXModuleUse& use = spawnGroup[m];
            if ( !use.Enabled )
                continue;
            return Common::MakeFormattedError<Result>(
                 "emitter '{}' EmitterSpawn module {} '{}' ({}): the engine runs no CPU module in EmitterSpawn - "
                 "a burst is {} in EmitterUpdate (once per loop; Loop Once for a one-shot)",
                 emitter.Name, m, use.Module.Path, use.Module.Guid, kVFXSpawnBurstModule.Name );
        }

        const auto& group = emitter.Stack.EmitterUpdate;
        for ( std::size_t m = 0; m < group.size(); ++m )
        {
            const S::VFXModuleUse& use = group[m];
            if ( !use.Enabled )
                continue;
            const std::string where = std::format( "emitter '{}' EmitterUpdate module {} '{}' ({})", emitter.Name, m,
                                                   use.Module.Path, use.Module.Guid );
            if ( UsesModule( use, kVFXSpawnRateModule ) )
            {
                auto in = ReadInputs( system, overrides, use, kSpawnRateInputs, where );
                if ( !in.IsSuccess() )
                    return Common::MakeError<Result>( in.GetError() );
                if ( in.GetValue()[0] < 0.0f )
                    return Common::MakeFormattedError<Result>( "{}: SpawnRate {} is negative", where,
                                                               in.GetValue()[0] );
                plan.Rate += in.GetValue()[0];
            }
            else if ( UsesModule( use, kVFXSpawnBurstModule ) )
            {
                auto in = ReadInputs( system, overrides, use, kSpawnBurstInputs, where );
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
        return Common::MakeSuccess( std::move( plan ) );
    }

    std::uint32_t SpawnState::Step( const VFXSpawnPlan& plan, double stepSeconds, double spawnCountScale )
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

        std::uint32_t born = RateCarry.Step( plan.Rate * spawnCountScale, hi - lo );

        // A burst fires in the step whose window [lo, hi) holds k * LoopDuration + Time, for every loop k. The
        // windows of consecutive steps share their bounds exactly, so each firing lands in exactly one step.
        for ( double k = std::floor( lo / loop ); k * loop < hi; k += 1.0 )
            for ( const VFXSpawnBurst& burst : plan.Bursts )
            {
                const double at = k * loop + burst.Time;
                if ( at >= lo && at < hi )
                {
                    // Not a truncating cast: Count 1 x a scale below 1 was 0 births in every loop, forever.
                    BurstCarry += static_cast<double>( burst.Count ) * spawnCountScale;
                    const double whole = std::floor( BurstCarry );
                    BurstCarry -= whole;
                    born += static_cast<std::uint32_t>( whole );
                }
            }
        return born;
    }
} // namespace Desert::VFX
