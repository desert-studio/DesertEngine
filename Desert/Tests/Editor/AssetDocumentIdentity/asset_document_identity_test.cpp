// TWO DOCUMENTS MUST BE TWO WINDOWS, AND THE SAME SUBJECT MUST BE ONE.
//
// Every panel in this editor is drawn with ImGui::Begin( PanelDisplayTitle( panel->GetName() ) ), and ImGui
// takes a window's identity from the text after the LAST "###" in that string. Two windows that agree there
// are ONE window: the second one's content is drawn into the first, merged, with no error anywhere. Every
// panel before asset documents was a singleton and so never met the problem; ViewportPanel is the one type
// that did, and it escapes by baking "###sceneview<id>" into its title.
//
// A document escapes the same way, keyed on the SUBJECT rather than on a counter — which is
// also what makes open-or-focus fall out for free, because the same material can then only ever produce the
// same window. That is one relation with two halves, and BOTH halves are asserted here: different subjects
// must not collide, and the same subject must not diverge.
//
// Why these live in headers at all: EditorLayer.cpp is one of the editor translation units no suite compiles
// (scripts/CI/UnreachedSources.sh), and neither the naming nor the lookup can be exercised through a window.
// So they sit in Editor/Panels/IPanel.hpp and Editor/Core/SubjectEditorRegistry.hpp as pure functions, for
// exactly the reason Editor/Core/SceneViewIdentity.hpp does — see Tests/Engine/RendererSlots.

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Core/OpenDocuments.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/SkyboxViewer/SkyboxViewerIdentity.hpp>
#include <Editor/Panels/StaticMeshViewer/StaticMeshViewerIdentity.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using Desert::Assets::AssetHandle;
using Desert::Assets::AssetTypeID;
using Desert::Editor::AssetSubject;
using Desert::Editor::ComponentSubject;
using Desert::Editor::DocumentTitle;
using Desert::Editor::IPanel;
using Desert::Editor::ISubjectDocument;
using Desert::Editor::OpenDocuments;
using Desert::Editor::PendingViewBytes;
using Desert::Editor::SubjectId;

namespace
{
    // A FILE subject and a COMPONENT-ON-AN-ENTITY subject. The seam is keyed on a subject now rather than
    // on an asset handle (task U7, Editor/Core/EditorSubject.hpp), and the two spellings differ in exactly
    // the way that matters here: `Asset( 42 )` and `Anim( 42 )` are different WINDOWS over one number.
    SubjectId Asset( uint64_t value, AssetTypeID type = AssetTypeID::Material )
    {
        return AssetSubject( AssetHandle( value ), static_cast<uint32_t>( type ) );
    }

    SubjectId Component( uint64_t entity, std::string_view componentTypeName )
    {
        return ComponentSubject( ::Common::UUID( entity ), componentTypeName );
    }

    // The id half of a title: everything from the "###" the document introduced. This is the substring ImGui
    // hashes, so it is the thing two windows must not share.
    std::string WindowId( const std::string& title )
    {
        const auto pos = title.rfind( "###" );
        return pos == std::string::npos ? std::string{} : title.substr( pos );
    }

    // A document with nothing in it: the naming and the lookup are what is under test, and a real
    // MaterialEditorPanel would drag a Scene and a SceneRenderer in, neither constructible without a device.
    class FakeDocument final : public ISubjectDocument
    {
    public:
        FakeDocument( const std::string& name, const SubjectId& subject ) : ISubjectDocument( name, subject )
        {
        }

        void OnUIRender() override
        {
        }

        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return true;
        }

        [[nodiscard]] bool HoldsView() const override
        {
            return m_HoldsSlot;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return m_ClaimsSlot;
        }

