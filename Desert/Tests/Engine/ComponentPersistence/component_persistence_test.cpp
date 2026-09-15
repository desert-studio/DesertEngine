// THE CENSUS OF WHAT SURVIVES A RELOAD, and the defect it exists for.
//
// VisibilityComponent had no entry in ComponentRegistry.cpp. The outliner's eye therefore worked
// perfectly for exactly as long as the process lived: hide an object, save, reopen the scene, and it is
// visible again — with no error, no warning and no mark. The user's own edit was discarded by a table
// they cannot see. The lock one row up in the same panel had the identical defect and it was found by
// someone fixing the lock, not by any check.
//
// So "is this component persisted?" stops being something a reader has to notice. Every
// `<Something>Component` in the ECS headers must fall into exactly one of three buckets, and the third
// is WRITTEN DOWN HERE WITH A REASON:
//
//   1. registered in ComponentRegistry.cpp        — the ordinary answer;
//   2. written by EntitySerializer.cpp            — the entity RECORD's own fields (id, parent, Tag,
//                                                   Prefab, Transform), which are not component blocks;
//   3. kTransient                                 — deliberately not persisted, with the reason.
//
// A component in none of them fails this suite, which is the whole point — adding a component is the
// moment the decision is cheap, and it is the only moment anybody is thinking about it.
//
// THERE USED TO BE A FOURTH BUCKET AND IT IS GONE (U13). `kOwed` held four components that were
// authored in Details and thrown away on load — the census that fixed VisibilityComponent found them,
// named them, and left them. Naming a gap is not closing one: for four more weeks an artist's foliage
// density, locomotion clips, blendshape weights and socket offsets went on being discarded silently,
// with the defect written down in a file they will never open. Worse, the bucket made the census
// SATISFIABLE BY TYPING: the honest way to pass it with a fifth broken component was to add a fifth
// line here. So the escape is removed, and the test below closes the door it was holding open —
// membership of "authored in Details" is DERIVED from the editor's own registration source, and a
// component in that set has no bucket to fall into except the first.
//
// NOTHING IS LINKED FROM THE ENGINE. The question is "does this table name that type", a relation
// between two files, so both are read as TEXT (the same argument ComponentPools makes next door). It
// needs no GPU, no scene and no asset manager, and it cannot go stale against a build.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // The repository root, found by walking up from wherever the binary was started — the same approach
    // ComponentPools and SettingConsumers use, so none of them has to be run from one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
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

    // Line comments removed. Without this a component NAMED in prose — and these headers explain
    // themselves at length — is counted as declared, and a registration that was commented out to see
    // what breaks is counted as present.
    std::string WithoutLineComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        std::size_t at = 0;
        while ( at < source.size() )
        {
            const std::size_t line = source.find( '\n', at );
            const std::size_t end  = line == std::string::npos ? source.size() : line;
            std::string       text = source.substr( at, end - at );
            if ( const auto comment = text.find( "//" ); comment != std::string::npos )
                text = text.substr( 0, comment );
            out += text;
            out += '\n';
            at = end == source.size() ? end : end + 1;
        }
        return out;
    }

    // CONTAINS, not ENDS WITH, and the difference is one real component. `UITextComponent2D` is an ECS
    // component like any other and an `EndsWith` test could not see it at all — it was neither counted
    // among the declared components nor checked against the table, so had it been the one nobody
    // registered, this census would have reported everything fine. It happens to be registered; the
    // hole was in the census, not in the tree, and a census with a hole in it is the more expensive of
    // the two because it reads as an answer.
    bool NamesAComponent( const std::string& name )
    {
        static const std::string token = "Component";
        const std::size_t        at    = name.find( token );
        return at != std::string::npos && at > 0;
    }

    std::string Identifier( const std::string& source, std::size_t& cursor )
    {
        while ( cursor < source.size() && std::isspace( static_cast<unsigned char>( source[cursor] ) ) )
            ++cursor;
        const std::size_t start = cursor;
        while ( cursor < source.size() &&
                ( std::isalnum( static_cast<unsigned char>( source[cursor] ) ) || source[cursor] == '_' ) )
            ++cursor;
        return source.substr( start, cursor - start );
    }

    // Every `struct <Something>Component` DECLARED in @p source, with the byte offset of its name so the
    // body can be read back (the marker check below needs it).
    std::map<std::string, std::size_t> DeclaredComponents( const std::string& source )
    {
        static const std::string keyword = "struct ";

        std::map<std::string, std::size_t> found;
        for ( std::size_t at = source.find( keyword ); at != std::string::npos;
              at             = source.find( keyword, at + 1 ) )
        {
            std::size_t       cursor = at + keyword.size();
            const std::string name   = Identifier( source, cursor );
            if ( !NamesAComponent( name ) )
                continue;

            // A forward declaration (`struct XComponent;`) declares no fields and stores nothing.
            std::size_t probe = cursor;
            while ( probe < source.size() && std::isspace( static_cast<unsigned char>( source[probe] ) ) )
                ++probe;
            if ( probe >= source.size() || source[probe] != '{' )
                continue;

            found[name] = probe;
        }
        return found;
    }

    // Every `ECS::<Something>Component` named anywhere in @p source.
    std::set<std::string> ComponentsNamedIn( const std::string& source )
    {
        static const std::string qualifier = "ECS::";

        std::set<std::string> found;
        for ( std::size_t at = source.find( qualifier ); at != std::string::npos;
              at             = source.find( qualifier, at + 1 ) )
        {
            std::size_t       cursor = at + qualifier.size();
            const std::string name   = Identifier( source, cursor );
            if ( NamesAComponent( name ) )
                found.insert( name );
        }
        return found;
    }

    // The headers that declare components. Listed rather than globbed: a new file here is a decision, and
    // this suite failing to know about it is exactly the silence it exists to remove — the four
    // sky/cloud components already live outside Components.hpp.
    const std::vector<std::string> kComponentHeaders = {
         "Desert/Desert/Source/Engine/ECS/Components.hpp",
         "Desert/Desert/Source/Engine/ECS/SkyAtmosphereComponent.hpp",
         "Desert/Desert/Source/Engine/ECS/ExponentialHeightFogComponent.hpp",
         "Desert/Desert/Source/Engine/ECS/VolumetricCloudComponent.hpp",
         "Desert/Desert/Source/Engine/ECS/HeroCloudComponent.hpp",
    };

    constexpr const char* kRegistry = "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp";
    constexpr const char* kEntity   = "Desert/Desert/Source/Engine/Core/Serialize/EntitySerializer.cpp";

    // ── WHAT THE DETAILS PANEL CAN ADD, DERIVED FROM THE EDITOR RATHER THAN TYPED HERE ─────────────
    //
    // The census above answers "is anybody thinking about this component?" and a list of names can
    // satisfy it. This half answers the question that actually hurt — "can a person set this and lose
    // it?" — and nothing can satisfy it except a registration, because its membership is READ OUT OF
    // THE EDITOR'S OWN SOURCE. A sixth component that gets a Details entry and no serializer reddens
    // this suite the moment its entry is written, and it reddens it BY NAME.
    //
    // The Details panel's add-menu is exactly `ComponentWidgetRegistry::Get().Entries()`, so the file
    // set is every source under Editor/Source that mentions that registry — three files today, and any
    // fourth is found by the scan rather than by somebody remembering to widen a list here. An entry
    // names its component in one of two ways and both are read:
    //
    //   DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::X, ... )   the reflected one-liner;
    //   e.Add = [](Entity& en){ en.AddComponent<C>(); } + `using C = ::Desert::ECS::X;`  a custom entry.
    //
    // `C` is reused by a dozen different entries in one file, so an alias resolves against the NEAREST
    // PRECEDING definition, not against a whole-file table — a table would have given every entry the
    // last function's component and the scan would have been confidently wrong.
    struct AliasDefinition
    {
        std::size_t At = 0;
        std::string Name;
        std::string Component;
    };

    std::string LastNameSegment( const std::string& spelling )
    {
        const auto colon = spelling.rfind( ':' );
        return colon == std::string::npos ? spelling : spelling.substr( colon + 1 );
    }

    std::string Trimmed( const std::string& text )
    {
        const auto first = text.find_first_not_of( " \t\r\n" );
        if ( first == std::string::npos )
            return {};
        const auto last = text.find_last_not_of( " \t\r\n" );
        return text.substr( first, last - first + 1 );
    }

    // `using <name> = ::Desert::ECS::<Component>;`, in file order.
    std::vector<AliasDefinition> AliasesIn( const std::string& source )
    {
        static const std::string keyword   = "using ";
        static const std::string qualifier = "= ::Desert::ECS::";

        std::vector<AliasDefinition> found;
        for ( std::size_t at = source.find( keyword ); at != std::string::npos;
              at             = source.find( keyword, at + 1 ) )
        {
            std::size_t       cursor = at + keyword.size();
            const std::string name   = Identifier( source, cursor );
            if ( name.empty() )
                continue;
            while ( cursor < source.size() && ( source[cursor] == ' ' || source[cursor] == '\t' ) )
                ++cursor;
            if ( source.compare( cursor, qualifier.size(), qualifier ) != 0 )
                continue;
            cursor += qualifier.size();
            const std::string component = Identifier( source, cursor );
            if ( NamesAComponent( component ) )
                found.push_back( AliasDefinition{ at, name, component } );
        }
        return found;
    }

    // The text between the `<` at @p open and its matching `>`.
    std::string TemplateArgument( const std::string& source, std::size_t open )
    {
        const std::size_t close = source.find( '>', open );
        if ( close == std::string::npos )
            return {};
        return Trimmed( source.substr( open + 1, close - open - 1 ) );
    }

    std::set<std::string> AuthoredInDetails( const std::string&                        source,
                                             const std::map<std::string, std::size_t>& declared )
    {
        const std::vector<AliasDefinition> aliases = AliasesIn( source );

        std::set<std::string> authored;

        static const std::string add = "AddComponent<";
        for ( std::size_t at = source.find( add ); at != std::string::npos; at = source.find( add, at + 1 ) )
        {
            const std::string spelling = TemplateArgument( source, at + add.size() - 1 );
            if ( spelling.empty() )
                continue;

            const std::string direct = LastNameSegment( spelling );
            if ( declared.count( direct ) != 0 )
            {
                authored.insert( direct );
                continue;
            }

            // An alias, or a template parameter of some helper — resolve against the nearest preceding
            // `using`. Anything that resolves to nothing is neither, and is left alone.
            const AliasDefinition* nearest = nullptr;
            for ( const auto& alias : aliases )
            {
                if ( alias.At < at && alias.Name == spelling )
                    nearest = &alias;
            }
            if ( nearest && declared.count( nearest->Component ) != 0 )
                authored.insert( nearest->Component );
        }

        static const std::string macro = "DESERT_REGISTER_REFLECTED_COMPONENT(";
        for ( std::size_t at = source.find( macro ); at != std::string::npos; at = source.find( macro, at + 1 ) )
        {
            const std::size_t comma = source.find( ',', at );
            if ( comma == std::string::npos )
                continue;
            const std::string name =
                 LastNameSegment( Trimmed( source.substr( at + macro.size(), comma - at - macro.size() ) ) );
            if ( declared.count( name ) != 0 )
                authored.insert( name );
        }

        return authored;
    }

    // Every editor source that mentions the Details add-menu's registry. Walked, not listed: a fourth
    // file registering component entries is exactly the event a hand-written list here would miss, and
    // it is also exactly the event that introduces the next unpersisted component.
    constexpr const char* kEditorSourceRoot = "Editor/Source";

    std::vector<std::string> DetailsRegistrationSources( const std::string& root )
    {
        std::vector<std::string> found;
        std::error_code          ec;
        for ( std::filesystem::recursive_directory_iterator it( root + kEditorSourceRoot, ec ), end;
              it != end && !ec; it.increment( ec ) )
        {
            if ( !it->is_regular_file() )
                continue;
            const std::string extension = it->path().extension().string();
            if ( extension != ".cpp" && extension != ".hpp" )
                continue;
            const std::string source = ReadFile( it->path().string() );
            if ( source.find( "ComponentWidgetRegistry" ) != std::string::npos )
                found.push_back( it->path().string() );
        }
        std::sort( found.begin(), found.end() );
        return found;
    }

    // NOT PERSISTED, ON PURPOSE. Each line is the reason, and a name here that is no longer a component
    // fails this suite too — a stale exemption is how a list like this stops meaning anything.
    //
    // It was EMPTY until Ю19, and that was the state of the tree rather than an oversight. Its previous
    // entry was `ProjectileComponent`, exempted as "spawned by a script during Play, so there is no
    // authored projectile to save". U13 measured that claim against the editor and it was false: the
    // Details panel offers `Add Component > Projectile` and then four editable fields, and a bullet is
    // normally authored exactly that way and saved as a prefab for the firing script to spawn. It is
    // registered now. The bucket stays because a genuinely transient component is a real thing; a name
    // entering it has to bring a reason that survives being checked, which that one did not.
    const std::map<std::string, std::string> kTransient = {
         { "PrefabInstanceComponent",
           "DERIVED, and the only copy would be the stale one. It records which RECORD of which prefab "
           "file an entity was instantiated from, and PrefabFactory stamps it on every entity it creates, "
           "at every nesting depth, on every instantiation — so it is rebuilt in full whenever the "
           "instance is. Writing it into a `.desce` would put a second copy of a fact the `.deprefab` "
           "already states, and the two would part company the first time the prefab was re-authored: a "
           "scene's copy would then address records that no longer exist and the overrides keyed by it "
           "would silently stop applying. CHECKED AGAINST THE EDITOR, which is what this bucket demands: "
           "the Details panel cannot add it (it is not in the Add Component menu and carries no PROPERTY "
           "field), and nothing a user can set lives on it — it holds one vector of record ids." },
    };
} // namespace

