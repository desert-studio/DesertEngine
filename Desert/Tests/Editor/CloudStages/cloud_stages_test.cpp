// THE CLOUDS WINDOW'S RAIL: six stages, in the order the sky is built, and each one resolving to the
// subject that edits it.
//
// WHY THERE IS A WINDOW AT ALL. Task O7 went looking for a defect in the noise panel and found a wider one:
// clouds are authored in SIX places and not one of them mentions the other five. The owner picked variant B
// from the drawn sheets -- one window, a rail of the stages in build order, the selected stage's own editor
// embedded beside it. Sheet 13 of the O9 drawings wrote the mapping out as an argument; CloudStages.hpp is
// that mapping as code, and this suite is the mapping asserted:
//
//   component header -> 1 Layer          Material slot  -> 2 Material
//   Global Pattern   -> 3 Layout         Cloud Type 1-4 -> 4 Types
//   Noise Volume     -> 5 Noise          Hero body      -> 6 Hero bodies
//
// The relations that matter, and none of them is about a single function:
//
//   1. THE ORDER IS THE ARGUMENT. Read down the rail and you have read what feeds what. A rail that sorted
//      itself, or a second list of names beside the enum, would be an order that can drift from the chain.
//   2. A STAGE'S SUBJECT IS THE ONE THE EDITOR FOR THAT KIND IS REGISTERED UNDER. A stage whose subject
//      type nothing can open is a rail row that draws an empty pane for ever and logs a warning nobody
//      reads -- so it is counted, and the count is zero.
//   3. NOTHING IN THE SKY AND NOTHING IN A STAGE ARE DIFFERENT FACTS (contract §1.4). A scene with no cloud
//      layer must not read as a layer whose six slots happen to be empty.
//   4. THE MAPPING IS WHAT DETAILS USES TOO. Details can reach exactly two of the six -- the component
//      header and the Material slot -- because since O1 the layout, the types and the noise are MATERIAL
//      parameters and the component names none of them. That is a property of the split and it is asserted
//      here rather than left as a sentence in a panel.
//
// Why this lives in a header at all: CloudsPanel.cpp is one of the editor translation units no suite
// compiles (scripts/CI/UnreachedSources.sh), so a rule written there is a rule nothing can assert.

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/Clouds/CloudStages.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::AssetTypeID;
using Desert::Editor::CensusOfCloudStages;
using Desert::Editor::CloudChain;
using Desert::Editor::CloudHeroBody;
using Desert::Editor::CloudStage;
using Desert::Editor::CloudStageAssetTypes;
using Desert::Editor::CloudStageEmbedsDocument;
using Desert::Editor::CloudStageName;
using Desert::Editor::CloudStageSubject;
using Desert::Editor::CloudStageSubtitle;
using Desert::Editor::ComponentSubject;
using Desert::Editor::kCloudStageCount;
using Desert::Editor::kCloudStageTypeSlots;
using Desert::Editor::SubjectDomain;
using Desert::Editor::SubjectEditorRegistry;
using Desert::Editor::SubjectId;

namespace
{
    ::Common::AssetHandle Handle( uint64_t value )
    {
        return ::Common::AssetHandle( value );
    }

    // The asset types the editor registers its cloud documents under. THE REAL ENUMERATORS, not numbers
    // invented here: the whole point of the census is that a stage resolves to the type an editor exists
    // for, and a suite that made its own numbers up would assert nothing.
    CloudStageAssetTypes Types()
    {
        return CloudStageAssetTypes{ static_cast<uint32_t>( AssetTypeID::Material ),
                                     static_cast<uint32_t>( AssetTypeID::CloudLayout ),
                                     static_cast<uint32_t>( AssetTypeID::CloudType ),
                                     static_cast<uint32_t>( AssetTypeID::CloudNoiseVolume ),
                                     static_cast<uint32_t>( AssetTypeID::CloudModellingVolume ) };
    }