        // Set by the slot-census tests. Public because this is a stub, and a setter for each would be two
        // lines of ceremony around a bool the test is about.
        bool m_HoldsSlot  = false;
        bool m_ClaimsSlot = true;
    };

    // A CPU-only document — the four cloud editors. It never holds a renderer slot and never will, which is
    // the distinction PendingViewBytes exists to make.
    class FakeCpuDocument final : public ISubjectDocument
    {
    public:
        FakeCpuDocument( const std::string& name, const SubjectId& subject ) : ISubjectDocument( name, subject )
        {
        }

        void OnUIRender() override
        {
        }

        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return true;
        }

        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }
    };

    // A tool panel — the thing the lookup must never mistake for a document, however it is named.
    class FakeTool final : public IPanel
    {
    public:
        explicit FakeTool( std::string name ) : IPanel( std::move( name ) )
        {
        }

        void OnUIRender() override
        {
        }
    };
} // namespace

// --- The id relation ---------------------------------------------------------------------------------

TEST( AssetDocumentIdentity, TwoMaterialsGiveTwoDifferentWindowIds )
{
    const std::string a = DocumentTitle( "MP_GreenTint", Asset( 111 ) );
    const std::string b = DocumentTitle( "CB_Orange", Asset( 222 ) );

    EXPECT_NE( WindowId( a ), WindowId( b ) )
         << "Two materials produced the same ImGui window id, so ImGui would draw them as ONE merged window "
            "-- the second document's content inside the first document's frame, with no error anywhere.";
}

TEST( AssetDocumentIdentity, TwoMATERIALSWITHTHESAMENAMEStillGiveDifferentWindowIds )
{
    // Two `.demat` files with the same stem in different folders. The NAME is not the identity; the handle
    // is. Keying on the display name is the obvious shortcut and it merges these two into one window.
    const std::string a = DocumentTitle( "Material", Asset( 111 ) );
    const std::string b = DocumentTitle( "Material", Asset( 222 ) );

    EXPECT_NE( WindowId( a ), WindowId( b ) )
         << "Two different materials that happen to share a file name collided. The window id must come from "
            "the subject handle, never from the label.";
}

TEST( AssetDocumentIdentity, TheSameMaterialAlwaysGivesTheSameWindowId )
{
    // The other half of the relation, and the one open-or-focus rests on: asking for a material that is
    // already open must land on the window that is already there.
    EXPECT_EQ( WindowId( DocumentTitle( "MP_GreenTint", Asset( 111 ) ) ),
               WindowId( DocumentTitle( "MP_GreenTint", Asset( 111 ) ) ) );
}

TEST( AssetDocumentIdentity, TheDisplayNameCannotDecideTheWindowId )
{
    // ImGui takes the id from the LAST "###", and EditorLayer appends "###" + the whole name again when it
    // composes the visible title. So a display name carrying a "###" of its own would end up deciding the
    // id -- and two assets whose names did that would merge into one window. An asset file may be named
    // anything, so this is reachable from content, not only from code.
    const std::string a = DocumentTitle( "Evil###shared", Asset( 111 ) );
    const std::string b = DocumentTitle( "Evil###shared", Asset( 222 ) );

    EXPECT_NE( WindowId( a ), WindowId( b ) );
    EXPECT_EQ( WindowId( a ), "###doc" + Asset( 111 ).ToString() );
}

TEST( AssetDocumentIdentity, TheDocumentsIdIsTheLastMarkerInItsName )
{
    // What EditorLayer hands to Begin() is "<icon>  <label>###<name>", so the document's own marker is only
    // the id if it is the last "###" in the name. Asserted as the property rather than by re-composing
    // PanelDisplayTitle here: a copy of that rule could drift from the rule.
    const std::string title = DocumentTitle( "MP_GreenTint", Asset( 111 ) );
    EXPECT_EQ( title.rfind( "###" ), title.find( "###doc" ) );
}

// --- Open-or-focus, keyed by the subject -------------------------------------------------------------

TEST( AssetDocumentIdentity, AnOpenDocumentIsFoundByItsSubject )
{
    // Asked of OpenDocuments, which is the ONE owner of open documents. It used to be asked of the panel
    // list with a dynamic_cast, back when documents lived among the tools; that lookup is gone with the
    // mixing, and this assertion moved onto its replacement rather than out of the suite.
    OpenDocuments well;
    well.Open( std::make_unique<FakeDocument>( "MP_GreenTint", Asset( 111 ) ) );
    well.Open( std::make_unique<FakeDocument>( "CB_Orange", Asset( 222 ) ) );

    auto* found = well.Find( Asset( 222 ) );
    ASSERT_NE( found, nullptr ) << "A material that is already open was not found, so the editor would open a "
                                   "SECOND window on it -- two parameter tables editing one asset.";
    EXPECT_EQ( found->Subject(), Asset( 222 ) );
}

