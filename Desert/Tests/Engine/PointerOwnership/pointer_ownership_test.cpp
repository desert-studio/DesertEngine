// THE POINTER-OWNERSHIP CENSUS.
//
// The owner asked for an audit of where `shared_ptr`, `unique_ptr` and raw pointers are used and which
// of them each place should be. The failure mode of that request is five hundred lines of taste, so
// every verdict here is the answer to two FACTUAL questions instead:
//
//   Q1. Who is OBLIGED to destroy this object?  One known owner -> unique_ptr. Several owners whose
//       deaths are not ordered -> shared_ptr. No owner, only a watcher -> a raw pointer or a weak_ptr,
//       and then Q2 is compulsory.
//   Q2. Can the observed die before the observer?  Yes and unguarded -> that is a DEFECT, not a style.
//       No -> the row must name WHAT guarantees it.
//
// The subject is the pointer-typed DATA MEMBER (pointer_ownership_scan.hpp says why), the verdicts are
// in pointer_ownership_register.hpp, and this file is what makes them fail.
//
// WHY A CENSUS AND NOT A DOCUMENT IN `Docs/`. A report goes stale in silence; this goes red. A pointer
// member added tomorrow has no row, and `EveryRawPointerMemberNamesItsGuard` names it with its file and
// line and asks its author the two questions above.

#include "pointer_ownership_register.hpp"
#include "pointer_ownership_scan.hpp"

#include <Engine/Graphic/Render2D/Render2DExecutorRetire.hpp>

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    using namespace Desert::Tests::PointerCensus;
    namespace Text = Desert::Tests::ConsumerText;

    const std::vector<Member>& Members()
    {
        static const std::vector<Member> members = []
        {
            const std::string root = RepoRoot();
            return root.empty() ? std::vector<Member>{} : ScanMembers( root );
        }();
        return members;
    }

    int CountOf( Form f )
    {
        int n = 0;
        for ( const Member& m : Members() )
            n += m.Kind == f ? 1 : 0;
        return n;
    }

    std::string ReadRepoFile( const char* relative )
    {
        return Text::StripCommentsAndLiterals( ReadAll( fs::path( RepoRoot() ) / relative ) );
    }
} // namespace

// The listing the register was WRITTEN from, and the one the next stage will widen it from. Disabled
// because it asserts nothing; run it with
//   ./PointerOwnership --gtest_also_run_disabled_tests --gtest_filter=*Dump*
// after adding a tree to ScannedTrees(), and every new member arrives with its file, line and
// declaration ready to be answered for.
TEST( PointerOwnership, DISABLED_DumpEveryMember )
{
    ASSERT_FALSE( RepoRoot().empty() );
    int counts[4] = { 0, 0, 0, 0 };
    for ( const Member& m : Members() )
    {
        ++counts[static_cast<int>( m.Kind )];
        std::printf( "%s|%d|%s|%s|%s|%s\n", FormName( m.Kind ), m.Line, m.File.c_str(), m.Class.c_str(),
                     m.Name.c_str(), m.Decl.c_str() );
    }
    std::printf( "TOTAL %zu unique=%d weak=%d shared=%d raw=%d\n", Members().size(), counts[0], counts[1],
                 counts[2], counts[3] );
}

// ------------------------------------------------------------------------------------------------
// The population
// ------------------------------------------------------------------------------------------------

namespace
{
    // One named member per shape the classifier must tell apart: each form, a container of each, and a
    // shared_ptr spelled through an alias. A reader that stops seeing a form, or files it under the wrong
    // one, turns this red with the member's name. These rows move only when the member itself moves.
    struct Sentinel
    {
        const char* File;
        const char* Class;
        const char* Member;
        Form        Kind;
    };