    // A fully authored sky: a layer on an entity, a material, a painted layout, one type in slot 0, the
    // noise that type names, and one hero body.
    CloudChain FullSky()
    {
        CloudChain chain;
        chain.HasLayer      = true;
        chain.LayerEntity   = ::Common::UUID( 88 );
        chain.LayerName     = "Sky";
        chain.Material      = Handle( 1001 );
        chain.LayoutPattern = Handle( 1002 );
        chain.LayoutMask    = Handle( 1003 );
        chain.CloudTypes[0] = Handle( 1004 );
        chain.NoiseVolume   = Handle( 1005 );
        chain.HeroBodies.push_back( CloudHeroBody{ ::Common::UUID( 99 ), "Hero", Handle( 1006 ) } );
        return chain;
    }

    // The editor as it is really registered: the five asset kinds that have a document, and the two
    // component kinds that do (neither of which is the cloud layer -- see the stage-1 test below).
    SubjectEditorRegistry RegistryAsShipped()
    {
        SubjectEditorRegistry registry;
        const auto            add = [&registry]( uint32_t assetType, const char* name )
        {
            registry.Register( Desert::Editor::AssetSubjectType( assetType ),
                               SubjectEditorRegistry::Registration{ name, "icon",
                                                                    []( const SubjectId& ) { return nullptr; },
                                                                    []( const SubjectId& ) { return true; } } );
        };
        add( static_cast<uint32_t>( AssetTypeID::Material ), "Material" );
        add( static_cast<uint32_t>( AssetTypeID::CloudLayout ), "CloudLayout" );
        add( static_cast<uint32_t>( AssetTypeID::CloudType ), "CloudType" );
        add( static_cast<uint32_t>( AssetTypeID::CloudNoiseVolume ), "CloudNoiseVolume" );
        add( static_cast<uint32_t>( AssetTypeID::CloudModellingVolume ), "CloudModellingVolume" );
        return registry;
    }
} // namespace

// =================================================================================================
// 1. The rail: six stages, in build order, each named
// =================================================================================================

TEST( CloudRail, ThereAreSixStagesAndTheOrderIsTheChain )
{
    ASSERT_EQ( kCloudStageCount, 6u );

    // The enumerator order IS the rail order — nothing sorts it. Spelled out here so that a reordering
    // shows up as a red test naming the two stages that swapped, rather than as a window whose rail reads
    // in an order that no longer describes what feeds what.
    EXPECT_EQ( static_cast<uint32_t>( CloudStage::Layer ), 0u );
    EXPECT_EQ( static_cast<uint32_t>( CloudStage::Material ), 1u );
    EXPECT_EQ( static_cast<uint32_t>( CloudStage::Layout ), 2u );
    EXPECT_EQ( static_cast<uint32_t>( CloudStage::Types ), 3u );
    EXPECT_EQ( static_cast<uint32_t>( CloudStage::Noise ), 4u );
    EXPECT_EQ( static_cast<uint32_t>( CloudStage::HeroBodies ), 5u );
}

TEST( CloudRail, EveryStageHasItsOwnNameAndItsOwnLine )
{
    // A name table that quietly falls behind its enum puts the wrong word in the one place whose whole job
    // is to be trusted — which is the argument SubjectDomainName and Assets::AssetTypeName both carry. The
    // switch has no `default:`, so the COMPILER catches an added stage; this catches a duplicated one,
    // which the compiler cannot.
    std::set<std::string> names;
    std::set<std::string> subtitles;
    for ( uint32_t i = 0; i < kCloudStageCount; ++i )
    {
        const auto        stage = static_cast<CloudStage>( i );
        const std::string name( CloudStageName( stage ) );
        const std::string subtitle( CloudStageSubtitle( stage ) );

        EXPECT_FALSE( name.empty() ) << "stage " << i << " has no name";
        EXPECT_FALSE( subtitle.empty() ) << "stage " << i << " has no line under its name";
        EXPECT_NE( name, "unknown" ) << "stage " << i << " fell off the end of the name table";

        EXPECT_TRUE( names.insert( name ).second ) << "two stages are called '" << name << "'";
        EXPECT_TRUE( subtitles.insert( subtitle ).second )
             << "two stages are explained with the same line: '" << subtitle << "'";
    }
}

