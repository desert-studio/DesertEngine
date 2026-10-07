#include <gtest/gtest.h>

#include <Common/Core/ResultStr.hpp>

TEST( Result, err )
{
    struct dummy
    {
    };
    Common::ResultStr<dummy> res = Common::MakeError<dummy>( "Test Message" );
    EXPECT_EQ( res.IsSuccess(), false );
    EXPECT_EQ( res.GetError(), "Test Message" );

    Common::ResultStr<dummy> res2 = Common::MakeFormattedError<dummy>( "Test Formated {}", "Message" );
    EXPECT_EQ( res2.IsSuccess(), false );
    EXPECT_EQ( res2.GetError(), "Test Formated Message" );
}

TEST( Result, succ )
{
    struct dummy
    {
        int a;
    };
    Common::ResultStr<dummy> res = Common::MakeSuccess( dummy{ 112233 } );
    EXPECT_EQ( res.IsSuccess(), true );
    EXPECT_EQ( res.GetValue().a, 112233 );
}
