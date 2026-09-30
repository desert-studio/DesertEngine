#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

namespace Desert::Core::ShadingModels
{
    namespace
    {
        using LoadResult = Common::ResultStr<LoadedShadingModels>;

        // Writes the generated include only when its bytes differ, so an unchanged set never touches the file's
        // write time (the per-process shader file cache compares it). The comparison reads through the VFS: in
        // a packaged game the include is the cooked copy in the pak and matches, so the player's install is
        // never written to.
        std::string WriteIfChanged( const std::filesystem::path& path, const std::string& text )
        {
            const auto current = Common::Utils::FileSystem::ReadFileContentIfExists( path );
            if ( current.IsSuccess() && current.GetValue().has_value() && *current.GetValue() == text )
                return {};
            std::ofstream out( path, std::ios::binary | std::ios::trunc );
            out << text;
            out.close();
            if ( !out )
                return std::format( "{}: cannot write the generated shading-model include",
                                    path.generic_string() );
            return {};
        }

        LoadResult Load( const std::filesystem::path& shaderRoot )
        {
            auto registry = ShadingModelRegistry::Scan( shaderRoot );
            if ( !registry.IsSuccess() )
                return Common::MakeError<LoadedShadingModels>( registry.GetError() );

            LoadedShadingModels loaded;
            loaded.Registry       = registry.ExtractValue();
            loaded.GeneratedGlsl  = loaded.Registry.GenerateGlsl();
            loaded.IndexLayoutKey = loaded.Registry.IndexLayoutKey();
            if ( const std::string error = WriteIfChanged( shaderRoot / kGeneratedInclude, loaded.GeneratedGlsl );
                 !error.empty() )
                return Common::MakeError<LoadedShadingModels>( error );
            return Common::MakeSuccess( std::move( loaded ) );
        }
    } // namespace

    namespace
    {
        struct RootSets
        {
            std::mutex                                     Mutex;
            std::map<std::filesystem::path, ShaderRootSet> Loaded;
        };

        RootSets& Sets()
        {
            static RootSets s_Sets;
            return s_Sets;
        }

        // Keyed by the ABSOLUTE root: a process that changes directory (a test on a private copy of the root)
        // gets that root's set, never the first one's.
        std::filesystem::path CurrentRoot()
        {
            return std::filesystem::absolute( Common::Constants::Path::SHADERDIR_PATH ).lexically_normal();
        }
    } // namespace

    ShaderRootSet ShaderRootShadingModels()
    {
        const std::filesystem::path root = CurrentRoot();
        RootSets&                   sets = Sets();
        std::lock_guard             lock( sets.Mutex );
        auto&                       slot = sets.Loaded[root];
        if ( !slot )
            slot = std::make_shared<const LoadResult>( Load( root ) );
        return slot;
    }

    Common::BoolResultStr ReloadShaderRootShadingModels()
    {
        const std::filesystem::path root = CurrentRoot();
        RootSets&                   sets = Sets();
        // Under the lock for the whole scan: two reloads (or a first load racing a reload) must not write
        // kGeneratedInclude twice from two different sets.
        std::lock_guard lock( sets.Mutex );
        auto            loaded = std::make_shared<const LoadResult>( Load( root ) );
        if ( !loaded->IsSuccess() )
            return Common::MakeError<bool>( loaded->GetError() );
        sets.Loaded[root] = std::move( loaded );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Core::ShadingModels