// =================================================================================================
// 2. Which subject a stage edits
// =================================================================================================

TEST( CloudStageMapping, EachStageResolvesToTheThingSheet13Says )
{
    const CloudChain chain = FullSky();
    const auto       types = Types();

    // 1 · Layer — a COMPONENT, not an asset. This is the case task U7 made expressible at all: while a
    // subject was an AssetHandle the layer could not be named, and the window would have had to start at
    // the material.
    const SubjectId layer = CloudStageSubject( CloudStage::Layer, chain, types );
    EXPECT_EQ( layer.Domain, SubjectDomain::EntityComponent );
    EXPECT_EQ( layer, ComponentSubject( ::Common::UUID( 88 ), "VolumetricCloudComponent" ) );

    // 2..6 — assets, each of its own kind.
    const auto expectAsset = [&]( CloudStage stage, AssetTypeID type, uint64_t owner )
    {
        const SubjectId subject = CloudStageSubject( stage, chain, types );
        EXPECT_EQ( subject.Domain, SubjectDomain::Asset ) << CloudStageName( stage );
        EXPECT_EQ( subject.Facet, static_cast<uint32_t>( type ) ) << CloudStageName( stage );
        EXPECT_EQ( static_cast<uint64_t>( subject.Owner ), owner ) << CloudStageName( stage );
    };
    expectAsset( CloudStage::Material, AssetTypeID::Material, 1001 );
    expectAsset( CloudStage::Layout, AssetTypeID::CloudLayout, 1002 );
    expectAsset( CloudStage::Types, AssetTypeID::CloudType, 1004 );
    expectAsset( CloudStage::Noise, AssetTypeID::CloudNoiseVolume, 1005 );
    expectAsset( CloudStage::HeroBodies, AssetTypeID::CloudModellingVolume, 1006 );
}

TEST( CloudStageMapping, TheTypeSlotAndTheHeroIndexChooseWhichOneOfASET )
{
    // Stages 4 and 6 stand for a SET rather than for one thing: a layer has four type slots and a scene may
    // hold several hero bodies. The selection is part of what decides the subject, which is why it lives in
    // the chain rather than beside the drawing code — otherwise this could not be asserted at all.
    CloudChain chain    = FullSky();
    chain.CloudTypes[2] = Handle( 2004 );
    chain.HeroBodies.push_back( CloudHeroBody{ ::Common::UUID( 100 ), "Hero2", Handle( 2006 ) } );
    const auto types = Types();

    chain.SelectedTypeSlot = 0;
    EXPECT_EQ( static_cast<uint64_t>( CloudStageSubject( CloudStage::Types, chain, types ).Owner ), 1004u );
    chain.SelectedTypeSlot = 2;
    EXPECT_EQ( static_cast<uint64_t>( CloudStageSubject( CloudStage::Types, chain, types ).Owner ), 2004u );

    chain.SelectedHero = 0;
    EXPECT_EQ( static_cast<uint64_t>( CloudStageSubject( CloudStage::HeroBodies, chain, types ).Owner ), 1006u );
    chain.SelectedHero = 1;
    EXPECT_EQ( static_cast<uint64_t>( CloudStageSubject( CloudStage::HeroBodies, chain, types ).Owner ), 2006u );

    // Out of range is NOTHING, not the first one. A hero index left over from a scene with more bodies
    // than this one has would otherwise silently show a different cloud's body.
    chain.SelectedHero = 7;
    EXPECT_TRUE( CloudStageSubject( CloudStage::HeroBodies, chain, types ).IsNull() );
    chain.SelectedTypeSlot = kCloudStageTypeSlots;
    EXPECT_TRUE( CloudStageSubject( CloudStage::Types, chain, types ).IsNull() );
}

