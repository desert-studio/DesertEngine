#include "EditorPreferences.hpp"

#include <Engine/Project/ProjectContext.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <Common/Json/Json.hpp>

// glm::vec3 <-> JSON reflector (OutlineColor). Must be visible before the rfl::json read/write below.
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <sstream>

namespace Desert::Editor
{
    EditorPreferences& EditorPreferences::Get()
    {
        static EditorPreferences s_Instance;
        return s_Instance;
    }

    std::string EditorPreferences::ConfigDirectory()
    {
        // Single source of truth for the user config dir lives with the engine's project system.
        return ::Desert::Project::ProjectContext::ConfigDirectory();
    }

    static std::string PrefsFile()
    {
        return EditorPreferences::ConfigDirectory() + "/editor.json";
    }

    // THE RETIRED PINNED-FOLDERS FILE (К5). It is named in exactly one place — here — and only so that the
    // migration below can read it once and delete it. Nothing reads the pinned folders from it.
    //
    // The directory it sits in is asked of ConfigDirectory() rather than resolved again. `$HOME` was
    // spelled out by hand in the content browser, which made a THIRD statement of where this user's
    // configuration lives, differing from ProjectContext's in the one respect that mattered: it did not
    // create the directory. Two of the three copies were already one function.
    static std::filesystem::path LegacyFavouritesFile()
    {
        return std::filesystem::path( EditorPreferences::ConfigDirectory() ) / "asset_favorites.txt";
    }

    // WHICH PROJECT'S PINS ARE BEING ASKED ABOUT, and "" when the answer is none. The key is the `Name`
    // out of the open `.deproj`; see the field's comment for why that and not the project's path.
    static std::string CurrentProjectKey()
    {
        if ( !::Desert::Project::ProjectContext::HasProject() )
            return {};
        return ::Desert::Project::ProjectContext::Current().Name;
    }

    // IS THIS PIN'S FOLDER PROVABLY GONE? True only when the filesystem gave a definite answer and that
    // answer was "there is nothing here" (or "what is here is not a folder"). False when the question could
    // not be asked at all — an unmounted volume, a network share that is down, a parent this process may
    // not traverse — because dropping a pin on that is destroying a user's data because a disk was busy.
    //
    // WHY exists() AND NOT is_directory(). Both take an error_code, and they disagree about what a MISSING
    // path is. Measured on this toolchain (libc++, macOS 15): `is_directory("/no/such/path", ec)` returns
    // false AND SETS ec TO ENOENT, while `exists("/no/such/path", ec)` returns false and leaves ec clear.
    // So the obvious one-liner — `!is_directory(p, ec)` guarded by `!ec` — is wrong in the direction that
    // does not show: it never prunes anything, because the case it is meant to catch is the case it reads
    // as an error. That was the first version of this, and the test below it is what said so.
    static bool PinIsProvablyGone( const std::filesystem::path& folder )
    {
        std::error_code ec;
        const bool      there = std::filesystem::exists( folder, ec );
        if ( ec )
            return false; // the filesystem could not answer; the pin stays
        if ( !there )
            return true;

        // It is there, so this cannot fail with "not found"; a failure now is again an unanswerable
        // question and again keeps the pin.
        std::error_code kindEc;
        const bool      isFolder = std::filesystem::is_directory( folder, kindEc );
        return !kindEc && !isFolder;
    }

    // A folder inside the assets root, in the form the field stores: relative, `/`-separated, "." for the
    // root itself. Empty means the path is NOT inside the root and therefore cannot be stored — the caller
    // decides what to say about that, because the two callers say different things.
    //
    // LEXICALLY, and not std::filesystem::relative: that one resolves symlinks against the real disk, so
    // the answer would depend on whether the folder still exists — and the migration has to be able to
    // classify a line naming a folder that was deleted years ago.
    static std::string RelativeToAssets( const std::filesystem::path& assetsRoot,
                                         const std::filesystem::path& folder )
    {
        const std::filesystem::path rel =
             folder.lexically_normal().lexically_relative( assetsRoot.lexically_normal() );
        if ( rel.empty() || *rel.begin() == ".." )
            return {};
        return rel.generic_string();
    }