// ── THE CENSUS ─────────────────────────────────────────────────────────────────────────────────────
TEST( ComponentPersistence, EveryEcsComponentIsPersistedOrIsWrittenDownAsNotBeing )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    std::map<std::string, std::size_t> declared;
    for ( const auto& header : kComponentHeaders )
    {
        const std::string source = ReadFile( root + header );
        ASSERT_FALSE( source.empty() ) << "could not read " << header;
        for ( const auto& [name, body] : DeclaredComponents( WithoutLineComments( source ) ) )
            declared[name] = body;
    }

    // A floor, not a target: it only has to catch a scan that found nothing because a path moved.
    ASSERT_GT( declared.size(), 40u ) << "the component headers were not parsed — " << declared.size()
                                      << " components found, which cannot be right";

    const std::string registrySource = ReadFile( root + kRegistry );
    ASSERT_FALSE( registrySource.empty() ) << "could not read " << kRegistry;
    const std::set<std::string> registered = ComponentsNamedIn( WithoutLineComments( registrySource ) );

    const std::string entitySource = ReadFile( root + kEntity );
    ASSERT_FALSE( entitySource.empty() ) << "could not read " << kEntity;
    const std::set<std::string> byRecord = ComponentsNamedIn( WithoutLineComments( entitySource ) );

    std::vector<std::string> unclassified;
    for ( const auto& [name, body] : declared )
    {
        (void)body;
        if ( registered.count( name ) != 0 || byRecord.count( name ) != 0 )
            continue;
        if ( kTransient.count( name ) != 0 )
            continue;
        unclassified.push_back( name );
    }

    std::string message;
    for ( const auto& name : unclassified )
        message += "\n  " + name;

    EXPECT_TRUE( unclassified.empty() )
         << "These components are not written by ComponentRegistry.cpp, are not part of the entity "
            "record in EntitySerializer.cpp, and are not written down as transient or owed. Whatever a "
            "user sets on them is silently discarded on the next load — which is how the outliner's "
            "visibility toggle came to be decoration. Register it, or add it to kTransient in this file "
            "WITH A REASON THAT SURVIVES BEING CHECKED AGAINST THE EDITOR:"
         << message;

    // A stale exemption is worse than none: it reads as a decision somebody took about a type that no
    // longer exists, and it hides the next type that takes the same name.
    for ( const auto& [name, reason] : kTransient )
    {
        (void)reason;
        EXPECT_NE( declared.count( name ), 0u ) << name
                                                << " is listed as deliberately transient but is no "
                                                   "longer an ECS component — delete the entry.";
        EXPECT_EQ( registered.count( name ), 0u ) << name
                                                  << " is listed as deliberately transient but IS "
                                                     "registered — the list and the table disagree.";
    }
}

