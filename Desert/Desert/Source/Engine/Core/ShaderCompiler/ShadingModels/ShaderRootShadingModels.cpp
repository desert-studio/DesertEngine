#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>

#include <Common/Core/Constants.hpp>

#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace Desert::Core::ShadingModels
{
    namespace
    {
        using LoadResult = Common::ResultStr<LoadedShadingModels>;

        // Writes the generated include only when its bytes differ, so an unchanged set never touches the file's
        // write time (the per-process shader file cache compares it).
        std::string WriteIfChanged( const std::filesystem::path& path, const std::string& text )
        {
            {
                std::ifstream      in( path, std::ios::binary );
                std::ostringstream current;
                current << in.rdbuf();
                if ( in && current.str() == text )
                    return {};
            }
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

    const Common::ResultStr<LoadedShadingModels>& ShaderRootShadingModels()
    {
        // Keyed by the ABSOLUTE root: a process that changes directory (a test on a private copy of the root)
        // gets that root's set, never the first one's.
        static std::mutex                                                   s_Mutex;
        static std::map<std::filesystem::path, std::unique_ptr<LoadResult>> s_Loaded;

        const std::filesystem::path root =
             std::filesystem::absolute( Common::Constants::Path::SHADERDIR_PATH ).lexically_normal();
        std::lock_guard lock( s_Mutex );
        auto&           slot = s_Loaded[root];
        if ( !slot )
            slot = std::make_unique<LoadResult>( Load( root ) );
        return *slot;
    }
} // namespace Desert::Core::ShadingModels
