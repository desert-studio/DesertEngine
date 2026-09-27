// "NO CALL SITE WRITES THROUGH A MAPPING IT DID NOT CHECK" — AS A CENSUS, NOT AS A CHECK OF ONE PLACE.
//
// `MappedMemoryGuard` proves the guarded type cannot leak a pointer. It cannot prove that the production
// code goes through it — that is a statement about the SOURCE TEXT, and this suite makes it one. The two
// halves fail in opposite directions and neither alone is evidence: a correct type nobody uses is worth
// exactly as much as no type at all.
//
// WHAT THIS SCAN REPORTED ON `dev` (85a80feb), RUN RATHER THAN ESTIMATED: **11 of 16 mapping call sites
// unsafe**. Reproduce it by restoring the twelve production files from `dev` and running this binary; the
// per-site verdicts are the enumeration below.
//
//     10  passed the result straight to memcpy/memset with nothing between   <- these ten crash on null
//      1  checked it, refused, and still held a RAW POINTER (VulkanSwapChain::TakeCapturedFrameRGBA8)
//      1  checked it before its memset (VulkanUniformBuffer::RT_Invalidate)
//      4  bound the pointer and never dereferenced it at the site
//
// The census flags eleven, not ten, and the eleventh is deliberate: VulkanSwapChain did everything right
// and is flagged anyway, because rule (a) below is about the SHAPE. A `void* mapped = ...` with a hand-
// written `if` beside it is one careless edit away from the other ten, and the whole argument of this task
// is that the ten were written by people who did not know while the one was written by somebody who did.
// A census that graded intent rather than shape would have to be re-earned at every edit.
//
// THE NUMBERS IN THE BRIEF THAT OPENED THE TASK, CHECKED. "Sixteen call sites" is exact. "One checks" is
// two — VulkanUniformBuffer::RT_Invalidate tested the pointer before its memset. "Thirteen dereference
// unchecked" is TEN: four sites take the pointer without dereferencing it there, not two, because the two
// persistently-mapped buffers store it and every reader of the stored value tests it. (The comment Г7 left
// on the primitive said eleven, which is also not the crash count.) None of that changes the fix — all
// sixteen go through the type — but a number quoted in a commit message ought to be a number somebody ran.
//
// HOW THE SCAN DECIDES. Comments and literals are blanked first with the shared reader (Д33) — a census
// that counted the memcpys named in prose would report this file's own paragraphs as defects. A call site
// is a `MapMemory` token preceded by `.` or `->`, which distinguishes a CALL from the declaration and the
// definition without parsing C++. From there, two rules:
//
//   (a) the result may not be bound to a RAW POINTER, whatever is done with it next;
//   (b) no memcpy/memmove/memset in the enclosing block may name the binding unless the binding is null-
//       tested between the two.
//
// IT IS CONSERVATIVE IN THE SAFE DIRECTION, DELIBERATELY. A guard it cannot recognise reads as a defect,
// and the author must then say where the check is; the reverse — a defect it cannot see — is what makes a
// census worthless, and this one has no way to produce it: the copy has to name the binding, and the
// binding comes from the call itself.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every project source the engine compiles. Editor and Runtime are in scope because a mapping call
    // site is not confined to the backend by anything except habit.
    std::vector<fs::path> ProjectSources( const std::string& root )
    {
        std::vector<fs::path> out;
        for ( const char* tree :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
        {
            std::error_code ec;
            const fs::path  base = fs::path( root ) / tree;
            for ( auto it = fs::recursive_directory_iterator( base, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.extension() == ".cpp" || p.extension() == ".hpp" )
                    out.push_back( p );
            }
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    int LineOf( const std::string& src, std::size_t at )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + at, '\n' ) );
    }

    // The identifier a `... = <call>` binds, with any subscript removed: `m_Mappings[i] =` binds
    // `m_Mappings`, `void* dst =` binds `dst`. The shared reader's DeclaredNameBeforeAssignment answers
    // the second and not the first — it returns the LAST identifier before the `=`, which for a
    // subscripted member is the index variable, and an index variable is a word that appears in half the
    // file. Getting that wrong is how a census invents defects.
    std::string BoundName( const std::string& declaration )
    {
        std::size_t end = declaration.size();
        while ( end > 0 && std::isspace( static_cast<unsigned char>( declaration[end - 1] ) ) != 0 )
            --end;
        if ( end > 0 && declaration[end - 1] == ']' )
        {
            int depth = 0;
            while ( end > 0 )
            {
                --end;
                if ( declaration[end] == ']' )
                    ++depth;
                else if ( declaration[end] == '[' && --depth == 0 )
                    break;
            }
        }
        const std::size_t last = end;
        while ( end > 0 && Desert::Tests::ConsumerText::IsIdentChar( declaration[end - 1] ) )
            --end;
        return declaration.substr( end, last - end );
    }

    // The text left of the binding `=` in `stmt`, or empty when `stmt` binds nothing.
    std::string DeclarationBeforeAssignment( const std::string& stmt )
    {
        for ( std::size_t i = stmt.size(); i-- > 0; )
        {
            if ( stmt[i] != '=' )
                continue;
            const char prev = i > 0 ? stmt[i - 1] : ' ';
            const char next = i + 1 < stmt.size() ? stmt[i + 1] : ' ';
            if ( prev == '=' || prev == '!' || prev == '<' || prev == '>' || prev == '+' || prev == '-' ||
                 next == '=' )
                continue; // a comparison or a compound assignment, not a binding
            return stmt.substr( 0, i );
        }
        return {};
    }

    // [ at, the `}` that closes the block `at` sits in ). Where a mapping obtained at `at` can still be
    // written through without another statement having re-bound it.
    std::size_t EnclosingBlockEnd( const std::string& src, std::size_t at )
    {
        int depth = 0;
        for ( std::size_t i = at; i < src.size(); ++i )
        {
            if ( src[i] == '{' )
                ++depth;
            else if ( src[i] == '}' )
            {
                if ( depth == 0 )
                    return i;
                --depth;
            }
        }
        return src.size();
    }

    struct Site
    {
        std::string File;
        int         Line = 0;
        std::string Binding;
        std::string Verdict; // empty = the site is safe
    };

    // Is `name` tested for null between `from` and `to`? Any `if` whose text mentions the binding counts:
    // the shapes in this engine are `if ( p == nullptr )`, `if ( !p )` and `if ( p )`, and pinning the
    // spelling would only make an honest edit look like a defect.
    bool NullTestedBetween( const std::string& src, std::size_t from, std::size_t to, const std::string& name )
    {
        if ( to <= from )
            return false;
        const std::string between = src.substr( from, to - from );
        if ( between.find( "if" ) == std::string::npos )
            return false;
        return !Desert::Tests::ConsumerText::WordPositions( between, name ).empty();
    }

    std::vector<Site> ScanMappingCallSites( const std::string& root )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<Site> sites;
        for ( const fs::path& path : ProjectSources( root ) )
        {
            const std::string src = StripCommentsAndLiterals( ReadAll( path ) );

            for ( std::size_t at : WordPositions( src, "MapMemory" ) )
            {
                // A CALL, not a declaration or a definition: the token is reached through an object.
                std::size_t back = at;
                while ( back > 0 && std::isspace( static_cast<unsigned char>( src[back - 1] ) ) != 0 )
                    --back;
                const bool throughDot   = back > 0 && src[back - 1] == '.';
                const bool throughArrow = back > 1 && src[back - 2] == '-' && src[back - 1] == '>';
                if ( !throughDot && !throughArrow )
                    continue;

                const std::size_t open = SkipSpace( src, at + std::string( "MapMemory" ).size() );
                if ( open >= src.size() || src[open] != '(' )
                    continue;

                Site site;
                site.File = path.string();
                site.Line = LineOf( src, at );

                const std::string declaration = DeclarationBeforeAssignment( StatementBefore( src, at ) );
                site.Binding                  = BoundName( declaration );

                if ( site.Binding.empty() )
                {
                    // Nothing holds the result. Nothing can be written through it either, so the site is
                    // safe — but it is also a mapping that leaks until the temporary dies, which the
                    // guarded type handles and a raw pointer did not.
                    sites.push_back( site );
                    continue;
                }

                // A RAW POINTER BINDING IS THE DEFECT ITSELF, whatever is done with it afterwards: it is
                // the shape from which `memcpy` is one line away and no diagnostic exists.
                if ( declaration.find( '*' ) != std::string::npos )
                {
                    site.Verdict = "binds the mapping to a RAW POINTER ('" + site.Binding +
                                   "'), so nothing obliges the next line to ask whether it exists";
                    sites.push_back( site );
                    continue;
                }

                const std::size_t blockEnd = EnclosingBlockEnd( src, at );
                for ( const char* primitive : { "memcpy", "memmove", "memset" } )
                {
                    for ( std::size_t copyAt : WordPositions( src, primitive ) )
                    {
                        if ( copyAt <= at || copyAt >= blockEnd )
                            continue;
                        const std::size_t argsOpen = SkipSpace( src, copyAt + std::strlen( primitive ) );
                        if ( argsOpen >= src.size() || src[argsOpen] != '(' )
                            continue;
                        const std::size_t argsEnd = src.find( ')', argsOpen );
                        if ( argsEnd == std::string::npos )
                            continue;
                        const std::string args = src.substr( argsOpen, argsEnd - argsOpen );
                        if ( WordPositions( args, site.Binding ).empty() )
                            continue;
                        if ( NullTestedBetween( src, at, copyAt, site.Binding ) )
                            continue;

                        site.Verdict = std::string( primitive ) + " at line " +
                                       std::to_string( LineOf( src, copyAt ) ) + " writes through '" +
                                       site.Binding + "' with no test that the mapping exists";
                        break;
                    }
                    if ( !site.Verdict.empty() )
                        break;
                }

                sites.push_back( site );
            }
        }
        return sites;
    }
} // namespace