TEST( CloudStageMapping, NoCloudLayerEmptiesEverySTAGEAndIsItsOwnFact )
{
    // §1.4. "This scene has no cloud layer" and "this layer's slots are empty" are different things to tell
    // a person, and a window that showed six rows of "None" for the first would read as an editor that
    // failed to load something. HasLayer is the flag that keeps them apart, and every stage answers null
    // beneath it — including the hero bodies, which are gathered anyway because they sit on their own
    // entities and a body with no sky around it renders nothing.
    CloudChain chain = FullSky();
    chain.HasLayer   = false;
    const auto types = Types();

    for ( uint32_t i = 0; i < kCloudStageCount; ++i )
        EXPECT_TRUE( CloudStageSubject( static_cast<CloudStage>( i ), chain, types ).IsNull() )
             << CloudStageName( static_cast<CloudStage>( i ) ) << " named something in a scene with no clouds";
}

TEST( CloudStageMapping, AnUnauthoredSlotIsEmptyAndNotAWrongDocument )
{
    // An empty slot must not resolve to SOMETHING. AssetSubject over the null handle produces a SubjectId
    // whose IsNull is true (the null handle is "no asset"), and that is what makes "the stage offers Create
    // instead of an editor" expressible without a sentinel.
    CloudChain chain    = FullSky();
    chain.Material      = Handle( 0 );
    chain.LayoutPattern = Handle( 0 );
    chain.CloudTypes[0] = Handle( 0 );
    chain.NoiseVolume   = Handle( 0 );
    const auto types    = Types();

    EXPECT_TRUE( CloudStageSubject( CloudStage::Material, chain, types ).IsNull() );
    EXPECT_TRUE( CloudStageSubject( CloudStage::Layout, chain, types ).IsNull() );
    EXPECT_TRUE( CloudStageSubject( CloudStage::Types, chain, types ).IsNull() );
    EXPECT_TRUE( CloudStageSubject( CloudStage::Noise, chain, types ).IsNull() );

    // The layer itself is still there: an unauthored material does not remove the component.
    EXPECT_FALSE( CloudStageSubject( CloudStage::Layer, chain, types ).IsNull() );
}

TEST( CloudStageMapping, EveryStageResolvesToADIFFERENTSubject )
{
    // The window's whole promise is that the six rows are six different things. Two stages landing on one
    // subject would make the rail's second row a duplicate of the first and — because a document's ImGui
    // window id is its subject — would merge two panes into one.
    const CloudChain       chain = FullSky();
    const auto             types = Types();
    std::vector<SubjectId> seen;
    for ( uint32_t i = 0; i < kCloudStageCount; ++i )
    {
        const SubjectId subject = CloudStageSubject( static_cast<CloudStage>( i ), chain, types );
        ASSERT_FALSE( subject.IsNull() );
        for ( const SubjectId& previous : seen )
            EXPECT_FALSE( previous == subject )
                 << "two stages resolve to the same subject: " << subject.ToString();
        seen.push_back( subject );
    }
}

// =================================================================================================
// 3. Stage 1 is a component, and that is why it does not embed a document
// =================================================================================================

TEST( CloudStageMapping, OnlyTheLayerDrawsSomethingOtherThanADocument )
{
    EXPECT_FALSE( CloudStageEmbedsDocument( CloudStage::Layer ) );
    for ( uint32_t i = 1; i < kCloudStageCount; ++i )
        EXPECT_TRUE( CloudStageEmbedsDocument( static_cast<CloudStage>( i ) ) )
             << CloudStageName( static_cast<CloudStage>( i ) ) << " must embed the document that edits it";
}

