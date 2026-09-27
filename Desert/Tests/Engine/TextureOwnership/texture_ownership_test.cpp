// A TEXTURE2D OWNS ITS IMAGE. Both factories register the GPU image in the process-wide ImageService, and
// until RT2n nothing took it back out: ~Texture2D was the default, so every asset texture, cooked panorama,
// GIF frame, video frame and BRDF LUT stayed resident for the session, and Render2D released its white
// texture by hand. The claim is a relation between two objects -- the texture's lifetime and the service's
// slot -- so each test below holds one side and reads the other.
//
// The real Texture.cpp and ImageService.cpp are linked. Everything with a device behind it is stubbed here:
// Image2D::Create returns a fake that counts itself, and the registry getter hands out this file's service.

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

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

    std::shared_ptr<Desert::Graphic::Texture2D> MakeProcedural()
    {
        auto created =
             Desert::Graphic::Texture2D::Create( "test", 1, 1, Desert::Core::Formats::ImageFormat::RGBA8F,
                                                 std::vector<unsigned char>{ 255, 255, 255, 255 } );
        EXPECT_TRUE( created.IsSuccess() ) << created.GetError();
        return created.IsSuccess() ? created.ExtractValue() : nullptr;
    }
} // namespace

// ── Stubs for the device half ─────────────────────────────────────────────────────────────────────────────
namespace Desert::Graphic
{
    std::shared_ptr<Image2D> Image2D::Create( const Core::Formats::Image2DSpecification& spec )
    {
        return std::make_shared<FakeImage2D>( spec );
    }
} // namespace Desert::Graphic

namespace Desert::Runtime
{
    ImageService* ResourceRegistry::GetImageService()
    {
        return &Service();
    }
} // namespace Desert::Runtime

namespace Desert::Assets
{
    Common::ResultStr<std::string> LoadTexturePlatformData( const std::filesystem::path& )
    {
        return Common::MakeSuccess( std::string( "stub" ) );
    }

    namespace Serialization
    {
        Common::ResultStr<TextureAssetData> DecodeTextureBinary( const std::string_view, const std::string_view )
        {
            TextureAssetData data;
            data.Width      = 1;
            data.Height     = 1;
            data.Format     = Core::Formats::ImageFormat::RGBA8F;
            data.Kind       = TextureKind::Texture2D;
            data.LayerCount = 1;
            data.Pixels     = { 1, 2, 3, 4 };
            TextureLevel level;
            level.Width    = 1;
            level.Height   = 1;
            level.ByteSize = 4;
            data.Levels.push_back( level );
            return Common::MakeSuccess( std::move( data ) );
        }
    } // namespace Serialization
} // namespace Desert::Assets

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
// panorama SceneEnvironment builds and drops inside one function.
TEST( TextureOwnership, DroppingAnAssetTextureReleasesItsImage )
{
    const int before  = s_LiveImages;
    auto      created = Desert::Graphic::Texture2D::CreateFromAsset( "stub.detex" );
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

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
