#include "MachineSettings.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace Common::Settings
{
    namespace
    {
        // The file Load() was given. Empty until it is called, and Save() refuses on an empty one rather
        // than inventing a location — a store with no path is not a store.
        std::filesystem::path s_File;

        // WHAT THIS PROCESS BELIEVES THE FILE HOLDS, in the canonical text Save() writes. Empty means
        // "there is no file" or "we cannot claim to know" (a failed read, a corrupt parse), in which case
        // the next write must happen unconditionally.
        //
        // A MEMO, NOT A SECOND STORE: nothing ever reads a setting out of it; it is only compared, as
        // text, against what is about to be written. The struct stays the sole authority on every value,
        // so the worst a stale memo can do is skip a write — and the re-read below is what keeps it from
        // being stale in the ordinary case.
        std::string s_OnDisk;

        // Which top-level keys differ between two serializations of this file, by name. The key list is
        // the UNION of what the two texts actually contain rather than the fields MachineSettings declares,
        // because a key held by another build is a key of this file and this is the function that has to
        // be able to say one appeared or went away.
        std::vector<std::string> ChangedKeys( const std::string& before, const std::string& after )
        {
            const auto lhs = Json::ObjectMembers( before );
            const auto rhs = Json::ObjectMembers( after );
            if ( !lhs || !rhs )
                return {};

            const auto valueOf = []( const auto& members, const std::string& key ) -> const std::string*
            {
                for ( const auto& [name, value] : members )
                    if ( name == key )
                        return &value;
                return nullptr;
            };

            std::vector<std::string> keys;
            for ( const auto& [name, value] : lhs.GetValue() )
                keys.push_back( name );
            for ( const auto& [name, value] : rhs.GetValue() )
                if ( std::find( keys.begin(), keys.end(), name ) == keys.end() )
                    keys.push_back( name );

            std::vector<std::string> differing;
            for ( const std::string& key : keys )
            {
                const std::string* a = valueOf( lhs.GetValue(), key );
                const std::string* b = valueOf( rhs.GetValue(), key );
                if ( a == nullptr || b == nullptr || *a != *b )
                    differing.push_back( key );
            }
            return differing;
        }

        // THE EVENT, not the call: which settings moved, taken from the diff against what is already on
        // disk. "Saved <path>" says only that this function ran, which is the least informative thing it
        // knows.
        std::string DescribeChange( const std::string& before, const std::string& after )
        {
            if ( before.empty() )
                return "created";

            const auto changed = ChangedKeys( before, after );
            if ( changed.empty() )
            {
                // The texts differ but no key does: only reachable if the JSON writer's own output
                // changes between builds. Said plainly rather than reported as a settings change.
                return "reserialized";
            }

            std::string list = changed.front();
            for ( std::size_t i = 1; i < changed.size(); ++i )
                list += ", " + changed[i];
            return list;
        }

        // WHAT THE FILE HOLDS AT THIS INSTANT, and the unknown keys taken from it — refreshed into
        // Get().UnknownKeys. Returns the file's canonical text, or nothing when there is no file or it
        // could not be read or parsed.
        //
        // A LOAD-TIME SNAPSHOT IS NOT ENOUGH, and this file has two hosts rather than one. The editor
        // starts at 10:00 with a carrier that has never heard of a key a newer build writes at 10:30; a
        // save at 11:00 from that carrier would delete it. So the keys another build owns are re-read at
        // the moment of WRITING.
        //
        // IT ADOPTS KEYS AND NEVER VALUES: every field this struct declares stays exactly as the user
        // left it, so two hosts disagreeing about MeshLOD still resolve last-writer-wins. Adopting a
        // field would be a save that changes a setting the user did not touch.
        std::optional<std::string> AdoptUnknownKeysFromDisk()
        {
            if ( !std::filesystem::exists( s_File ) )
                return std::nullopt;

            const auto raw = Common::Utils::FileSystem::ReadFileContent( s_File );
            if ( !raw || raw.GetValue().empty() )
            {
                LOG_WARN( "[Machine] {} exists but could not be read before saving; keys written by "
                          "another build cannot be preserved this time and the file will be replaced "
                          "wholesale.",
                          s_File.string() );
                return std::nullopt;
            }

            auto parsed = Json::Read<MachineSettings>( raw.GetValue() );
            if ( !parsed )
            {
                LOG_WARN( "[Machine] {} is corrupt ({}); it is being replaced rather than merged, so any "
                          "key another build put in it is lost with it.",
                          s_File.string(), parsed.GetError() );
                return std::nullopt;
            }

            MachineSettings fromDisk = parsed.ExtractValue();

            const std::string canonical        = Json::Write( fromDisk );
            MachineSettings::Get().UnknownKeys = std::move( fromDisk.UnknownKeys );
            return canonical;
        }
    } // namespace

    MachineSettings& MachineSettings::Get()
    {
        static MachineSettings s_Instance;
        return s_Instance;
    }

    const std::filesystem::path& MachineSettings::File()
    {
        return s_File;
    }

    void MachineSettings::Load( const std::filesystem::path& file )
    {
        s_File = file;

        // Anything short of a file we read AND understood leaves the memo empty, which means "we cannot
        // claim the disk agrees with us" and forces the next save to write. That is the safe direction.
        s_OnDisk.clear();

        // This machine has never chosen anything: the defaults stand, and no error line — an absent file
        // is the expected first-run state, not a failure.
        if ( !std::filesystem::exists( s_File ) )
            return;

        const auto raw = Common::Utils::FileSystem::ReadFileContent( s_File );
        if ( !raw )
        {
            // The file EXISTS — tested above — so a failed read is a permission or I/O problem, and
            // running on defaults without saying so is the silent fallback §1.4 forbids.
            LOG_ERROR( "[Machine] {} exists but could not be read: {} — this session runs at the default "
                       "quality, and the next change will overwrite the file.",
                       s_File.string(), raw.GetError() );
            return;
        }
        if ( raw.GetValue().empty() )
        {
            LOG_WARN( "[Machine] {} is empty; using the default quality.", s_File.string() );
            return;
        }

        // Read LENIENTLY, by the type's own DESERT_JSON_LENIENT mark: a file written by a build with fewer
        // fields keeps loading — a new field takes its in-struct default instead of failing the whole file.
        auto parsed = Json::Read<MachineSettings>( raw.GetValue() );
        if ( !parsed )
        {
            LOG_WARN( "[Machine] {} is corrupt, using the default quality: {}", s_File.string(),
                      parsed.GetError() );
            return;
        }

        Get() = parsed.ExtractValue();
        // The canonical form of what the file holds, NOT the raw bytes: an older build's key order or
        // spacing is not a settings change, and a memo taken from the raw text would report the whole
        // struct as changed on the first save after an upgrade.
        s_OnDisk = Json::Write( Get() );

        // NAMED, not numbered: this line is what a support ticket's engine_log.txt has to answer "what
        // was this machine actually rendering at" with, and `2` is not an answer. The names come from the
        // same rfl call that writes them to the file, so the log and the file cannot disagree.
        LOG_INFO( "[Machine] {} — MSAA {}x, post AA {}, mesh LOD {}, filter {} {}x, clouds {}", s_File.string(),
                  Get().MSAASamples, rfl::enum_to_string( Get().AA ), Get().MeshLOD ? "on" : "off",
                  rfl::enum_to_string( Get().TextureFilterMode ), Get().Anisotropy,
                  rfl::enum_to_string( Get().CloudQualityTier ) );

        // CARRYING A KEY WE DO NOT UNDERSTAND IS AN EVENT, NOT A DETAIL: another build owns settings this
        // binary cannot show or edit. Named rather than counted, because a count tells a reader nothing
        // about whether to go and look for the build that wrote them.
        if ( !Get().UnknownKeys.empty() )
        {
            std::string names = Get().UnknownKeys.begin()->first;
            for ( auto it = std::next( Get().UnknownKeys.begin() ); it != Get().UnknownKeys.end(); ++it )
                names += ", " + it->first;

            LOG_INFO( "[Machine] {} holds {} key(s) this build does not know ({}); they belong to another "
                      "build and are preserved on save, not dropped.",
                      s_File.string(), Get().UnknownKeys.size(), names );
        }
    }

    bool MachineSettings::Save()
    {
        if ( s_File.empty() )
        {
            LOG_ERROR( "[Machine] a quality change was not saved: this process never called "
                       "MachineSettings::Load(), so there is no file to write. The change applies to this "
                       "session only." );
            return false;
        }

        // Whatever another build has put in the file since this one read it, and what the file actually
        // says right now — which is what makes the skip below honest.
        if ( const std::optional<std::string> onDisk = AdoptUnknownKeysFromDisk(); onDisk.has_value() )
            s_OnDisk = *onDisk;
        else if ( std::filesystem::exists( s_File ) )
            s_OnDisk.clear();

        const std::string json = Json::Write( Get() );
        if ( json == s_OnDisk && std::filesystem::exists( s_File ) )
        {
            // The file already says exactly this. No write and no log line about one. The exists() is not
            // belt and braces: without it `true` would mean "the memo says the disk agrees" rather than
            // "the disk agrees", and a file removed behind a running editor would never be rebuilt.
            return true;
        }

        const std::string event = DescribeChange( s_OnDisk, json );

        std::error_code ec;
        std::filesystem::create_directories( s_File.parent_path(), ec );

        const auto written = Json::WriteFileAtomic( s_File, Get() );
        if ( !written )
        {
            // The memo is deliberately NOT updated: the next save must try again rather than assume the
            // disk agrees with us.
            LOG_ERROR( "[Machine] {} was NOT saved: {} — this quality applies to this session only.",
                       s_File.string(), written.GetError() );
            return false;
        }

        s_OnDisk = json;
        LOG_INFO( "[Machine] {} -> {}", event, s_File.string() );
        return true;
    }

    std::filesystem::path GameUserDirectory( const std::string& product )
    {
        // A `.deproj` Name reaches a PATH here, so it is sanitised rather than trusted: separators and
        // the two relative names are what turn a product name into a write somewhere else entirely.
        std::string safe;
        for ( const char c : product )
            safe += ( c == '/' || c == '\\' || c == ':' ) ? '_' : c;
        if ( safe.empty() || safe == "." || safe == ".." )
            safe = "DesertGame";

        // ASKED OF THE COMPILER, NOT OF THE BUILD SYSTEM. `DESERT_PLATFORM_*` comes from a per-project
        // premake block that every test suite forgets (Common/Core/Core.hpp §DESERT_DEBUG_BREAK records
        // what that cost there), and a suite compiled without it would silently be told this machine is
        // Linux. `_WIN32` and `__APPLE__` are the compiler's own and cannot go missing.
        const char* home = std::getenv( "HOME" );
#if defined( _WIN32 )
        if ( !home )
            home = std::getenv( "USERPROFILE" );
        const char*           appData = std::getenv( "APPDATA" );
        std::filesystem::path base    = appData ? std::filesystem::path( appData )
                                                : std::filesystem::path( home ? home : "." ) / "AppData" / "Roaming";
#elif defined( __APPLE__ )
        std::filesystem::path base =
             std::filesystem::path( home ? home : "." ) / "Library" / "Application Support";
#else
        const char*           dataHome = std::getenv( "XDG_DATA_HOME" );
        std::filesystem::path base     = dataHome ? std::filesystem::path( dataHome )
                                                  : std::filesystem::path( home ? home : "." ) / ".local" / "share";
#endif

        std::filesystem::path dir = base / safe;
        std::error_code       ec;
        std::filesystem::create_directories( dir, ec );
        return dir;
    }
} // namespace Common::Settings