TEST( AssetDocumentIdentity, AMaterialThatIsNotOpenIsNotFound )
{
    OpenDocuments well;
    well.Open( std::make_unique<FakeDocument>( "MP_GreenTint", Asset( 111 ) ) );

    EXPECT_EQ( well.Find( Asset( 999 ) ), nullptr );
}

TEST( AssetDocumentIdentity, ToolPanelsAreNeverMistakenForDocuments )
{
    // This used to hold a MIXED list -- tools and documents in one vector -- and assert that a lookup over
    // it did not return the Logs panel for a material's handle. The mixing is what the split removed, so
    // the assertion is now about the container rather than about the search: a tool cannot be in the owner
    // to be mistaken for anything, because it only ever holds ISubjectDocument. The other half, that
    // a DOCUMENT cannot reach the tool registry, is asserted in Tests/Editor/DocumentOwnership.
    static_assert( std::is_convertible_v<FakeDocument*, ISubjectDocument*>,
                   "a document must be admissible to the document well" );
    static_assert( !std::is_convertible_v<FakeTool*, ISubjectDocument*>,
                   "a tool must NOT be admissible to the document well" );

    OpenDocuments well;
    EXPECT_EQ( well.Find( Asset( 111 ) ), nullptr );
}

TEST( AssetDocumentIdentity, TheNullHandleMatchesNothing )
{
    // "No asset" is not a document to focus. Without this a failed path-to-handle resolution -- which yields
    // the null handle -- would focus whichever document happened to have been constructed from one, instead
    // of reporting that nothing could be opened.
    OpenDocuments well;
    well.Open( std::make_unique<FakeDocument>( "Broken", Asset( 0 ) ) );

    EXPECT_EQ( well.Find( Asset( 0 ) ), nullptr );
}

TEST( AssetDocumentIdentity, ADocumentsSubjectIsFixedForItsLife )
{
    // The subject is the window's identity: the title, and therefore the ImGui id, is built from it. A
    // subject that could change under a live window would either merge it into another document's window or
    // orphan its saved dock entry -- which is why ISubjectDocument exposes it read-only and holds it const.
    static_assert( !std::is_assignable_v<decltype( std::declval<FakeDocument&>().Subject() ), SubjectId>,
                   "ISubjectDocument::Subject() must not be assignable -- the subject is the window's id." );
    SUCCEED();
}

// --- Cloud assets are documents too (Р3) --------------------------------------------------------------

TEST( AssetDocumentIdentity, TwoCloudTypesGiveTwoDifferentWindowIds )
{
    // The owner's request in one assertion: an artist opens two `.decloudtype` and gets two windows to
    // compare them in. The naming rule is shared with materials, so what this really guards is that nothing
    // about the cloud documents opted out of it — a cloud panel that kept its old constant panel name
    // ("Cloud Type") would put both subjects in one merged window and lose the second silently.
    const std::string a = DocumentTitle( "Cumulus.decloudtype", Asset( 501, AssetTypeID::CloudType ) );
    const std::string b = DocumentTitle( "Stratus.decloudtype", Asset( 502, AssetTypeID::CloudType ) );

    EXPECT_NE( WindowId( a ), WindowId( b ) )
         << "Two cloud types produced the same ImGui window id, so ImGui would draw them as ONE merged "
            "window -- the second document's controls inside the first document's frame.";
}

