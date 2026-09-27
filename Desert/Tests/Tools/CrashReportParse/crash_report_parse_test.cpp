// CR1c — the crash reporter's reader of crash.txt (Tools/CrashReporter/Source/CrashReport.cpp), driven with
// report texts in the DESERTCRASH 1 format that Common/Core/CrashHandler.cpp writes. Three shapes: a whole
// report, the same report with the CR1c gpu_* / game keys, and the damaged ones a real crash can leave (a
// process killed mid-write, a garbled stack line, a file that is not a report at all).

#include <CrashReport.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{
    // A report as the writer emits it, minus the keys CR1c added — i.e. what an older engine left behind.
    const std::string kHead = "DESERTCRASH 1\n"
                              "[report]\n"
                              "host=Runtime\n"
                              "pid=4242\n"
                              "tid=17\n"
                              "started=20260927-071500\n"
                              "crash_epoch=1790000000\n"
                              "[exception]\n"
                              "kind=exception\n"
                              "code=0xC0000005\n"
                              "codename=EXCEPTION_ACCESS_VIOLATION\n"
                              "address=0x7FF600001234\n"
                              "synthesized=0\n"
                              "module=Runtime.exe\n"
                              "module_offset=0x1234\n"
                              "function=Common::Crash::Detail::CrashTestSegv\n"
                              "fault_frame=0\n"
                              "[build]\n"
                              "version=0.1.0\n"
                              "sha=bca1e0a98\n"
                              "branch=task/CR1c\n"
                              "dirty=1\n"
                              "[context]\n"
                              "machine=DESKTOP\n"
                              "scene=none\n"
                              "os=Windows 11 Pro 10.0.26200\n"
                              "gpu=NVIDIA GeForce RTX 3070 Ti\n";

    const std::string kTail =
         "[stack]\n"
         "frame=0|0x7FF600001234|Runtime.exe|Common::Crash::Detail::CrashTestSegv|CrashHandler.cpp:1460\n"
         "frame=1|0x7FF600005678|Runtime.exe|CreateApplication|Main.cpp:240\n"
         "[log]\n"
         "log=[info] mounting Content.dpak\n"
         "log=[info] about to crash\n"
         "[end]\n"
         "written=complete\n";

    const std::string kGpuKeys = "gpu_vendor=0x10DE\n"
                                 "gpu_device=0x2482\n"
                                 "gpu_driver=591.86\n"
                                 "gpu_api=1.4.303\n"
                                 "game=Sandbox\n";
} // namespace

TEST( CrashReportParse, AWholeReportFillsEveryField )
{
    const CrashReporter::Report r = CrashReporter::ParseCrashText( kHead + kTail, "crash.txt" );
    ASSERT_TRUE( r.valid ) << r.error;
    EXPECT_TRUE( r.error.empty() ) << r.error;
    EXPECT_TRUE( r.complete );
    EXPECT_EQ( r.formatVersion, 1 );
    EXPECT_EQ( r.host, "Runtime" );
    EXPECT_EQ( r.pid, "4242" );
    EXPECT_EQ( r.codename, "EXCEPTION_ACCESS_VIOLATION" );
    EXPECT_EQ( r.function, "Common::Crash::Detail::CrashTestSegv" );
    EXPECT_EQ( r.sha, "bca1e0a98" );
    EXPECT_EQ( r.os, "Windows 11 Pro 10.0.26200" );
    EXPECT_EQ( r.gpu, "NVIDIA GeForce RTX 3070 Ti" );
    ASSERT_EQ( r.frames.size(), 2u );
    EXPECT_EQ( r.frames[1].function, "CreateApplication" );
    EXPECT_EQ( r.frames[1].source, "Main.cpp:240" );
    ASSERT_EQ( r.log.size(), 2u );
    EXPECT_EQ( r.log[0], "[info] mounting Content.dpak" );
    // An older report has no gpu_* keys: the fields stay empty rather than inventing a value.
    EXPECT_TRUE( r.gpuDriver.empty() );
    EXPECT_TRUE( r.game.empty() );
}