    const Sentinel kSentinels[] = {
         { "Desert/Desert/Source/Engine/Graphic/Materials/Material.hpp", "Material", "m_MaterialExecutor",
           Form::Unique },
         { "Desert/Common/Source/Common/Core/LayerStack.hpp", "LayerStack", "m_Layers", Form::Unique },
         { "Desert/Desert/Source/Engine/Core/EngineContext.hpp", "EngineContext", "m_Window", Form::Weak },
         { "Desert/Desert/Source/Engine/Assets/AnimGraphAsset.hpp", "AnimGraphAsset", "m_Graph", Form::Shared },
         { "Desert/Desert/Source/Engine/ECS/Components.hpp", "StaticMeshComponent", "RuntimeMaterialInstances",
           Form::Shared },
         { "Desert/Desert/Source/Engine/Core/WorldStreamer.hpp", "WorldStreamer", "m_Assets", Form::Raw },
         { "Desert/Common/Source/Common/Core/AutoRegistry.hpp", "AutoRegistry", "m_Instances", Form::Raw },
    };
} // namespace

// NO TOTAL IS PINNED HERE, ON PURPOSE. This test used to pin the four per-form counts and their sum as
// literals, so every branch that added a pointer member edited the same five numbers and every merge of
// two such branches conflicted on them. The truth of the census is the register (one row per raw member)
// and the scan; the counts are derived from those two and asserted as relations, so two independent
// additions touch only their own register rows.
TEST( PointerOwnership, TheScanFindsTheCensusedPopulation )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository from the working directory";

    // A census that finds nothing to census has stopped working, and it fails in the confident voice it
    // uses when a tree really is clean. This is the line that tells the two apart.
    ASSERT_FALSE( Members().empty() ) << "the scan found no pointer members at all -- the reader is blind, "
                                         "and every assertion below is vacuous.";

    for ( const Sentinel& s : kSentinels )
    {
        const auto it = std::find_if( Members().begin(), Members().end(), [&s]( const Member& m )
                                      { return m.File == s.File && m.Class == s.Class && m.Name == s.Member; } );
        if ( it == Members().end() )
        {
            ADD_FAILURE() << s.File << " " << s.Class << "::" << s.Member << " (" << FormName( s.Kind )
                          << ") is no longer found by the scan: the reader lost this shape, or the member moved "
                             "-- then point the sentinel at another member of the same shape.";
            continue;
        }
        EXPECT_EQ( it->Kind, s.Kind ) << s.File << ":" << it->Line << " " << s.Class << "::" << s.Member << " ("
                                      << it->Decl << ") is classified " << FormName( it->Kind ) << ", expected "
                                      << FormName( s.Kind );
    }

    // Every form is populated: a classifier that silently folds one form into another empties it.
    for ( const Form f : { Form::Unique, Form::Weak, Form::Shared, Form::Raw } )
        EXPECT_GT( CountOf( f ), 0 ) << "the scan classified no member as " << FormName( f );

    // The derived identity: with every raw member owning exactly one row and every row naming a scanned
    // raw member (the two tests below), the register's size IS the number of distinct raw members. The
    // key is (file, class, member), not the declaration: two same-named local structs in one file (the
    // Frame of SerializeReflected and of DeserializeReflected) are one key and share one row.
    std::set<std::tuple<std::string, std::string, std::string>> rawKeys;
    for ( const Member& m : Members() )
        if ( m.Kind == Form::Raw )
            rawKeys.emplace( m.File, m.Class, m.Name );
    EXPECT_EQ( rawKeys.size(), Register().size() )
         << "the raw population and the register disagree; the tests below name the member or row.";
    EXPECT_EQ( CountOf( Form::Unique ) + CountOf( Form::Weak ) + CountOf( Form::Shared ) + CountOf( Form::Raw ),
               static_cast<int>( Members().size() ) );
}