TEST( CloudStageMapping, NothingIsRegisteredToOpenTheLayerAndThatIsWhyStage1IsDifferent )
{
    // The reason stage 1 draws the component's own registered editor instead of embedding a document,
    // stated as a fact about the registry rather than as a sentence in a panel. If somebody later DOES
    // register a document over VolumetricCloudComponent, this goes red and the window should be changed
    // to embed it — which is the conversation the red test is for.
    const SubjectEditorRegistry registry = RegistryAsShipped();
    const CloudChain            chain    = FullSky();

    const SubjectId layer = CloudStageSubject( CloudStage::Layer, chain, Types() );
    EXPECT_FALSE( registry.HasEditorFor( layer.Type() ) );
}

// =================================================================================================
// 4. The census: no rail row is a dead end
// =================================================================================================

TEST( CloudStageCensusRelation, EveryFilledStageThatEmbedsADocumentCanBeOpened )
{
    const SubjectEditorRegistry registry = RegistryAsShipped();
    const CloudChain            chain    = FullSky();

    const auto census = CensusOfCloudStages( chain, Types(), [&registry]( const auto& type )
                                             { return registry.HasEditorFor( type ); } );

    EXPECT_EQ( census.Stages, kCloudStageCount );
    EXPECT_EQ( census.Filled, kCloudStageCount );
    EXPECT_EQ( census.Empty, 0u );
    EXPECT_EQ( census.Unopenable, 0u );
    EXPECT_TRUE( census.EveryFilledStageCanBeOpened() );
}

TEST( CloudStageCensusRelation, AStageWhoseEditorIsMissingIsCaughtRatherThanDrawnBlank )
{
    // The census going RED, on purpose. Without this the green above proves only that the shipped
    // arrangement passes; what has to be shown is that a stage pointing at a kind nothing opens is
    // CAUGHT — because the symptom otherwise is a pane that says "Opening..." for ever.
    SubjectEditorRegistry registry = RegistryAsShipped();
    // ...as it would be if the cloud layout editor were never registered. Built by leaving it out rather
    // than by removing it, because the registry has no unregister and should not grow one.
    SubjectEditorRegistry withoutLayout;
    for ( const auto& type : registry.RegisteredTypes() )
    {
        if ( type.Facet == static_cast<uint32_t>( AssetTypeID::CloudLayout ) )
            continue;
        withoutLayout.Register( type,
                                SubjectEditorRegistry::Registration{ registry.TypeName( type ), "icon",
                                                                     []( const SubjectId& ) { return nullptr; },
                                                                     []( const SubjectId& ) { return true; } } );
    }

    const auto census = CensusOfCloudStages( FullSky(), Types(), [&withoutLayout]( const auto& type )
                                             { return withoutLayout.HasEditorFor( type ); } );

    EXPECT_EQ( census.Unopenable, 1u );
    EXPECT_FALSE( census.EveryFilledStageCanBeOpened() );
}

TEST( CloudStageCensusRelation, AnUnauthoredSkyIsEmptyStagesAndNotUnopenableOnes )
{
    // "Nothing here yet" is not a failure and must not be counted as one: the window offers Create there,
    // which is the Light Mixer's own move. Counting the two apart is what lets a red Unopenable mean
    // something.
    CloudChain chain    = FullSky();
    chain.Material      = Handle( 0 );
    chain.LayoutPattern = Handle( 0 );
    chain.CloudTypes[0] = Handle( 0 );
    chain.NoiseVolume   = Handle( 0 );
    chain.HeroBodies.clear();

    const SubjectEditorRegistry registry = RegistryAsShipped();
    const auto                  census   = CensusOfCloudStages( chain, Types(), [&registry]( const auto& type )
                                                                { return registry.HasEditorFor( type ); } );

    EXPECT_EQ( census.Filled, 1u ) << "only the layer itself is there";
    EXPECT_EQ( census.Empty, kCloudStageCount - 1u );
    EXPECT_EQ( census.Unopenable, 0u );
}

// =================================================================================================
// 5. The rail's slot count is the engine's
// =================================================================================================