TEST( CrashReportParse, TheGpuAndGameKeysAreReadWithoutDisturbingTheRest )
{
    const CrashReporter::Report r = CrashReporter::ParseCrashText( kHead + kGpuKeys + kTail, "crash.txt" );
    ASSERT_TRUE( r.valid ) << r.error;
    EXPECT_TRUE( r.complete );
    EXPECT_EQ( r.gpu, "NVIDIA GeForce RTX 3070 Ti" );
    EXPECT_EQ( r.gpuVendor, "0x10DE" );
    EXPECT_EQ( r.gpuDevice, "0x2482" );
    EXPECT_EQ( r.gpuDriver, "591.86" );
    EXPECT_EQ( r.gpuApi, "1.4.303" );
    EXPECT_EQ( r.game, "Sandbox" );
    EXPECT_EQ( r.frames.size(), 2u );
}

TEST( CrashReportParse, UnknownKeysAndSectionsAreIgnored )
{
    // A newer writer appends; this reader must keep working on the fields it knows.
    const std::string           text = kHead + "gpu_future=42\n[newsection]\nwhatever=1\n" + kTail;
    const CrashReporter::Report r    = CrashReporter::ParseCrashText( text, "crash.txt" );
    ASSERT_TRUE( r.valid ) << r.error;
    EXPECT_EQ( r.gpu, "NVIDIA GeForce RTX 3070 Ti" );
    EXPECT_EQ( r.frames.size(), 2u );
    EXPECT_TRUE( r.complete );
}

TEST( CrashReportParse, AReportCutOffMidWriteIsShownButMarkedIncomplete )
{
    // The process died inside [context]: no [stack], no [end].
    const std::string           text = kHead.substr( 0, kHead.find( "os=" ) );
    const CrashReporter::Report r    = CrashReporter::ParseCrashText( text, "crash.txt" );
    ASSERT_TRUE( r.valid ) << r.error;
    EXPECT_FALSE( r.complete ) << "a report without [end] written=complete must not read as whole";
    EXPECT_EQ( r.machine, "DESKTOP" );
    EXPECT_TRUE( r.os.empty() );
    EXPECT_TRUE( r.frames.empty() );
}

TEST( CrashReportParse, AGarbledStackLineIsDroppedAndNamed )
{
    const std::string text =
         kHead + "[stack]\nframe=0|0x1|Runtime.exe\nframe=1|0x2|m|f|\n[end]\nwritten=complete\n";
    const CrashReporter::Report r = CrashReporter::ParseCrashText( text, "crash.txt" );
    ASSERT_TRUE( r.valid );
    ASSERT_EQ( r.frames.size(), 1u );
    EXPECT_EQ( r.frames[0].index, "1" );
    EXPECT_NE( r.error.find( "1 [stack] line(s)" ), std::string::npos ) << r.error;
}

TEST( CrashReportParse, NotAReportIsRefusedWithThePath )
{
    const CrashReporter::Report empty = CrashReporter::ParseCrashText( "", "a/crash.txt" );
    EXPECT_FALSE( empty.valid );
    EXPECT_NE( empty.error.find( "a/crash.txt" ), std::string::npos );

    const CrashReporter::Report foreign = CrashReporter::ParseCrashText( "PK\x03\x04garbage\n", "b/crash.txt" );
    EXPECT_FALSE( foreign.valid );
    EXPECT_NE( foreign.error.find( "not a DESERTCRASH report" ), std::string::npos ) << foreign.error;

    const CrashReporter::Report future =
         CrashReporter::ParseCrashText( "DESERTCRASH 2\n[report]\n", "c/crash.txt" );
    EXPECT_FALSE( future.valid );
    EXPECT_NE( future.error.find( "version 2" ), std::string::npos ) << future.error;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