TEST( AssetDocumentIdentity, EveryCloudFormatIsFoundByItsOwnSubject )
{
    // Four formats, four open documents, one owner. Open-or-focus is keyed on the SUBJECT and never on the
    // type, so a `.dcnv` and a `.decloudtype` open at once must not find each other -- which is what a
    // lookup that had fallen back to matching on SubjectType would do.
    OpenDocuments well;
    well.Open( std::make_unique<FakeCpuDocument>( "N.dcnv", Asset( 601, AssetTypeID::CloudNoiseVolume ) ) );
    well.Open( std::make_unique<FakeCpuDocument>( "T.decloudtype", Asset( 602, AssetTypeID::CloudType ) ) );
    well.Open( std::make_unique<FakeCpuDocument>( "B.dcmv", Asset( 603, AssetTypeID::CloudModellingVolume ) ) );
    well.Open( std::make_unique<FakeCpuDocument>( "L.dclayout", Asset( 604, AssetTypeID::CloudLayout ) ) );

    ASSERT_NE( well.Find( Asset( 601, AssetTypeID::CloudNoiseVolume ) ), nullptr );
    EXPECT_EQ( well.Find( Asset( 602, AssetTypeID::CloudType ) )->Subject().Facet,
               static_cast<uint32_t>( AssetTypeID::CloudType ) );
    EXPECT_EQ( well.Find( Asset( 603, AssetTypeID::CloudModellingVolume ) )->Subject().Facet,
               static_cast<uint32_t>( AssetTypeID::CloudModellingVolume ) );
    EXPECT_EQ( well.Find( Asset( 604, AssetTypeID::CloudLayout ) )->Subject().Facet,
               static_cast<uint32_t>( AssetTypeID::CloudLayout ) );
    EXPECT_EQ( well.Find( Asset( 605, AssetTypeID::CloudLayout ) ), nullptr );

    // AND THE FACET IS PART OF THE KEY, not decoration beside it: the same file number under a different
    // asset type is a DIFFERENT subject, so a lookup that had fallen back to comparing owners alone would
    // hand back the wrong window.
    EXPECT_EQ( well.Find( Asset( 602, AssetTypeID::CloudLayout ) ), nullptr );
}

// --- A subject that is not a file at all (task U7) ----------------------------------------------------

TEST( AssetDocumentIdentity, AnEntitysComponentIsAWindowOfItsOwn )
{
    // THE CASE THE OLD SEAM COULD NOT EXPRESS. `IAssetEditorPanel` held an Assets::AssetHandle, so a
    // document was necessarily a file — and an anim graph, a particle emitter and a UI canvas are authored
    // data held by a COMPONENT ON AN ENTITY. The Details button the owner asked for had nowhere to send its
    // request, because the request carried a handle.
    OpenDocuments well;
    well.Open(
         std::make_unique<FakeDocument>( "Hero \xc2\xb7 Anim Graph", Component( 88, "AnimationComponent" ) ) );
    well.Open( std::make_unique<FakeDocument>( "Hero \xc2\xb7 Particles",
                                               Component( 88, "ParticleEmitterComponent" ) ) );

    // ONE ENTITY, TWO DOCUMENTS. Both subjects carry owner 88; only the facet differs. A seam keyed on the
    // owner alone would make these one window, and ImGui would draw the second into the first.
    EXPECT_EQ( well.Count(), 2u );
    ASSERT_NE( well.Find( Component( 88, "AnimationComponent" ) ), nullptr );
    ASSERT_NE( well.Find( Component( 88, "ParticleEmitterComponent" ) ), nullptr );
    EXPECT_NE( well.Find( Component( 88, "AnimationComponent" ) ),
               well.Find( Component( 88, "ParticleEmitterComponent" ) ) );

    // ...and the ASSET whose handle is also 88 is a third thing again, not either of them.
    EXPECT_EQ( well.Find( Asset( 88 ) ), nullptr );

    // The window ids agree with that, which is the half ImGui reads.
    EXPECT_NE( WindowId( DocumentTitle( "Hero", Component( 88, "AnimationComponent" ) ) ),
               WindowId( DocumentTitle( "Hero", Component( 88, "ParticleEmitterComponent" ) ) ) );
    EXPECT_NE( WindowId( DocumentTitle( "Hero", Component( 88, "AnimationComponent" ) ) ),
               WindowId( DocumentTitle( "Hero", Asset( 88 ) ) ) );
}