TEST( CloudStagesAgreeWithTheEngine, TheRailAndTheLayerCountTheSameTypeSlots )
{
    // CloudStages.hpp writes out 4 rather than including Engine/ECS, so that a header the whole editor
    // reaches does not drag the ECS in. That makes it a MIRROR, and a mirror is guarded rather than
    // trusted: the day the layer grows a fifth slot, this reddens instead of the rail quietly showing four
    // of five.
    EXPECT_EQ( kCloudStageTypeSlots, static_cast<uint32_t>( Desert::ECS::kCloudTypeSlots ) );
}

// =================================================================================================
// 6. NOBODY RE-DERIVES A CLOUD TYPE'S NOISE VOLUME — a census over the SOURCE TEXT
// =================================================================================================
//
// WHY A TEXT CENSUS AND NOT AN ASSERTION. The rule is about a call that must not be written, in a file no
// suite compiles (CloudsPanel.cpp), against a registry no suite here has. Every part of that is outside
// what a linked test can reach, and the rule still has to be kept — the same trade
// Desert/Tests/Engine/GpuWriteCensus makes, with the same reader (comments and string literals are
// blanked first, so this file's own prose cannot satisfy or violate its own rule).
//
// THE RULE: a `.decloudtype` stores its noise volume as a path RELATIVE TO THE ASSETS ROOT, and exactly
// one place in the engine joins it to that root — `Assets::CloudTypeAsset::ResolveDependencies`, which
// binds the handle and reports by name, with both spellings, when the file is not registered. Every other
// reader asks the asset (`CloudTypeAsset::GetNoiseVolume`) or the service that caches its answer
// (`CloudTypeService::GetNoiseVolume`).
//
// WHAT IT COST WHEN IT WAS BROKEN, and why "the registry deduplicates spellings" does not cover it. The
// Clouds window handed the stored spelling to `AssetManager::FindByPath` verbatim. `AssetKey` does reduce
// every spelling of one file to one key — but it resolves a RELATIVE spelling against the WORKING
// DIRECTORY, not against the assets root, so `Clouds/CloudNoise_FineWisp.dcnv` minted the untagged key
// `Clouds/CloudNoise_FineWisp.dcnv` while the preloader had registered the file as
// `assets:Clouds/CloudNoise_FineWisp.dcnv`. The lookup missed for the one shipped type that names a
// volume, and stage 5 said "the built-in volume" about a sky that was not using it. The mechanism is
// pinned in Desert/Tests/Engine/AssetPathIdentity; this is the gate that stops it being written again.
namespace
{
    namespace fs = std::filesystem;

