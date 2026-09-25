// THE DECISIVE RELATION OF THIS WHOLE SUBSYSTEM, and it has two directions that fail independently:
//
//     changing the language MUST change what a keyed element draws
//     changing the language MUST NOT change what a literal element draws
//
// One of them alone proves nothing. A build that translated everything it could find would pass the
// first and destroy every probe scene; a build that translated nothing would pass the second. Both
// directions are asserted here, over the same two strings, in the same test.
//
// The rest of the suite is about the ways a table can be wrong, and every one of them is a refusal that
// names the offending value rather than a default that hides it.

#include <rflcpp/rfl/json.hpp>
#include <gtest/gtest.h>

#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Localization/StringTable.hpp>

#include <map>
#include <string>
#include <string_view>

using namespace Desert::Localization;

// Since STRT 2 (T7b) a table opens with the text asset header; a fixture states its payload and gets the header
// this build writes, so the tests below keep testing what they name rather than the header.
static std::string Headed( const std::string& json )
{
    const std::string header = rfl::json::write(
         Desert::Assets::StampTextHeader( std::nullopt, Common::Content::ContentKind::StringTable,
                                          Desert::Localization::StringTableTextSubsystems() ) );
    const std::size_t brace = json.find( '{' );
    return json.substr( 0, brace + 1 ) + "\"Header\":" + header + "," + json.substr( brace + 1 );
}

namespace
{
    // Two keys and two languages: a plain label, and a counted noun whose Russian needs three forms.
    const char* kTable = R"({
      "DisplayName": "Suite fixture",
      "Entries": [
        { "Key": "menu.play",   "Forms": { "en": { "other": "PLAY" },
                                           "ru": { "other": "\u0418\u0413\u0420\u0410\u0422\u042C" } } },
        { "Key": "files.count", "Forms": { "en": { "one": "{n} file", "other": "{n} files" },
                                           "ru": { "one":   "{n} \u0444\u0430\u0439\u043B",
                                                   "few":   "{n} \u0444\u0430\u0439\u043B\u0430",
                                                   "many":  "{n} \u0444\u0430\u0439\u043B\u043E\u0432",
                                                   "other": "{n} \u0444\u0430\u0439\u043B\u0430" } } },
        { "Key": "joined",      "Forms": { "ru": { "masculine": "\u0432\u043E\u0448\u0451\u043B",
                                                   "feminine":  "\u0432\u043E\u0448\u043B\u0430",
                                                   "other":     "\u0432\u043E\u0448\u0451\u043B" },
                                           "en": { "other": "joined" } } }
      ]})";

    // The service is a process-wide singleton, exactly like UI::UIDataStore next door. Every test puts it
    // back the way it found it, or the next one is testing the previous one's leftovers.
    struct Fixture : public ::testing::Test
    {
        void SetUp() override
        {
            Localization::Get().Clear();
            ASSERT_TRUE( Localization::Get().SetLanguage( Localization::kSourceLanguage ) );
            auto parsed = ParseStringTable( Headed( kTable ) );
            ASSERT_TRUE( parsed ) << parsed.GetError();
            ASSERT_TRUE( Localization::Get().RegisterTable( "suite.destrings", parsed.ExtractValue() ) );
        }

        void TearDown() override
        {
            Localization::Get().Clear();
            EXPECT_TRUE( Localization::Get().SetLanguage( Localization::kSourceLanguage ) );
        }
    };
} // namespace