// ── THE GATE WITH NO ESCAPE: AUTHORED IN DETAILS ⇒ SURVIVES A RELOAD ───────────────────────────────
//
// Five components reached `dev` in this exact shape — VisibilityComponent and LockComponent found one
// at a time, then Foliage, Locomotion, Morph and SocketAttachment found together and left in a list,
// then Projectile found hiding behind a transient exemption whose reason did not survive being read.
// Five instances is not a run of bad luck, it is a missing check, and the check has to be one that
// CANNOT be satisfied by editing this file — which is why the left-hand side of the relation is read
// out of the editor and the right-hand side out of the registry, and there is no third list.
TEST( ComponentPersistence, EveryComponentTheDetailsPanelCanAddSurvivesAReload )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::map<std::string, std::size_t> declared;
    for ( const auto& header : kComponentHeaders )
    {
        const std::string source = ReadFile( root + header );
        ASSERT_FALSE( source.empty() ) << "could not read " << header;
        for ( const auto& [name, body] : DeclaredComponents( WithoutLineComments( source ) ) )
            declared[name] = body;
    }
    ASSERT_GT( declared.size(), 40u ) << "the component headers were not parsed";

    const std::vector<std::string> sources = DetailsRegistrationSources( root );
    ASSERT_FALSE( sources.empty() ) << "no editor source mentioning ComponentWidgetRegistry was found "
                                       "under "
                                    << root << kEditorSourceRoot
                                    << " — the scan is looking in the wrong place and would pass on "
                                       "an empty set, which is the one answer it must never give";

    std::map<std::string, std::string> authoredIn; // component -> the file that offers it
    for ( const auto& file : sources )
    {
        const std::string source = WithoutLineComments( ReadFile( file ) );
        for ( const auto& name : AuthoredInDetails( source, declared ) )
            authoredIn.emplace( name, file );
    }

    // A floor on the LEFT-hand side, for the same reason the census has one: a parse that silently
    // found nothing agrees with every registry there is.
    EXPECT_GT( authoredIn.size(), 30u )
         << "only " << authoredIn.size()
         << " components were found to be addable from the Details panel, which cannot be right — the "
            "add-menu is larger than that, so the scan has stopped matching how entries are written";

    const std::string registrySource = ReadFile( root + kRegistry );
    ASSERT_FALSE( registrySource.empty() ) << "could not read " << kRegistry;
    const std::set<std::string> registered = ComponentsNamedIn( WithoutLineComments( registrySource ) );

    const std::string entitySource = ReadFile( root + kEntity );
    ASSERT_FALSE( entitySource.empty() ) << "could not read " << kEntity;
    const std::set<std::string> byRecord = ComponentsNamedIn( WithoutLineComments( entitySource ) );

    std::string lost;
    for ( const auto& [name, file] : authoredIn )
    {
        if ( registered.count( name ) != 0 || byRecord.count( name ) != 0 )
            continue;
        lost += "\n  " + name + "  (offered by " + file + ")";
    }

    EXPECT_TRUE( lost.empty() )
         << "The Details panel can add these components to an entity, and ComponentRegistry.cpp does "
            "not write them. Everything a person types into them is discarded — by the next Open "
            "Scene, and also by Ctrl+C, by delete-undo, by prefab instancing and by Play/Stop, because "
            "all of those are that same registry. There is no exemption list for this check on "
            "purpose: if it can be authored it has to survive. Add a serializer in "
         << kRegistry << " (see AuthoredComponentIO.hpp for the hand-mapped ones):" << lost;
}