    // THE ONE PUSH THIS FILE USED TO MAKE IS GONE. `PushToRenderConfig` copied MSAASamples into
    // Graphic::RenderConfig for SceneRenderer::Init to read, and its own comment argued — correctly —
    // that the push was safe because the copy had exactly one writer. What it could not argue is that
    // this was the right FILE: the packaged Runtime never opens editor.json, so the push was also the
    // only writer the value had anywhere, and a shipped game ran at one sample whatever its player
    // chose. К3 moved the field into Common::Settings::MachineSettings, which the renderer reads
    // directly, and with it the last reason for this file to reach into another layer at all.

    // WHAT THIS PROCESS BELIEVES editor.json HOLDS, in the canonical text Save() writes. Empty means it
    // believes there is no file (first run), or that it cannot claim to know — a failed read, a corrupt
    // parse — in which case the next write must happen unconditionally.
    //
    // IT IS A MEMO, NOT A SECOND STORE, and that distinction is the whole of why it is safe. Nothing ever
    // reads a preference out of it; it is only compared, as text, against what is about to be written. The
    // struct above stays the sole authority on every value, so this cannot become the К6 shape (one value,
    // two stores, writers disagreeing about the authority) however it drifts — the worst a stale memo can
    // do is skip a write, and the file's own contents are what it is stale WITH RESPECT TO.
    //
    // It exists because К8 made "write the same bytes again" the common case rather than a rarity: every
    // control in the Preferences window now commits when the user lets go of it, and three other panels
    // commit on a click. Letting go of a control you only hovered, re-picking the MSAA level you are
    // already on, or dragging a slider back where it started were each a file write and a log line.
    //
    // К9 DEMOTED IT FROM A BELIEF TO A FALLBACK, which is a strictly smaller job and the right one. Every
    // save now re-reads the file first (AdoptUnknownKeysFromDisk below), so this holds what the file
    // ACTUALLY said a moment ago whenever the file could be read at all; the remembered value is what is
    // left when it could not. A belief about a file several processes write was always the weaker of the
    // two, and reading a few hundred bytes before a write nobody makes per frame costs nothing.
    static std::string s_OnDisk;

    // Which top-level keys of editor.json differ between two of its serializations, by name.
    //
    // THE KEY LIST IS THE UNION OF WHAT THE TWO TEXTS ACTUALLY CONTAIN, and it used to be
    // rfl::fields<EditorPreferences>(). Both derive the list rather than typing it, so both were
    // field-count-proof; the difference is that the struct's field list stopped being the whole of the
    // file's key list when UnknownKeys arrived. A key held by another build is a key of this file, and
    // this is the function that has to be able to say a migration dropped one — asking rfl::fields<>
    // about it would name nothing, which is the "container derived from the same source as the question"
    // shape the contract's §1.4 warns about, in miniature.
    static std::vector<std::string> ChangedFields( const std::string& before, const std::string& after )
    {
        // Both texts are this file's own canonical serializations, so each is a JSON object; a text that is
        // not one names no key and the caller's log line says nothing changed rather than guessing.
        const auto lhs = Common::Json::Read<Common::Json::Object>( before );
        const auto rhs = Common::Json::Read<Common::Json::Object>( after );
        if ( !lhs || !rhs )
            return {};
        const Common::Json::Object& lhsObject = lhs.GetValue();
        const Common::Json::Object& rhsObject = rhs.GetValue();

        std::vector<std::string> keys;
        for ( const auto& [name, value] : lhsObject )
            keys.push_back( name );
        for ( const auto& [name, value] : rhsObject )
            if ( std::find( keys.begin(), keys.end(), name ) == keys.end() )
                keys.push_back( name );

        std::vector<std::string> differing;
        for ( const std::string& key : keys )
        {
            const auto a = lhsObject.get( key );
            const auto b = rhsObject.get( key );
            if ( !a.has_value() || !b.has_value() )
            {
                differing.push_back( key );
                continue;
            }
            if ( Common::Json::Write( a.value() ) != Common::Json::Write( b.value() ) )
                differing.push_back( key );
        }
        return differing;
    }

