// EVERY COMMITTED TEXT ASSET READS STRICTLY (JS1b). The asset formats moved onto Common::Json, whose reads
// refuse a missing field and an unknown key instead of filling in a default. That move is only safe if no
// file the repository ships leans on the old leniency, so this walks every committed file of each moved kind
// (git ls-files: the editor's assets, the cooked meshes, the test fixtures) and reads it through the format's own
// parser. A file that fails here is either migrated or its field is made std::optional with a reason, never read
// leniently to hide it. Prefabs are walked by PrefabVersionGate, which reads the same corpus the same way.

#include <gtest/gtest.h>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/Serialization/ShaderGraph.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/UIThemeData.hpp>
#include <Engine/Localization/StringTable.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace
{
    namespace fs = std::filesystem;

    fs::path RepoRoot()
    {
        fs::path at = fs::current_path();
        for ( int up = 0; up < 8; ++up, at = at.parent_path() )
            if ( fs::exists( at / "Desert/Common/Source/Common/Json/Json.hpp" ) )
                return at;
        return {};
    }

    std::string ReadAll( const fs::path& file )
    {
        std::ifstream      in( file, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::vector<std::string> TrackedFiles( const fs::path& root )
    {
        std::vector<std::string> files;
        const std::string        command = "git -C \"" + root.string() + "\" ls-files";
        FILE*                    pipe    = popen( command.c_str(), "r" );
        if ( pipe == nullptr )
            return files;
        std::array<char, 4096> line{};
        while ( fgets( line.data(), static_cast<int>( line.size() ), pipe ) != nullptr )
        {
            std::string rel( line.data() );
            while ( !rel.empty() && ( rel.back() == '\n' || rel.back() == '\r' ) )
                rel.pop_back();
            files.push_back( std::move( rel ) );
        }
        pclose( pipe );
        return files;
    }

    // Extension -> "" when the file reads, else the parser's refusal.
    using Reader = std::function<std::string( const fs::path&, const std::string& )>;

    template <typename Result>
    std::string Verdict( const Result& result )
    {
        return result ? std::string() : std::string( result.GetError() );
    }

    const std::map<std::string, Reader>& Readers()
    {
        namespace S                                        = Desert::Assets::Serialization;
        static const std::map<std::string, Reader> readers = {
             { ".danimgraph", []( const fs::path&, const std::string& t )
               { return Verdict( Desert::Animation::Graph::Deserialize( t ) ); } },
             { ".decloudtype", []( const fs::path&, const std::string& t )
               { return Verdict( Desert::Assets::ParseCloudType( t ) ); } },
             { ".demat", []( const fs::path& f, const std::string& t )
               { return Verdict( Desert::Assets::ParseMaterialJson( f.generic_string(), t ) ); } },
             { ".anim",
               []( const fs::path&, const std::string& t ) { return Verdict( S::ReadAnimationJson( t ) ); } },
             { ".derig",
               []( const fs::path&, const std::string& t ) { return Verdict( S::ParseControlRig( t ) ); } },
             { ".retarget",
               []( const fs::path&, const std::string& t ) { return Verdict( S::ParseRetarget( t ) ); } },
             { ".dgraph", []( const fs::path&, const std::string& t )
               { return Verdict( S::ShaderGraph::ParseShaderGraph( t ) ); } },
             { ".skeleton",
               []( const fs::path&, const std::string& t ) { return Verdict( S::ReadSkeletonJson( t ) ); } },
             { ".detheme", []( const fs::path&, const std::string& t )
               { return Verdict( Desert::Assets::ParseUITheme( t ) ); } },
             { ".destrings", []( const fs::path&, const std::string& t )
               { return Verdict( Desert::Localization::ParseStringTable( t ) ); } },
        };
        return readers;
    }
} // namespace

TEST( StrictCorpus, EveryCommittedFileOfAMovedFormatReadsStrictly )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";

    // COMMITTED files, asked of git and not of the disk: a build tree also holds generated files the
    // repository ignores (a locally cooked probe rig), and those are not the corpus this proves.
    std::map<std::string, int> read;
    for ( const std::string& rel : TrackedFiles( root ) )
    {
        const auto reader = Readers().find( fs::path( rel ).extension().string() );
        if ( reader == Readers().end() )
            continue;
        ++read[reader->first];
        EXPECT_EQ( reader->second( root / rel, ReadAll( root / rel ) ), "" ) << rel;
    }

    // Every moved format has committed files; a kind that reads none means the walk missed it.
    for ( const auto& [extension, reader] : Readers() )
        EXPECT_GT( read[extension], 0 ) << "no committed " << extension << " file was found";
}
