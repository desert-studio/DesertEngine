// AM3: an evicted texture's image is released, and the editor's UI texture id written against it goes with it.
//
// Editor::UI::ImageTextureIds is the bookkeeping UICacheTextureImGui keeps its ImGui descriptor sets in, and
// Assets::FrameRetireQueue is what TextureService::EvictBuilt parks the image into. Both are free of Vulkan and
// ImGui, so the relation between them is held here with a stand-in image and integer ids: the cache must never
// answer a dead image's id — not while the image is parked, not after it is destroyed, and not to a new image
// that happens to occupy the dead one's address — and must hand each dead id to the release exactly once.
#include <Editor/Widgets/UIHelper/ImageTextureIds.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

namespace
{
    using Desert::Assets::FrameRetireQueue;
    using Desert::Editor::UI::ImageTextureIds;

    struct StandInImage
    {
        std::uint64_t Generation = 0;
    };

    using TextureId = std::uintptr_t;

    constexpr std::uint32_t kFramesInFlight = 2;

    /// TextureService reduced to the rule under test: Get builds on a miss, EvictBuilt parks the built image.
    struct StandInTextureService
    {
        std::shared_ptr<StandInImage>                   Built;
        FrameRetireQueue<std::shared_ptr<StandInImage>> Retiring;
        std::uint64_t                                   NextGeneration = 0;
        int                                             Builds         = 0;

        std::shared_ptr<StandInImage> Get()
        {
            if ( !Built )
            {
                Built = std::make_shared<StandInImage>( StandInImage{ ++NextGeneration } );
                ++Builds;
            }
            return Built;
        }

        void EvictBuilt( std::uint64_t frame )
        {
            Retiring.Park( std::move( Built ), frame );
            Built.reset();
        }
    };

    /// The UI cache around ImageTextureIds: `make` mints ids, `retire` records what was given back.
    struct StandInUiCache
    {
        ImageTextureIds<StandInImage, TextureId> Ids;
        TextureId                                NextId = 0;
        std::vector<TextureId>                   Retired;

        TextureId Acquire( const std::shared_ptr<StandInImage>& image )
        {
            return Ids.Acquire(
                 image, image->Generation, [this] { return ++NextId; },
                 [this]( TextureId id ) { Retired.push_back( id ); } );
        }

        std::size_t Sweep()
        {
            return Ids.Sweep( [this]( TextureId id ) { Retired.push_back( id ); } );
        }
    };
} // namespace

// RELATION: the evicted texture's id leaves the UI cache once its image is gone, exactly once, and the next Get
// builds a new image that receives a NEW id — the dead one is never handed out again.
TEST( UiTextureIds, AnEvictedTexturesIdIsReleasedWithItsImageAndTheRebuiltTextureGetsANewOne )
{
    StandInTextureService textures;
    StandInUiCache        ui;

    auto            first   = textures.Get();
    const TextureId firstId = ui.Acquire( first );
    ASSERT_EQ( ui.Acquire( first ), firstId ) << "a live image asked twice wrote a second descriptor set";

    const std::weak_ptr<StandInImage> observed    = first;
    constexpr std::uint64_t           kSweepFrame = 10;
    textures.EvictBuilt( kSweepFrame );
    // The panel that drew it lets go too; only the retire queue still holds the image.
    first.reset();

    // Frames recorded before the sweep may still sample it: the image is alive, so its set must stay.
    EXPECT_EQ( textures.Retiring.Collect( kSweepFrame + 1, kFramesInFlight ), 0u );
    EXPECT_EQ( ui.Sweep(), 0u ) << "the set was released while its image was still parked for frames in flight";
    EXPECT_TRUE( ui.Retired.empty() );

    // Once no frame in flight can sample it, the image is destroyed and the sweep gives the set back.
    EXPECT_EQ( textures.Retiring.Collect( kSweepFrame + kFramesInFlight + 1, kFramesInFlight ), 1u );
    ASSERT_TRUE( observed.expired() ) << "the retire queue did not release the evicted image";
    EXPECT_EQ( ui.Sweep(), 1u );
    ASSERT_EQ( ui.Retired, std::vector<TextureId>{ firstId } ) << "the dead image's set was not handed back once";
    EXPECT_EQ( ui.Ids.Size(), 0u ) << "the UI cache still holds an entry for an evicted texture";

    // The next Get rebuilds, and the rebuilt image is shown through a set written for IT.
    const auto      rebuilt   = textures.Get();
    const TextureId rebuiltId = ui.Acquire( rebuilt );
    EXPECT_EQ( textures.Builds, 2 ) << "Get after eviction did not rebuild the texture";
    EXPECT_NE( rebuiltId, firstId ) << "the rebuilt texture was handed the evicted one's descriptor set";
    EXPECT_TRUE( ui.Ids.Holds( rebuilt ) );
    EXPECT_EQ( ui.Sweep(), 0u );
    EXPECT_EQ( ui.Retired.size(), 1u );
}

// RELATION: the key is the image's address only while the image that made the entry is alive. A new image at a
// dead one's address — which is exactly what an allocator does — must not inherit its set.
TEST( UiTextureIds, ANewImageAtADeadImagesAddressDoesNotInheritItsId )
{
    alignas( StandInImage ) unsigned char storage[sizeof( StandInImage )];
    const auto                            placed = [&storage]( std::uint64_t generation )
    {
        auto* image = ::new ( static_cast<void*>( storage ) ) StandInImage{ generation };
        return std::shared_ptr<StandInImage>( image, []( StandInImage* p ) { p->~StandInImage(); } );
    };

    StandInUiCache ui;
    auto           dead   = placed( 7 );
    const auto     deadId = ui.Acquire( dead );
    dead.reset();

    // Same address, and deliberately the same generation: only the owner tells the two apart.
    const auto reborn = placed( 7 );
    ASSERT_EQ( static_cast<const void*>( reborn.get() ), static_cast<const void*>( storage ) );
    const auto rebornId = ui.Acquire( reborn );

    EXPECT_NE( rebornId, deadId ) << "a new image at a dead one's address was handed the dead image's set";
    EXPECT_EQ( ui.Retired, std::vector<TextureId>{ deadId } ) << "the dead image's set was not given back";
    EXPECT_TRUE( ui.Ids.Holds( reborn ) );
}

// RELATION: the same image re-created (resized, or its sampler rebuilt) has a new view and sampler, so the set
// written against the old ones is replaced, not reused and not leaked.
TEST( UiTextureIds, ARecreatedImageReplacesItsIdAndGivesTheOldOneBack )
{
    StandInUiCache ui;
    const auto     image = std::make_shared<StandInImage>( StandInImage{ 1 } );
    const auto     old   = ui.Acquire( image );

    image->Generation = 2;
    const auto fresh  = ui.Acquire( image );

    EXPECT_NE( fresh, old ) << "a re-created image was shown through the set written for its previous view";
    EXPECT_EQ( ui.Retired, std::vector<TextureId>{ old } ) << "the previous view's set was leaked";
    EXPECT_EQ( ui.Ids.Size(), 1u );

    // Released and not re-created: Drop gives the set back and the cache forgets the image.
    EXPECT_TRUE( ui.Ids.Drop( image, [&ui]( TextureId id ) { ui.Retired.push_back( id ); } ) );
    EXPECT_EQ( ui.Retired, ( std::vector<TextureId>{ old, fresh } ) );
    EXPECT_FALSE( ui.Ids.Holds( image ) );
}
