// EVERY COMPONENT FIELD THAT NAMES AN ASSET MUST BE VISITED BY THE EVICTION ROOT WALK.
//
// This is the relation that keeps asset eviction from deleting things the world is still using, and it is
// the one part of the feature the compiler cannot help with. `Core::CollectAssetRoots` walks a scene's
// registry and marks the handles its components name; a component type added next month whose field nobody
// adds to that walk is not a compile error, not a warning, and not a log line. It is an asset released
// while an entity is drawing it — and because both services rebuild on a miss, even THAT is silent: the
// mesh reappears a frame later, having been read from disk again, and the only symptom is work.
//
// So the census reads the two files and asserts they agree. `Components.hpp` is the source of truth for
// what exists; `SceneAssetRoots.cpp` is the source of truth for what is visited. Neither is a hand-kept
// list, which is the whole point: a list somebody maintains is a list that drifts, and this project has
// paid for that shape — a middle link dropping a property — seven times in one day.
//
// WHAT IT MATCHES ON, AND WHY IT IS DELIBERATELY COARSE. For each `Assets::AssetHandle` member (and each
// `std::vector<Assets::AssetHandle>`), it takes the FIELD NAME and requires the root walk to mention it.
// It does not try to prove the read is anchored to the right receiver — `SettingConsumers` does that with
// a real parser and needs one because a field name there is shared by several components. Here a miss is
// not subtle: a field the walk never names cannot possibly be marked, and a field it names is being read
// for no other reason. The failure this closes is OMISSION, and omission is exactly what a name match
// sees.
//
// THE HANDLE'S OTHER SPELLING IS COUNTED TOO. `MaterialTextureOverride::TextureHandle` is a `uint64_t`
// carrying an AssetHandle — the comment beside it says so — so a search for the TYPE alone would miss the
// one component field that overrides a texture per entity. It is named explicitly below rather than
// guessed at, and the row says why.

#include <gtest/gtest.h>

#include <array>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // Walks up from the working directory looking for a file only the repository has. Same shape as
    // AssetReferenceCensus and MaterialIdentity, which need it for the same reason: the test runner's
    // working directory is not fixed. (The suites share no header; copy-paste is this directory's
    // convention.)
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadWholeFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    bool IsIdentChar( const char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
    }

    // Every `Assets::AssetHandle <Name>` and `std::vector<Assets::AssetHandle> <Name>` declaration, by
    // field name. Declarations only: a use site is `something.Name`, which never has the type in front of
    // it, so this cannot pick up the root walk's own reads by accident.
    std::set<std::string> DeclaredHandleFields( const std::string& source )
    {
        std::set<std::string> fields;

        for ( const std::string& pattern :
              { std::string( "std::vector<Assets::AssetHandle>" ), std::string( "Assets::AssetHandle" ) } )
        {
            std::size_t at = 0;
            while ( ( at = source.find( pattern, at ) ) != std::string::npos )
            {
                std::size_t cursor = at + pattern.size();

                // `std::vector<Assets::AssetHandle>` also contains `Assets::AssetHandle`, and the vector
                // pass runs first — but the plain pass would then find the inner occurrence and read the
                // `>` as the start of a name. Skipping a `>` here is what makes the two passes agree.
                if ( cursor < source.size() && source[cursor] == '>' )
                    ++cursor;

                while ( cursor < source.size() && ( source[cursor] == ' ' || source[cursor] == '\t' ) )
                    ++cursor;

                const std::size_t nameStart = cursor;
                while ( cursor < source.size() && IsIdentChar( source[cursor] ) )
                    ++cursor;

                if ( cursor > nameStart )
                    fields.insert( source.substr( nameStart, cursor - nameStart ) );

                at += pattern.size();
            }
        }

        return fields;
    }
} // namespace

TEST( AssetRootsCensus, EveryComponentFieldThatNamesAnAssetIsVisitedByTheRootWalk )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::string components = ReadWholeFile( root + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
    const std::string walk       = ReadWholeFile( root + "Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp" );

    ASSERT_FALSE( components.empty() ) << "Components.hpp could not be read; the census would pass vacuously";
    ASSERT_FALSE( walk.empty() ) << "SceneAssetRoots.cpp could not be read; the census would pass vacuously";

    const std::set<std::string> declared = DeclaredHandleFields( components );

    // A floor on what the scan found. Without it, a change that broke the SCANNER — a renamed type, a
    // reformatted declaration — would empty this set and the census would certify everything while
    // reading nothing. Д33 rebuilt a whole consumer reader after exactly that failure.
    ASSERT_GE( declared.size(), 10u )
         << "the scan found only " << declared.size()
         << " asset-handle field(s) in Components.hpp. That is not a component census, it is a broken "
            "scanner: the type's spelling or the declaration's shape has changed.";

    std::vector<std::string> unvisited;
    for ( const std::string& field : declared )
    {
        if ( walk.find( field ) == std::string::npos )
            unvisited.push_back( field );
    }

    EXPECT_TRUE( unvisited.empty() )
         << "these component fields name an asset and the eviction root walk never mentions them, so "
            "nothing marks what they point at and eviction will release it while an entity is drawing "
            "it: "
         << [&unvisited]
    {
        std::string list;
        for ( const std::string& field : unvisited )
            list += ( list.empty() ? "" : ", " ) + field;
        return list;
    }() << ". Add them to Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp.";
}

