// THE ENGINE'S CPU-SIDE DIRECTORIES REACH NO VULKAN OR WINDOW-SYSTEM HEADER (BUILD1 P1,
// BuildScripts/BUILD1-CONTRACT.md §4).
//
// Before BUILD1 a suite compiled the engine sources it tested into its own binary, and a suite that
// compiled, say, Animation/Pose.cpp with no Vulkan include path PROVED, as a side effect of building, that
// Pose.cpp needs no Vulkan. The runners link Desert.lib instead (every engine source compiled once, with
// the engine's own flags), so that proof is gone unless something states it. This census states it, on
// purpose and over every file of the directory rather than over the files some suite happened to pick:
// the transitive closure of each file's quoted includes, resolved the way the engine's include paths
// resolve them, must not reach <vulkan/...>, volk, VMA, GLFW or Engine/Graphic/API/Vulkan/.
//
// The directories are the ones that were clean on 2026-10-06 (measured over the whole tree, not chosen).
// A directory that is mostly CPU-side but holds a GPU-side file lists that file, with what it is for.

#include "TestSupport/source_roots.hpp"
#include <gtest/gtest.h>

#include "../../TestSupport/scratch_dir.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kCpuSideDirectories[] = { "Animation",    "Audio",   "Geometry", "Hair",
                                                    "Localization", "Physics", "Project",  "Reflection",
                                                    "Text",         "Vector",  "World" };

    struct GpuSideFile
    {
        const char* path; // relative to Engine/
        const char* why;
    };
    constexpr GpuSideFile kGpuSideFiles[] = {
         { "World/Foliage/FoliagePrefabsScene.cpp", "instantiates foliage prefabs into a live Scene" },
    };

    // The checkout the build baked in (scratch_dir.hpp), not one searched for from the working directory.
    fs::path RepoRoot()
    {
        return Desert::TestSupport::RepositoryRoot();
    }

    bool IsWindowOrGpuApiHeader( const std::string& include )
    {
        static const std::regex kForbidden(
             R"(^(vulkan/|volk\.h|vk_mem_alloc|GLFW/|Engine/Graphic/API/Vulkan/))" );
        return std::regex_search( include, kForbidden );
    }

    std::vector<std::string> Includes( const fs::path& file )
    {
        static const std::regex  kInclude( R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])" );
        std::ifstream            in( file );
        std::vector<std::string> out;
        for ( std::string line; std::getline( in, line ); )
        {
            std::smatch m;
            if ( std::regex_search( line, m, kInclude ) )
                out.push_back( m[1].str() );
        }
        return out;
    }

    // Resolves include paths the way the engine's include directories do: next to the including file, then
    // every library tree (TestSupport/source_roots.hpp). Third-party headers do not resolve and are not walked.
    class IncludeGraph
    {
    public:
        explicit IncludeGraph( const fs::path& root )
             : m_Roots( Desert::TestSupport::Under( root, Desert::TestSupport::LibraryRoots() ) )
        {
        }

        // The include chain from `file` to the first forbidden header, or nullopt when there is none. A
        // depth-first walk over an explicit stack: each frame is a file and the position of its next include. The
        // memo holds every file entered, so a file already on the current path (a cycle) contributes nothing new.
        std::optional<std::string> ChainToForbidden( const fs::path& file )
        {
            const fs::path root = file.lexically_normal();
            if ( const auto it = m_Memo.find( root ); it != m_Memo.end() )
                return it->second;

            std::vector<Frame> stack;
            Enter( stack, root );
            while ( !stack.empty() )
            {
                Frame& top = stack.back();
                if ( top.Next == top.Headers.size() )
                {
                    stack.pop_back(); // every include walked, none forbidden: the memo keeps nullopt
                    continue;
                }
                const std::string include = top.Headers[top.Next++];
                if ( IsWindowOrGpuApiHeader( include ) )
                    return Settle( stack, include );
                const auto resolved = Resolve( top.Key, include );
                if ( !resolved )
                    continue;
                if ( const auto it = m_Memo.find( *resolved ); it != m_Memo.end() )
                {
                    if ( const std::optional<std::string>& chain = it->second; chain.has_value() )
                        return Settle( stack, std::format( "{} -> {}", include, chain.value() ) );
                    continue;
                }
                Enter( stack, *resolved );
            }
            return std::nullopt;
        }

    private:
        struct Frame
        {
            fs::path                 Key;
            std::vector<std::string> Headers;
            std::size_t              Next = 0;
        };

        void Enter( std::vector<Frame>& stack, const fs::path& key )
        {
            m_Memo[key] = std::nullopt;
            stack.push_back( Frame{ .Key = key, .Headers = Includes( key ), .Next = 0 } );
        }

        // `chain` starts at the top frame's file; every frame below reached it through its last-read include.
        std::string Settle( const std::vector<Frame>& stack, std::string chain )
        {
            m_Memo[stack.back().Key] = chain;
            for ( auto frame = stack.rbegin() + 1; frame != stack.rend(); ++frame )
            {
                chain              = std::format( "{} -> {}", frame->Headers[frame->Next - 1], chain );
                m_Memo[frame->Key] = chain;
            }
            return chain;
        }

        [[nodiscard]] std::optional<fs::path> Resolve( const fs::path& from, const std::string& include ) const
        {
            if ( fs::path local = ( from.parent_path() / include ).lexically_normal();
                 fs::is_regular_file( local ) )
                return local;
            for ( const fs::path& base : m_Roots )
            {
                if ( fs::path p = ( base / include ).lexically_normal(); fs::is_regular_file( p ) )
                    return p;
            }
            return std::nullopt;
        }

        std::vector<fs::path>                          m_Roots;
        std::map<fs::path, std::optional<std::string>> m_Memo;
    };

    TEST( EngineIncludeLayering, ForbiddenHeadersAreTheGpuApiAndTheWindowSystem )
    {
        EXPECT_TRUE( IsWindowOrGpuApiHeader( "vulkan/vulkan.h" ) );
        EXPECT_TRUE( IsWindowOrGpuApiHeader( "volk.h" ) );
        EXPECT_TRUE( IsWindowOrGpuApiHeader( "vk_mem_alloc.h" ) );
        EXPECT_TRUE( IsWindowOrGpuApiHeader( "GLFW/glfw3.h" ) );
        EXPECT_TRUE( IsWindowOrGpuApiHeader( "Engine/Graphic/API/Vulkan/VulkanDevice.hpp" ) );
        EXPECT_FALSE( IsWindowOrGpuApiHeader( "Engine/Graphic/ViewSettings.hpp" ) );
        EXPECT_FALSE( IsWindowOrGpuApiHeader( "glm/glm.hpp" ) );
    }

    TEST( EngineIncludeLayering, CpuSideDirectoriesReachNoGpuApiHeader )
    {
        const fs::path root = RepoRoot();
        ASSERT_TRUE( fs::is_directory( root / "Desert" / "Tests" / "TestSupport" ) )
             << root << " (DESERT_TEST_REPO_ROOT) is not the checkout";
        const fs::path     engine = root / "Desert" / "Desert" / "Source" / "Engine";
        std::set<fs::path> gpuSide;
        for ( const GpuSideFile& file : kGpuSideFiles )
        {
            const fs::path p = ( engine / file.path ).lexically_normal();
            EXPECT_TRUE( fs::is_regular_file( p ) )
                 << "kGpuSideFiles names " << file.path << ", which does not exist";
            gpuSide.insert( p );
        }
        IncludeGraph graph( root );
        size_t       checked = 0;
        for ( const char* directory : kCpuSideDirectories )
        {
            ASSERT_TRUE( fs::is_directory( engine / directory ) ) << "Engine/" << directory << " no longer exists";
            for ( const auto& entry : fs::recursive_directory_iterator( engine / directory ) )
            {
                const std::string ext = entry.path().extension().string();
                if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                    continue;
                const fs::path file = entry.path().lexically_normal();
                ++checked;
                const auto chain = graph.ChainToForbidden( file );
                if ( gpuSide.contains( file ) )
                {
                    EXPECT_TRUE( chain.has_value() )
                         << fs::relative( file, engine ).generic_string()
                         << " no longer reaches a GPU header: remove it from kGpuSideFiles";
                    continue;
                }
                EXPECT_FALSE( chain.has_value() ) << "Engine/" << fs::relative( file, engine ).generic_string()
                                                  << " reaches a GPU/window header: " << chain.value_or( "" )
                                                  << ". This directory is CPU-side; move the GPU part out of it";
            }
        }
        EXPECT_GT( checked, 250u ) << "the census walked almost nothing: it would pass blind";
    }
} // namespace
