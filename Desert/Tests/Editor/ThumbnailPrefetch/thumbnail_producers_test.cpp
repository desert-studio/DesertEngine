#include <Editor/Widgets/ThumbnailProducers.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace Desert::Editor;
using ThumbnailProducers::Producer;

// THM1n: every asset kind has exactly one row in the producer table — a kind added to FileType without a
// row is a census failure here, not an icon nobody notices.
TEST( ThumbnailProducers, EveryAssetKindHasExactlyOneRow )
{
    for ( int k = static_cast<int>( FileType::Unknown ); k <= static_cast<int>( kLastFileType ); ++k )
    {
        const auto type  = static_cast<FileType>( k );
        const auto count = std::count_if( ThumbnailProducers::kTable.begin(), ThumbnailProducers::kTable.end(),
                                          [&]( const ThumbnailProducers::Row& row ) { return row.Type == type; } );
        EXPECT_EQ( count, 1 ) << "FileType " << k << " must have exactly one producer row";
        EXPECT_TRUE( ThumbnailProducers::ProducerOf( type ).has_value() ) << "FileType " << k;
    }
}

// The debt register and the table agree: a kind is NotYetProduced exactly when the register names it.
TEST( ThumbnailProducers, TheNotYetProducedRegisterMatchesTheTable )
{
    for ( const ThumbnailProducers::Row& row : ThumbnailProducers::kTable )
    {
        const bool registered =
             std::find( ThumbnailProducers::kNotYetProduced.begin(), ThumbnailProducers::kNotYetProduced.end(),
                        row.Type ) != ThumbnailProducers::kNotYetProduced.end();
        EXPECT_EQ( row.How == Producer::NotYetProduced, registered ) << row.Why;
        EXPECT_FALSE( row.Why.empty() );
    }
}

// The kinds the browser photographs, decodes or paints today, pinned by name.
TEST( ThumbnailProducers, ThePicturedKindsKeepTheirProducers )
{
    EXPECT_EQ( ThumbnailProducers::ProducerOf( FileType::Model ), Producer::RenderedMesh );
    EXPECT_EQ( ThumbnailProducers::ProducerOf( FileType::Material ), Producer::RenderedMaterial );
    EXPECT_EQ( ThumbnailProducers::ProducerOf( FileType::Texture ), Producer::Decoded );
    EXPECT_EQ( ThumbnailProducers::ProducerOf( FileType::Cubemap ), Producer::Painted );
    EXPECT_EQ( ThumbnailProducers::ProducerOf( FileType::Cloud ), Producer::Painted );
    EXPECT_EQ( ThumbnailProducers::ProducerOf( FileType::UITheme ), Producer::Painted );
    // A foliage type is photographed as its mesh (THM1n-6): the same producer as a model.
    EXPECT_EQ( ThumbnailProducers::ProducerOfPath( "Assets/Foliage/Grass.defoliage" ), Producer::RenderedMesh );
    // Typed (THM1n-4) and still owed a picture: pinned through the whole chain from the extension.
    for ( const char* path : { "Assets/Hero/Hero.skmesh", "Assets/Hero/Hero.skeleton", "Assets/Hero/Run.anim" } )
        EXPECT_EQ( ThumbnailProducers::ProducerOfPath( path ), Producer::NotYetProduced ) << path;
}

// The documents UE draws with their class icon are rows too — "no renderer" is an answer, never a gap.
TEST( ThumbnailProducers, TheIconKindsAreRowsToo )
{
    for ( const char* path : { "A/Rig.derig", "A/Graph.danimgraph", "A/Map.retarget", "A/UI.destrings",
                               "W.dwworld/0_0.dwcell", "W.dwworld/Index.dwindex" } )
        EXPECT_EQ( ThumbnailProducers::ProducerOfPath( path ), Producer::TypeIcon ) << path;
}
