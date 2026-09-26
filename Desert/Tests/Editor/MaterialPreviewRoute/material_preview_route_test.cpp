// THE PREVIEW MUST NOT FLATTER: a material shown in the Material Preview window has to reach the GPU by
// the same route a mesh in the scene does, so the two are the same picture by construction.
//
// The engine has two routes for a data-driven material, and they are NOT equivalent:
//
//   * the per-SLOT route -- StaticMeshComponent::MaterialSlots -> MaterialFactory -> a DataDrivenMaterial
//     that IS the asset. What a real scene mesh takes.
//   * the shader-OVERRIDE route -- MaterialComponent::ShaderName + per-draw overrides, driving a material
//     shared by shader name. MeshRenderer re-applies ApplyDefaults() to it EVERY frame, so an asset's
//     authored values would be overwritten by schema defaults.
//
// The old 128 px preview took the override route, and that is exactly how it came to show a correct
// material while the scene rendered black two frames in three (Docs/MaterialEditor/STAGE1_END_TO_END.md).
// The fix for the black frames landed in MeshRenderer; this test guards the OTHER half -- that the
// preview keeps asking the same question the game asks.
//
// It fails in BOTH directions on purpose. Someone who moves the preview onto the override route "because
// it is faster" must be stopped and told why, or in six months the flattering preview comes back and
// nobody remembers what it cost to remove.
//
// Why a source-level assertion rather than a behavioural one: PreviewViewport owns a Scene and a
// SceneRenderer, neither of which can be constructed without a Vulkan device, and Scene.cpp is compiled
// by no suite in this repository. Reading the source is the established alternative here --
// Tests/Engine/SettingConsumers does the same thing to Components.hpp, and for the same reason.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    // The repository root, found by walking up from wherever the test binary was started -- the same
    // approach SettingConsumers and the font-baker test use, so none of them needs one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Widgets/PreviewViewport.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Source with // line comments stripped, so a comment that merely NAMES the override route (this file's
    // own subject matter is discussed in those headers at length) can never be mistaken for using it.
    std::string CodeOnly( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        size_t i = 0;
        while ( i < source.size() )
        {
            if ( source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/' )
            {
                while ( i < source.size() && source[i] != '\n' )
                    ++i;
            }
            else
            {
                out.push_back( source[i++] );
            }
        }
        return out;
    }

    // The body of one function, from its signature to the matching closing brace.
    //
    // Scoped rather than searching the whole file, and that is not fussiness: the first version of this
    // test looked for "MaterialSlots" anywhere in PreviewViewport.cpp, and a sabotage that tore the slot
    // assignment out of SetMaterial stayed GREEN -- because SetMesh and Clear mention MaterialSlots too.
    // A test that cannot tell which function did the thing is not guarding the function.
    std::string FunctionBody( const std::string& code, const std::string& signature )
    {
        const size_t sig = code.find( signature );
        if ( sig == std::string::npos )
            return {};
        const size_t open = code.find( '{', sig );
        if ( open == std::string::npos )
            return {};

        int    depth = 0;
        size_t i     = open;
        for ( ; i < code.size(); ++i )
        {
            if ( code[i] == '{' )
                ++depth;
            else if ( code[i] == '}' && --depth == 0 )
                break;
        }
        return code.substr( open, i - open );
    }

    // The balanced { ... } block that follows @p marker. Asks whether a control lives INSIDE a particular
    // gate, which is a different question from whether it appears somewhere in the same function -- and the
    // difference is the whole value of the assertion that uses it.
    std::string BlockAfter( const std::string& code, const std::string& marker )
    {
        const size_t at = code.find( marker );
        if ( at == std::string::npos )
            return {};
        const size_t open = code.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int    depth = 0;
        size_t i     = open;
        for ( ; i < code.size(); ++i )
        {
            if ( code[i] == '{' )
                ++depth;
            else if ( code[i] == '}' && --depth == 0 )
                break;
        }
        return code.substr( open, i - open );
    }

    // Non-overlapping occurrences. "Is this control ONLY inside that gate" is a counting question: asking it
    // by presence alone lets a second copy outside the gate through, which is the shape the gate exists to
    // forbid.
    std::size_t CountOf( const std::string& haystack, const std::string& needle )
    {
        std::size_t n  = 0;
        std::size_t at = haystack.find( needle );
        while ( at != std::string::npos )
        {
            ++n;
            at = haystack.find( needle, at + needle.size() );
        }
        return n;
    }
} // namespace

