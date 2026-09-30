#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace Desert::Core::ShadingModels
{
    namespace
    {
        using LoadResult = Common::ResultStr<LoadedShadingModels>;

        LoadResult Load( const std::filesystem::path& shaderRoot )
        {
            auto registry = ShadingModelRegistry::Scan( shaderRoot );
            if ( !registry.IsSuccess() )
                return Common::MakeError<LoadedShadingModels>( registry.GetError() );

            LoadedShadingModels loaded;
            loaded.Registry       = registry.ExtractValue();
            loaded.GeneratedGlsl  = loaded.Registry.GenerateGlsl();
            loaded.IndexLayoutKey = loaded.Registry.IndexLayoutKey();
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
        const std::lock_guard       lock( sets.Mutex );
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
        // two different sets into one slot.
        const std::lock_guard lock( sets.Mutex );
        auto                  loaded = std::make_shared<const LoadResult>( Load( root ) );
        if ( !loaded->IsSuccess() )
            return Common::MakeError<bool>( loaded->GetError() );
        sets.Loaded[root] = std::move( loaded );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Core::ShadingModels