// ── THE DEFECT THAT LOOKS EXACTLY LIKE THE FIX ─────────────────────────────────────────────────────
//
// MakeMarker serializes an empty object and re-adds the component on load, so the component comes back
// at its STRUCT DEFAULTS. That is correct for a marker (FolderComponent, LockComponent: presence IS the
// state) and silently wrong for anything carrying a field — `Visible = false` would round-trip to
// `true`, and the registration would look like the persistence it is not. Applying MakeMarker to
// VisibilityComponent was the obvious repair and it would have shipped the same defect under a fix.
TEST( ComponentPersistence, EveryMarkerRegistrationIsOnAComponentThatCarriesNothing )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::map<std::string, std::size_t> declared;
    std::map<std::string, std::string> sourceOf;
    for ( const auto& header : kComponentHeaders )
    {
        const std::string source = WithoutLineComments( ReadFile( root + header ) );
        ASSERT_FALSE( source.empty() ) << "could not read " << header;
        for ( const auto& [name, body] : DeclaredComponents( source ) )
        {
            declared[name] = body;
            sourceOf[name] = source;
        }
    }

    const std::string registrySource = WithoutLineComments( ReadFile( root + kRegistry ) );
    ASSERT_FALSE( registrySource.empty() );

    static const std::string call = "MakeMarker<";

    std::size_t              markers = 0;
    std::vector<std::string> failures;
    for ( std::size_t at = registrySource.find( call ); at != std::string::npos;
          at             = registrySource.find( call, at + 1 ) )
    {
        const std::size_t close = registrySource.find( '>', at );
        ASSERT_NE( close, std::string::npos );
        std::string name = registrySource.substr( at + call.size(), close - at - call.size() );
        if ( const auto colon = name.rfind( ':' ); colon != std::string::npos )
            name = name.substr( colon + 1 );

        // The template's own definition, `ComponentSerializer MakeMarker( std::string key )`, contains no
        // `MakeMarker<` — every hit here is a call site.
        if ( declared.count( name ) == 0 )
            continue;
        ++markers;

        const std::string& source = sourceOf[name];
        const std::size_t  open   = declared[name];
        const std::size_t  end    = source.find( '}', open );
        ASSERT_NE( end, std::string::npos );

        const std::string body = source.substr( open + 1, end - open - 1 );
        if ( body.find_first_not_of( " \t\r\n" ) != std::string::npos )
            failures.push_back( name + " is registered with MakeMarker but its body is not empty: {" + body +
                                "}" );
    }

    EXPECT_GT( markers, 0u ) << "no MakeMarker<> call sites found — this check is asserting nothing";

    std::string message;
    for ( const auto& f : failures )
        message += "\n  " + f;

    EXPECT_TRUE( failures.empty() )
         << "MakeMarker writes an empty object and re-adds the component at its struct defaults, so every "
            "field of these components round-trips to its default while the registration reads as "
            "persistence. Use a maker that writes the fields (MakeFlag for a single bool, MakeReflected "
            "for a data block):"
         << message;
}

// The regression pin for U11 itself. The census above would also catch a removal, but only by naming it
// among a list; this says the sentence.
TEST( ComponentPersistence, TheOutlinerEyeIsWrittenToTheScene )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string registrySource = WithoutLineComments( ReadFile( root + kRegistry ) );
    ASSERT_FALSE( registrySource.empty() );

    EXPECT_NE( registrySource.find( "MakeFlag<ECS::VisibilityComponent>" ), std::string::npos )
         << "VisibilityComponent has no registration, so hiding an object in the outliner lasts until the "
            "scene is next opened and nothing tells the user.";
    EXPECT_NE( registrySource.find( "\"Visibility\"" ), std::string::npos )
         << "the scene key for the visibility flag is gone; every .desce already written carries "
            "\"Visibility\" and would silently load as visible.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