    std::string RepoRootForCensus()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Panels/Clouds/CloudsPanel.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAllForCensus( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every source the engine, the editor and the runtime compile. Not narrowed to the cloud directories:
    // the rule is about a call anybody can write, and the site that broke it was in the editor while the
    // format it misread is in the engine.
    std::vector<fs::path> CensusSources( const std::string& root )
    {
        std::vector<fs::path> out;
        for ( const char* tree :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
        {
            std::error_code ec;
            for ( auto it = fs::recursive_directory_iterator( fs::path( root ) / tree, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.string().find( "lightweightvk" ) != std::string::npos )
                    continue;
                if ( p.extension() == ".cpp" || p.extension() == ".hpp" )
                    out.push_back( p );
            }
        }
        return out;
    }

    struct Finding
    {
        std::string File;
        std::size_t Line;
        std::string What;
    };

    std::size_t LineOfOffset( const std::string& text, std::size_t at )
    {
        return 1u + static_cast<std::size_t>( std::count( text.begin(), text.begin() + at, '\n' ) );
    }

    // The template argument list of one call, `<...>`, or empty when there is none. Read separately from
    // the value arguments because WHAT is being looked up lives here and WITH WHAT lives there.
    std::string TemplateArgumentsAt( const std::string& src, std::size_t nameEnd )
    {
        std::size_t i = Desert::Tests::ConsumerText::SkipSpace( src, nameEnd );
        if ( i >= src.size() || src[i] != '<' )
            return {};

        const std::size_t begin = i;
        int               angle = 0;
        for ( ; i < src.size(); ++i )
        {
            if ( src[i] == '<' )
                ++angle;
            else if ( src[i] == '>' && --angle == 0 )
                return src.substr( begin, i - begin + 1 );
        }
        return {};
    }

    // The argument text of one call, from the '(' after the name to its matching ')'. Nesting is counted
    // so a call inside the argument list does not end it early.
    std::string ArgumentsAt( const std::string& src, std::size_t nameEnd )
    {
        std::size_t i = Desert::Tests::ConsumerText::SkipSpace( src, nameEnd );
        // A template argument list may sit between the name and the '(' — FindByPath<T>( path ).
        if ( i < src.size() && src[i] == '<' )
        {
            int angle = 0;
            for ( ; i < src.size(); ++i )
            {
                if ( src[i] == '<' )
                    ++angle;
                else if ( src[i] == '>' && --angle == 0 )
                {
                    ++i;
                    break;
                }
            }
            i = Desert::Tests::ConsumerText::SkipSpace( src, i );
        }
        if ( i >= src.size() || src[i] != '(' )
            return {};

        int         depth = 0;
        std::size_t begin = i;
        for ( ; i < src.size(); ++i )
        {
            if ( src[i] == '(' )
                ++depth;
            else if ( src[i] == ')' && --depth == 0 )
                return src.substr( begin, i - begin + 1 );
        }
        return {};
    }
} // namespace

TEST( CloudStagesCensus, OnlyTheFormatsOwnReaderLooksUpANoiseVolumeByPath )
{
    const std::string root = RepoRootForCensus();
    ASSERT_FALSE( root.empty() ) << "the census could not find the repository from the working directory";

    // NAMED ROWS, NOT A COUNT. A gate that pinned "there are two such calls" is satisfied by editing the
    // two; what is pinned is WHICH file may make one and WHY, so a third site has to be argued for here
    // before it can be written. The number examined is printed beside it so that a census which quietly
    // stopped looking is visible.
    struct AllowedSite
    {
        const char* File;
        const char* Why;
    };
    // CloudTypeAsset left this register in T7h: a type names its volume by GUID (CLTY 5) and resolves it by
    // handle, so no stored path is joined to a root any more.
    const AllowedSite kAllowed[] = {
         { "Editor/Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp",
           "re-registers a file it has just WRITTEN, by the absolute path it wrote to — not a stored "
           "reference, so no root is being guessed at" },
    };

    std::vector<Finding> findings;
    std::size_t          callsExamined = 0;
    std::size_t          volumeLookups = 0;

    for ( const fs::path& path : CensusSources( root ) )
    {
        const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAllForCensus( path ) );
        const std::string generic = fs::path( path ).generic_string();

        for ( const std::size_t at : Desert::Tests::ConsumerText::WordPositions( src, "FindByPath" ) )
        {
            const std::size_t nameEnd = at + std::string( "FindByPath" ).size();
            const std::string call    = TemplateArgumentsAt( src, nameEnd );
            const std::string args    = ArgumentsAt( src, nameEnd );
            if ( args.empty() )
                continue;
            ++callsExamined;

            if ( call.find( "CloudNoiseVolumeAsset" ) == std::string::npos )
                continue;
            ++volumeLookups;

            bool allowed = false;
            for ( const AllowedSite& row : kAllowed )
            {
                const std::string suffix( row.File );
                if ( generic.size() >= suffix.size() &&
                     generic.compare( generic.size() - suffix.size(), suffix.size(), suffix ) == 0 )
                {
                    allowed = true;
                    break;
                }
            }
            if ( allowed )
                continue;

            findings.push_back( { generic, LineOfOffset( src, at ),
                                  "a cloud noise volume is looked up by path outside the register above: "
                                  "FindByPath" +
                                       call + args } );
        }
    }

    EXPECT_GT( callsExamined, 0u ) << "the census found no FindByPath call at all — it has stopped looking";
    EXPECT_EQ( volumeLookups, std::size( kAllowed ) )
         << "the register names " << std::size( kAllowed ) << " sites and the tree has " << volumeLookups
         << ": a row whose site is gone is a row that no longer defends anything";
    std::cout << "[ CENSUS   ] FindByPath calls examined: " << callsExamined
              << ", noise-volume lookups: " << volumeLookups << ", outside the register: " << findings.size()
              << std::endl;

    for ( const Finding& f : findings )
        ADD_FAILURE() << f.File << ":" << f.Line << " — " << f.What;
    EXPECT_TRUE( findings.empty() );
}

// THE DEFECT'S OWN SHAPE, and the reason the register above is not enough on its own. The site this gate
// was written for held the stored path in a local called `path`, so nothing about the CALL said where the
// string had come from — only the line above it did. What names it unambiguously is the READ: a cloud
// type's data is where the stored, root-relative spelling lives, and nothing outside the asset itself has
// any business taking it out of there. The asset has already resolved it; `GetNoiseVolume` is the answer.
TEST( CloudStagesCensus, NobodyTakesTheStoredPathOutOfACloudTypesData )
{
    const std::string root = RepoRootForCensus();
    ASSERT_FALSE( root.empty() );

    std::vector<Finding> findings;
    std::size_t          dataReads = 0;

    for ( const fs::path& path : CensusSources( root ) )
    {
        const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAllForCensus( path ) );

        for ( const std::size_t at : Desert::Tests::ConsumerText::WordPositions( src, "GetData" ) )
        {
            std::size_t i = Desert::Tests::ConsumerText::SkipSpace( src, at + 7 );
            if ( i >= src.size() || src[i] != '(' )
                continue;
            i = Desert::Tests::ConsumerText::SkipSpace( src, i + 1 );
            if ( i >= src.size() || src[i] != ')' )
                continue;
            ++dataReads;
            i = Desert::Tests::ConsumerText::SkipSpace( src, i + 1 );
            if ( i < src.size() && src[i] == '.' &&
                 Desert::Tests::ConsumerText::WordAt( src, i + 1, "NoiseVolume" ) )
                findings.push_back( { fs::path( path ).generic_string(), LineOfOffset( src, at ),
                                      "the stored, root-relative noise-volume path is read out of a cloud "
                                      "type's data; ask the asset for the handle it bound instead" } );
        }
    }

