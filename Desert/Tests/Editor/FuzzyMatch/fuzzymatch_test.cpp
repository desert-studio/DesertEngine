#include <gtest/gtest.h>

#include <Editor/Core/FuzzyMatch.hpp>

using Desert::Editor::FuzzyMatch;

TEST( FuzzyMatch, MatchesSubsequence )
{
    int score = -1;
    EXPECT_TRUE( FuzzyMatch( "sc", "Scene", score ) );
    EXPECT_TRUE( FuzzyMatch( "sn", "Scene", score ) ); // non-adjacent still matches
}

TEST( FuzzyMatch, RejectsNonSubsequence )
{
    int score = 0;
    EXPECT_FALSE( FuzzyMatch( "xz", "Scene", score ) );
    EXPECT_FALSE( FuzzyMatch( "es", "Scene", score ) ); // wrong order
}

TEST( FuzzyMatch, EmptyQueryMatchesEmptyText )
{
    int score = -1;
    EXPECT_TRUE( FuzzyMatch( "", "anything", score ) );
    EXPECT_EQ( score, 0 );
    EXPECT_FALSE( FuzzyMatch( "a", "", score ) );
}

TEST( FuzzyMatch, IsCaseInsensitive )
{
    int a = 0, b = 0;
    EXPECT_TRUE( FuzzyMatch( "SCENE", "scene", a ) );
    EXPECT_TRUE( FuzzyMatch( "scene", "SCENE", b ) );
}

TEST( FuzzyMatch, PrefixRanksAboveMidString )
{
    int logs = 0, backlog = 0;
    ASSERT_TRUE( FuzzyMatch( "log", "Logs", logs ) );
    ASSERT_TRUE( FuzzyMatch( "log", "Backlog", backlog ) );
    EXPECT_GT( logs, backlog );
}

TEST( FuzzyMatch, ConsecutiveRanksAboveScattered )
{
    int shader = 0, mesh = 0;
    ASSERT_TRUE( FuzzyMatch( "sh", "Shader", shader ) );
    ASSERT_TRUE( FuzzyMatch( "sh", "Mesh", mesh ) );
    EXPECT_GT( shader, mesh );
}