    // THE EVENT, not the call. `LOG_INFO( "[Prefs] Saved {}", path )` fired on every save and said only
    // that this function had run — which is the least informative thing it knows, and with three panels
    // saving on a click the Logs panel filled with identical lines that named neither the setting nor the
    // panel. Commit-on-edit would have made that strictly worse. What a reader needs is which settings
    // moved, and the diff against what is already on disk has that for free.
    static std::string DescribeChange( const std::string& before, const std::string& after )
    {
        if ( before.empty() )
            return "created";

        const auto changed = ChangedFields( before, after );
        if ( changed.empty() )
        {
            // The texts differ but no field does: only reachable if the JSON writer's own output changes
            // between builds. Said plainly rather than reported as a settings change that did not happen.
            return "reserialized";
        }

        std::string list = changed.front();
        for ( std::size_t i = 1; i < changed.size(); ++i )
            list += ", " + changed[i];
        return list;
    }

    // KEYS THIS PROJECT DELETED ON PURPOSE, WHICH IS A DIFFERENT THING FROM A KEY IT DOES NOT KNOW.
    //
    // Before К9 a removed field needed no entry here: the struct stopped naming it, so the next save
    // rewrote the file without it and the key was gone. UnknownKeys ended that — every key survives now,
    // including the ones somebody meant to destroy — so the deletion has to be stated somewhere, and this
    // is that somewhere. The invariant it protects is contract §4's: a retirement finishes.
    //
    // The first two are К1's. `PhotogrammetryCaptureCommand` documented a `{photos}` substitution that
    // was never implemented and `PhotogrammetryMode` an Object/Face preset switch that does not exist;
    // they were serialized into every editor.json and read by nothing.
    //
    // `MSAASamples` is К3's, and it is the first row here that names a LIVE setting rather than a dead
    // one. The value did not stop existing — it moved to Common::Settings::MachineSettings, because this
    // file is an Editor-target concept the packaged Runtime never opens and a per-machine quality knob
    // stored here is a knob the player does not get. What is retired is the KEY: this build no longer
    // declares it, so without this row UnknownKeys would preserve it in every existing editor.json for
    // ever, and a reader opening that file a year from now would find a quality setting that has had no
    // consumer since 2026-09-08. The new store is NOT seeded from it — two files written by different
    // processes at different moments, one silently deciding the other, is the hazard this whole task is
    // about; the machine store's default is 1, which is what every editor.json in existence states.
    //
    // EXPIRY: a row leaves this list when no config in circulation can still carry the key. It costs one
    // string compare per unknown key per read, and a read has neither in the ordinary case.
    static bool IsRetiredKey( const std::string& key )
    {
        static const std::vector<std::string> retired = { "PhotogrammetryCaptureCommand", "PhotogrammetryMode",
                                                          "MSAASamples" };
        return std::find( retired.begin(), retired.end(), key ) != retired.end();
    }

    // Drops the retired keys from `p`, naming each in `raised` when one is given.
    //
    // BOTH PATHS THAT TAKE UNKNOWN KEYS OFF DISK CALL THIS, and they have to: MigrateLoaded() for the read
    // at startup, AdoptUnknownKeysFromDisk() for the re-read at the moment of writing. One rule, one
    // implementation — a second copy of "which keys are dead" is the two-stores shape К6 removed, and it
    // would show up as a key that comes back whenever the OTHER path is the one that ran.
    //
    // Rebuilt rather than erased in place because rfl::Object is an ordered vector of pairs with no
    // erase(). Rebuilding also preserves the order of what is kept, which matters because PersistCurrent
    // compares the serialized TEXT.
    static void DropRetiredKeys( EditorPreferences& p, std::vector<std::string>* raised )
    {
        if ( p.UnknownKeys.empty() )
            return;

        Common::Json::KeyedValues kept;
        for ( const auto& [key, value] : p.UnknownKeys )
        {
            if ( !IsRetiredKey( key ) )
            {
                kept.insert( key, value );
                continue;
            }
            if ( raised != nullptr )
                raised->push_back( "retired key '" + key + "' dropped (this build does not declare it)" );
        }
        p.UnknownKeys = std::move( kept );
    }

