#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <format>
#include <set>
#include <string>
#include <string_view>

namespace Desert::Assets::Serialization
{
    namespace
    {

        bool Finite( const glm::vec4& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z ) && std::isfinite( v.w );
        }

        bool Finite( const glm::vec3& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
        }

        /// A value of @p type: finite, unused components zero, Int/Bool components whole (Bool 0 or 1).
        Common::BoolResultStr CheckValue( const glm::vec4& v, VFXValueType type, std::string_view what )
        {
            if ( !Finite( v ) )
                return Common::MakeFormattedError<bool>( "{} is not finite", what );
            const uint32_t used = ComponentCount( type );
            for ( uint32_t i = used; i < 4; ++i )
                if ( v[static_cast<int>( i )] != 0.0f )
                    return Common::MakeFormattedError<bool>( "{} has component {} = {}, unused by its type", what,
                                                             i, v[static_cast<int>( i )] );
            if ( type == VFXValueType::Int && v.x != std::trunc( v.x ) )
                return Common::MakeFormattedError<bool>( "{} is {}, not a whole number", what, v.x );
            if ( type == VFXValueType::Bool && v.x != 0.0f && v.x != 1.0f )
                return Common::MakeFormattedError<bool>( "{} is {}, a Bool is 0 or 1", what, v.x );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr CheckInput( const VFXModuleInput& in, const VFXSystemData& system,
                                          const std::string& where )
        {
            const std::string what = std::format( "{} input '{}'", where, in.Name );
            if ( in.Name.empty() )
                return Common::MakeFormattedError<bool>( "{} input has an empty Name", where );
            const bool value = in.Source == VFXInputSource::Value, curve = in.Source == VFXInputSource::Curve,
                       binding = in.Source == VFXInputSource::Binding,
                       random  = in.Source == VFXInputSource::Random;
            if ( in.Value.has_value() != value || in.Curve.has_value() != curve ||
                 in.Binding.has_value() != binding || in.Random.has_value() != random )
                return Common::MakeFormattedError<bool>(
                     "{}: exactly the member its Source names must be present (Value/Curve/Binding/Random)",
                     what );
            if ( value )
                return CheckValue( *in.Value, in.Type, what );
            if ( random )
            {
                if ( in.Type == VFXValueType::Bool )
                    return Common::MakeFormattedError<bool>( "{}: a Bool has no random range", what );
                if ( auto ok = CheckValue( in.Random->Min, in.Type, std::format( "{} Min", what ) ); !ok )
                    return ok;
                if ( auto ok = CheckValue( in.Random->Max, in.Type, std::format( "{} Max", what ) ); !ok )
                    return ok;
                for ( int i = 0; i < 4; ++i )
                    if ( in.Random->Min[i] > in.Random->Max[i] )
                        return Common::MakeFormattedError<bool>( "{}: Min > Max in component {}", what, i );
                return BOOLSUCCESS;
            }
            if ( curve )
            {
                if ( in.Type == VFXValueType::Bool || in.Type == VFXValueType::Int )
                    return Common::MakeFormattedError<bool>( "{}: a curve drives a float type only", what );
                if ( in.Curve->size() != ComponentCount( in.Type ) )
                    return Common::MakeFormattedError<bool>( "{}: {} curve channels for {} components", what,
                                                             in.Curve->size(), ComponentCount( in.Type ) );
                for ( std::size_t c = 0; c < in.Curve->size(); ++c )
                {
                    const auto& keys = ( *in.Curve )[c];
                    if ( keys.empty() )
                        return Common::MakeFormattedError<bool>( "{}: curve channel {} has no keys", what, c );
                    for ( std::size_t k = 0; k < keys.size(); ++k )
                    {
                        const VFXCurveKey& key = keys[k];
                        if ( !std::isfinite( key.Time ) || !std::isfinite( key.Value ) ||
                             !std::isfinite( key.ArriveTangent ) || !std::isfinite( key.LeaveTangent ) )
                            return Common::MakeFormattedError<bool>( "{}: channel {} key {} is not finite", what,
                                                                     c, k );
                        if ( k > 0 && !( keys[k - 1].Time < key.Time ) )
                            return Common::MakeFormattedError<bool>(
                                 "{}: channel {} key {} at {} does not follow {}", what, c, k, key.Time,
                                 keys[k - 1].Time );
                    }
                }
                return BOOLSUCCESS;
            }
            // Binding.
            const std::string& b = *in.Binding;
            if ( b.starts_with( kVFXUserPrefix ) )
            {
                const std::string_view name = std::string_view( b ).substr( kVFXUserPrefix.size() );
                for ( const VFXUserParam& p : system.UserParams )
                    if ( p.Name == name )
                    {
                        if ( p.Type != in.Type )
                            return Common::MakeFormattedError<bool>( "{}: '{}' is of another type", what, b );
                        return BOOLSUCCESS;
                    }
                return Common::MakeFormattedError<bool>( "{}: '{}' names no UserParams row", what, b );
            }
            if ( b.starts_with( kVFXParticlesPrefix ) && b.size() > kVFXParticlesPrefix.size() )
                return BOOLSUCCESS; // attributes are derived from the stack (VFX-04); the name is all a file
                                    // states
            return Common::MakeFormattedError<bool>(
                 "{}: Binding '{}' is neither User.<param> nor Particles.<attr>", what, b );
        }

        Common::BoolResultStr CheckGroup( const std::vector<VFXModuleUse>& group, const VFXSystemData& system,
                                          const std::string& where )
        {
            for ( std::size_t i = 0; i < group.size(); ++i )
            {
                const VFXModuleUse& use = group[i];
                const std::string   at  = std::format( "{} module {} '{}'", where, i, use.Module );
                if ( use.Module.starts_with( kVFXEnginePrefix ) )
                {
                    if ( use.Module.size() == kVFXEnginePrefix.size() )
                        return Common::MakeFormattedError<bool>( "{}: no engine module name", at );
                }
                else if ( use.Module.starts_with( kVFXLocalPrefix ) )
                {
                    const std::string_view id    = std::string_view( use.Module ).substr( kVFXLocalPrefix.size() );
                    bool                   found = false;
                    for ( const VFXLocalModule& m : system.LocalModules )
                        found = found || m.Id == id;
                    if ( !found )
                        return Common::MakeFormattedError<bool>( "{}: names no LocalModules row", at );
                }
                else
                    return Common::MakeFormattedError<bool>( "{}: Module is neither engine:<Name> nor local:<Id>",
                                                             at );
                std::set<std::string> names;
                for ( const VFXModuleInput& in : use.Inputs )
                {
                    if ( !names.insert( in.Name ).second )
                        return Common::MakeFormattedError<bool>( "{}: input '{}' twice", at, in.Name );
                    if ( auto ok = CheckInput( in, system, at ); !ok )
                        return ok;
                }
            }
            return BOOLSUCCESS;
        }
    } // namespace

