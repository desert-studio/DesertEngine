// The identity of a cloud noise volume asset — a suite about a RELATION, not about a function.
//
// The relation: THE SAME PATH MUST GIVE THE SAME HANDLE, and it must do so in runs that share nothing.
//
// Why that is the wording. `CloudNoiseVolumeAsset` used to take the random uuid `AssetBase` hands out,
// so every launch of the editor gave the volume at a given path a different handle. Nothing referenced a
// volume by handle at the time — a `.decloudtype` names its volume by a relative path — so the defect was
// invisible, and it was reported without being fixed for exactly that reason. It is a mine, not a bug:
// the first reflected field to accept a `.dcnv` would have stored a handle that resolved to nothing after
// a restart, and the symptom an artist reports for that is "the clouds are different since I reopened the
// editor", which is a very long way from its cause.
//
// Why a CHILD PROCESS and not two objects. A test that builds two assets inside one process proves the
// derivation is not a counter, and that is worth having, but it is not the property that was broken.
// Anything seeded once per process — a random engine, a monotonic id, a pointer value — passes an
// in-process comparison and still hands out a fresh handle on the next launch. The only test that can
// tell those apart runs the derivation in two processes that share no state. So this binary re-executes
// ITSELF with `--print-handle <path>`, twice, and compares what the two runs printed. That is the
// "two independent loads" the requirement asks for, taken literally.

#include <gtest/gtest.h>

#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // argv[0] of this run, captured by main. The child is this same binary.
    std::string g_ExecutablePath;

    constexpr const char* kPrintHandleFlag = "--print-handle";

    // The subject: the handle the asset carries the moment it is constructed. In the constructor and not
    // after Load, because the AssetManager keys a not-yet-loaded shell by it.
    uint64_t HandleOf( const std::string& path )
    {
        const Desert::Assets::CloudNoiseVolumeAsset asset{ Common::Filepath( path ) };
        return static_cast<uint64_t>( asset.GetMetadata().Handle );
    }

    std::string Trimmed( const std::string& text )
    {
        const auto first = text.find_first_not_of( " \t\r\n" );
        if ( first == std::string::npos )
            return {};
        const auto last = text.find_last_not_of( " \t\r\n" );
        return text.substr( first, last - first + 1 );
    }

    // Runs THIS binary again, in a process of its own, and returns what its `--print-handle` branch
    // printed. Empty means the child could not be run or printed nothing — the callers treat that as a
    // failure of the test rather than as a pass, because a silently skipped guard is not a guard.
    std::string HandleFromAnIndependentProcess( const std::string& path )
    {
        if ( g_ExecutablePath.empty() )
            return {};

#ifdef DESERT_PLATFORM_WINDOWS
        // cmd.exe strips the outer pair of quotes off the whole command line, so a quoted executable AND
        // a quoted argument need one more pair around the lot.
        const std::string command = "\"\"" + g_ExecutablePath + "\" " + kPrintHandleFlag + " \"" + path + "\"\"";
        FILE*             pipe    = _popen( command.c_str(), "r" );
#else
        const std::string command = "'" + g_ExecutablePath + "' " + kPrintHandleFlag + " '" + path + "'";
        FILE*             pipe    = popen( command.c_str(), "r" );
#endif
        if ( !pipe )
            return {};

        std::string output;
        char        buffer[256];
        while ( fgets( buffer, sizeof( buffer ), pipe ) != nullptr )
            output += buffer;

#ifdef DESERT_PLATFORM_WINDOWS
        _pclose( pipe );
#else
        pclose( pipe );
#endif
        return Trimmed( output );
    }

    // A path that is REPRESENTATIVE rather than arbitrary: this is where the shipped volume actually
    // lives, so the string under test is one the engine really derives a handle from.
    const std::string kVolumePath = "Assets/Clouds/CloudNoise_FineWisp.dcnv";
} // namespace

// THE test. Two runs of the same derivation, in two processes, over the same path.
TEST( CloudNoiseVolumeHandle, TheSamePathGivesTheSameHandleInTwoIndependentRuns )
{
    const std::string firstRun  = HandleFromAnIndependentProcess( kVolumePath );
    const std::string secondRun = HandleFromAnIndependentProcess( kVolumePath );

    ASSERT_FALSE( firstRun.empty() ) << "the first child run printed nothing; the guard did not run at all";
    ASSERT_FALSE( secondRun.empty() ) << "the second child run printed nothing; the guard did not run at all";

    EXPECT_EQ( firstRun, secondRun ) << "the volume at '" << kVolumePath << "' was handle " << firstRun
                                     << " in one run and " << secondRun
                                     << " in the next. Any scene storing that handle would resolve to "
                                        "nothing after a restart.";

    // And the running process agrees with both, which is what makes the value a property of the PATH
    // rather than of a process that happens to be consistent with itself.
    EXPECT_EQ( std::to_string( HandleOf( kVolumePath ) ), firstRun );
}

// The null handle reads as "no asset" everywhere in the engine, so a derivation that produced it would be
// stable and useless. FromKey already refuses to return 0; this is the guard that says so at this site.
TEST( CloudNoiseVolumeHandle, TheDerivedHandleIsNotTheNullHandle )
{
    EXPECT_NE( HandleOf( kVolumePath ), 0u );
}

// Stability is only half of the relation. A derivation that returned a constant would pass the test
// above and collapse every volume in the library onto one handle.
TEST( CloudNoiseVolumeHandle, DifferentPathsGiveDifferentHandles )
{
    EXPECT_NE( HandleOf( "Assets/Clouds/CloudNoise_FineWisp.dcnv" ),
               HandleOf( "Assets/Clouds/CloudNoise_Default.dcnv" ) );
}

// Two spellings of one file are one file. The derivation normalizes before hashing, so a caller that
// joined a path with a `.` or a `..` in it lands on the same asset as one that did not — which is the
// difference between a handle that identifies a FILE and one that identifies a STRING.
TEST( CloudNoiseVolumeHandle, EquivalentSpellingsOfOnePathGiveOneHandle )
{
    const uint64_t direct = HandleOf( "Assets/Clouds/CloudNoise_FineWisp.dcnv" );
    EXPECT_EQ( HandleOf( "Assets/./Clouds/CloudNoise_FineWisp.dcnv" ), direct );
    EXPECT_EQ( HandleOf( "Assets/Clouds/../Clouds/CloudNoise_FineWisp.dcnv" ), direct );
}

// Constructing the same asset twice inside ONE process is the cheap half of the property. It catches a
// counter or an address-derived id immediately, without paying for a child process.
TEST( CloudNoiseVolumeHandle, TwoAssetsOverOnePathAgreeWithinASingleProcess )
{
    EXPECT_EQ( HandleOf( kVolumePath ), HandleOf( kVolumePath ) );
}

int main( int argc, char** argv )
{
    // The child branch. Deliberately before InitGoogleTest: this invocation is not a test run, it is one
    // half of the measurement the test above makes.
    if ( argc >= 3 && std::strcmp( argv[1], kPrintHandleFlag ) == 0 )
    {
        printf( "%llu\n", static_cast<unsigned long long>( HandleOf( argv[2] ) ) );
        return 0;
    }

    g_ExecutablePath = argv[0];

    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