TEST( MappedMemoryCensus, NoCallSiteWritesThroughAMappingItDidNotCheck )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository from the working directory";

    const std::vector<Site> sites = ScanMappingCallSites( root );

    // A census that finds nothing to census is a census that has stopped working — the same failure as a
    // DeviceLostCensus row naming a function that no longer exists. If the primitive is renamed, this
    // line is what says so instead of passing silently on an empty set.
    ASSERT_FALSE( sites.empty() )
         << "the scan found no mapping call sites at all. Either the primitive was renamed, or the reader "
            "is blind -- both make every assertion below vacuous.";

    int unsafe = 0;
    for ( const Site& site : sites )
    {
        if ( site.Verdict.empty() )
            continue;
        ++unsafe;
        ADD_FAILURE() << site.File << ":" << site.Line << " " << site.Verdict
                      << ".\nA memcpy through a failed mapping is not a refusal -- it is memory "
                         "corruption or an unexplained death of the process. Take the mapping as a "
                         "Desert::Graphic::MappedMemory and name the transfer (Write/ReadInto/Fill), "
                         "which answers a Common::BoolResultStr and cannot be performed unmapped.";
    }
    EXPECT_EQ( unsafe, 0 ) << unsafe << " of " << sites.size()
                           << " mapping call sites write through a pointer nobody checked.";
}

