// The render graph's pass ORDER has no sorter left to test: there are no phases, no phase registry and no
// numeric placement (ARCH1b). The order of the frame is the order of the calls in SceneRenderer's AddFrame*
// functions - each render system hands its passes to the frame-build function that owns that stage (UE:
// AddBasePass, RenderTranslucency, AddPostProcessingPasses) - asserted by
// RenderGraphCompile.SceneRendererAddsItsPassesInTheFrameOrder. Passes from outside the engine join at the named
// RDG::ExtensionPoint calls (RenderGraphCompile.AnExtensionPassLandsBetweenItsPointsNeighbours).
//
// What is left here is the census that keeps the old machinery from coming back under its old names.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Red when any engine or editor source names the removed ladder (RenderPassOrder / OrderInPhase), the removed
// phases (RenderPhase, RenderPhaseID, RenderPhaseRegistry, k_BuiltinOrder, k_DeferredOverlayPhases,
// IsDeferredOverlay) or the per-system registration hook that fed them (RegisterPasses) - comments included,
// so a sentence cannot keep describing an order the code no longer has.
TEST( PassOrder, EngineAndEditorHaveNoPhaseOrNumericPassPlacement )
{
    fs::path root = ".";
    for ( int up = 0; up < 6 && !fs::exists( root / "Desert/Desert/Source/Engine" ); ++up )
        root /= "..";
    ASSERT_TRUE( fs::exists( root / "Desert/Desert/Source/Engine" ) ) << "run from inside the repository";

    std::vector<std::string> offenders;
    std::size_t              scanned = 0;
    for ( const char* tree : { "Desert/Desert/Source/Engine", "Editor/Source" } )
    {
        ASSERT_TRUE( fs::exists( root / tree ) ) << tree << " is gone";
        for ( const auto& entry : fs::recursive_directory_iterator( root / tree ) )
        {
            const std::string extension = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( extension != ".hpp" && extension != ".cpp" && extension != ".h" ) )
                continue;
            std::ifstream     file( entry.path(), std::ios::binary );
            std::stringstream text;
            text << file.rdbuf();
            ++scanned;
            const std::string source = text.str();
            for ( const char* symbol : { "RenderPassOrder", "OrderInPhase", "RenderPhase", "k_BuiltinOrder",
                                         "k_DeferredOverlayPhases", "IsDeferredOverlay", "RegisterPasses" } )
            {
                if ( source.find( symbol ) != std::string::npos )
                    offenders.push_back( entry.path().generic_string() + ": " + symbol );
            }
        }
    }
    EXPECT_GT( scanned, 100u ) << "the census read almost nothing; the roots moved";
    EXPECT_TRUE( offenders.empty() ) << ::testing::PrintToString( offenders );
}