// THE FIELD NAME IS NOT ENOUGH: `Material` is visited for a landscape, and that one spelling hid for a whole
// programme that a cloud layer's `Data.Material` was never marked — eviction released it after every sweep and the
// renderer parsed it again inside a frame (AL1-8b). One row per component that names a material it draws with.
TEST( AssetRootsCensus, EveryComponentThatDrawsWithAMaterialIsVisitedByName )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";
    const std::string walk = ReadWholeFile( root + "Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp" );
    ASSERT_FALSE( walk.empty() ) << "SceneAssetRoots.cpp could not be read; the census would pass vacuously";

    const std::array<std::pair<const char*, const char*>, 5> components{ {
         { "StaticMeshComponent", "its material slots" },
         { "SkinnedMeshComponent", "its material slots" },
         { "InstancedStaticMeshComponent", "its material slots" },
         { "LandscapeMaterialComponent", "the landscape's surface material" },
         { "VolumetricCloudComponent", "the medium the cloud layer marches, read every frame" },
    } };
    for ( const auto& [component, why] : components )
        EXPECT_NE( walk.find( std::string( "ECS::" ) + component + ">" ), std::string::npos )
             << component << " is not visited by the eviction root walk, so " << why
             << " is released while it is drawn. Add it to Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp.";
}

TEST( AssetRootsCensus, TheHandleSpeltAsAPlainIntegerIsVisitedToo )
{
    // `MaterialTextureOverride::TextureHandle` is a `uint64_t` holding an AssetHandle — its own comment
    // says "Assets::AssetHandle as uint64". A census that matched on the TYPE would not see it, and it is
    // the field behind every per-entity texture override, so missing it means an overridden texture is
    // released while the entity that overrides it is on screen.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string components = ReadWholeFile( root + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
    const std::string walk       = ReadWholeFile( root + "Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp" );
    ASSERT_FALSE( components.empty() );
    ASSERT_FALSE( walk.empty() );

    ASSERT_NE( components.find( "struct MaterialTextureOverride" ), std::string::npos )
         << "MaterialTextureOverride is gone; this row is stale and should be removed with it";

    EXPECT_NE( walk.find( "MaterialComponent" ), std::string::npos )
         << "the root walk no longer visits MaterialComponent, whose Textures carry asset handles spelt "
            "as plain uint64";
    EXPECT_NE( walk.find( "TextureHandle" ), std::string::npos )
         << "the root walk no longer reads MaterialTextureOverride::TextureHandle";
}

TEST( AssetRootsCensus, TheSceneSettingsFieldThatNamesAnAssetIsVisitedToo )
{
    // The other root source: the scene's own settings. Two fields today (`SplashSprite`, `DefaultPawn`), and the same
    // omission risk — it is not a component, so the component loop above cannot see it.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string settings = ReadWholeFile( root + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
    const std::string walk     = ReadWholeFile( root + "Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp" );
    ASSERT_FALSE( settings.empty() );
    ASSERT_FALSE( walk.empty() );

    const std::set<std::string> declared = DeclaredHandleFields( settings );
    ASSERT_FALSE( declared.empty() ) << "SceneSettings declares no asset handle at all; either the scan "
                                        "broke or the field moved, and both need reading";

    // The READ, not the name: the walk's comment about DefaultPawn names Core::SpawnDefaultPawn, and a bare
    // name search passed with the Mark line deleted (SPAWN1 mutation) - Play then found a released prefab.
    for ( const std::string& field : declared )
    {
        EXPECT_NE( walk.find( "GetSettings()." + field ), std::string::npos )
             << "SceneSettings::" << field << " names an asset and the eviction root walk never reads it";
    }
}

TEST( AssetEvictionCensus, EveryAssetClassThatNamesAnotherAssetIsExpandedByTheTrace )
{
    // The edges, from the other side. A root set only names what a COMPONENT points at; everything an
    // asset's own FILE points at is reached by AssetEviction::Expand, and an edge missing there releases
    // an asset that is genuinely in use — a material's texture, a cloud type's noise volume, a skinned
    // mesh's rig.
    //
    // Matched on the CLASS name rather than on the field, because the trace reaches these through typed
    // lookups (`FindByHandle<SurfaceMaterialAsset>`), which is where a missing edge would show as a
    // missing class.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string expand = ReadWholeFile( root + "Desert/Desert/Source/Engine/Assets/AssetEviction.cpp" );
    ASSERT_FALSE( expand.empty() );

    struct Edge
    {
        const char* Class;
        const char* Why;
    };

    // Every asset class in Engine/Assets that carries a handle to ANOTHER asset. Adding a fourth is what
    // this row list is for; the reasons are here so a future reader knows what a failure costs.
    constexpr Edge kEdges[] = {
         { "MeshAsset", "a mesh names one material per submesh; losing the edge draws it in the default grey" },
         { "SkinnedMeshAsset", "a skinned mesh names its skeleton; losing the edge un-rigs the character" },
         { "SurfaceMaterialAsset",
           "a material names its textures AND its cloud types and layouts, all by number and by nothing "
           "else; losing the edge draws the surface untextured with nothing in the log" },
         { "CloudTypeAsset", "a cloud type names the noise volume its edge is cut from" },
    };

    for ( const Edge& edge : kEdges )
    {
        EXPECT_NE( expand.find( edge.Class ), std::string::npos )
             << "AssetEviction::Expand no longer follows " << edge.Class << ": " << edge.Why;
    }
}