    // WHAT editor.json HOLDS AT THIS INSTANT, and the unknown keys taken from it — refreshed into
    // Get().UnknownKeys. Returns the file's canonical text, or nothing when there is no file or it could
    // not be read or parsed.
    //
    // A LOAD-TIME SNAPSHOT IS NOT ENOUGH, AND THAT IS THE HALF OF К9 THE OBVIOUS FIX MISSES. Several
    // editors are open against one `~/.desertengine/editor.json` here all day. Editor A starts at 10:00,
    // when the file holds nothing A does not know, so A's carrier is empty. B's newer build writes
    // `PackageAppBundle` at 10:30. A saves at 11:00 from a carrier captured an hour earlier — and deletes
    // it. That is the same defect arriving by a different road, and a carrier filled only at Load() has
    // no way to see it. The keys another build owns are therefore re-read at the moment of WRITING.
    //
    // IT ADOPTS KEYS AND NEVER VALUES. Only UnknownKeys is taken from the file; every field this struct
    // declares stays exactly as the user left it, so a concurrent editor's disagreement about CameraSpeed
    // is still resolved last-writer-wins the way it always was. Adopting a field would be the opposite of
    // this file's rule — a save that changes a setting the user did not touch (К6).
    //
    // Cost: one read and one parse per save, of a file measured in hundreds of bytes. A save happens when
    // a control is released or a menu item clicked, never per frame.
    static std::optional<std::string> AdoptUnknownKeysFromDisk()
    {
        if ( !std::filesystem::exists( PrefsFile() ) )
            return std::nullopt;

        const auto raw = Common::Utils::FileSystem::ReadFileContent( PrefsFile() );
        if ( !raw || raw.GetValue().empty() )
        {
            LOG_WARN( "[Prefs] {} exists but could not be read before saving; keys written by another "
                      "build cannot be preserved this time and the file will be replaced wholesale.",
                      PrefsFile() );
            return std::nullopt;
        }

        auto parsed = Common::Json::Read<EditorPreferences>( raw.GetValue() );
        if ( !parsed )
        {
            LOG_WARN( "[Prefs] {} is corrupt ({}); it is being replaced rather than merged, so any key "
                      "another build put in it is lost with it.",
                      PrefsFile(), parsed.GetError() );
            return std::nullopt;
        }

        EditorPreferences fromDisk = parsed.GetValue();

        // The file's canonical text is taken BEFORE the retirement, on purpose: it is the memo the write
        // is decided against, and a retired key still sitting on disk has to read as a difference or the
        // save that removes it would be skipped as redundant.
        const std::string canonical = Common::Json::Write( fromDisk );

        DropRetiredKeys( fromDisk, nullptr );
        EditorPreferences::Get().UnknownKeys = std::move( fromDisk.UnknownKeys );
        return canonical;
    }

    // THE ONLY WRITER OF editor.json. Both public entry points funnel here; they differ in one thing, the
    // sentence that reaches the log, which is exactly the difference between "the user changed a setting"
    // and "this build raised a stored value".
    //
    // `event` empty means "derive it from what actually differs" — Save() has no sentence of its own to
    // offer, and the diff is a better one than any fixed string.
    static bool PersistCurrent( std::string event )
    {
        // A SAVE MUST NOT CHANGE A SINGLE FIELD THE USER DID NOT TOUCH, AND NOW IT TOUCHES NOTHING AT
        // ALL OUTSIDE THIS FILE. This was once ApplyToGizmoState(), which reverted the four gizmo snap
        // values to whatever this struct last held — so toggling the Perf HUD or starring a field in
        // Details silently undid a snap step chosen from the toolbar. К6 cut that down to one derived
        // push (RenderConfig::MSAASamples); К3 took the field itself out of this file, so there is no
        // push left. Desert/Tests/Editor/PreferenceOwnership asserts the relation.

        // Whatever another build has put in the file since this one read it. See the header above: this is
        // what stops a long-running editor deleting a key that appeared after it started.
        //
        // The memo becomes what the file ACTUALLY says whenever the file can be read, which is strictly
        // better than what this process last wrote and is what makes the skip below honest. When it cannot
        // be read the memo is cleared instead, so the next write is unconditional — the safe direction, and
        // the same one Load() takes for the same reason.
        if ( const std::optional<std::string> onDisk = AdoptUnknownKeysFromDisk(); onDisk.has_value() )
            s_OnDisk = *onDisk;
        else if ( std::filesystem::exists( PrefsFile() ) )
            s_OnDisk.clear();

        const std::string json = Common::Json::Write( EditorPreferences::Get() );
        if ( json == s_OnDisk && std::filesystem::exists( PrefsFile() ) )
        {
            // The file already says exactly this. No write, and no log line about one — a line saying
            // nothing changed is the same noise as a line saying something did, only wronger.
            //
            // THE exists() IS NOT BELT AND BRACES. Without it, `true` would mean "the memo says the disk
            // agrees" rather than "the disk agrees", and a file removed behind a running editor would
            // never be rebuilt — a successful answer that is silently wrong, which is exactly what §1.4
            // forbids. It only runs when the memo already matched, so an ordinary save pays nothing.
            return true;
        }

        if ( event.empty() )
            event = DescribeChange( s_OnDisk, json );

        // Load() checks its read and handles a corrupt file; Save() logged success unconditionally, so
        // preferences silently stopped persisting the moment the config directory became unwritable.
        const auto written =
             Common::Utils::FileSystem::WriteContentToFileAtomic( std::filesystem::path( PrefsFile() ), json );
        if ( !written )
        {
            // The memo is deliberately NOT updated here: the next save must try again rather than assume
            // the disk agrees with us.
            LOG_ERROR( "[Prefs] {} was NOT saved: {} — these settings apply to this session only.", PrefsFile(),
                       written.GetError() );
            return false;
        }

        s_OnDisk = json;
        LOG_INFO( "[Prefs] {} -> {}", event, PrefsFile() );
        return true;
    }

