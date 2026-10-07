// A TEXTURE2D OWNS ITS IMAGE. Both factories register the GPU image in the process-wide ImageService, and
// until RT2n nothing took it back out: ~Texture2D was the default, so every asset texture, cooked panorama,
// GIF frame, video frame and BRDF LUT stayed resident for the session, and Render2D released its white
// texture by hand. The claim is a relation between two objects -- the texture's lifetime and the service's
// slot -- so each test below holds one side and reads the other.
//
// The real Texture.cpp and ImageService.cpp run. The device is the one thing replaced, and through the seam the
// texture already takes (TextureBackend): a mock IImageFactory makes a fake image that counts itself, and the
// textures register in this file's own ImageService, so the counts below see no other suite's images.

#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
    int s_LiveImages = 0;

    class FakeImage2D final : public Desert::Graphic::Image2D
    {
    public:
        explicit FakeImage2D( const Desert::Core::Formats::Image2DSpecification& spec ) : m_Spec( spec )
        {
            ++s_LiveImages;
        }
        ~FakeImage2D() override
        {
            --s_LiveImages;
        }

        [[nodiscard]] uint32_t GetWidth() const override
        {
            return m_Spec.Width;
        }
        [[nodiscard]] uint32_t GetHeight() const override
        {
            return m_Spec.Height;
        }
        [[nodiscard]] uint32_t GetMipmapLevels() const override
        {
            return 1;
        }
        Desert::Core::Formats::Image2DSpecification& GetImageSpecification() override
        {
            return m_Spec;
        }
        Common::BoolResultStr Invalidate() override
        {
            return BOOLSUCCESS;
        }
        Common::BoolResultStr Release() override
        {
            return BOOLSUCCESS;
        }

    private:
        Desert::Core::Formats::Image2DSpecification m_Spec;
    };

    Desert::Runtime::ImageService& Service()
    {
        static Desert::Runtime::ImageService service;
        return service;
    }

    // The mock device: every image it makes is a FakeImage2D, so s_LiveImages is the number alive.
    class MockImageFactory final : public Desert::Graphic::IImageFactory
    {
    public:
        [[nodiscard]] std::shared_ptr<Desert::Graphic::Image2D>
        CreateImage2D( const Desert::Core::Formats::Image2DSpecification& spec ) const override
        {
            return std::make_shared<FakeImage2D>( spec );
        }
    };

    Desert::Graphic::TextureBackend Backend()
    {
        static const MockImageFactory factory;
        return Desert::Graphic::TextureBackend{ factory, Service() };
    }

    std::shared_ptr<Desert::Graphic::Texture2D> MakeProcedural()
    {
        auto created =
             Desert::Graphic::Texture2D::Create( "test", 1, 1, Desert::Core::Formats::ImageFormat::RGBA8F,
                                                 std::vector<unsigned char>{ 255, 255, 255, 255 }, Backend() );
        EXPECT_TRUE( created.IsSuccess() ) << created.GetError();
        return created.IsSuccess() ? created.ExtractValue() : nullptr;
    }
} // namespace

// ── The relation ──────────────────────────────────────────────────────────────────────────────────────────

// RELATION: the procedural factory (BRDF LUT, GIF and video frames, Render2D's white texture) registers an
// image, and dropping the texture is what releases it -- the slot resolves to nothing and the image is gone.
TEST( TextureOwnership, DroppingAProceduralTextureReleasesItsImage )
{
    const int before  = s_LiveImages;
    auto      texture = MakeProcedural();
    ASSERT_NE( texture, nullptr );
    const Desert::Runtime::ImageHandle handle = texture->GetImageHandle();
    ASSERT_NE( Service().Resolve( handle ), nullptr ) << "the factory did not register the image at all";
    EXPECT_EQ( s_LiveImages, before + 1 );

    texture.reset();

    EXPECT_EQ( Service().Resolve( handle ), nullptr )
         << "the texture is gone and the ImageService still hands out its image: nothing unregistered it";
    EXPECT_EQ( s_LiveImages, before ) << "the service slot was dropped but the image object survived it";
}

// RELATION: the same for the asset factory, which is every material texture a scene loads and the cooked
// panorama SceneEnvironment builds and drops inside one function. Its GPU half -- CreateFromCooked, where the
// image is made and adopted -- is driven from a decoded texture; CreateFromAsset is ReadCooked (CPU only, no
// image) followed by exactly this call.
TEST( TextureOwnership, DroppingAnAssetTextureReleasesItsImage )
{
    const int before  = s_LiveImages;
    Desert::Graphic::CookedTexture2D cooked;
    cooked.Tag    = "asset";
    cooked.Width  = 1;
    cooked.Height = 1;
    cooked.Format = Desert::Core::Formats::ImageFormat::RGBA8F;
    cooked.Pixels = std::vector<unsigned char>{ 1, 2, 3, 4 };
    cooked.Levels = { Desert::Core::Formats::MipLevelSpan{ 0, 4 } };
    auto created  = Desert::Graphic::Texture2D::CreateFromCooked( std::move( cooked ), Backend() );
    ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
    auto                               texture = created.ExtractValue();
    const Desert::Runtime::ImageHandle handle  = texture->GetImageHandle();
    ASSERT_NE( Service().Resolve( handle ), nullptr );

    texture.reset();

    EXPECT_EQ( Service().Resolve( handle ), nullptr );
    EXPECT_EQ( s_LiveImages, before );
}

// RELATION: a texture releases ITS slot and no other. The service reuses slots, so a destructor that
// released "the last one" or a fixed index would pass the tests above and take a neighbour's image here.
TEST( TextureOwnership, ATextureReleasesOnlyItsOwnImage )
{
    auto first  = MakeProcedural();
    auto second = MakeProcedural();
    ASSERT_NE( first, nullptr );
    ASSERT_NE( second, nullptr );
    const Desert::Runtime::ImageHandle kept = second->GetImageHandle();

    first.reset();

    EXPECT_NE( Service().Resolve( kept ), nullptr ) << "dropping one texture released another texture's image";
}

// RELATION: a hundred create/drop cycles leave the service where they found it -- the open-close loop of a
// view or a material document, which is what RT2l's allocation ledger grew by one row per cycle on.
TEST( TextureOwnership, CreateDropCyclesLeaveNoImageBehind )
{
    const int before = s_LiveImages;
    for ( int i = 0; i < 100; ++i )
        (void)MakeProcedural();
    EXPECT_EQ( s_LiveImages, before );

    size_t occupied = 0;
    for ( const auto& image : Service().All() )
        occupied += image ? 1U : 0U;
    EXPECT_EQ( occupied, static_cast<size_t>( before ) )
         << "the service holds more images than there are live textures";
}