class MaterialPreviewRoute : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = RepoRoot();
        ASSERT_FALSE( m_Root.empty() ) << "repository root not found from the test's working directory";
    }

    std::string Code( const std::string& relative ) const
    {
        const std::string text = ReadFile( m_Root + relative );
        EXPECT_FALSE( text.empty() ) << "could not read " << relative;
        return CodeOnly( text );
    }

    std::string m_Root;
};

// The positive half: SetMaterial itself puts the material in a SLOT.
TEST_F( MaterialPreviewRoute, PreviewViewportPutsTheMaterialInASlot )
{
    const std::string code = Code( "Editor/Source/Editor/Widgets/PreviewViewport.cpp" );
    const std::string body = FunctionBody( code, "PreviewViewport::SetMaterial" );

    ASSERT_FALSE( body.empty() ) << "PreviewViewport::SetMaterial not found — the preview's material entry "
                                    "point is what this suite guards; if it was renamed, re-point the guard "
                                    "rather than deleting it.";
    EXPECT_NE( body.find( "MaterialSlots" ), std::string::npos )
         << "PreviewViewport::SetMaterial no longer assigns StaticMeshComponent::MaterialSlots. The preview "
            "must show a material the way a scene mesh does -- through a material slot -- or it is showing "
            "something the game will not.";
}

// The negative half, and the one that matters in six months: the preview must never reach for the
// override route.
TEST_F( MaterialPreviewRoute, PreviewViewportNeverUsesTheShaderOverrideRoute )
{
    const std::string code = Code( "Editor/Source/Editor/Widgets/PreviewViewport.cpp" );

    EXPECT_EQ( code.find( "MaterialComponent" ), std::string::npos )
         << "PreviewViewport now uses MaterialComponent -- the shader-OVERRIDE route. MeshRenderer calls "
            "ApplyDefaults() on that material every frame, so an asset's authored parameter values are "
            "replaced by schema defaults: the preview would show something the scene does not. This is "
            "precisely what the old 128 px thumbnail did. If a bare shader with no material asset genuinely "
            "needs previewing, give PreviewViewport a separate entry point and leave SetMaterial alone.";
}