TEST_F( Fixture, ALanguageChangeMovesAKeyAndCannotMoveALiteral )
{
    Localization& loc = Localization::Get();

    const std::string keyedEnglish   = loc.Resolve( "#menu.play" ).Text;
    const std::string literalEnglish = loc.Resolve( "PLAY" ).Text;
    EXPECT_EQ( keyedEnglish, "PLAY" );
    EXPECT_EQ( literalEnglish, "PLAY" );

    ASSERT_TRUE( loc.SetLanguage( "ru" ) );

    const std::string keyedRussian   = loc.Resolve( "#menu.play" ).Text;
    const std::string literalRussian = loc.Resolve( "PLAY" ).Text;

    // Direction one: the key moved.
    EXPECT_NE( keyedRussian, keyedEnglish );
    EXPECT_EQ( keyedRussian, "\xD0\x98\xD0\x93\xD0\xA0\xD0\x90\xD0\xA2\xD0\xAC" );
    // Direction two: the literal did not. The two authored strings were IDENTICAL in English, so nothing
    // but the sigil can be what separated them.
    EXPECT_EQ( literalRussian, literalEnglish );

    EXPECT_EQ( loc.Resolve( "#menu.play" ).Outcome, Localization::Outcome::Translated );
    EXPECT_EQ( loc.Resolve( "PLAY" ).Outcome, Localization::Outcome::Literal );
}

TEST_F( Fixture, RussianCountsGetTheRightFormThroughTheWholeStack )
{
    Localization& loc = Localization::Get();
    ASSERT_TRUE( loc.SetLanguage( "ru" ) );

    auto say = [&loc]( const double n )
    {
        FormatArguments args;
        args.Count = n;
        return loc.Resolve( "#files.count", args ).Text;
    };

    // The six the brief named. This is the same relation PluralRules pins, asserted here THROUGH the
    // table and the placeholder substitution — the selector being right is not the same claim as the
    // right form reaching a string.
    EXPECT_EQ( say( 1 ), "1 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB" );
    EXPECT_EQ( say( 2 ), "2 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0" );
    EXPECT_EQ( say( 5 ), "5 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2" );
    EXPECT_EQ( say( 11 ), "11 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2" );
    EXPECT_EQ( say( 21 ), "21 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB" );
    EXPECT_EQ( say( 111 ), "111 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2" );

    // And the English table, whose author only had to write two forms, still works.
    ASSERT_TRUE( loc.SetLanguage( "en" ) );
    EXPECT_EQ( say( 1 ), "1 file" );
    EXPECT_EQ( say( 21 ), "21 files" );
}

TEST_F( Fixture, GenderIsTheCALLERSFactAndTheTranslATORSChoice )
{
    Localization& loc = Localization::Get();
    ASSERT_TRUE( loc.SetLanguage( "ru" ) );

    FormatArguments masculine;
    masculine.Subject = Gender::Masculine;
    FormatArguments feminine;
    feminine.Subject = Gender::Feminine;

    EXPECT_EQ( loc.Resolve( "#joined", masculine ).Text, "\xD0\xB2\xD0\xBE\xD1\x88\xD1\x91\xD0\xBB" );
    EXPECT_EQ( loc.Resolve( "#joined", feminine ).Text, "\xD0\xB2\xD0\xBE\xD1\x88\xD0\xBB\xD0\xB0" );

    // English does not distinguish, and its translator did not have to say so: one `other` answers both.
    ASSERT_TRUE( loc.SetLanguage( "en" ) );
    EXPECT_EQ( loc.Resolve( "#joined", masculine ).Text, "joined" );
    EXPECT_EQ( loc.Resolve( "#joined", feminine ).Text, "joined" );
}

TEST( LocalizedText, TheFormLadderIsAnORDEREDLookupAndNotASearch )
{
    const std::map<std::string, std::string> full = {
         { "feminine.one", "A" }, { "feminine", "B" }, { "one", "C" }, { "other", "D" } };

    EXPECT_EQ( *SelectForm( full, PluralCategory::One, Gender::Feminine ), "A" );
    EXPECT_EQ( *SelectForm( full, PluralCategory::Many, Gender::Feminine ), "B" );
    EXPECT_EQ( *SelectForm( full, PluralCategory::One, Gender::Unspecified ), "C" );
    EXPECT_EQ( *SelectForm( full, PluralCategory::Many, Gender::Unspecified ), "D" );
    // A caller who says nothing about gender must NOT reach a gendered form. Picking one would be the
    // engine deciding the sex of somebody's player character.
    EXPECT_EQ( *SelectForm( { { "feminine", "B" }, { "other", "D" } }, PluralCategory::One, Gender::Unspecified ),
               "D" );
    // Nothing at all is a miss, not the first entry that happens to be there.
    EXPECT_EQ( SelectForm( { { "few", "x" } }, PluralCategory::One, Gender::Unspecified ), nullptr );
}