    uint32_t ComponentCount( const VFXValueType type )
    {
        switch ( type )
        {
            case VFXValueType::Float:
            case VFXValueType::Int:
            case VFXValueType::Bool:
                return 1;
            case VFXValueType::Vec2:
                return 2;
            case VFXValueType::Vec3:
                return 3;
            case VFXValueType::Vec4:
                return 4;
        }
        return 0;
    }

    Common::BoolResultStr ValidateVFXSystemData( const VFXSystemData& data )
    {
        if ( !std::isfinite( data.Duration ) || data.Duration < 0.0f )
            return Common::MakeFormattedError<bool>( "Duration {} is not a finite non-negative time",
                                                     data.Duration );
        if ( data.Loop && data.Duration <= 0.0f )
            return Common::MakeFormattedError<bool>( "a looping system needs Duration > 0, has {}",
                                                     data.Duration );
        if ( !Finite( data.Bounds.Min ) || !Finite( data.Bounds.Max ) || data.Bounds.Min.x > data.Bounds.Max.x ||
             data.Bounds.Min.y > data.Bounds.Max.y || data.Bounds.Min.z > data.Bounds.Max.z )
            return Common::MakeFormattedError<bool>( "Bounds are not a finite box with Min <= Max" );

        std::set<std::string> tags;
        for ( const std::string& tag : data.Tags )
            if ( tag.empty() || !tags.insert( tag ).second )
                return Common::MakeFormattedError<bool>( "tag '{}' is empty or repeated", tag );

        std::set<std::string> params;
        for ( const VFXUserParam& p : data.UserParams )
        {
            if ( p.Name.empty() || p.Name.find( '.' ) != std::string::npos || !params.insert( p.Name ).second )
                return Common::MakeFormattedError<bool>( "user parameter '{}' is empty, dotted or repeated",
                                                         p.Name );
            if ( auto ok = CheckValue( p.Default, p.Type, std::format( "User.{} Default", p.Name ) ); !ok )
                return ok;
        }

        std::set<std::string> modules;
        for ( const VFXLocalModule& m : data.LocalModules )
            if ( m.Id.empty() || !modules.insert( m.Id ).second )
                return Common::MakeFormattedError<bool>( "local module id '{}' is empty or repeated", m.Id );

        std::set<std::string> emitters;
        for ( const VFXEmitterData& e : data.Emitters )
        {
            if ( e.Name.empty() || !emitters.insert( e.Name ).second )
                return Common::MakeFormattedError<bool>( "emitter name '{}' is empty or repeated", e.Name );
            if ( e.Capacity == 0 )
                return Common::MakeFormattedError<bool>( "emitter '{}' has Capacity 0", e.Name );
            const VFXEmitterLifecycle& l = e.Lifecycle;
            if ( !std::isfinite( l.Delay ) || l.Delay < 0.0f || !std::isfinite( l.LoopDuration ) ||
                 l.LoopDuration <= 0.0f )
                return Common::MakeFormattedError<bool>( "emitter '{}' has Delay {} / LoopDuration {}", e.Name,
                                                         l.Delay, l.LoopDuration );
            if ( l.Loop == VFXLoopBehavior::Multiple && l.LoopCount == 0 )
                return Common::MakeFormattedError<bool>( "emitter '{}' loops Multiple with LoopCount 0", e.Name );
            const std::string at = std::format( "emitter '{}'", e.Name );
            if ( auto ok = CheckGroup( e.Stack.EmitterUpdate, data, std::format( "{} EmitterUpdate", at ) ); !ok )
                return ok;
            if ( auto ok = CheckGroup( e.Stack.ParticleSpawn, data, std::format( "{} ParticleSpawn", at ) ); !ok )
                return ok;
            if ( auto ok = CheckGroup( e.Stack.ParticleUpdate, data, std::format( "{} ParticleUpdate", at ) );
                 !ok )
                return ok;
        }
        return BOOLSUCCESS;
    }