    EXPECT_GT( dataReads, 0u ) << "the census found no GetData() call at all — it has stopped looking";
    std::cout << "[ CENSUS   ] GetData() reads examined: " << dataReads
              << ", noise-volume paths taken out: " << findings.size() << std::endl;

    for ( const Finding& f : findings )
        ADD_FAILURE() << f.File << ":" << f.Line << " — " << f.What;
    EXPECT_TRUE( findings.empty() );
}

// The positive half. A register that is empty because nobody resolves the volume AT ALL would also be
// empty, and the window would then draw an empty stage 5 for ever — which is the failure §1.4 is about.
TEST( CloudStagesCensus, TheCloudsWindowReadsTheHandleTheTypeAlreadyBound )
{
    const std::string root = RepoRootForCensus();
    ASSERT_FALSE( root.empty() );

    const std::string panel = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAllForCensus( fs::path( root ) / "Editor/Source/Editor/Panels/Clouds/CloudsPanel.cpp" ) );
    ASSERT_FALSE( panel.empty() );

    EXPECT_FALSE( Desert::Tests::ConsumerText::WordPositions( panel, "GetNoiseVolume" ).empty() )
         << "CloudsPanel.cpp no longer reads the handle CloudTypeAsset resolved, so stage 5 of the rail "
            "resolves to nothing whatever the type names";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
