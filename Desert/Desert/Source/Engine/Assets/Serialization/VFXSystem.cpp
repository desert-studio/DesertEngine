#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <string>
#include <string_view>
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
            const bool value   = in.Source == VFXInputSource::Value;
            const bool curve   = in.Source == VFXInputSource::Curve;
            const bool binding = in.Source == VFXInputSource::Binding;
            const bool random  = in.Source == VFXInputSource::Random;
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
            if ( b.starts_with( kVFXDataChannelPrefix ) && b.size() > kVFXDataChannelPrefix.size() )
                return BOOLSUCCESS; // the channel is the scene's: CompileSpawnPlan reads the spelling, the world
                                    // binds the fields to the channel's layout (VFX/VFXDataChannel
                                    // BindChannelSpawn)
            return Common::MakeFormattedError<bool>(
                 "{}: Binding '{}' is neither User.<param>, Particles.<attr> nor DataChannel.<channel>[.<field>]",
                 what, b );
        }

        /// An emitter's Event Handlers: each names an emitter of the same system, spawns at least one particle per
        /// event, inherits a finite velocity scale; a Collision handler's source must collide (an enabled
        /// engine:Collision in its ParticleUpdate), or the handler could never fire - a dead setting, refused.
        Common::BoolResultStr CheckEventHandlers( const VFXEmitterData& e, const VFXSystemData& system,
                                                  const std::string& at )
        {
            for ( std::size_t h = 0; h < e.EventHandlers.size(); ++h )
            {
                const VFXEventHandler& handler = e.EventHandlers[h];
                const auto             source  = std::find_if( system.Emitters.begin(), system.Emitters.end(),
                                                               [&handler]( const VFXEmitterData& other )
                                                               { return other.Name == handler.SourceEmitter; } );
                if ( source == system.Emitters.end() )
                    return Common::MakeFormattedError<bool>( "{} event handler {}: SourceEmitter '{}' names no "
                                                             "emitter of the system",
                                                             at, h, handler.SourceEmitter );
                if ( handler.SpawnCount == 0 )
                    return Common::MakeFormattedError<bool>( "{} event handler {}: SpawnCount 0 spawns nothing",
                                                             at, h );
                if ( !std::isfinite( handler.InheritVelocity ) )
                    return Common::MakeFormattedError<bool>(
                         "{} event handler {}: InheritVelocity {} is not finite", at, h, handler.InheritVelocity );
                if ( handler.Event == VFXEventKind::Collision &&
                     std::none_of( source->Stack.ParticleUpdate.begin(), source->Stack.ParticleUpdate.end(),
                                   []( const VFXModuleUse& use )
                                   { return use.Enabled && use.Module == kVFXCollisionModule; } ) )
                    return Common::MakeFormattedError<bool>( "{} event handler {}: a Collision handler of '{}', "
                                                             "whose ParticleUpdate has no enabled {}",
                                                             at, h, handler.SourceEmitter, kVFXCollisionModule );
            }
            return BOOLSUCCESS;
        }

        Common::BoolResultStr CheckGroup( const std::vector<VFXModuleUse>& group, const VFXSystemData& system,
                                          const std::string& where )
        {
            for ( std::size_t i = 0; i < group.size(); ++i )
            {
                const VFXModuleUse& use = group[i];
                const std::string   at  = std::format( "{} module {} '{}'", where, i, use.Module.Path );
                if ( const auto guid = Common::Content::AssetGuidFromText( use.Module.Guid );
                     !guid || guid.GetValue().IsNull() )
                    return Common::MakeFormattedError<bool>( "{}: Module GUID '{}' is not well-formed", at,
                                                             use.Module.Guid );
                if ( use.Module.Path.empty() )
                    return Common::MakeFormattedError<bool>( "{}: Module {} states no Path", at, use.Module.Guid );
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

        Common::BoolResultStr CheckGuidRef( const AssetGuidRef& ref, const VFXEmitterData& e, const std::size_t i,
                                            const std::string_view member )
        {
            const auto guid = Common::Content::AssetGuidFromText( ref.Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<bool>(
                     "emitter '{}' renderer {} names {} '{}', not a well-formed "
                     "GUID",
                     e.Name, i, member, ref.Guid );
            if ( ref.Path.empty() )
                return Common::MakeFormattedError<bool>( "emitter '{}' renderer {} names {} {} without a Path",
                                                         e.Name, i, member, ref.Guid );
            return BOOLSUCCESS;
        }

        /// A renderer row (UE UNiagaraRendererProperties): exactly the settings member its Kind names, well-formed
        /// material and mesh references, and settings a draw can honour. Any number of rows of any kind: every
        /// enabled row is its own draw (ParticleRenderer's ViewDraw).
        Common::BoolResultStr CheckRenderers( const VFXEmitterData& e )
        {
            for ( std::size_t i = 0; i < e.Renderers.size(); ++i )
            {
                const VFXRendererData& r      = e.Renderers[i];
                const bool             mesh   = r.Kind == VFXRendererKind::Mesh;
                const bool             ribbon = r.Kind == VFXRendererKind::Ribbon;
                if ( r.Mesh.has_value() != mesh || r.Ribbon.has_value() != ribbon )
                    return Common::MakeFormattedError<bool>( "emitter '{}' renderer {}: exactly the member its "
                                                             "Kind names must be present (Mesh/Ribbon)",
                                                             e.Name, i );
                if ( r.Material )
                    if ( auto ok = CheckGuidRef( *r.Material, e, i, "Material" ); !ok )
                        return ok;
                if ( mesh )
                    if ( auto ok = CheckGuidRef( r.Mesh->Mesh, e, i, "Mesh" ); !ok )
                        return ok;
                if ( ribbon && !( r.Ribbon->UVTiling > 0.0f ) )
                    return Common::MakeFormattedError<bool>(
                         "emitter '{}' renderer {}: Ribbon UVTiling is {}, it must be > 0", e.Name, i,
                         r.Ribbon->UVTiling );
            }
            return BOOLSUCCESS;
        }
    } // namespace

    std::vector<std::string> VFXSystemDependencies( const VFXSystemData& data )
    {
        std::set<std::string> guids;
        for ( const VFXEmitterData& e : data.Emitters )
        {
            for ( const auto* group : { &e.Stack.EmitterUpdate, &e.Stack.ParticleSpawn, &e.Stack.ParticleUpdate } )
                for ( const VFXModuleUse& use : *group )
                    guids.insert( use.Module.Guid );
            for ( const VFXRendererData& r : e.Renderers )
            {
                if ( r.Material )
                    guids.insert( r.Material->Guid );
                if ( r.Mesh )
                    guids.insert( r.Mesh->Mesh.Guid );
            }
        }
        return { guids.begin(), guids.end() };
    }

    Common::AssetHandle VFXRendererMaterialHandle( const VFXRendererData& renderer )
    {
        if ( !renderer.Material )
            return Common::AssetHandle::Null();
        const auto guid = Common::Content::AssetGuidFromText( renderer.Material->Guid );
        if ( !guid )
            return Common::AssetHandle::Null();
        return Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) );
    }

    Common::AssetHandle VFXRendererMeshHandle( const VFXRendererData& renderer )
    {
        if ( !renderer.Mesh )
            return Common::AssetHandle::Null();
        const auto guid = Common::Content::AssetGuidFromText( renderer.Mesh->Mesh.Guid );
        if ( !guid )
            return Common::AssetHandle::Null();
        return Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) );
    }

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

        std::set<std::string> emitters;
        for ( const VFXEmitterData& e : data.Emitters )
        {
            if ( e.Name.empty() || !emitters.insert( e.Name ).second )
                return Common::MakeFormattedError<bool>( "emitter name '{}' is empty or repeated", e.Name );
            if ( e.Capacity == 0 )
                return Common::MakeFormattedError<bool>( "emitter '{}' has Capacity 0", e.Name );
            if ( !Finite( e.Bounds.Min ) || !Finite( e.Bounds.Max ) || e.Bounds.Min.x > e.Bounds.Max.x ||
                 e.Bounds.Min.y > e.Bounds.Max.y || e.Bounds.Min.z > e.Bounds.Max.z )
                return Common::MakeFormattedError<bool>(
                     "emitter '{}' Bounds are not a finite box with Min <= Max (Min {} {} {}, Max {} {} {})",
                     e.Name, e.Bounds.Min.x, e.Bounds.Min.y, e.Bounds.Min.z, e.Bounds.Max.x, e.Bounds.Max.y,
                     e.Bounds.Max.z );
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
            if ( auto ok = CheckRenderers( e ); !ok )
                return ok;
            if ( auto ok = CheckEventHandlers( e, data, at ); !ok )
                return ok;
        }
        return BOOLSUCCESS;
    }

    bool VFXCategoryRegister::Contains( const std::string_view id ) const
    {
        return std::ranges::any_of( Categories, [id]( const VFXCategory& c ) { return c.Id == id; } );
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

        // The header's Dependencies are the cook's and the packager's edges (ContentRegistry::Closure): a module,
        // material or mesh a row names and the header leaves out would be missing from a packaged game, so they
        // must agree.
        if ( const std::vector<std::string> expected = VFXSystemDependencies( data );
             !data.Header || data.Header->Dependencies != expected )
            return Common::MakeFormattedError<VFXSystemData>(
                 "the header states {} Dependencies but the stack and renderer rows name {} module(s), material(s) "
                 "and mesh(es); they must be exactly those GUIDs, sorted - run Tools/SceneMigrator",
                 data.Header ? data.Header->Dependencies.size() : std::size_t{ 0 }, expected.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteVFXSystem( const VFXSystemData& data )
    {
        VFXSystemData out = data;
        out.Header        = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::VFXSystem,
                                                     VFXSystemTextSubsystems() );
        out.Header->Dependencies = VFXSystemDependencies( data );
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