// The window must go through the widget's material entry point rather than growing its own path.
//
// RE-POINTED, NOT WEAKENED. The singleton MaterialPreviewPanel became one MaterialEditorPanel per `.demat`
// (Docs/MaterialEditor/PLAN_STAGE3_ASSET_DOCUMENTS.md, M1). The route it must take is unchanged and so is
// every assertion below; only the file that has to take it moved.
TEST_F( MaterialPreviewRoute, TheWindowPreviewsAMaterialAsset )
{
    const std::string code = Code( "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );

    EXPECT_NE( code.find( "SetMaterial" ), std::string::npos )
         << "MaterialEditorPanel no longer calls PreviewViewport::SetMaterial. A material editor that "
            "previews anything other than the material asset is not previewing what ships.";
    EXPECT_EQ( code.find( "MaterialComponent" ), std::string::npos )
         << "MaterialEditorPanel reaches for MaterialComponent, i.e. the shader-override route. See the "
            "note on PreviewViewportNeverUsesTheShaderOverrideRoute.";
}

// A recompile is only half published: the Shader object reloads itself, but pipelines are cached per
// SceneRenderer and AssetHotReload invalidates only the MAIN scene's cache. A preview that does not drop
// its own would keep drawing the previous compile -- the same disease, new clothes.
TEST_F( MaterialPreviewRoute, ThePreviewDropsItsOwnPipelinesOnARecompile )
{
    const std::string widget = Code( "Editor/Source/Editor/Widgets/PreviewViewport.cpp" );
    EXPECT_NE( widget.find( "InvalidateByShader" ), std::string::npos )
         << "PreviewViewport no longer invalidates its own pipeline cache. AssetHotReload only invalidates "
            "the main scene's, so the preview would keep drawing the shader modules from before the "
            "recompile while the viewport drew the new ones.";

    const std::string graph = Code( "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.cpp" );
    EXPECT_NE( graph.find( "MaterialShaderRebuild::Publish" ), std::string::npos )
         << "Compile no longer tells the Material Editor windows that the shader was rebuilt, so they have "
            "no reason to drop their stale pipelines.";

    // And the window has to LISTEN. The publisher and the listener are asserted together because either one
    // alone is a wire with nothing on the other end -- and one window consuming a shared pending flag is
    // precisely how the several-windows version of this would go wrong silently.
    const std::string window = Code( "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    EXPECT_NE( window.find( "MaterialShaderRebuild::CountFor" ), std::string::npos )
         << "MaterialEditorPanel no longer reads the rebuild count, so a recompile leaves it drawing the "
            "shader modules from before the compile while the viewport draws the ones from after.";
    EXPECT_NE( window.find( "InvalidatePipelines" ), std::string::npos )
         << "MaterialEditorPanel hears about the rebuild and does nothing with it.";
}

// THE DOMAIN CHOOSES WHAT FILLS THE PANE. This pins that DECISION, not the code that carries it out.
//
// The preview's extension point is "what fills the pane", never "which shape from a list". A Surface
// material rides a real primitive through the slot route the tests above guard; the cubemap (Skybox) domain
// cannot -- MeshRenderer::DrawGenericMeshes refuses a non-Surface program by name -- so it brings its OWN
// draw, a ball its cubemap is wrapped onto. A volume domain is already queued behind it and will arrive as a
// miniature march, again not as a new Shape entry. Get this branch wrong and the pane silently shows the
// wrong domain's answer: a Skybox material pushed down the slot route draws an empty scene, which is
// indistinguishable from a preview that merely failed.
//
// ASSERTED AS A RELATION between the two halves, not as two presence checks. "OnPreUpdate mentions Skybox
// and mentions SetCubemapMaterial" stays GREEN when the two calls are swapped -- and swapping them is the
// one mutation that matters here. Same lesson as the FunctionBody helper above, one level finer: it is not
// enough to know the right function did something, the right BRANCH has to be the one that did it.
TEST_F( MaterialPreviewRoute, TheCubemapDomainTakesTheCubemapRouteAndSurfaceTheSlotRoute )
{
    const std::string code = Code( "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    const std::string body = FunctionBody( code, "MaterialEditorPanel::OnPreUpdate" );

    ASSERT_FALSE( body.empty() ) << "MaterialEditorPanel::OnPreUpdate not found — the push site is where the "
                                    "domain picks the pane's content; if it moved, re-point this guard "
                                    "rather than deleting it.";

    const std::size_t cubemapGate = body.find( "ShaderDomain::Skybox" );
    ASSERT_NE( cubemapGate, std::string::npos )
         << "OnPreUpdate no longer branches on the Skybox domain, so every material now takes one route. A "
            "cubemap material pushed down the mesh/slot route draws an EMPTY pane, which reads as a broken "
            "preview rather than as a wrong decision.";

    const std::size_t otherwise = body.find( "else", cubemapGate );
    ASSERT_NE( otherwise, std::string::npos )
         << "the Skybox branch has no else — the surface domain must still reach SetMaterial.";

    const std::string cubemapBranch = body.substr( cubemapGate, otherwise - cubemapGate );
    const std::string surfaceBranch = body.substr( otherwise );

    EXPECT_NE( cubemapBranch.find( "SetCubemapMaterial" ), std::string::npos )
         << "the Skybox branch does not call PreviewViewport::SetCubemapMaterial. That entry point IS the "
            "cubemap domain's draw (an external pass ray-tracing the ball, re-resolving the cube every "
            "frame); without it the domain has no way to fill the pane.";
    EXPECT_EQ( cubemapBranch.find( "SetMaterial(" ), std::string::npos )
         << "the Skybox branch reaches for SetMaterial — the mesh/slot route. The routes are SWAPPED. A "
            "Skybox-domain program cannot ride a StaticMeshComponent slot at all (MeshRenderer refuses it by "
            "name), so this draws nothing and says nothing.";
    EXPECT_NE( surfaceBranch.find( "SetMaterial(" ), std::string::npos )
         << "the non-cubemap branch no longer calls SetMaterial, so an ordinary surface material has lost "
            "the slot route the rest of this suite exists to protect.";
}

// The VOLUME domain is the third route, and the only one whose picture is not of an object.
//
// A cloud material describes a MEDIUM. Its layout is a painting on a sky map, its base and top are
// kilometres on the CloudType assets it points at, and one cell of its weather lattice is about 3 km
// across; none of that survives being wrapped onto a one-metre ball, which is the same class of scale
// error as the "clouds hang too low" complaint. So it takes a dome, and the two assertions below are the
// two halves of that: it must reach the dome entry point, and it must NOT reach the ball's.
TEST_F( MaterialPreviewRoute, TheVolumeDomainTakesTheDomeRoute )
{
    const std::string code = Code( "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    const std::string body = FunctionBody( code, "MaterialEditorPanel::OnPreUpdate" );
    ASSERT_FALSE( body.empty() );

    const std::size_t volumeGate = body.find( "ShaderDomain::Volume" );
    ASSERT_NE( volumeGate, std::string::npos )
         << "OnPreUpdate no longer branches on the Volume domain. Without it a cloud material falls through "
            "to the mesh/slot route, where a Volume program cannot be drawn at all — an empty pane, which "
            "reads as a broken preview rather than as a decision.";

    const std::string afterGate = body.substr( volumeGate );
    EXPECT_NE( afterGate.find( "SetVolumeMaterial" ), std::string::npos )
         << "the Volume branch does not call PreviewViewport::SetVolumeMaterial, which IS that domain's "
            "draw (a preview sky with ground, sun and a wide vertical lens).";

    // And the pane's own refusal must survive underneath it. The dome is this material in a preview WORLD;
    // the viewport is this material in a LEVEL, and the scene's layer carries budgets and a region size no
    // material holds. A convincing picture with that sentence removed is a promise the pane cannot keep.
    EXPECT_NE( code.find( "PreviewSceneNote" ), std::string::npos )
         << "MaterialEditorPanel no longer carries PreviewSceneNote. The Volume domain's refusal was "
            "REPLACED by a picture rather than kept under one, so nothing in the frame says the level's "
            "cloud layer is not this material's preview world.";
}

// The Shape combo is a SURFACE control, not a preview control -- the other half of the same decision.
//
// Only the surface domain fills the pane with a primitive whose shape is a free choice (a grass card wants a
// plane). A cubemap's content IS a ball, so Cube/Plane there would be two menu entries that rebuild the
// preview into the same picture; the refused domains draw nothing at all. Offering the combo everywhere and
// disabling it is the version of this that looks harmless and teaches the artist that the pane is a shape
// picker -- which is precisely the model this design rejects.
TEST_F( MaterialPreviewRoute, TheShapeComboBelongsToTheSurfaceDomainAlone )
{
    // The combo lives on the VIEWPORT's toolbar since the document became a viewport column beside the
    // details (UE's Material Editor layout); the rule it carries did not move with it and is asserted there.
    const std::string code = Code( "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    const std::string body = FunctionBody( code, "MaterialEditorPanel::DrawViewportToolbar" );

    ASSERT_FALSE( body.empty() ) << "MaterialEditorPanel::DrawViewportToolbar not found — re-point this guard "
                                    "rather than deleting it.";

    const std::string surfaceOnly = BlockAfter( body, "ShaderDomain::Surface" );
    ASSERT_FALSE( surfaceOnly.empty() )
         << "the viewport toolbar no longer gates anything on the Surface domain, so the Shape combo is offered "
            "for every domain — including the ones whose content has exactly one shape or no shape at all.";

    EXPECT_NE( surfaceOnly.find( "\"##preview_shape\"" ), std::string::npos )
         << "the Shape combo is not inside the Surface-domain gate. It is a surface control: it must exist "
            "where it means something instead of being shown and disabled everywhere else.";
    EXPECT_EQ( CountOf( body, "\"##preview_shape\"" ), CountOf( surfaceOnly, "\"##preview_shape\"" ) )
         << "a Shape combo is drawn OUTSIDE the Surface-domain gate as well. One copy inside the gate does "
            "not help if another is unconditional — a cubemap material would still be offered Cube/Plane.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