// ------------------------------------------------------------------------------------------------
// Every raw pointer answers Q2
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, EveryRawPointerMemberNamesItsGuard )
{
    ASSERT_FALSE( Members().empty() );

    int unlisted = 0;
    for ( const Member& m : Members() )
    {
        if ( m.Kind != Form::Raw )
            continue;
        const auto rows =
             std::count_if( Register().begin(), Register().end(), [&m]( const Row& r )
                            { return m.Class == r.Class && m.Name == r.Member && m.File == r.File; } );
        // Exactly one: a second row for the same member is two arguments where one is read and the
        // other rots, and it would also break the derived raw count in TheScanFindsTheCensusedPopulation.
        EXPECT_LE( rows, 1 ) << m.File << ":" << m.Line << " " << m.Class << "::" << m.Name << " has " << rows
                             << " register rows; keep exactly one.";
        if ( rows != 0 )
            continue;
        ++unlisted;
        ADD_FAILURE() << m.File << ":" << m.Line << " " << m.Class << "::" << m.Name << " (" << m.Decl
                      << ")\nis a raw pointer member with no row in the register. A raw pointer answers NEITHER "
                         "ownership question by itself, so add a row to pointer_ownership_register.hpp saying:\n"
                         "  (1) who is obliged to destroy the pointee, and\n"
                         "  (2) what stops it dying before this object does.\n"
                         "If the answer to (2) is 'nothing', that is a defect: fix it, or file it as Guard::Debt "
                         "with the task that owns the fix.";
    }
    EXPECT_EQ( unlisted, 0 );
}

TEST( PointerOwnership, TheRegisterDescribesMembersThatStillExist )
{
    // The other direction, and the one a census normally loses first: a row whose member was renamed or
    // deleted goes on asserting nothing while looking like coverage. DeviceLostCensus lost sight this
    // way, which is why the check is here rather than assumed.
    ASSERT_FALSE( Members().empty() );

    for ( const Row& r : Register() )
    {
        const bool alive = std::any_of(
             Members().begin(), Members().end(), [&r]( const Member& m )
             { return m.Kind == Form::Raw && m.File == r.File && m.Class == r.Class && m.Name == r.Member; } );
        EXPECT_TRUE( alive ) << r.File << " " << r.Class << "::" << r.Member
                             << " has a register row but the scan no longer finds the member. Delete the "
                                "row, or find out why the scan stopped seeing it.";
    }
}

TEST( PointerOwnership, EveryRowCarriesAnArgument )
{
    for ( const Row& r : Register() )
    {
        EXPECT_GT( std::string( r.Why ).size(), 30u )
             << r.Class << "::" << r.Member
             << " has no argument. A row without one is a name in a list, which is what this census "
                "exists instead of.";
        if ( r.How != Guard::Debt )
            continue;

        // A DEBT WITH NO TASK NAME IS UNREADABLE IN A MONTH. The same rule ConfigOwnership enforces on
        // its own debt register, for the same reason.
        EXPECT_FALSE( std::string( r.Task ).empty() )
             << r.Class << "::" << r.Member
             << " is recorded as a live lifetime defect and names no task. Name the task that owns the "
                "fix, or fix it here.";
    }
}

// ------------------------------------------------------------------------------------------------
// The guards that rest on a checkable fact are checked
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, MaterialPropertyStorageIsAddressStable )
{
    // 46 of the 322 rows rest on ONE argument: a material's cached `Texture2DProperty*` cannot dangle
    // because the property lives in the material's own executor. That argument has three legs and all
    // three are facts about the source, so all three are checked here rather than believed.
    ASSERT_FALSE( RepoRoot().empty() );

    const std::string executorHpp =
         ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Materials/MaterialExecutor.hpp" );
    const std::string executorCpp =
         ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Materials/MaterialExecutor.cpp" );
    const std::string materialHpp = ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Materials/Material.hpp" );

    // (1) The storage holds HANDLES, not objects. `std::vector<T>` would move every property when it
    //     grew, and all 46 cached pointers would dangle on the next `emplace`.
    EXPECT_NE( executorHpp.find( "using PropertyStorage = std::vector<std::shared_ptr<T>>" ), std::string::npos )
         << "MaterialExecutor::PropertyStorage is no longer a vector of shared_ptr. If the properties "
            "are stored BY VALUE, every material's cached property pointer dangles the moment the "
            "vector grows.";

    // (2) The material owns the executor, so the observed dies with the observer.
    EXPECT_NE( materialHpp.find( "std::unique_ptr<MaterialExecutor> m_MaterialExecutor" ), std::string::npos )
         << "Material no longer owns its MaterialExecutor by unique_ptr; the 46 cached property pointers "
            "lose the reason they cannot outlive their pointee.";

    // (3) The table is built ONCE, in the constructor, and never rebuilt. A shader hot-reload that
    //     called InitializeProperties() again would leave every cached pointer pointing at a released
    //     property.
    int initializeCalls = 0;
    for ( std::size_t at : Text::WordPositions( executorCpp, "InitializeProperties" ) )
    {
        // The definition itself (`void MaterialExecutor::InitializeProperties()`) is not a call.
        const std::string before = executorCpp.substr( at < 64 ? 0 : at - 64, at < 64 ? at : 64 );
        if ( before.find( "MaterialExecutor::" ) != std::string::npos )
            continue;
        ++initializeCalls;
    }
    EXPECT_EQ( initializeCalls, 1 ) << "InitializeProperties() is called " << initializeCalls
                                    << " times. It must be called exactly once, from the constructor: a "
                                       "second call rebuilds the property table under 46 cached pointers.";

    for ( const char* mutation : { "PropertiesStorage.clear", "PropertiesStorage.erase",
                                   "PropertiesStorage.resize", "PropertiesStorage.pop_back" } )
    {
        EXPECT_EQ( executorCpp.find( mutation ), std::string::npos )
             << "MaterialExecutor now does `" << mutation
             << "`. Removing a property releases the object 46 materials hold a raw pointer to.";
        EXPECT_EQ( executorHpp.find( mutation ), std::string::npos );
    }
}