    std::vector<std::string> EditorPreferences::MigrateLoaded( EditorPreferences& p )
    {
        std::vector<std::string> raised;

        DropRetiredKeys( p, &raised );

        // METRE-ERA TranslateSnap -> centimetres.
        //
        // The field's default was 0.5 with the comment "world units" from when a unit was a metre. A
        // world unit is a CENTIMETRE now, so every stored value is a hundredth of what its author meant,
        // and the Preferences slider that wrote it was labelled "Move (m)" while feeding a centimetre
        // system. Sub-centimetre snapping is meaningless at this scale — the smallest step the editor
        // offers is 1 cm — so a stored value below 1 can only be a metre-era number.
        //
        // EXPIRY: this raises pre-2026-09 preference files and nothing else. Delete it once no one is
        // carrying a config written before У5; it cannot fire on a value this build can produce.
        if ( p.TranslateSnap > 0.0f && p.TranslateSnap < 1.0f )
        {
            const float old = p.TranslateSnap;
            p.TranslateSnap *= 100.0f;
            raised.push_back( "TranslateSnap " + std::to_string( old ) + " -> " +
                              std::to_string( p.TranslateSnap ) + " cm (1 world unit = 1 cm)" );
        }

        return raised;
    }

    std::vector<std::string> EditorPreferences::MigrateFavouritesFile( EditorPreferences&              p,
                                                                       const std::string&              project,
                                                                       const std::filesystem::path&    assetsRoot,
                                                                       const std::vector<std::string>& lines )
    {
        std::vector<std::string> raised;
        if ( project.empty() )
            return raised;

        std::vector<std::string>& pins = p.FavouriteFolders[project];
        const std::size_t         had  = pins.size();

        std::size_t foreign = 0;
        for ( const std::string& line : lines )
        {
            if ( line.empty() )
                continue;

            const std::string rel = RelativeToAssets( assetsRoot, line );
            if ( rel.empty() )
            {
                ++foreign;
                raised.push_back( "pinned folder '" + line +
                                  "' dropped: it is outside this project's assets root, and the retired "
                                  "file recorded no project to attribute it to" );
                continue;
            }
            if ( std::find( pins.begin(), pins.end(), rel ) == pins.end() )
                pins.push_back( rel );
        }

        if ( pins.size() > had )
            raised.push_back( std::to_string( pins.size() - had ) + " pinned folder(s) moved into '" + project +
                              "' from the retired asset_favorites.txt" );
        else if ( foreign == 0 && !lines.empty() )
            raised.push_back( "the retired asset_favorites.txt held nothing this project did not already have" );

        // A key with nothing behind it is what the operator[] above would otherwise leave for a file that
        // was empty, or held only another project's folders. See ToggleFavouriteFolder.
        if ( pins.empty() )
            p.FavouriteFolders.erase( project );

        return raised;
    }

