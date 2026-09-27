// SanitizeProductName: the one rule a .deproj Name goes through before it names a folder (PKG1c).
// A table, because the rule IS a table of cases — every row names the clause it pins.
#include <Common/Settings/ProductName.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    struct Case
    {
        std::string input;
        std::string expected;
        const char* why;
    };

    const std::vector<Case>& Cases()
    {
        static const std::vector<Case> kCases = {
             // Kept as the player wrote it.
             { "My Game", "My Game", "an inner space is part of the name" },
             { "Wüste 2", "Wüste 2", "UTF-8 bytes are not control characters" },
             { "game.v2", "game.v2", "an inner dot is kept" },
             // Windows' forbidden characters, each one.
             { "a<b>c", "a_b_c", "< and >" },
             { "C:drive", "C_drive", "colon (drive / stream separator)" },
             { "say \"hi\"", "say _hi_", "double quote" },
             { "a/b\\c", "a_b_c", "both separators" },
             { "a|b?c*", "a_b_c_", "pipe, question mark, star" },
             { std::string( "tab\there" ), "tab_here", "control character" },
             { std::string( "del\x7F" ), "del_", "DEL" },
             { std::string( "nul\0x", 5 ), "nul_x", "an embedded NUL byte" },
             // Trailing dots and spaces.
             { "Game.", "Game", "trailing dot" },
             { "Game . . ", "Game", "trailing dots and spaces together" },
             { " Game", " Game", "a LEADING space is legal and kept" },
             // Empty and the relative names -> the one default.
             { "", "DesertGame", "empty" },
             { ".", "DesertGame", "current directory" },
             { "..", "DesertGame", "parent directory" },
             { "  ...  ", "DesertGame", "nothing but trailing junk" },
             // Reserved device names: any case, with or without an extension.
             { "CON", "CON_", "console" },
             { "con", "con_", "case-insensitive" },
             { "Nul.txt", "Nul_.txt", "reserved even with an extension" },
             { "aux.tar.gz", "aux_.tar.gz", "the stem is before the FIRST dot" },
             { "PRN.", "PRN_", "trailing dot trimmed first, then reserved" },
             { "COM1", "COM1_", "COM1-9" },
             { "lpt9.log", "lpt9_.log", "LPT1-9" },
             { "COM0", "COM0", "COM0 is not reserved" },
             { "COM10", "COM10", "only one digit" },
             { "CONSOLE", "CONSOLE", "a longer word is not the device" },
             { "my.con", "my.con", "the extension is not the stem" },
             { "a/con", "a_con", "a separator does not split into components" },
        };
        return kCases;
    }
} // namespace

TEST( ProductName, EveryRowOfTheRule )
{
    for ( const Case& c : Cases() )
        EXPECT_EQ( Common::Settings::SanitizeProductName( c.input ), c.expected ) << c.why;
}

TEST( ProductName, TheResultIsAFixedPoint )
{
    // Sanitising a sanitised name must not change it again: the packager and GameUserDirectory may be
    // handed a name that already went through the rule (a package folder renamed back into a Name).
    for ( const Case& c : Cases() )
    {
        const std::string once = Common::Settings::SanitizeProductName( c.input );
        EXPECT_EQ( Common::Settings::SanitizeProductName( once ), once ) << c.why;
    }
}

TEST( ProductName, TheDefaultIsItselfAValidName )
{
    EXPECT_EQ( Common::Settings::SanitizeProductName( Common::Settings::kDefaultProductDirectoryName ),
               Common::Settings::kDefaultProductDirectoryName );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