TEST( PointerOwnership, TheUniformImageKeepsTheDescriptorAndNotTheImage )
{
    // The shape this audit wants preferred, asserted where it already exists: a class that must survive
    // an image it does not own keeps a COPY OF THE VALUE it needs (the descriptor, snapshotted at set
    // time) rather than a handle on somebody else's lifetime. Г12 removed the pointers; this is what
    // stops them coming back.
    ASSERT_FALSE( RepoRoot().empty() );

    for ( const char* header :
          { "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan/VulkanUniformImage2D.hpp",
            "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan/VulkanUniformImageCube.hpp" } )
    {
        const std::string src = ReadRepoFile( header );
        EXPECT_NE( src.find( "VkDescriptorImageInfo m_DescriptorInfo" ), std::string::npos )
             << header << " must keep the descriptor it copied out at SetImage time.";
        EXPECT_EQ( src.find( "Graphic::Image2D* m_Image" ), std::string::npos )
             << header << " has taken a raw pointer to an image it does not own back as a member.";
        EXPECT_EQ( src.find( "Graphic::ImageCube* m_Image" ), std::string::npos ) << header;
    }
}

TEST( PointerOwnership, NoRawPointerMemberIsDeletedByItsHolder )
{
    // AN OWNING RAW POINTER WEARING AN OBSERVER'S CLOTHES. `delete m_Something` in a class whose
    // `m_Something` is a raw pointer member means Q1's answer is "this class" and the type does not say
    // so — which is the one case where the audit's verdict is mechanical: it should be a unique_ptr.
    // The two legitimate owning raws in this tree hold VMA handles, which have no C++ destructor and
    // are released through the allocator; neither is `delete`d.
    ASSERT_FALSE( Members().empty() );

    // THE MEMBER IS DECLARED IN THE HEADER AND DELETED IN THE .cpp, so scanning only the declaring file
    // sees nothing. Caught by the mutation check for this very test: a `delete m_Backdrop;` planted in
    // Render2D.cpp left it green. The class's own translation unit is where a class frees its own
    // members, so both halves of the pair are read.
    std::map<std::string, std::vector<std::string>> byFile;
    for ( const Member& m : Members() )
    {
        if ( m.Kind != Form::Raw )
            continue;
        byFile[m.File].push_back( m.Name );
        if ( m.File.size() > 4 && m.File.compare( m.File.size() - 4, 4, ".hpp" ) == 0 )
        {
            const std::string sibling = m.File.substr( 0, m.File.size() - 4 ) + ".cpp";
            if ( fs::exists( fs::path( RepoRoot() ) / sibling ) )
                byFile[sibling].push_back( m.Name );
        }
    }

    for ( const auto& [file, names] : byFile )
    {
        const std::string src = ReadRepoFile( file.c_str() );
        for ( std::size_t at : Text::WordPositions( src, "delete" ) )
        {
            const std::size_t i    = Text::SkipSpace( src, at + 6 );
            const std::string what = Text::IdentAt( src, i );
            for ( const std::string& name : names )
            {
                if ( what != name )
                    continue;
                ADD_FAILURE() << file << ":" << LineOf( src, at ) << " deletes its own raw pointer member `"
                              << name
                              << "`. That is sole ownership spelled without saying so -- make it a "
                                 "std::unique_ptr, which cannot be forgotten on an early return.";
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// The shared_ptr half: a claim of shared ownership, and what it costs when it is not true
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, SharedOwnershipIsTheMajorityAndThatIsTheMeasuredAnswer )
{
    // THE AUDIT'S LARGEST SINGLE RESULT IS A REFUSAL, and it is recorded here so the next person does
    // not re-derive it. Roughly two fifths of the members in these trees are `shared_ptr`, and for the GPU
    // resources that is the CORRECT form rather than a habit: an Image2D is held at once by the
    // framebuffer that allocated it, by the descriptor sets that sample it and by the deletion queue
    // that outlives both, and no two of those have an ordered death. Converting them to `unique_ptr`
    // would not be a cleanup, it would be a use-after-free.
    //
    // What the audit did NOT find is worth saying plainly: not one `shared_ptr` member in these trees
    // sits in a per-frame hot loop where the atomic refcount pair is measurable. The cost of a wrong
    // `shared_ptr` here is a false impression of shared ownership, and the register's job is to make
    // the true owner findable instead of mass-replacing them for uniformity -- churn that would hide
    // the seven real findings in a diff of two hundred files.
    EXPECT_GT( CountOf( Form::Shared ), CountOf( Form::Unique ) + CountOf( Form::Weak ) );
}

TEST( PointerOwnership, NoECSSystemHandsARenderCommandAnAddress )
{
    // THE RULE A8-3 COST, WRITTEN WHERE IT CAN FAIL. A render command is recorded by an ECS system and
    // read later — after every remaining system has run, ScriptSystem and its user Lua among them, and
    // after the command buffer has been executed. Anything the command holds must therefore be either a
    // value or a CO-OWNED handle; the address of a component member is neither, and it is the shape that
    // put five members of this register in debt.
    //
    // The check is deliberately blunt: an argument to `Emplace<...>` may not begin with `&`. It catches
    // `&mesh.RuntimeSlotPtrs` and `&ism.InstanceTransforms`, which is what it is for, and it also catches
    // the address of a LOCAL — which is equally wrong here for the same reason and equally worth a
    // conversation. Measured on the fixed tree: sixteen Emplace sites across eight systems, zero
    // addresses.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const fs::path systems = fs::path( root ) / "Desert/Desert/Source/Engine/ECS/System";
    int            sites   = 0;

    std::error_code ec;
    for ( auto it = fs::recursive_directory_iterator( systems, ec );
          !ec && it != fs::recursive_directory_iterator(); ++it )
    {
        const fs::path& path = it->path();
        if ( path.extension() != ".hpp" && path.extension() != ".cpp" )
            continue;

        const std::string src = ReadRepoFile( fs::relative( path, root ).generic_string().c_str() );
        for ( std::size_t at : Text::WordPositions( src, "Emplace" ) )
        {
            const std::size_t open = src.find( '(', at );
            if ( open == std::string::npos || src.find( '<', at ) > open )
                continue;
            ++sites;

            int depth = 0;
            for ( std::size_t i = open; i < src.size(); ++i )
            {
                if ( src[i] == '(' )
                    ++depth;
                else if ( src[i] == ')' && --depth == 0 )
                    break;
                if ( depth != 1 || ( src[i] != '(' && src[i] != ',' ) )
                    continue;

                const std::size_t arg = Text::SkipSpace( src, i + 1 );
                if ( arg >= src.size() || src[arg] != '&' )
                    continue;
                if ( arg + 1 >= src.size() || !Text::IsIdentChar( src[arg + 1] ) )
                    continue;

                ADD_FAILURE()
                     << fs::relative( path, root ).generic_string() << ":" << LineOf( src, arg )
                     << " hands a render command the ADDRESS of `" << Text::IdentAt( src, arg + 1 )
                     << "`.\nA command outlives the system that recorded it: every later ECS system runs "
                        "before it is executed, ScriptSystem among them, and that one runs user Lua which "
                        "can add or destroy entities. Pass a value, or a co-owned handle "
                        "(Graphic::MaterialSlotBinding is the one this rule was written for).";
            }
        }
    }

    EXPECT_GT( sites, 8 ) << "the scan found almost no Emplace sites -- either the commands are recorded "
                             "some other way now, or this check has stopped looking at anything.";
}

// ------------------------------------------------------------------------------------------------
// The fact three Debt rows rest on, instantiated rather than reasoned about
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, EnttComponentAddressesAreNotStable )
{
    // FIVE OF THE REGISTER'S ROWS SAY "the address of a std::vector member of an ECS component", and
    // whether that is a defect turns entirely on one property of the vendored entt: does a component
    // keep its address when the pool changes? THE ANSWER IS NO, and it is asserted here rather than
    // read off a header, because the header is the thing that would change under us.
    //
    // `storage<Entity, Type>` keeps `std::vector<object_type> instances` (entt.hpp) — components live
    // BY VALUE in a flat vector. `emplace` push_backs, so growth moves every component; `erase` is
    // swap-and-pop, so destroying one entity moves ANOTHER component over it. Newer entt uses paged
    // storage and would make both of these stable; this test is what will say so on the day the
    // submodule moves.
    //
    // Why it matters here and not merely in the abstract: MeshECSSystem records `&mesh.RuntimeSlotPtrs`
    // into the render command buffer, ScriptSystem is registered AFTER it and runs Lua (`World
    // .spawnMarker` adds a StaticMeshComponent, `entity:destroy()` removes one), and the commands are
    // only executed and dereferenced afterwards. The two ends of that window are in different files and
    // nothing between them says the pointer must survive it.
    struct Probe
    {
        std::vector<int> Slots;
        int              Filler = 0;
    };

    entt::registry reg;

    // Growth. Reserving would only postpone it; the point is that nothing in the engine reserves.
    const auto first                  = reg.create();
    reg.emplace<Probe>( first ).Slots = { 1, 2, 3 };
    const void* before                = &reg.get<Probe>( first ).Slots;

    bool moved = false;
    for ( int i = 0; i < 64 && !moved; ++i )
    {
        const auto e                  = reg.create();
        reg.emplace<Probe>( e ).Slots = { i };
        moved                         = ( &reg.get<Probe>( first ).Slots ) != before;
    }
    EXPECT_TRUE( moved )
         << "adding components no longer moves the ones already in the pool. That would make five of "
            "this register's Debt rows obsolete -- check whether the ECS storage became paged, and if "
            "it did, close A8-3 with this test as the evidence.";

    // Swap-and-pop. The LAST component in the pool is moved over the erased slot, so a pointer to the
    // last one is stolen from under its holder even though that entity was never touched.
    entt::registry two;
    const auto     keep               = two.create();
    const auto     tail               = two.create();
    two.emplace<Probe>( keep ).Slots  = { 1 };
    two.emplace<Probe>( tail ).Slots  = { 2, 3 };
    const std::vector<int>* tailSlots = &two.get<Probe>( tail ).Slots;

    two.destroy( keep );
    ASSERT_TRUE( two.valid( tail ) ) << "the surviving entity must still be valid";
    EXPECT_NE( &two.get<Probe>( tail ).Slots, tailSlots )
         << "destroying ANOTHER entity no longer moves this one's component. Same question as above.";
}

TEST( PointerOwnership, EditorLayerDeclaresItsHostsBeforeItsPanels )
{
    // TWENTY ROWS OF THE EDITOR HALF REST ON ONE FACT: a panel holding `AssetManager*` or
    // `AnimationLibrary*` cannot outlive what it points at, because EditorLayer declares those members
    // BEFORE m_Panels and C++ destroys members in reverse declaration order. That is not a property of
    // the panels, it is a property of ONE LINE ORDER in one header — the weakest kind of guarantee in
    // this register, and the easiest to break by moving a member while tidying up. So it is asserted.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src = ReadRepoFile( "Editor/Source/EditorLayer.hpp" );
    ASSERT_FALSE( src.empty() );

    const std::size_t panels = src.find( "PanelRegistry m_Panels" );
    ASSERT_NE( panels, std::string::npos ) << "EditorLayer no longer declares m_Panels -- the twenty "
                                              "Guard::HostOutlivesUs rows that cite this order need "
                                              "re-deriving, not a renamed search string.";

    for ( const char* host :
          { "std::shared_ptr<Assets::AssetManager>", "m_AnimationLibrary", "OpenDocuments m_OpenDocuments" } )
    {
        const std::size_t at = src.find( host );
        ASSERT_NE( at, std::string::npos ) << host << " is no longer a member of EditorLayer.";
        EXPECT_LT( at, panels )
             << host
             << " is now declared AFTER m_Panels, so it is destroyed BEFORE the panels that point at it. "
                "Every panel holding a raw pointer to it is then reading freed memory during its own "
                "destructor. Move it back above m_Panels, or give the panels a weak handle.";
    }

    // AND THE SAME ORDER FOR DOCUMENTS, WHICH THIS TEST DID NOT COVER. Half a dozen rows in the register
    // are about panels that are no longer panels: U7 moved the anim graph and the particle editor into
    // m_OpenDocuments and U7-2 moved the sequencer and the UI editor, and a DOCUMENT holding
    // `AnimationLibrary*` is guarded by its host preceding m_OpenDocuments — not by the host preceding
    // m_Panels, which is a different member and could satisfy the loop above while this failed.
    //
    // The rows said "before m_Panels" for a whole release after their subject stopped being a panel. They
    // happened to be true, because m_OpenDocuments is itself above m_Panels; a true sentence about the
    // wrong container is exactly the guarantee that stops holding the day somebody reorders one of the
    // three, and nothing would have gone red.
    const std::size_t documents = src.find( "OpenDocuments m_OpenDocuments" );
    ASSERT_NE( documents, std::string::npos );
    for ( const char* host : { "std::shared_ptr<Assets::AssetManager>", "m_AnimationLibrary" } )
    {
        const std::size_t at = src.find( host );
        ASSERT_NE( at, std::string::npos ) << host << " is no longer a member of EditorLayer.";
        EXPECT_LT( at, documents )
             << host
             << " is now declared AFTER m_OpenDocuments, so it is destroyed BEFORE the documents that point "
                "at it. Every document holding a raw pointer to it is then reading freed memory during its "
                "own destructor. Move it back above m_OpenDocuments, or give the documents a weak handle.";
    }
}

// ------------------------------------------------------------------------------------------------
// A8-1: the condition a cache eviction has to respect, as an assertion rather than a comment
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, Render2DExecutorRetirementRespectsFramesInFlight )
{
    using Desert::Graphic::Render2D::MayRetireExecutor;

    // The three Render2D executor caches are keyed by a texture ADDRESS and, until A8-1, nothing ever
    // removed an entry: every viewport resize and every new texture left a MaterialExecutor and its
    // descriptor set behind for the life of the process. The reason the obvious `cache.clear()` was
    // REFUSED rather than written is the whole content of this test — destroying an executor destroys
    // descriptor sets a submitted frame may still be reading, which corrupts a frame instead of crashing
    // a process. So the window is the condition, and here it is where it can fail.
    constexpr uint32_t kWindow = 9; // 3 frames in flight x a margin of 3, a plausible ExecutorRetireWindow()

    // Nothing may be retired inside the window, and the EDGE belongs to the GPU: an entry last used
    // exactly `window` frames ago is still reachable by the oldest frame in flight.
    for ( uint64_t age = 0; age <= kWindow; ++age )
        EXPECT_FALSE( MayRetireExecutor( 100, 100 + age, kWindow ) )
             << "an executor last used " << age << " frames ago was retired with a window of " << kWindow
             << ". A frame recorded against it may still be in flight, and destroying its descriptor set "
                "corrupts that frame rather than failing loudly.";

    EXPECT_TRUE( MayRetireExecutor( 100, 100 + kWindow + 1, kWindow ) )
         << "nothing is ever retired, which is the leak this was written to close.";
    EXPECT_TRUE( MayRetireExecutor( 0, 1000, kWindow ) );

    // A counter that has not moved (the first frames of a process, or two reads inside one frame) must
    // not retire anything -- and must not underflow while deciding, which is what an unsigned subtraction
    // written the obvious way does.
    EXPECT_FALSE( MayRetireExecutor( 100, 100, kWindow ) );
    EXPECT_FALSE( MayRetireExecutor( 100, 50, kWindow ) );

    // A window of zero would mean "retire on the next frame", which is inside the in-flight range for
    // every real configuration. It is not reachable through ExecutorRetireWindow() -- that
    // function floors frames-in-flight at 3 -- and the sweep must not invent its own number.
    const std::string src = ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp" );
    EXPECT_NE( src.find( "ExecutorRetireWindow()" ), std::string::npos )
         << "Render2D::RetireUnusedExecutors no longer takes its window from ExecutorRetireWindow(). A literal "
            "here is a second answer to 'how long does a frame live', and the "
            "two would drift.";
    EXPECT_NE( src.find( "MayRetireExecutor(" ), std::string::npos )
         << "the sweep no longer goes through the tested predicate.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

TEST( PointerOwnership, EditorLayerSeedsEveryScenePanelAtRegistration )
{
    // A PANEL THAT FOLLOWS THE ACTIVE SCENE MUST BE BORN WITH ONE (L8e). IPanel::SetScene is called only
    // by EditorLayer::SetActiveScene, which returns early when the scene asked for is already active -- and
    // the primary scene IS active when the panels are registered. So a panel that overrides SetScene but is
    // constructed without m_MainScene holds no scene until the user focuses a second view and comes back.
    // LandscapePanel was registered that way and drew "no scene" in every normal session, while every
    // palette command (which reads the scene directly) answered ok. The fact is a relation between two
    // files, so it is asserted over both rather than trusted.
    namespace fs           = std::filesystem;
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string layer = ReadRepoFile( "Editor/Source/EditorLayer.cpp" );
    ASSERT_FALSE( layer.empty() );

    std::map<std::string, std::string> headers; // panel class -> header text
    for ( const auto& entry :
          fs::recursive_directory_iterator( fs::path( root ) / "Editor/Source/Editor/Panels" ) )
    {
        if ( entry.path().extension() != ".hpp" )
            continue;
        std::ifstream      in( entry.path() );
        std::ostringstream text;
        text << in.rdbuf();
        headers[entry.path().stem().string()] = text.str();
    }

    const std::string marker       = "m_Panels.Add<Editor::";
    int               checked      = 0;
    bool              sawLandscape = false;
    for ( std::size_t at = layer.find( marker ); at != std::string::npos; at = layer.find( marker, at + 1 ) )
    {
        const std::size_t nameBegin = at + marker.size();
        const std::size_t nameEnd   = layer.find( '>', nameBegin );
        const std::size_t callEnd   = layer.find( ';', nameEnd );
        ASSERT_NE( callEnd, std::string::npos );
        const std::string panel = layer.substr( nameBegin, nameEnd - nameBegin );
        const auto        it    = headers.find( panel );
        if ( it == headers.end() || it->second.find( "void SetScene(" ) == std::string::npos )
            continue;
        ++checked;
        sawLandscape = sawLandscape || panel == "LandscapePanel";
        EXPECT_NE( layer.substr( nameEnd, callEnd - nameEnd ).find( "m_MainScene" ), std::string::npos )
             << panel << " overrides SetScene but EditorLayer registers it without m_MainScene; SetActiveScene "
             << "skips the already-active primary scene, so the panel has NO scene until the user switches "
             << "views. Pass m_MainScene to its constructor.";
    }
    // Negative control: the census must actually see the panel whose defect it was written for.
    EXPECT_TRUE( sawLandscape ) << "the census no longer finds LandscapePanel's registration";
    EXPECT_GE( checked, 3 ) << "fewer scene-following panels found than exist today; the search is broken";
}