    // READ THE RETIRED FILE ONCE, MOVE WHAT IT HOLDS, DELETE IT — in that order, and the order is the
    // whole safety of it (К5). The write is confirmed before the deletion, so a run that cannot save
    // preferences leaves the old file exactly where it was and tries again next launch; the alternative is
    // a migration that destroys the only copy of the data it failed to move. That is the И2 shape — a
    // writer reporting success over a file it has just emptied — and this function is a deletion, which is
    // the same shape one step further along.
    static void AdoptLegacyFavourites()
    {
        const std::filesystem::path legacy = LegacyFavouritesFile();
        std::error_code             ec;
        if ( !std::filesystem::exists( legacy, ec ) )
            return;

        const std::string project = CurrentProjectKey();
        if ( project.empty() )
        {
            // NOT AN ERROR AND NOT A DELETION. Every line in the file needs a project to be attributed to,
            // so a session without one cannot migrate — and must not destroy the file trying.
            LOG_INFO( "[Prefs] {} is waiting for a project: its pinned folders are moved into editor.json "
                      "the first time this editor opens one.",
                      legacy.string() );
            return;
        }

        std::vector<std::string> lines;
        {
            const auto raw = Common::Utils::FileSystem::ReadFileContent( legacy.string() );
            if ( !raw )
            {
                LOG_ERROR( "[Prefs] {} exists but could not be read: {} — the pinned folders it holds are "
                           "not migrated and the file is left in place.",
                           legacy.string(), raw.GetError() );
                return;
            }

            std::istringstream in( raw.GetValue() );
            std::string        line;
            while ( std::getline( in, line ) )
            {
                if ( !line.empty() && line.back() == '\r' )
                    line.pop_back();
                lines.push_back( line );
            }
        }

        const auto raised = EditorPreferences::MigrateFavouritesFile(
             EditorPreferences::Get(), project, Common::Constants::Path::ASSETS_PATH, lines );

        // Contract §4.7: which file, from what to what, and how many entries moved.
        for ( const std::string& line : raised )
            LOG_INFO( "[Prefs] {} migrated: {}", legacy.string(), line );

        if ( !PersistCurrent( "pinned folders moved out of asset_favorites.txt (" +
                              std::to_string( raised.size() ) + " note(s))" ) )
        {
            LOG_ERROR( "[Prefs] {} is NOT deleted: the pinned folders could not be written to {}, so the "
                       "old file is the only copy and stays until a save succeeds.",
                       legacy.string(), PrefsFile() );
            return;
        }

        std::filesystem::remove( legacy, ec );
        if ( ec )
        {
            // The pins are already in editor.json, so nothing is lost — but a file that survives its own
            // migration is read again next launch, and saying so is what stops that being a mystery.
            LOG_ERROR( "[Prefs] {} was migrated but could NOT be deleted: {} — it will be read (and its "
                       "entries re-merged, harmlessly) on every launch until it is removed by hand.",
                       legacy.string(), ec.message() );
            return;
        }
        LOG_INFO( "[Prefs] {} is retired and deleted; pinned folders live in {} now.", legacy.string(),
                  PrefsFile() );
    }