TEST( LocalizedText, TheSigilSeparatesAKeyFromALiteralAndTheEscapeIsReversible )
{
    EXPECT_TRUE( IsKeyReference( "#menu.play" ) );
    EXPECT_FALSE( IsKeyReference( "menu.play" ) );
    EXPECT_FALSE( IsKeyReference( "##menu.play" ) ); // the escape
    EXPECT_FALSE( IsKeyReference( "" ) );
    EXPECT_EQ( KeyOf( "#menu.play" ), "menu.play" );

    EXPECT_EQ( LiteralOf( "##1 hash" ), "#1 hash" );
    EXPECT_EQ( LiteralOf( "plain" ), "plain" );
    // Only the LEADING pair is an escape. Rich-text colours are full of hashes and doubling them all
    // would have broken every one — "[color=#FF7A33]" is authored in this repository's main menu today.
    EXPECT_EQ( LiteralOf( "[color=#FF7A33]RP[/color]" ), "[color=#FF7A33]RP[/color]" );

    // Escape then unescape is the identity, which is what makes the scene migration safe.
    for ( const char* sample : { "plain", "#looks like a key", "##already escaped", "[color=#fff]x", "" } )
        EXPECT_EQ( LiteralOf( EscapeLiteral( sample ) ), std::string( sample ) ) << sample;
    // And an escaped literal is never mistaken for a key afterwards.
    EXPECT_FALSE( IsKeyReference( EscapeLiteral( "#looks like a key" ) ) );
}

TEST_F( Fixture, AMissingKeyIsVISIBLEAndIsNotAnotherLanguagesText )
{
    Localization& loc = Localization::Get();

    const auto missing = loc.Resolve( "#no.such.key" );
    EXPECT_EQ( missing.Outcome, Localization::Outcome::MissingKey );
    // The three things it must not be: empty (undiagnosable on a screen), another language's string
    // (a wrong answer that looks right), or the key without its sigil (indistinguishable from a literal).
    EXPECT_FALSE( missing.Text.empty() );
    EXPECT_EQ( missing.Text, "#no.such.key" );
    EXPECT_EQ( loc.Misses().size(), 1u );

    // A key that EXISTS but has no translation in the current language is a different miss with the same
    // visible outcome — and no fallback to English, because a German build silently shipping in English
    // is the defect this rule exists to prevent.
    ASSERT_TRUE( loc.SetLanguage( "de" ) );
    const auto untranslated = loc.Resolve( "#menu.play" );
    EXPECT_EQ( untranslated.Outcome, Localization::Outcome::MissingLanguage );
    EXPECT_EQ( untranslated.Text, "#menu.play" );
    EXPECT_NE( untranslated.Text, "PLAY" );
}

TEST_F( Fixture, TheMissRegisterFollowsTheLanguageAndTheGenerationFollowsEveryChange )
{
    Localization&  loc    = Localization::Get();
    const uint32_t before = loc.Generation();

    ASSERT_TRUE( loc.SetLanguage( "ru" ) );
    EXPECT_GT( loc.Generation(), before );
    // Setting the language it is already in is not a change and must not churn the generation, or every
    // consumer that caches on it rebuilds for nothing.
    const uint32_t settled = loc.Generation();
    ASSERT_TRUE( loc.SetLanguage( "ru" ) );
    EXPECT_EQ( loc.Generation(), settled );

    EXPECT_TRUE( loc.Misses().empty() );
    (void)loc.Resolve( "#no.such.key" );
    EXPECT_EQ( loc.Misses().size(), 1u );
    // A miss belongs to a (key, language) pair. Switching language clears the register, because a key
    // missing in Russian says nothing about German and a stale register is a report about a session that
    // no longer exists.
    ASSERT_TRUE( loc.SetLanguage( "en" ) );
    EXPECT_TRUE( loc.Misses().empty() );
}

