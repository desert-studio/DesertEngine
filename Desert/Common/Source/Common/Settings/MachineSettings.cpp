#include "MachineSettings.hpp"
#include "ProductName.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Template.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <variant>
#include <format>
#include <iterator>
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

        // The Quality.Levels groups the last Load() refused (ScreenLevels). Read by the host through
        // MachineSettings::UnreadLevels() into StartFrom; a load-time fact about the file, never written.
        MachineSettings::LevelGroups s_UnreadLevels;

        // The last Load() migrated Quality.Levels (MachineSettings::LevelsMigrated).
        bool s_LevelsMigrated = false;

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

            return Common::Text::Join( changed, ", " );
        }

        // ONE ENTRY OF THE FILE THAT WAS NOT TAKEN AS WRITTEN, as the sentence the log prints after the file name.
        // Dropped = the entry is not in the loaded settings (the next saved change writes the file without it);
        // otherwise it is kept and carried, only refused its effect (an override naming a parameter of another
        // build).
        struct Rejection
        {
            std::string Problem;
            bool        Dropped = true;
        };

        // What the file says, entry by entry. A syntax error, or a document that is not an object, is the one
        // refusal of the whole file: nothing in it can be located. Anything else is refused BY ENTRY, so one
        // misspelt override does not take the rest of the machine's choice down with it (RT1016-MAC-2B: an
        // override written {"Parameter": ...} sent a whole Epic selection to the defaults).
        struct ScreenedFile
        {
            MachineSettings        Settings;
            std::vector<Rejection> Rejections;
            // The Quality.Levels groups whose element was refused (ScreenLevels); empty when Quality was refused.
            MachineSettings::LevelGroups UnreadLevels;
            // WARN lines (a group the file does not name) and whether Quality.Levels was migrated to this build's
            // form (the positional list, or a missing group): the host then writes the file back.
            std::vector<std::string> Warnings;
            bool                     LevelsMigrated = false;
        };

        constexpr std::string_view kOverrideForm =
             R"(an override is {"Key": "<Group.Parameter>", "Value": <integer>})";

        // One element of Quality.Overrides: kept, kept with an error (a key this build has no parameter for), or
        // dropped with the reason (a field that is not Key/Value, a Value that is not an integer, no Key, a Value
        // outside the parameter's spec range).
        std::optional<Rejection> ScreenOverride( const Json::Node& entry )
        {
            const std::string text = Json::Write( entry.Raw() );
            auto              read = entry.As<Scalability::ParameterOverride>();
            if ( !read )
                return Rejection{
                     std::format( "override {} is ignored: {} ({})", text, read.GetError(), kOverrideForm ) };

            const Scalability::ParameterOverride& value = read.GetValue();
            if ( value.Key.empty() )
                return Rejection{ std::format( "{}: override {} is ignored: it has no Key ({})",
                                               entry.Where().ToString(), text, kOverrideForm ) };

            const auto specs = Scalability::ParameterSpecs();
            const auto spec  = std::find_if( specs.begin(), specs.end(), [&]( const Scalability::ParameterSpec& s )
                                             { return s.Key == value.Key; } );
            if ( spec == specs.end() )
                return Rejection{
                     std::format( "{}: override {} names no parameter of this build; it is not applied "
                                  "and stays in the file for the build that wrote it",
                                  entry.Where().ToString(), text ),
                     false };

            if ( value.Value < spec->Min || value.Value > spec->Max )
                return Rejection{
                     std::format( "{}: override {} is ignored: {} is outside {}'s range [{}, {}]; the {} "
                                  "level's value stands",
                                  entry.Where().ToString(), text, value.Value, spec->Key, spec->Min, spec->Max,
                                  Scalability::GroupKey( spec->Owner ) ) };
            return std::nullopt;
        }

        // Quality.Overrides with every dropped override taken out. Overrides that is not an array comes back as
        // written: the member check refuses it whole.
        Json::Value ScreenOverrides( const Json::Node& overrides, std::vector<Rejection>& rejections )
        {
            if ( overrides.GetKind() != Json::Kind::Array )
                return overrides.Raw();

            Json::Value::Array kept;
            overrides.ForEachElement(
                 [&]( std::size_t, const Json::Node& entry )
                 {
                     std::optional<Rejection> rejection = ScreenOverride( entry );
                     if ( !rejection || !rejection->Dropped )
                         kept.push_back( entry.Raw() );
                     if ( rejection )
                         rejections.push_back( std::move( *rejection ) );
                 } );
            return Json::Value( std::move( kept ) );
        }

        // What ScreenLevels reports besides the screened value.
        struct LevelsScreen
        {
            std::vector<Rejection>&       Rejections;
            std::vector<std::string>&     Warnings;
            MachineSettings::LevelGroups& Unread;
            bool&                         Migrated;
        };

        std::optional<std::size_t> GroupIndex( std::string_view key )
        {
            for ( std::size_t g = 0; g < Scalability::kGroupCount; ++g )
                if ( Scalability::GroupKey( static_cast<Scalability::Group>( g ) ) == key )
                    return g;
            return std::nullopt;
        }

        // Quality.Levels read PER GROUP, BY GROUP NAME ({"Textures": "High", ...}, Scalability::GroupLevels):
        //   - a name that is no group of this build is an ERROR and is dropped (the next write leaves it out);
        //   - a value that is not a level name is an ERROR and marks the group in `unread`;
        //   - a group the file does not name is a WARN and marks it in `unread` (a group this build added);
        // MachineSettings::StartFrom gives an unread group the level this device starts on with no choice, so the
        // other groups and the overrides stand. The marked group is written as High only to keep the object
        // readable — the mask, not that value, is what StartFrom reads.
        // THE FORMER POSITIONAL LIST (a level per group by enum position) is migrated here: element i is group i,
        // a group past the list's end is missing as above, and the load is marked migrated so the host rewrites
        // the file by name. A list longer than the group count cannot be matched to groups and comes back as
        // written: the member check refuses Quality whole. Anything else also comes back as written.
        Json::Value ScreenLevels( const Json::Node& levels, LevelsScreen out )
        {
            std::array<std::optional<Json::Value>, Scalability::kGroupCount> byGroup;
            const auto readLevel = [&]( std::size_t g, const Json::Node& entry )
            {
                const std::string_view group = Scalability::GroupKey( static_cast<Scalability::Group>( g ) );
                if ( entry.As<Scalability::Level>() )
                {
                    byGroup[g] = entry.Raw();
                    return;
                }
                std::string names;
                for ( std::size_t l = 0; l < Scalability::kLevelCount; ++l )
                    std::format_to( std::back_inserter( names ), "{}{}", l == 0 ? "" : ", ",
                                    Scalability::LevelKey( static_cast<Scalability::Level>( l ) ) );
                out.Rejections.push_back( { std::format(
                     "{}: the {} group's level {} is not a level ({}); the {} group runs at the level this "
                     "device starts on with no choice (the benchmark's recommendation when it is valid "
                     "for this device, else High), the other groups and the overrides stand",
                     entry.Where().ToString(), group, Json::Write( entry.Raw() ), names, group ) } );
                out.Unread.set( g );
            };

            if ( levels.GetKind() == Json::Kind::Array )
            {
                std::size_t elements = 0;
                levels.ForEachElement( [&]( std::size_t, const Json::Node& ) { ++elements; } );
                if ( elements > Scalability::kGroupCount )
                    return levels.Raw();
                out.Migrated = true;
                levels.ForEachElement( readLevel );
            }
            else if ( levels.GetKind() == Json::Kind::Object )
            {
                levels.ForEachMember(
                     [&]( std::string_view name, const Json::Node& entry )
                     {
                         if ( const std::optional<std::size_t> g = GroupIndex( name ) )
                         {
                             readLevel( *g, entry );
                             return;
                         }
                         out.Rejections.push_back(
                              { std::format( "{}: Quality.Levels names '{}', which is no quality group of this "
                                             "build; it is ignored",
                                             entry.Where().ToString(), name ) } );
                     } );
            }
            else
                return levels.Raw();

            Json::ObjectBuilder rebuilt;
            for ( std::size_t g = 0; g < Scalability::kGroupCount; ++g )
            {
                const std::string_view group = Scalability::GroupKey( static_cast<Scalability::Group>( g ) );
                if ( !byGroup[g] && !out.Unread.test( g ) )
                {
                    out.Warnings.push_back(
                         std::format( "Quality.Levels has no level for the {} group; it runs at the level this "
                                      "device starts on with no choice and the file is rewritten with it",
                                      group ) );
                    out.Unread.set( g );
                    out.Migrated = true;
                }
                rebuilt.Set( group, byGroup[g] ? *byGroup[g]
                                               : Json::Value( std::string(
                                                      Scalability::LevelKey( Scalability::Level::High ) ) ) );
            }
            return Json::Value( rebuilt.Build() );
        }

        // Recommended.Levels (the benchmark cache) in the former positional list form becomes the by-name object
        // (element i is group i). A cache that misses a group then refuses at the member read and is measured
        // again: it is a cache, the benchmark is its source.
        Json::Value ScreenRecommended( const Json::Node& recommended, bool& migrated )
        {
            if ( recommended.GetKind() != Json::Kind::Object )
                return recommended.Raw();
            Json::ObjectBuilder rebuilt;
            recommended.ForEachMember(
                 [&]( std::string_view name, const Json::Node& member )
                 {
                     std::size_t elements = 0;
                     member.ForEachElement( [&]( std::size_t, const Json::Node& ) { ++elements; } );
                     if ( name != "Levels" || member.GetKind() != Json::Kind::Array ||
                          elements > Scalability::kGroupCount )
                     {
                         rebuilt.Set( name, member.Raw() );
                         return;
                     }
                     migrated = true;
                     Json::ObjectBuilder levels;
                     member.ForEachElement(
                          [&]( std::size_t g, const Json::Node& entry )
                          { levels.Set( Scalability::GroupKey( static_cast<Scalability::Group>( g ) ), entry.Raw() ); } );
                     rebuilt.Set( name, Json::Value( levels.Build() ) );
                 } );
            return Json::Value( rebuilt.Build() );
        }

        // The Quality member with its Levels and Overrides screened. A Quality that is not an object comes back as
        // written: the member check refuses it whole.
        Json::Value ScreenQuality( const Json::Node& quality, std::vector<Rejection>& rejections,
                                   LevelsScreen levelsOut )
        {
            if ( quality.GetKind() != Json::Kind::Object )
                return quality.Raw();

            Json::ObjectBuilder rebuilt;
            quality.ForEachMember(
                 [&]( std::string_view name, const Json::Node& member )
                 {
                     if ( name == "Levels" )
                         rebuilt.Set( name, ScreenLevels( member, levelsOut ) );
                     else if ( name == "Overrides" )
                         rebuilt.Set( name, ScreenOverrides( member, rejections ) );
                     else
                         rebuilt.Set( name, member.Raw() );
                 } );
            return Json::Value( rebuilt.Build() );
        }

        Common::ResultStr<ScreenedFile> ScreenFile( std::string_view text )
        {
            auto parsed = Json::Parse( text );
            if ( !parsed )
                return MakeError<ScreenedFile>( parsed.GetError() );
            const Json::Value& document = parsed.GetValue();
            const Json::Node   root     = Json::Root( document );
            if ( root.GetKind() != Json::Kind::Object )
                return MakeError<ScreenedFile>(
                     std::format( "the document is {}, not an object", Json::Detail::DescribeFound( document ) ) );

            // Every top-level member is read ALONE against the struct (a key the struct does not declare lands in
            // UnknownKeys, as on the whole-file read); a member that does not read is refused by name and keeps
            // its in-struct default, the others stand.
            ScreenedFile        screened;
            Json::ObjectBuilder kept;
            root.ForEachMember(
                 [&]( std::string_view name, const Json::Node& member )
                 {
                     const Json::Value value =
                          name == "Quality"       ? ScreenQuality( member, screened.Rejections,
                                                                   { screened.Rejections, screened.Warnings,
                                                                     screened.UnreadLevels, screened.LevelsMigrated } )
                          : name == "Recommended" ? ScreenRecommended( member, screened.LevelsMigrated )
                                                  : member.Raw();
                     const Json::Value alone( Json::ObjectBuilder().Set( name, value ).Build() );
                     if ( auto read = Json::Root( alone ).As<MachineSettings>(); !read )
                     {
                         screened.Rejections.push_back( { std::format( "{} is ignored and keeps its default: {}",
                                                                       name, read.GetError() ) } );
                         if ( name == "Quality" )
                             screened.UnreadLevels.reset();
                         return;
                     }
                     kept.Set( name, value );
                 } );

            const Json::Value accepted( kept.Build() );
            auto              settings = Json::Root( accepted ).As<MachineSettings>();
            if ( !settings )
                return MakeError<ScreenedFile>( settings.GetError() );
            screened.Settings = settings.ExtractValue();
            return MakeSuccess( std::move( screened ) );
        }

        std::size_t CountDropped( const std::vector<Rejection>& rejections )
        {
            return static_cast<std::size_t>( std::count_if( rejections.begin(), rejections.end(),
                                                            []( const Rejection& r ) { return r.Dropped; } ) );
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
                LOG_ERROR( "[Machine] {} exists but could not be read before saving; keys written by "
                           "another build cannot be preserved this time and the file will be replaced "
                           "wholesale.",
                           s_File.string() );
                return std::nullopt;
            }

            auto parsed = ScreenFile( raw.GetValue() );
            if ( !parsed )
            {
                LOG_ERROR( "[Machine] {} is corrupt ({}); it is being replaced rather than merged, so any "
                           "key another build put in it is lost with it.",
                           s_File.string(), parsed.GetError() );
                return std::nullopt;
            }

            // The entries Load() refused (and logged one by one) are not part of the canonical text, so a save
            // that changes a setting writes the file without them; one that changes nothing leaves the file alone.
            if ( const std::size_t dropped = CountDropped( parsed.GetValue().Rejections ); dropped > 0 )
                LOG_WARN( "[Machine] {} holds {} entr{} refused at load; a write now leaves {} out.",
                          s_File.string(), dropped, dropped == 1 ? "y" : "ies", dropped == 1 ? "it" : "them" );

            ScreenedFile    screened = parsed.ExtractValue();
            MachineSettings fromDisk = std::move( screened.Settings );

            // A file whose Quality.Levels had to be MIGRATED (the former positional list, a missing group) does
            // not say what its canonical form says: an empty memo, so the write-back is not skipped as a no-op.
            const std::string canonical        = screened.LevelsMigrated ? std::string{} : Json::Write( fromDisk );
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
        s_UnreadLevels.reset();
        s_LevelsMigrated = false;

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
            // A machine config that is THERE and says nothing is broken, not a first run (UE GameUserSettings:
            // no file = first launch, a file that does not load = an error). ERROR, then the defaults.
            LOG_ERROR( "[Machine] {} is empty — this session runs at the default quality, and the next change "
                       "will overwrite the file.",
                       s_File.string() );
            return;
        }

        // A file written by a build with fewer fields keeps loading — Json::Read gives every field the file does
        // not state its in-struct default; keys of a build with MORE fields land in UnknownKeys.
        // An entry that does not read is refused BY ENTRY (ScreenFile), each one an ERROR naming the file, the
        // entry's path and the reason; only a file with no locatable entries (a syntax error) falls to the
        // defaults.
        auto parsed = ScreenFile( raw.GetValue() );
        if ( !parsed )
        {
            // The parse error names the offset that refused; the path names the file.
            LOG_ERROR( "[Machine] {} is corrupt: {} — this session runs at the default quality, and the next "
                       "change will overwrite the file.",
                       s_File.string(), parsed.GetError() );
            return;
        }

        ScreenedFile screened = parsed.ExtractValue();
        s_UnreadLevels        = screened.UnreadLevels;
        s_LevelsMigrated      = screened.LevelsMigrated && screened.Settings.Quality.has_value();
        for ( const std::string& warning : screened.Warnings )
            LOG_WARN( "[Machine] {}: {}", s_File.string(), warning );
        for ( const Rejection& rejection : screened.Rejections )
            LOG_ERROR( "[Machine] {}: {}{}", s_File.string(), rejection.Problem,
                       rejection.Dropped ? " — the next saved change writes the file without it." : "." );
        Get() = std::move( screened.Settings );
        // The canonical form of what the file holds, NOT the raw bytes: an older build's key order or
        // spacing is not a settings change, and a memo taken from the raw text would report the whole
        // struct as changed on the first save after an upgrade.
        s_OnDisk = Json::Write( Get() );

        // NAMED, not numbered: this line is what a support ticket's engine_log.txt has to answer "what was this
        // machine asking for" with. What it actually RUNS is QualityState's to say (it knows the device).
        if ( const auto& quality = Get().Quality )
        {
            std::string levels;
            for ( std::size_t g = 0; g < Scalability::kGroupCount; ++g )
                std::format_to( std::back_inserter( levels ), "{}{} {}", g == 0 ? "" : ", ",
                                Scalability::GroupKey( static_cast<Scalability::Group>( g ) ),
                                s_UnreadLevels.test( g ) ? std::string_view( "unread" )
                                                         : Scalability::LevelKey( quality->Levels[g] ) );
            LOG_INFO( "[Machine] {} — quality {}; {} override(s); VSync {}", s_File.string(), levels,
                      quality->Overrides.size(), Get().Display.VSync ? "on" : "off" );
        }
        else
            LOG_INFO( "[Machine] {} — no quality chosen yet; VSync {}", s_File.string(),
                      Get().Display.VSync ? "on" : "off" );

        // CARRYING A KEY WE DO NOT UNDERSTAND IS AN EVENT, NOT A DETAIL: another build owns settings this
        // binary cannot show or edit. Named rather than counted, because a count tells a reader nothing
        // about whether to go and look for the build that wrote them.
        if ( !Get().UnknownKeys.empty() )
        {
            const std::string names = Common::Text::Join( Get().UnknownKeys, ", ",
                                                          []( const auto& unknown ) { return unknown.first; } );

            LOG_WARN( "[Machine] {} holds {} key(s) this build does not know ({}); they belong to another "
                      "build and are preserved on save, not dropped.",
                      s_File.string(), Get().UnknownKeys.size(), names );
        }
    }

    MachineSettings::StartingQuality MachineSettings::StartFrom( const MachineSettings& settings,
                                                                 const LevelGroups&     unreadLevels,
                                                                 const Scalability::BenchmarkCacheKey& device )
    {
        // The answer for a machine that never chose: the benchmark's levels when they are valid for this device.
        StartingQuality noChoice{ HighSelection(), false };
        if ( const auto& cached = settings.Recommended;
             cached.has_value() && Scalability::CacheValid( cached, device ) )
            noChoice = { Scalability::QualitySelection{ cached->Levels, {} }, true };

        if ( !settings.Quality )
            return noChoice;
        Scalability::QualitySelection saved = *settings.Quality;
        for ( std::size_t g = 0; g < Scalability::kGroupCount; ++g )
            if ( unreadLevels.test( g ) )
                saved.Levels[g] = noChoice.Selection.Levels[g];
        return { std::move( saved ), false };
    }

    const MachineSettings::LevelGroups& MachineSettings::UnreadLevels()
    {
        return s_UnreadLevels;
    }

    bool MachineSettings::LevelsMigrated()
    {
        return s_LevelsMigrated;
    }

    Scalability::QualitySelection MachineSettings::HighSelection()
    {
        Scalability::QualitySelection selection;
        selection.Levels.fill( Scalability::Level::High );
        return selection;
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
        // A `.deproj` Name reaches a PATH here, so it goes through the one rule the packager also names
        // the package folder with (ProductName.hpp) rather than being trusted.
        const std::string safe = SanitizeProductName( product );

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
