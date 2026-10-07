#include <gtest/gtest.h>

#include <Editor/Core/MultiEdit.hpp>

#include <cstddef>
#include <vector>

using namespace Desert::Editor;

namespace
{
    struct S
    {
        int   a;
        float b;
    };
} // namespace

TEST( MultiEdit, FieldDiffersDetectsPerFieldChange )
{
    S x{ 1, 2.0f };
    S y{ 1, 9.0f };
    EXPECT_FALSE( FieldDiffers( &x, &y, offsetof( S, a ), sizeof( int ) ) );   // a equal
    EXPECT_TRUE( FieldDiffers( &x, &y, offsetof( S, b ), sizeof( float ) ) );  // b differs
}

TEST( MultiEdit, AnyFieldDiffersScansTheSelection )
{
    S base{ 5, 0.0f };
    S same{ 5, 0.0f };
    S diff{ 6, 0.0f };

    std::vector<void*> allSame{ &same };
    std::vector<void*> withDiff{ &same, &diff };

    EXPECT_FALSE( AnyFieldDiffers( &base, allSame, offsetof( S, a ), sizeof( int ) ) );
    EXPECT_TRUE( AnyFieldDiffers( &base, withDiff, offsetof( S, a ), sizeof( int ) ) );
    EXPECT_FALSE( AnyFieldDiffers( &base, {}, offsetof( S, a ), sizeof( int ) ) ); // empty selection
}

TEST( MultiEdit, BroadcastFieldCopiesOnlyThatField )
{
    S src{ 42, 3.5f };
    S d1{ 0, 1.0f };
    S d2{ 0, 2.0f };
    std::vector<void*> dst{ &d1, &d2 };

    BroadcastField( &src, dst, offsetof( S, a ), sizeof( int ) );

    EXPECT_EQ( d1.a, 42 );
    EXPECT_EQ( d2.a, 42 );
    // b must be untouched — only field 'a' was broadcast.
    EXPECT_FLOAT_EQ( d1.b, 1.0f );
    EXPECT_FLOAT_EQ( d2.b, 2.0f );
}

TEST( MultiEdit, HandlesNullAndEmptySafely )
{
    S src{ 1, 1.0f };
    EXPECT_FALSE( FieldDiffers( nullptr, &src, 0, sizeof( int ) ) );
    EXPECT_NO_THROW( BroadcastField( &src, {}, 0, sizeof( int ) ) );
    std::vector<void*> withNull{ nullptr };
    EXPECT_NO_THROW( BroadcastField( &src, withNull, 0, sizeof( int ) ) );
}