    void EditorPreferences::Load()
    {
        // Anything short of a file we read AND understood leaves the memo empty, which means "we cannot
        // claim the disk agrees with us" and forces the next save to write. That is the safe direction:
        // the alternative is a corrupt or unreadable editor.json that a later identical-looking save
        // declines to repair.
        s_OnDisk.clear();

        // First run: no prefs file yet — keep defaults, and skip the read's "could not read file"
        // error line, which would be noise for a state that is expected.
        //
        // A BARE `return` STOOD HERE AND BECAME A DEFECT THE MOMENT A SECOND MIGRATION JOINED THIS
        // FUNCTION — caught end to end, running the editor against a planted legacy file rather than by
        // reading the diff. `AdoptLegacyFavourites()` below is about a DIFFERENT file, and under the early
        // return it ran only for a user who also had an editor.json: a fresh install carrying pinned
        // folders and nothing else migrated silently never, and the file it should have retired stayed on
        // disk being ignored. The recurring middle-link shape — both ends right, the step between them not
        // reached — so the condition is a block now and the load has one exit.
        if ( std::filesystem::exists( PrefsFile() ) )
        {
            if ( const auto raw = Common::Utils::FileSystem::ReadFileContent( PrefsFile() ); !raw )
            {
                // The file EXISTS — that is tested above — so a failed read is a permission or I/O problem
                // and not the expected first-run case. It used to fall through a bare `if ( raw && ... )`
                // with nothing logged, which is §1.4's silent fallback: the editor came up on defaults and
                // the user's own settings were one unexplained launch from being overwritten.
                LOG_ERROR( "[Prefs] {} exists but could not be read: {} — this session runs on defaults, and "
                           "the next settings change will overwrite the file.",
                           PrefsFile(), raw.GetError() );
            }
            else if ( raw.GetValue().empty() )
            {
                LOG_WARN( "[Prefs] {} is empty; using defaults.", PrefsFile() );
            }
            // Lenient (DESERT_JSON_LENIENT in the header): prefs written by older builds (fewer fields) keep
            // loading — new fields just take their in-struct defaults instead of failing the whole file.
            else if ( auto parsed = Common::Json::Read<EditorPreferences>( raw.GetValue() ); parsed )
            {
                Get() = parsed.GetValue();
                // The canonical form of what the file holds, NOT the raw bytes: an older build's key order or
                // spacing is not a settings change, and a memo taken from the raw text would report the whole
                // struct as changed on the first save after an upgrade.
                s_OnDisk = Common::Json::Write( Get() );
            }
            else
            {
                LOG_WARN( "[Prefs] editor.json is corrupt, using defaults: {}", parsed.GetError() );
            }

            if ( const auto raised = MigrateLoaded( Get() ); !raised.empty() )
            {
                // Contract §4.7: a migration says which file, from what to what, and how many fields moved.
                for ( const std::string& line : raised )
                    LOG_INFO( "[Prefs] {} migrated: {}", PrefsFile(), line );

                // Written back in the new form so the migration runs once, not every launch — and through
                // SaveMigrated rather than Save, so the log names the migration instead of reporting a
                // settings change the user did not make.
                SaveMigrated( "migration write-back (" + std::to_string( raised.size() ) + " field(s) raised)" );
            }

            // CARRYING A KEY WE DO NOT UNDERSTAND IS AN EVENT, NOT A DETAIL. It means another build — an
            // agent's worktree, an older install, a branch that has since landed — owns settings this binary
            // cannot show or edit, and the only symptom otherwise available is the one К9 came from: nobody
            // noticing until the values were already gone. Named rather than counted, because "3 unknown
            // keys" tells a reader nothing about whether to go and look for the build that wrote them.
            if ( !Get().UnknownKeys.empty() )
            {
                std::string names = Get().UnknownKeys.begin()->first;
                for ( auto it = std::next( Get().UnknownKeys.begin() ); it != Get().UnknownKeys.end(); ++it )
                    names += ", " + it->first;

                LOG_INFO( "[Prefs] {} holds {} key(s) this build does not know ({}); they belong to another "
                          "build and are preserved on save, not dropped.",
                          PrefsFile(), Get().UnknownKeys.size(), names );
            }
        }

        // LAST, because it writes through this struct: the pinned folders it moves have to land on top of
        // what was just read, not be overwritten by it. Ordinarily a no-op — the file it reads exists only
        // on an installation that predates К5, and it deletes the file it migrates.
        AdoptLegacyFavourites();
    }

    bool EditorPreferences::IsFavouriteField( const std::string& key )
    {
        const auto& v = Get().FavouriteFields;
        return std::find( v.begin(), v.end(), key ) != v.end();
    }

    void EditorPreferences::ToggleFavouriteField( const std::string& key )
    {
        auto& v  = Get().FavouriteFields;
        auto  it = std::find( v.begin(), v.end(), key );
        if ( it != v.end() )
            v.erase( it );
        else
            v.push_back( key );
        Save();
    }

    bool EditorPreferences::IsComponentCollapsed( const std::string& name )
    {
        const auto& v = Get().CollapsedComponents;
        return std::find( v.begin(), v.end(), name ) != v.end();
    }