TEST( AssetUnloadCensus, EveryUnloadBodyEitherClearsItsReadinessOrRefuses )
{
    // THE CONTRACT OVER ALL THIRTEEN, INCLUDING THE ONE THE RUNTIME SUITE CANNOT LINK.
    //
    // Desert/Tests/Engine/AssetEviction constructs twelve of the thirteen and asserts the property
    // directly, which is stronger. PrefabAsset is the thirteenth: its CreateFromEntity reaches ECS::Entity
    // and the scene serializer, so linking it would drag the whole world layer into a suite whose point is
    // to run without one. It is held here instead, and so is every other body, so that the SET is complete
    // in one place - a fourteenth asset type is caught by this row list going stale rather than by
    // somebody remembering.
    //
    // What is asserted is the shape, not the behaviour: a body must contain either an assignment that
    // clears the type's readiness or a refusal that returns an error. Six of the thirteen used to be
    // `return BOOLSUCCESS;` and nothing else, and the shape is exactly what distinguishes those.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    struct Body
    {
        const char* File;
        const char* Class;
    };

    constexpr Body kBodies[] = {
         { "Desert/Desert/Source/Engine/Assets/TextureAsset.cpp", "TextureAsset" },
         { "Desert/Desert/Source/Engine/Assets/Shader/ShaderAsset.cpp", "ShaderAsset" },
         { "Desert/Desert/Source/Engine/Assets/Skybox/SkyboxAsset.cpp", "SkyboxAsset" },
         { "Desert/Desert/Source/Engine/Assets/Prefab/PrefabAsset.cpp", "PrefabAsset" },
         { "Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp", "StaticMeshAsset" },
         { "Desert/Desert/Source/Engine/Assets/Mesh/SkinnedMeshAsset.cpp", "SkinnedMeshAsset" },
         { "Desert/Desert/Source/Engine/Assets/Mesh/SkeletonAsset.cpp", "SkeletonAsset" },
         { "Desert/Desert/Source/Engine/Assets/Mesh/AnimationAsset.cpp", "AnimationAsset" },
         { "Desert/Desert/Source/Engine/Assets/Mesh/SurfaceMaterialAsset.cpp", "SurfaceMaterialAsset" },
         { "Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeAsset.cpp", "CloudNoiseVolumeAsset" },
         { "Desert/Desert/Source/Engine/Assets/CloudTypeAsset.cpp", "CloudTypeAsset" },
         { "Desert/Desert/Source/Engine/Assets/CloudModellingVolumeAsset.cpp", "CloudModellingVolumeAsset" },
         { "Desert/Desert/Source/Engine/Assets/CloudLayoutAsset.cpp", "CloudLayoutAsset" },
    };

    // The number is the finding this task started from and is worth pinning: thirteen implementations,
    // and until now zero callers.
    static_assert( sizeof( kBodies ) / sizeof( kBodies[0] ) == 13,
                   "the asset layer has gained or lost an Unload implementation; add it to this list" );

    for ( const Body& body : kBodies )
    {
        const std::string source = ReadWholeFile( root + body.File );
        ASSERT_FALSE( source.empty() ) << body.File << " could not be read";

        const std::string signature = std::string( body.Class ) + "::Unload()";
        const std::size_t at        = source.find( signature );
        ASSERT_NE( at, std::string::npos ) << body.Class << " no longer implements Unload";

        // From the signature to the closing brace of the function: the next line that is exactly four
        // spaces and a brace, i.e. namespace-member indentation. Coarse, and sufficient - every body in
        // this directory is formatted that way, and a miss makes the window LONGER, never shorter.
        const std::size_t end = source.find( "\n    }", at );
        ASSERT_NE( end, std::string::npos ) << body.Class << "::Unload has no recognisable end";
        const std::string bodyText = source.substr( at, end - at );

        const bool clearsReadiness =
             bodyText.find( "= false" ) != std::string::npos || bodyText.find( ".reset()" ) != std::string::npos;
        const bool refuses = bodyText.find( "MakeFormattedError" ) != std::string::npos ||
                             bodyText.find( "MakeError" ) != std::string::npos;

        EXPECT_TRUE( clearsReadiness || refuses )
             << body.Class
             << "::Unload neither clears the asset's readiness nor refuses. A body that returns success "
                "while leaving IsReadyForUse() true empties the asset AND makes it unreloadable: "
                "EnsureLoaded short-circuits on that flag, so every consumer downstream gets a valid "
                "object holding nothing, for ever. Six of the thirteen were `return BOOLSUCCESS;` and "
                "nothing else before this task.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