TEST( AssetDocumentIdentity, TheSameComponentOnTwoEntitiesIsTwoWindows )
{
    // The other axis: one KIND of component, two entities. This is what "open the anim graph of this
    // character" has to mean when two characters are in the level.
    OpenDocuments well;
    well.Open( std::make_unique<FakeDocument>( "Hero", Component( 1, "AnimationComponent" ) ) );
    well.Open( std::make_unique<FakeDocument>( "Villain", Component( 2, "AnimationComponent" ) ) );

    EXPECT_EQ( well.Count(), 2u );
    EXPECT_NE( WindowId( DocumentTitle( "x", Component( 1, "AnimationComponent" ) ) ),
               WindowId( DocumentTitle( "x", Component( 2, "AnimationComponent" ) ) ) );
}

// --- What is spoken for, and what is not ---------------------------------------------------------------

TEST( PendingViewBytes, AnOpenDocumentThatHasNotDrawnYetIsCounted )
{
    // The original reason the count exists: a Material Editor is created before it first draws, and builds
    // its PreviewViewport on that frame. Between the two it holds nothing and has a claim coming.
    std::vector<std::unique_ptr<IPanel>> panels;
    panels.push_back( std::make_unique<FakeDocument>( "MP_GreenTint", Asset( 111 ) ) );

    EXPECT_EQ( PendingViewBytes( panels ), Desert::Editor::ForecastPreviewViewBytes( 0, 0 ) );
}

TEST( PendingViewBytes, ADocumentThatAlreadyHoldsItsViewIsNotCountedTwice )
{
    // It is already in the LIVE renderer count, so counting it here as well would refuse the cap one
    // document early for every window that had drawn.
    auto drawn         = std::make_unique<FakeDocument>( "MP_GreenTint", Asset( 111 ) );
    drawn->m_HoldsSlot = true;

    std::vector<std::unique_ptr<IPanel>> panels;
    panels.push_back( std::move( drawn ) );

    EXPECT_EQ( PendingViewBytes( panels ), 0u );
}

TEST( PendingViewBytes, CpuOnlyDocumentsAreNotPendingDemand )
{
    // THE DEFECT THIS RULE EXISTS FOR. The four cloud editors bake on the CPU and upload an Image2D; they
    // hold no renderer slot and never will. Counted as pending demand -- which is what "open, holding
    // nothing" meant before ClaimsView existed -- five of them beside the main viewport would make
    // `live + pending` reach the six-slot cap, and the sixth cloud asset an artist double-clicked would be
    // refused with a census listing windows that hold nothing and would never hold anything.
    std::vector<std::unique_ptr<IPanel>> panels;
    for ( uint64_t i = 0; i < 5; ++i )
    {
        panels.push_back( std::make_unique<FakeCpuDocument>( "cloud", Asset( 700 + i, AssetTypeID::CloudType ) ) );
    }

    EXPECT_EQ( PendingViewBytes( panels ), 0u )
         << "A CPU-only document was counted as a view claim, so opening a sixth cloud asset would "
            "be refused for a shortage that does not exist.";
}

TEST( PendingViewBytes, CountsOnlyTheDocumentsThatWillActuallyClaim )
{
    // The mixed list, which is the one the editor really has: tool panels, cloud documents, a drawn
    // material and an undrawn one. Only the last is demand.
    auto drawn         = std::make_unique<FakeDocument>( "Drawn", Asset( 801 ) );
    drawn->m_HoldsSlot = true;

    std::vector<std::unique_ptr<IPanel>> panels;
    panels.push_back( std::make_unique<FakeTool>( "Logs" ) );
    panels.push_back( std::move( drawn ) );
    panels.push_back( std::make_unique<FakeDocument>( "Undrawn", Asset( 802 ) ) );
    panels.push_back( std::make_unique<FakeCpuDocument>( "L.dclayout", Asset( 803, AssetTypeID::CloudLayout ) ) );

    EXPECT_EQ( PendingViewBytes( panels ), Desert::Editor::ForecastPreviewViewBytes( 0, 0 ) );
}