    void EditorPreferences::SetComponentCollapsed( const std::string& name, bool collapsed )
    {
        auto& v  = Get().CollapsedComponents;
        auto  it = std::find( v.begin(), v.end(), name );
        // Already in the requested state. Since К8 this is no longer what keeps the file still — Save()
        // compares the bytes and would skip the write anyway — but a section header reports a click on
        // every frame it is hovered-and-pressed, so returning here also saves serializing the whole
        // struct to find out that nothing moved.
        if ( collapsed == ( it != v.end() ) )
            return;
        if ( collapsed )
            v.push_back( name );
        else
            v.erase( it );
        Save();
    }

    std::vector<std::string> EditorPreferences::CurrentFavouriteFolders()
    {
        const std::string project = CurrentProjectKey();
        if ( project.empty() )
            return {};

        const auto stored = Get().FavouriteFolders.find( project );
        if ( stored == Get().FavouriteFolders.end() )
            return {};

        const std::filesystem::path& root = Common::Constants::Path::ASSETS_PATH;

        std::vector<std::string> absolute;
        absolute.reserve( stored->second.size() );
        for ( const std::string& rel : stored->second )
            absolute.push_back( ( rel == "." ? root : root / rel ).generic_string() );
        return absolute;
    }

    bool EditorPreferences::IsFavouriteFolder( const std::string& absoluteFolder )
    {
        const std::string project = CurrentProjectKey();
        if ( project.empty() )
            return false;

        const auto stored = Get().FavouriteFolders.find( project );
        if ( stored == Get().FavouriteFolders.end() )
            return false;

        const std::string rel = RelativeToAssets( Common::Constants::Path::ASSETS_PATH, absoluteFolder );
        if ( rel.empty() )
            return false;
        return std::find( stored->second.begin(), stored->second.end(), rel ) != stored->second.end();
    }

    void EditorPreferences::ToggleFavouriteFolder( const std::string& absoluteFolder )
    {
        const std::string project = CurrentProjectKey();
        if ( project.empty() )
        {
            LOG_WARN( "[Prefs] '{}' was not pinned: a pinned folder belongs to a project and none is open.",
                      absoluteFolder );
            return;
        }

        const std::filesystem::path& root = Common::Constants::Path::ASSETS_PATH;
        const std::string            rel  = RelativeToAssets( root, absoluteFolder );
        if ( rel.empty() )
        {
            // The browser only ever walks inside the assets root, so this is unreachable from the panel —
            // and it is here rather than assumed away because the alternative is storing a path that
            // CurrentFavouriteFolders() would resolve back to a different folder in the next project.
            LOG_ERROR( "[Prefs] '{}' was not pinned: it is outside this project's assets root ({}).",
                       absoluteFolder, root.generic_string() );
            return;
        }

        std::vector<std::string>& pins = Get().FavouriteFolders[project];
        const auto                at   = std::find( pins.begin(), pins.end(), rel );
        if ( at != pins.end() )
            pins.erase( at );
        else
            pins.push_back( rel );

        // THE ONLY PLACE ANYTHING IS EVER REMOVED FROM THIS LIST WITHOUT THE USER ASKING, and it removes
        // only what it can PROVE is gone. The legacy file never shrank at all, so a pin of a folder deleted
        // two projects ago was still drawn in the sidebar. Done at the write and not at the read, because a
        // reader that mutates its store is a save the user did not make (К6) — and this IS a user action,
        // with a save already going out.
        //
        // "GONE" AND "CANNOT BE ANSWERED" ARE DIFFERENT ANSWERS (§1.4), and telling them apart is not the
        // one-liner it looks like — see PinIsProvablyGone.
        pins.erase( std::remove_if( pins.begin(), pins.end(), [&root]( const std::string& kept )
                                    { return PinIsProvablyGone( kept == "." ? root : root / kept ); } ),
                    pins.end() );

        // An empty list is not a project with no pins, it is a key with nothing behind it. Erasing it is
        // what keeps a user who tried the feature once from carrying that project's name for ever.
        if ( pins.empty() )
            Get().FavouriteFolders.erase( project );

        Save();
    }

    bool EditorPreferences::Save()
    {
        return PersistCurrent( {} );
    }

    bool EditorPreferences::SaveMigrated( const std::string& what )
    {
        return PersistCurrent( what );
    }
} // namespace Desert::Editor
