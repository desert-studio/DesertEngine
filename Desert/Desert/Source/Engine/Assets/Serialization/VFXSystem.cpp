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
            if ( b.starts_with( kVFXPointSetPrefix ) && b.size() > kVFXPointSetPrefix.size() )
            {
                if ( in.Type != VFXValueType::Vec3 )
                    return Common::MakeFormattedError<bool>( "{}: '{}' is a point set, read only by a Vec3 input",
                                                             what, b );
                return BOOLSUCCESS; // the set is the placement's (VFXComponentData::PointSets), not the file's
            }
            if ( b.starts_with( kVFXDataChannelPrefix ) && b.size() > kVFXDataChannelPrefix.size() )
                return BOOLSUCCESS; // the channel is the scene's: CompileSpawnPlan reads the spelling, the world
                                    // binds the fields to the channel's layout (VFX/VFXDataChannel
                                    // BindChannelSpawn)
            return Common::MakeFormattedError<bool>(
                 "{}: Binding '{}' is neither User.<param>, Particles.<attr>, PointSet.<set> nor "
                 "DataChannel.<channel>[.<field>]",
                 what, b );
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

        Common::BoolResultStr CheckRenderer( const VFXRendererData& r, const std::string& at )
        {
            if ( r.Material && r.Material->Guid.empty() )
                return Common::MakeFormattedError<bool>( "{}: Material states no Guid", at );
            if ( r.Ribbon.has_value() != ( r.Kind == VFXRendererKind::Ribbon ) )
                return Common::MakeFormattedError<bool>( "{}: Ribbon settings are stated exactly on a Ribbon row",
                                                         at );
            if ( r.Mesh.has_value() != ( r.Kind == VFXRendererKind::Mesh ) )
                return Common::MakeFormattedError<bool>( "{}: Mesh settings are stated exactly on a Mesh row",
                                                         at );
            if ( r.Light.has_value() != ( r.Kind == VFXRendererKind::Light ) )
                return Common::MakeFormattedError<bool>( "{}: Light settings are stated exactly on a Light row",
                                                         at );
            if ( r.Light )
            {
                const VFXLightRendererData& l = *r.Light;
                if ( r.Material )
                    return Common::MakeFormattedError<bool>( "{}: a Light row draws nothing and takes no Material",
                                                             at );
                if ( !std::isfinite( l.RadiusScale ) || l.RadiusScale <= 0.0f || !std::isfinite( l.Intensity ) ||
                     l.Intensity < 0.0f || !std::isfinite( l.ColorAdd.x ) || !std::isfinite( l.ColorAdd.y ) ||
                     !std::isfinite( l.ColorAdd.z ) || l.ColorAdd.x < 0.0f || l.ColorAdd.y < 0.0f ||
                     l.ColorAdd.z < 0.0f )
                    return Common::MakeFormattedError<bool>(
                         "{}: light RadiusScale {} not > 0, or Intensity {} / ColorAdd not finite and >= 0", at,
                         l.RadiusScale, l.Intensity );
                if ( l.MaxLights == 0u || l.MaxLights > kVFXLightMaxPerEmitter )
                    return Common::MakeFormattedError<bool>( "{}: light MaxLights {} is outside 1 .. {}", at,
                                                             l.MaxLights, kVFXLightMaxPerEmitter );
            }
            if ( r.Ribbon )
            {
                const VFXRibbonRendererData& rb = *r.Ribbon;
                if ( !std::isfinite( rb.Width ) || rb.Width <= 0.0f || !std::isfinite( rb.UVTileDistance ) ||
                     rb.UVTileDistance <= 0.0f )
                    return Common::MakeFormattedError<bool>( "{}: ribbon Width {} / UVTileDistance {} not > 0", at,
                                                             rb.Width, rb.UVTileDistance );
                if ( !rb.RibbonIdBinding.empty() && ( !rb.RibbonIdBinding.starts_with( kVFXParticlesPrefix ) ||
                                                      rb.RibbonIdBinding.size() == kVFXParticlesPrefix.size() ) )
                    return Common::MakeFormattedError<bool>(
                         "{}: RibbonIdBinding '{}' is not Particles.<attribute>", at, rb.RibbonIdBinding );
            }
            if ( r.Mesh )
            {
                if ( r.Mesh->Mesh.Guid.empty() )
                    return Common::MakeFormattedError<bool>( "{}: Mesh states no Guid", at );
                const glm::vec3& sc = r.Mesh->Scale;
                if ( !Finite( sc ) || sc.x <= 0.0f || sc.y <= 0.0f || sc.z <= 0.0f )
                    return Common::MakeFormattedError<bool>( "{}: mesh Scale is not finite and > 0", at );
            }
            return BOOLSUCCESS;
        }

        // The header's Dependencies of a system: its renderer rows' materials and meshes, each once, in emitter
        // then row order.
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

    glm::vec4 UserParamValue( const VFXUserParam& row, const std::vector<ECS::VFXUserOverride>& overrides )
    {
        for ( const ECS::VFXUserOverride& o : overrides )
            if ( o.Name == row.Name )
                return o.Value;
        return row.Default;
    }

    Common::BoolResultStr ValidateVFXSystemData( const VFXSystemData& data )
    {
        if ( !data.EffectType || data.EffectType->empty() ||
             data.EffectType->find_first_of( "/\\." ) != std::string::npos )
            return Common::MakeFormattedError<bool>( "EffectType '{}' is not an effect type name (a .dfxtype stem "
                                                     "in the VFX folder, e.g. \"Default\")",
                                                     data.EffectType.value_or( "" ) );
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
            if ( auto ok = CheckGroup( e.Stack.EmitterSpawn, data, std::format( "{} EmitterSpawn", at ) ); !ok )
                return ok;
            if ( auto ok = CheckGroup( e.Stack.EmitterUpdate, data, std::format( "{} EmitterUpdate", at ) ); !ok )
                return ok;
            if ( auto ok = CheckGroup( e.Stack.ParticleSpawn, data, std::format( "{} ParticleSpawn", at ) ); !ok )
                return ok;
            if ( auto ok = CheckGroup( e.Stack.ParticleUpdate, data, std::format( "{} ParticleUpdate", at ) );
                 !ok )
                return ok;
            for ( std::size_t i = 0; i < e.Renderers.size(); ++i )
                if ( auto ok = CheckRenderer( e.Renderers[i], std::format( "{} renderer {}", at, i ) ); !ok )
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

        // ONE REFERENCE, TWO STATEMENTS OF IT: the registry, the cook and the packager read the edges from the
        // header (ContentRegistry::Closure), the renderer and the stack compiler from the rows. A module or asset a
        // row names and the header leaves out would be missing from a packaged game.
        const auto  expected = VFXSystemDependencies( data );
        const auto& stated   = data.Header ? data.Header->Dependencies : std::vector<std::string>{};
        if ( stated.size() != expected.size() ||
             !std::is_permutation( stated.begin(), stated.end(), expected.begin() ) )
            return Common::MakeFormattedError<VFXSystemData>(
                 "the header states {} Dependencies; the system references exactly its {} module(s), renderer "
                 "material(s) and mesh(es) - run Tools/SceneMigrator",
                 stated.size(), expected.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    std::vector<std::string> VFXSystemDependencies( const VFXSystemData& data )
    {
        // Every asset a row names: each stack module (`.dfxmod`), each renderer's material and mesh. Sorted, each
        // once, so a rewrite of the same system states the same header.
        std::set<std::string> guids;
        for ( const VFXEmitterData& e : data.Emitters )
        {
            for ( const auto* group : { &e.Stack.EmitterSpawn, &e.Stack.EmitterUpdate, &e.Stack.ParticleSpawn,
                                        &e.Stack.ParticleUpdate } )
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