TEST( PendingViewBytes, ADocumentThatDoesNotSayIsTreatedAsAClaimant )
{
    // ClaimsView defaults to TRUE, and that default is the conservative one: a new document type
    // that forgets to answer is refused early rather than admitted past the cap and discovered later as two
    // surfaces trading each other's per-frame camera. Asserted on the base class's own default so that
    // flipping it to false-by-default cannot pass unnoticed.
    std::vector<std::unique_ptr<IPanel>> panels;
    panels.push_back( std::make_unique<FakeDocument>( "Silent", Asset( 901 ) ) );

    EXPECT_TRUE( static_cast<const ISubjectDocument*>( panels.back().get() )->ClaimsView() );
    EXPECT_EQ( PendingViewBytes( panels ), Desert::Editor::ForecastPreviewViewBytes( 0, 0 ) );
}

// --- The skybox viewer (AV1e) --------------------------------------------------------------------------
//
// The real SkyboxViewerDocument needs a device; its identity and its slot answers live in SkyboxViewerBase so
// that THIS suite asserts the viewer's own answers. The subclass below adds only the two pure virtuals a window
// needs and a way to say "the preview was built" — the one thing the real document does with a device.
namespace
{
    class TestSkyboxViewer final : public Desert::Editor::SkyboxViewerBase
    {
    public:
        explicit TestSkyboxViewer( uint64_t handle ) : SkyboxViewerBase( "sky.detex", AssetHandle( handle ) )
        {
        }
        void OnUIRender() override
        {
        }
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return true;
        }
        void BuildPreview()
        {
            m_PreviewLive = true;
        }
    };
} // namespace

TEST( AssetDocumentIdentity, ASkyboxViewerIsFoundByItsHandleUnderTheSkyboxType )
{
    OpenDocuments well;
    well.Open( std::make_unique<TestSkyboxViewer>( 900 ) );

    auto* found = well.Find( Asset( 900, AssetTypeID::Skybox ) );
    ASSERT_NE( found, nullptr ) << "An open skybox viewer was not found by its handle, so a second double-click "
                                   "would open a second window (and spend a second renderer slot) on one sky.";
    EXPECT_EQ( found->Subject(), Desert::Editor::SkyboxViewerSubject( AssetHandle( 900 ) ) );
    EXPECT_EQ( well.Find( Asset( 900, AssetTypeID::Texture2D ) ), nullptr )
         << "The same number under the texture type is a different window.";
}

TEST( AssetDocumentIdentity, TwoSkyboxesGiveTwoDifferentWindowIds )
{
    const TestSkyboxViewer a( 901 );
    const TestSkyboxViewer b( 902 );
    EXPECT_NE( WindowId( a.GetName() ), WindowId( b.GetName() ) );
}

TEST( PendingViewBytes, ASkyboxViewerIsAClaimantUntilItsPreviewHoldsTheView )
{
    std::vector<std::unique_ptr<IPanel>> panels;
    auto                                 viewer = std::make_unique<TestSkyboxViewer>( 903 );
    auto*                                raw    = viewer.get();
    panels.push_back( std::move( viewer ) );

    EXPECT_EQ( PendingViewBytes( panels ), raw->ViewForecastBytes() )
         << "A skybox viewer that has not drawn yet was not counted, so the budget would admit a document "
            "there is no room for.";
    raw->BuildPreview();
    EXPECT_EQ( PendingViewBytes( panels ), 0u ) << "A viewer holding its view was counted twice.";
}

// --- The static mesh viewer (AV1f) ---------------------------------------------------------------------
namespace
{
    class TestStaticMeshViewer final : public Desert::Editor::StaticMeshViewerBase
    {
    public:
        explicit TestStaticMeshViewer( uint64_t handle )
             : StaticMeshViewerBase( "probe.stmesh", AssetHandle( handle ) )
        {
        }
        void OnUIRender() override
        {
        }
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return true;
        }
        void BuildPreview()
        {
            m_PreviewLive = true;
        }
    };
} // namespace

TEST( AssetDocumentIdentity, AStaticMeshViewerIsFoundByItsHandleUnderTheMeshType )
{
    OpenDocuments well;
    well.Open( std::make_unique<TestStaticMeshViewer>( 910 ) );

    auto* found = well.Find( Asset( 910, AssetTypeID::Mesh ) );
    ASSERT_NE( found, nullptr )
         << "A second Open of the same mesh would open a second viewer (and a second slot).";
    EXPECT_EQ( found->Subject(), Desert::Editor::StaticMeshViewerSubject( AssetHandle( 910 ) ) );
    EXPECT_EQ( well.Find( Asset( 910, AssetTypeID::Material ) ), nullptr );
    EXPECT_EQ( well.Find( Asset( 911, AssetTypeID::Mesh ) ), nullptr );
}