TEST_F( Fixture, AvailableLanguagesAreDERIVEDFromTheLoadedRows )
{
    Localization& loc       = Localization::Get();
    const auto    available = loc.AvailableLanguages();
    // The fixture's table carries en and ru and nothing else, so those are the languages this "project"
    // has — nobody declares the list, and a language a project has no strings in is not one it supports.
    ASSERT_EQ( available.size(), 2u );
    EXPECT_EQ( available[0]->Tag, "en" );
    EXPECT_EQ( available[1]->Tag, "ru" );

    loc.Clear();
    EXPECT_TRUE( loc.AvailableLanguages().empty() );
}

TEST_F( Fixture, AnUnknownLanguageIsRefusedByNameAndChangesNothing )
{
    Localization&     loc = Localization::Get();
    const std::string was = std::string( loc.Language().Tag );

    const auto refused = loc.SetLanguage( "kl" );
    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "kl" ), std::string::npos );
    // The refusal must list what IS available, or the caller cannot act on it.
    EXPECT_NE( refused.GetError().find( "ru" ), std::string::npos );
    EXPECT_EQ( std::string( loc.Language().Tag ), was );
}

TEST_F( Fixture, TwoTablesCannotClaimOneKey )
{
    Localization& loc   = Localization::Get();
    const char*   rival = R"({"Entries":[
        {"Key":"menu.play","Forms":{"en":{"other":"START"}}},
        {"Key":"menu.quit","Forms":{"en":{"other":"QUIT"}}}]})";

    auto parsed = ParseStringTable( Headed( rival ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const auto refused = loc.RegisterTable( "rival.destrings", parsed.ExtractValue() );
    EXPECT_FALSE( refused );
    // Both files named, because "a key is defined twice" is unactionable without knowing where.
    EXPECT_NE( refused.GetError().find( "rival.destrings" ), std::string::npos );
    EXPECT_NE( refused.GetError().find( "suite.destrings" ), std::string::npos );

    // A REFUSED TABLE CONTRIBUTES NOTHING. Half of it landing would be worse than none of it: `menu.quit`
    // comes before the clash in file order, so a partial insert is exactly what a naive loop leaves.
    EXPECT_EQ( loc.Resolve( "#menu.quit" ).Outcome, Localization::Outcome::MissingKey );
    EXPECT_EQ( loc.Resolve( "#menu.play" ).Text, "PLAY" );
}

TEST_F( Fixture, ReRegisteringATableREPLACESItSoADeletedKeyIsDeleted )
{
    Localization& loc    = Localization::Get();
    const char*   shrunk = R"({"Entries":[
        {"Key":"menu.play","Forms":{"en":{"other":"GO"}}}]})";

    auto parsed = ParseStringTable( Headed( shrunk ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    ASSERT_TRUE( loc.RegisterTable( "suite.destrings", parsed.ExtractValue() ) );

    EXPECT_EQ( loc.Resolve( "#menu.play" ).Text, "GO" );
    // The key the new version of the file does NOT have must be gone. Leaving it would make a deletion a
    // no-op and the hot reload would report itself as working.
    EXPECT_EQ( loc.Resolve( "#files.count" ).Outcome, Localization::Outcome::MissingKey );
}

TEST( LocalizedText, PlaceholdersFormatThroughTheLOCALEAndAnUnansweredOneStaysVisible )
{
    const LocaleRow& ru = *FindLocale( "ru" );
    FormatArguments  args;
    args.Count = 1234567;
    args.Named.emplace_back( "who", "Nico" );

    const FormattedText ok = ApplyArguments( "{who}: {n}", ru, args );
    EXPECT_EQ( ok.Text, "Nico: 1\xC2\xA0"
                        "234\xC2\xA0"
                        "567" );
    EXPECT_TRUE( ok.Unresolved.empty() );

    // A fraction spec, and the locale's own decimal separator.
    args.Count = 0.5;
    EXPECT_EQ( ApplyArguments( "{n:2}", ru, args ).Text, "0,50" );

    // Braces are escapable, so a translation can contain one.
    EXPECT_EQ( ApplyArguments( "{{n}}", ru, args ).Text, "{n}" );

    // An unanswered placeholder is left ON SCREEN as written and reported, never replaced with a blank.
    const FormattedText hole = ApplyArguments( "score {score}", ru, {} );
    EXPECT_EQ( hole.Text, "score {score}" );
    ASSERT_EQ( hole.Unresolved.size(), 1u );
    EXPECT_EQ( hole.Unresolved[0], "score" );

    // An unterminated brace is text. Swallowing the rest of the string would delete a translation
    // because somebody typed one character.
    EXPECT_EQ( ApplyArguments( "half {n", ru, args ).Text, "half {n" );
}

TEST( LocalizedText, EveryWayATableCanBeWrongIsRefusedByName )
{
    struct Case
    {
        const char* json;
        const char* mustMention;
    };
    const Case cases[] = {
         { R"({"FormatVersion":1,"Entries":[]})", "SceneMigrator" },
         { R"({"Entries":[{"Key":"","Forms":{"en":{"other":"x"}}}]})", "empty Key" },
         { R"({"Entries":[{"Key":"Menu.Play","Forms":{"en":{"other":"x"}}}]})", "Menu.Play" },
         { R"({"Entries":[{"Key":"a","Forms":{"en":{"other":"x"}}},
                          {"Key":"a","Forms":{"en":{"other":"y"}}}]})",
           "twice" },
         { R"({"Entries":[{"Key":"a","Forms":{"gb":{"other":"x"}}}]})", "gb" },
         // `other` is present, so this case isolates the SELECTOR refusal from the missing-other one.
         { R"({"Entries":[{"Key":"a","Forms":{"en":{"other":"ok","singular":"x"}}}]})", "singular" },
         { R"({"Entries":[{"Key":"a","Forms":{"en":{}}}]})", "no forms" },
         { R"({"Entries":[{"Key":"a","Forms":{}}]})", "no languages" },
         { R"({"Entries":[{"Key":"a","Forms":{"en":{"other":""}}}]})", "EMPTY" },
         // No `other`: the entry cannot answer a caller that gives no count and no gender, and in Russian
         // it cannot answer a printed fraction either. Refused where the author can see the file.
         { R"({"Entries":[{"Key":"a","Forms":{"ru":{"one":"x","few":"y","many":"z"}}}]})", "other" },
         { "", "empty" },
         { "{ not json", "" },
    };

    for ( const Case& c : cases )
    {
        const std::string json =
             std::string_view( c.json ).starts_with( "{\"Entries\"" ) ? Headed( c.json ) : c.json;
        const auto parsed = ParseStringTable( json );
        ASSERT_FALSE( parsed ) << "accepted: " << c.json;
        if ( *c.mustMention != '\0' )
            EXPECT_NE( parsed.GetError().find( c.mustMention ), std::string::npos )
                 << "refusal did not name '" << c.mustMention << "': " << parsed.GetError();
    }

    // A gendered selector and a gender.plural selector are both legal, and so is every CLDR category.
    const auto good = ParseStringTable( Headed(
         R"({"Entries":[{"Key":"a","Comment":"note","Forms":{"ru":{"feminine.one":"x","masculine":"y",
             "zero":"z","two":"w","few":"v","many":"u","other":"t"}}}]})" ) );
    EXPECT_TRUE( good ) << ( good ? "" : good.GetError() );
}

TEST( LocalizedText, ATableRoundTripsThroughItsOwnWriter )
{
    auto parsed = ParseStringTable( Headed( R"({"DisplayName":"D","Entries":[
        {"Key":"a","Comment":"why","Forms":{"en":{"other":"A"},"ru":{"one":"B","few":"C","many":"D","other":"E"}}}]})" ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const StringTableData original = parsed.ExtractValue();

    const auto again = ParseStringTable( WriteStringTable( original ) );
    ASSERT_TRUE( again ) << again.GetError();
    EXPECT_EQ( again.GetValue(), original );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
