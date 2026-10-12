#include <Engine/VFX/VFXModuleLibrary.hpp>

#include <Engine/VFX/VFXEmitterSpawn.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    namespace
    {
        VFXModuleDecl Decl( const char* name, VFXValueType type, const char* units,
                            std::optional<VFXInputRange> range, std::optional<glm::vec4> value )
        {
            return VFXModuleDecl{ name, type, units, range, value };
        }
    } // namespace

    std::span<const VFXModuleInfo> CpuModuleLibrary()
    {
        using T                                       = VFXValueType;
        static const std::vector<VFXModuleInfo> table = {
             { std::string( kVFXSpawnRateModule ),
               { Decl( "SpawnRate", T::Float, "1/s", VFXInputRange{ 0.0f, 5000.0f }, glm::vec4( 100, 0, 0, 0 ) ) },
               true },
             { std::string( kVFXSpawnBurstModule ),
               { Decl( "SpawnCount", T::Int, "", VFXInputRange{ 0.0f, 5000.0f }, glm::vec4( 50, 0, 0, 0 ) ),
                 Decl( "SpawnTime", T::Float, "s", VFXInputRange{ 0.0f, 10.0f }, glm::vec4( 0.0f ) ) },
               true },
             { std::string( kVFXSpawnFromChannelModule ),
               { Decl( "ParticlesPerEntry", T::Int, "", VFXInputRange{ 1.0f, 1000.0f }, glm::vec4( 1, 0, 0, 0 ) ),
                 Decl( "MaxEntriesPerFrame", T::Int, "", VFXInputRange{ 1.0f, 4096.0f },
                       glm::vec4( 64, 0, 0, 0 ) ),
                 Decl( "MaxDistance", T::Float, "cm", VFXInputRange{ 0.0f, 100000.0f }, glm::vec4( 0.0f ) ),
                 Decl( "FilterOp", T::Int, "", std::nullopt, std::nullopt ),
                 Decl( "FilterValue", T::Float, "", std::nullopt, std::nullopt ) },
               false },
        };
        return table;
    }

    Common::ResultStr<std::vector<VFXModuleInfo>> ReadParticleModuleLibrary( const std::filesystem::path& dir )
    {
        std::error_code ec;
        if ( !std::filesystem::is_directory( dir, ec ) )
            return Common::MakeFormattedError<std::vector<VFXModuleInfo>>(
                 "the engine module directory '{}' does not exist", dir.string() );
        std::vector<VFXModuleInfo> modules;
        for ( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
                continue;
            std::ifstream     in( entry.path(), std::ios::binary );
            const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            auto              parsed = ParseParticleModule( text, entry.path().string() );
            if ( !parsed )
                return Common::MakeFormattedError<std::vector<VFXModuleInfo>>( "{}", parsed.GetError() );
            modules.push_back( VFXModuleInfo{ std::string( S::kVFXEnginePrefix ) + entry.path().stem().string(),
                                              parsed.ExtractValue().Inputs, true } );
        }
        if ( ec )
            return Common::MakeFormattedError<std::vector<VFXModuleInfo>>( "'{}' could not be listed: {}",
                                                                           dir.string(), ec.message() );
        std::ranges::sort( modules, {}, &VFXModuleInfo::Module );
        return Common::MakeSuccess( std::move( modules ) );
    }

    S::VFXModuleUse DefaultModuleUse( const VFXModuleInfo& module )
    {
        S::VFXModuleUse use;
        use.Module = module.Module;
        for ( const VFXModuleDecl& decl : module.Inputs )
        {
            S::VFXModuleInput in;
            in.Name   = decl.Name;
            in.Type   = decl.Type;
            in.Source = S::VFXInputSource::Value;
            in.Value  = decl.Default.value_or( glm::vec4( 0.0f ) );
            use.Inputs.push_back( std::move( in ) );
        }
        return use;
    }

    const VFXModuleDecl* FindModuleInput( std::span<const VFXModuleInfo> library, std::string_view module,
                                          std::string_view input )
    {
        for ( const VFXModuleInfo& info : library )
        {
            if ( info.Module != module )
                continue;
            for ( const VFXModuleDecl& decl : info.Inputs )
                if ( decl.Name == input )
                    return &decl;
        }
        return nullptr;
    }
} // namespace Desert::VFX