    bool VFXCategoryRegister::Contains( const std::string_view id ) const
    {
        for ( const VFXCategory& c : Categories )
            if ( c.Id == id )
                return true;
        return false;
    }

    Common::ResultStr<VFXCategoryRegister> ReadVFXCategories( const std::filesystem::path& path )
    {
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( path, ec ) )
            return Common::MakeFormattedError<VFXCategoryRegister>( "VFX category register '{}' does not exist",
                                                                    path.string() );
        auto read = Common::Json::ReadFile<VFXCategoryRegister>( path );
        if ( !read )
            return Common::MakeFormattedError<VFXCategoryRegister>( "{}", read.GetError() );
        VFXCategoryRegister   reg = read.ExtractValue();
        std::set<std::string> ids;
        for ( const VFXCategory& cat : reg.Categories )
            if ( cat.Id.empty() || !ids.insert( cat.Id ).second || !Finite( cat.Color ) )
                return Common::MakeFormattedError<VFXCategoryRegister>(
                     "VFX category register '{}': id '{}' is empty, repeated or has a non-finite Color",
                     path.string(), cat.Id );
        return Common::MakeSuccess( std::move( reg ) );
    }

    Common::ResultStr<VFXSystemData> ParseVFXSystem( const std::string&         text,
                                                     const VFXCategoryRegister& categories )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<VFXSystemData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kVFXSystemVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<VFXSystemData>( "VFX system {}", headed.GetError() );

        const auto parsed = Common::Json::Read<VFXSystemData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<VFXSystemData>( "{}", parsed.GetError() );
        VFXSystemData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::VFXSystem,
                                                      Assets::kVFXSystemSchemaTag, kVFXSystemVersion,
                                                      VFXSystemTextSubsystems() );
             !header )
            return Common::MakeFormattedError<VFXSystemData>( "VFX system {}", header.GetError() );

        if ( auto valid = ValidateVFXSystemData( data ); !valid )
            return Common::MakeFormattedError<VFXSystemData>( "{}", valid.GetError() );

        if ( !categories.Contains( data.Category ) )
            return Common::MakeFormattedError<VFXSystemData>(
                 "Category '{}' is not in the project's VFX category register ({})", data.Category,
                 kVFXCategoriesFileName );

        // A v1 system references no other asset, so a stated Dependency is an edge to nothing it names.
        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<VFXSystemData>( "the header states {} Dependencies; a VFX system "
                                                              "references no asset",
                                                              data.Header->Dependencies.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteVFXSystem( const VFXSystemData& data )
    {
        VFXSystemData out = data;
        out.Header        = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::VFXSystem,
                                                     VFXSystemTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveVFXSystemFile( const std::filesystem::path& path, const VFXSystemData& data )
    {
        if ( auto valid = ValidateVFXSystemData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write VFX system '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for VFX system: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteVFXSystem( data ) );
    }
} // namespace Desert::Assets::Serialization