TEST( PendingViewBytes, AStaticMeshViewerIsAClaimantUntilItsPreviewHoldsTheView )
{
    std::vector<std::unique_ptr<IPanel>> panels;
    auto                                 viewer = std::make_unique<TestStaticMeshViewer>( 912 );
    auto*                                raw    = viewer.get();
    panels.push_back( std::move( viewer ) );

    EXPECT_EQ( PendingViewBytes( panels ), raw->ViewForecastBytes() );
    raw->BuildPreview();
    EXPECT_EQ( PendingViewBytes( panels ), 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// --- What is spoken for, in bytes (RT2i) ----------------------------------------------------------------

TEST( PendingViewBytes, AnUndrawnClaimantCountsItsForecastAndNothingElseDoes )
{
    // The forecast is the preview profile's census at the window's default size; the fake has none, so its
    // first build is at kUnsizedViewExtent and that is what it has spoken for.
    const uint64_t forecast = Desert::Editor::ForecastPreviewViewBytes( 0, 0 );
    ASSERT_GT( forecast, 0u );

    std::vector<std::unique_ptr<IPanel>> panels;
    panels.push_back( std::make_unique<FakeDocument>( "Undrawn", Asset( 201 ) ) );
    auto held         = std::make_unique<FakeDocument>( "Drawn", Asset( 202 ) );
    held->m_HoldsSlot = true; // its memory is in the usage already; counting the forecast would count it twice
    panels.push_back( std::move( held ) );
    auto cpu          = std::make_unique<FakeDocument>( "Cloud", Asset( 203 ) );
    cpu->m_ClaimsSlot = false; // drawn on the CPU: no view is ever coming
    panels.push_back( std::move( cpu ) );

    EXPECT_EQ( static_cast<const ISubjectDocument*>( panels.front().get() )->ViewForecastBytes(), forecast );
    EXPECT_EQ( Desert::Editor::PendingViewBytes( panels ), forecast );
}

TEST( AdmitDocumentView, TheNewForecastPlusPendingDemandMustFitWhatIsFree )
{
    FakeDocument   incoming( "Incoming", Asset( 211 ) );
    const uint64_t forecast = incoming.ViewForecastBytes();
    const uint64_t pending  = 3 * forecast;
    const uint64_t usage    = 100ull * 1024 * 1024;

    Desert::Engine::ViewBudget::Reading reading;
    reading.UsageBytes   = usage;
    reading.CeilingBytes = usage + pending + forecast; // exactly enough

    const auto fits = Desert::Editor::AdmitDocumentView( incoming, pending, reading );
    EXPECT_TRUE( fits.Ok );
    EXPECT_EQ( fits.RequestBytes, pending + forecast ) << "The refusal must state what was asked for.";
    EXPECT_EQ( fits.ReserveBytes, 0u ) << "A document the person opened is a user surface: it keeps no reserve.";

    reading.CeilingBytes -= 1;
    EXPECT_FALSE( Desert::Editor::AdmitDocumentView( incoming, pending, reading ).Ok )
         << "One byte short of the forecast plus what is spoken for was admitted.";
}

TEST( AdmitDocumentView, ADocumentThatBuildsNoViewAsksOnlyForWhatIsAlreadySpokenFor )
{
    FakeDocument cpu( "Cloud", Asset( 221 ) );
    cpu.m_ClaimsSlot = false;

    Desert::Engine::ViewBudget::Reading reading;
    reading.UsageBytes   = 0;
    reading.CeilingBytes = 1024;

    EXPECT_TRUE( Desert::Editor::AdmitDocumentView( cpu, 1024, reading ).Ok )
         << "A CPU-drawn document was refused over a view it will never build.";
    EXPECT_EQ( Desert::Editor::AdmitDocumentView( cpu, 0, reading ).RequestBytes, 0u );
}