TEST( MappedMemoryCensus, EveryMappingComesFromTheOneGuardedPrimitive )
{
    // THE OTHER DIRECTION, and the one the type cannot close by itself: a new `vmaMapMemory` written
    // beside the guard rather than through it hands back a bare `void*` again, and no amount of care in
    // MappedMemory reaches it. There is exactly one caller, and its return type is the guard.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> callers;
    for ( const fs::path& path : ProjectSources( root ) )
    {
        const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( path ) );
        for ( std::size_t at : Desert::Tests::ConsumerText::WordPositions( src, "vmaMapMemory" ) )
            callers.push_back( path.filename().string() + ":" + std::to_string( LineOf( src, at ) ) );
    }

    ASSERT_EQ( callers.size(), 1u )
         << "vmaMapMemory must be called from exactly one place -- VulkanAllocator::MapMemory, which wraps "
            "it in the type that cannot be written through unchecked. Callers found: "
         << [&callers]
    {
        std::string all;
        for ( const std::string& c : callers )
            all += c + " ";
        return all;
    }();
    EXPECT_NE( callers.front().rfind( "VulkanAllocator.cpp", 0 ), std::string::npos ) << callers.front();

    const std::string allocator = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanAllocator.hpp" ) );
    EXPECT_NE( allocator.find( "MappedMemory MapMemory(" ), std::string::npos )
         << "VulkanAllocator::MapMemory must return MappedMemory. A raw uint8_t* here is the whole defect: "
            "every call site then holds a pointer that may be null and nothing makes it ask.";
}

TEST( MappedMemoryCensus, NoBufferInterfaceHandsOutARawMappingPointer )
{
    // The second surface the sixteen call sites came through. `BaseBuffer` used to declare
    // `uint8_t* MapMemory()`; both callers bound the result to a `[[maybe_unused]]` local and threw it
    // away, which means the pointer existed purely as an invitation. The question they were really asking
    // is now asked in a form that answers.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* header :
          { "Desert/Desert/Source/Engine/ShaderResources/BaseBuffer.hpp",
            "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan/VulkanUniformBuffer.hpp",
            "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp" } )
    {
        const std::string src =
             Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( fs::path( root ) / header ) );
        EXPECT_EQ( src.find( "uint8_t* MapMemory(" ), std::string::npos )
             << header << " still declares a raw-pointer mapping accessor.";
        EXPECT_NE( src.find( "EnsureMapped()" ), std::string::npos )
             << header << " must ask the mapping question through EnsureMapped(), which answers.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
