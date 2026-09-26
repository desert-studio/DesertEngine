// A material parameter must reach EVERY (view x frame in flight) copy of its uniform buffer, by
// whichever route the material was submitted — and one view's write must never reach another view's copy.
//
// The defect this suite was written for: a shader-graph material with a Color/Float Param rendered black on
// two frames out of three, because UniformBufferProperty::UpdateFields wrote only the copy of the frame that
// was recording. So the first relation asserted is still THE TWO ROUTES LEAVE THE SAME BYTES IN THE SAME
// COPIES: route A restates its value every frame (the override producer), route B applies it once and then
// flushes (the material-asset producer).
//
// Copies are per VIEW now (Engine/Graphic/ViewResources.hpp), made lazily, and that brings two regressions
// the renderer-slot matrix could not have, each pinned here by a test that is red without its fix:
//   B. a view opened after a one-shot write gets a NEW copy — seeded from the block's CPU image, not zeroed;
//   A. a descriptor set is rewritten when the copy it points at is not the copy the view binds now, even
//      when the property is clean — or a preview opened in a closed one's slot binds a freed buffer.
//
// No GPU: the double runs the PRODUCTION copy policy (ShaderResources::ViewCopiedBlock, the same object
// VulkanUniformBuffer holds) over byte vectors instead of mapped VkBuffers. A double that restated the
// policy would be two places obliged to agree with nothing checking that they do.

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Materials/MaterialBackend.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/ViewResources.hpp>
#include <Engine/ShaderResources/ViewCopiedBlock.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace Desert;
using namespace Desert::ShaderResources;

namespace
{
    constexpr uint32_t kFramesInFlight = 3; // what the editor's swapchain actually runs
    constexpr uint32_t kSlots          = Engine::kMaxRendererSlots;
    constexpr uint32_t kFieldSize      = sizeof( float ) * 4;

    class BytesCopy final : public IBlockCopy
    {
    public:
        explicit BytesCopy( uint32_t size ) : Bytes( size, std::byte{ 0xCD } ) // not zero: seeding must overwrite
        {
        }

        Common::BoolResultStr Write( const void* data, uint32_t size, uint32_t offset ) override
        {
            if ( static_cast<size_t>( offset ) + size > Bytes.size() )
                return Common::MakeFormattedError<bool>( "{} byte(s) at offset {} do not fit {}", size, offset,
                                                         Bytes.size() );
            if ( size != 0 )
                std::memcpy( Bytes.data() + offset, data, size );
            ++Writes;
            return Common::MakeSuccess( true );
        }

        std::vector<std::byte> Bytes;
        uint32_t               Writes = 0; // the seed counts as one
    };

    // Records what the GPU would have received, per (view, frame), through the production ViewCopiedBlock.
    class RecordingUniformBuffer final : public UniformBuffer
    {
    public:
        explicit RecordingUniformBuffer( const ShaderLayout::UniformBuffer& model )
             : UniformBuffer( model ), m_Block( model.Size )
        {
        }

        Common::BoolResultStr SetData( const void* data, uint32_t size, uint32_t offset ) override
        {
            return m_Block.Write( data, size, offset, Frame(), Maker() );
        }

        Common::BoolResultStr EnsureMapped() override
        {
            IBlockCopy* copy = nullptr;
            return m_Block.Resolve( Frame(), Maker(), copy );
        }

        const void* GetData() const override
        {
            return nullptr;
        }

        [[nodiscard]] uint64_t ActiveAppliedVersion() const override
        {
            return m_Block.ActiveAppliedVersion( Frame() );
        }

        void NoteActiveApplied( uint64_t version ) override
        {
            m_Block.NoteActiveApplied( Frame(), version );
        }

        // Writes `view`'s copy for `frame` has received, or 0 when it has no copy.
        [[nodiscard]] uint32_t Writes( const Graphic::ViewResources& view, uint32_t frame ) const
        {
            const auto* copy = view.Find( m_Block.GetKey(), frame );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            return copy != nullptr ? static_cast<const BytesCopy*>( copy )->Writes : 0;
        }

        // The copy the descriptor would be written with now (VulkanUniformBuffer::BindActiveCopy's id).
        uint64_t BindActiveCopy()
        {
            IBlockCopy* copy = nullptr;
            EXPECT_TRUE( m_Block.Resolve( Frame(), Maker(), copy ).IsSuccess() );
            return copy != nullptr ? copy->GetId() : 0;
        }

        // Bytes `view` holds for `frame`, or nullopt when it has no copy.
        [[nodiscard]] std::optional<std::vector<std::byte>> Copy( const Graphic::ViewResources& view,
                                                                  uint32_t                      frame ) const
        {
            const auto* copy = view.Find( m_Block.GetKey(), frame );
            if ( copy == nullptr )
                return std::nullopt;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            return static_cast<const BytesCopy*>( copy )->Bytes;
        }

    private:
        static uint32_t Frame()
        {
            return Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        }

        [[nodiscard]] ViewCopiedBlock::CopyMaker Maker() const
        {
            const uint32_t size = GetSize();
            return [size]( std::string_view, uint32_t, std::unique_ptr<IBlockCopy>& out )
            {
                out = std::make_unique<BytesCopy>( size );
                return Common::MakeSuccess( true );
            };
        }

        ViewCopiedBlock m_Block;
    };

    // Counts the backend's uniform-buffer applies; the other kinds are not under test.
    class RecordingBackend final : public Graphic::MaterialBackend
    {
    public:
        RecordingBackend() : MaterialBackend( nullptr )
        {
        }
        void ApplyUniformBuffer( Graphic::MaterialProperty* ) override
        {
            ++UniformApplies;
        }
        void ApplyStorageBuffer( Graphic::MaterialProperty* ) override
        {
        }
        void ApplyTexture2D( Graphic::MaterialProperty* ) override
        {
        }
        void ApplyTextureCube( Graphic::MaterialProperty* ) override
        {
        }
        void FlushUpdates() override
        {
        }
        uint32_t UniformApplies = 0;
    };

    ShaderLayout::UniformBuffer MakeModel()
    {
        ShaderLayout::UniformBuffer model;
        model.Name         = "MaterialUB";
        model.BindingPoint = 1;
        model.Size         = kFieldSize;
        model.Fields.push_back( { Core::Formats::ShaderValueType::Float4, "Tint", kFieldSize, 0, 1 } );
        return model;
    }

    std::vector<std::byte> Authored( float r, float g, float b, float a )
    {
        const float            v[4] = { r, g, b, a };
        std::vector<std::byte> out( kFieldSize );
        std::memcpy( out.data(), v, kFieldSize );
        return out;
    }

    class MaterialParamUpload : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            EngineContext::CreateInstance();
            Engine::FrameManager::CreateInstance().Initialize( kFramesInFlight );
            EngineContext::GetInstance().SetActiveRendererSlot( 0 );
        }
        void TearDown() override
        {
            Graphic::ViewResourceRegistry::FrameContext().Clear();
        }
    };

    enum class Route
    {
        RestateEveryFrame, // MaterialComponent shader override: re-applies its params per draw
        ApplyOnceThenFlush // material asset: params applied when the asset loaded, flushed per draw
    };

    void RunRoute( Graphic::UniformBufferProperty& prop, Route route, uint32_t frameCount,
                   const std::vector<std::byte>& value )
    {
        if ( route == Route::ApplyOnceThenFlush )
        {
            prop.WriteField( prop.GetField( "Tint" ), value.data(), value.size() );
            prop.UpdateFields();
        }
        for ( uint32_t i = 0; i < frameCount; ++i )
        {
            if ( route == Route::RestateEveryFrame )
            {
                prop.WriteField( prop.GetField( "Tint" ), value.data(), value.size() );
                prop.UpdateFields();
            }
            else if ( prop.HasDirtyFields() )
            {
                prop.UpdateFields();
            }
            Engine::FrameManager::GetInstance().NextFrame();
        }
    }
} // namespace

TEST_F( MaterialParamUpload, BothRoutesLeaveTheSameBytesInEveryFrameCopy )
{
    const auto                     value    = Authored( 0.9f, 0.12f, 0.08f, 1.0f );
    auto                           restated = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    auto                           asset    = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::ViewResources         view( "main" );
    const Graphic::ActiveViewScope scope( view );

    Graphic::UniformBufferProperty overrideProp( restated );
    RunRoute( overrideProp, Route::RestateEveryFrame, kFramesInFlight * 2, value );
    Engine::FrameManager::GetInstance().Initialize( kFramesInFlight );
    Graphic::UniformBufferProperty assetProp( asset );
    RunRoute( assetProp, Route::ApplyOnceThenFlush, kFramesInFlight * 2, value );

    for ( uint32_t f = 0; f < kFramesInFlight; ++f )
    {
        const auto assetCopy = asset->Copy( view, f );
        if ( !assetCopy )
        {
            ADD_FAILURE() << "asset route never made frame copy " << f;
            return;
        }
        EXPECT_EQ( *assetCopy, value ) << "asset route left frame copy " << f << " unwritten";
        EXPECT_EQ( asset->Copy( view, f ), restated->Copy( view, f ) ) << "the routes disagree on frame " << f;
    }
}

// The isolation invariant: two views, two copies, and a write inside A's scope never reaches B's copy.
TEST_F( MaterialParamUpload, AWriteInOneViewNeverReachesAnotherViewsExistingCopy )
{
    const auto             first  = Authored( 0.1f, 0.2f, 0.3f, 1.0f );
    const auto             second = Authored( 0.7f, 0.6f, 0.5f, 1.0f );
    auto                   buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::ViewResources a( "viewport" );
    Graphic::ViewResources b( "preview" );

    {
        const Graphic::ActiveViewScope scope( b );
        ASSERT_TRUE( buffer->SetData( first.data(), kFieldSize, 0 ).IsSuccess() );
    }
    {
        const Graphic::ActiveViewScope scope( a );
        ASSERT_TRUE( buffer->SetData( second.data(), kFieldSize, 0 ).IsSuccess() );
    }

    EXPECT_EQ( a.CopyCount(), 1u );
    EXPECT_EQ( b.CopyCount(), 1u );
    const auto copyA = buffer->Copy( a, 0 );
    const auto copyB = buffer->Copy( b, 0 );
    if ( !copyA || !copyB )
    {
        ADD_FAILURE() << "a view lost its copy (A: " << copyA.has_value() << ", B: " << copyB.has_value() << ")";
        return;
    }
    EXPECT_EQ( *copyA, second );
    EXPECT_EQ( *copyB, first ) << "view A's write reached view B's existing copy";
}

// THE RISK THE LEAD NAMED: a write outside every scope (UI, bakes) must land in the frame context, not in
// the copy of whichever view recorded last.
TEST_F( MaterialParamUpload, AWriteOutsideEveryViewLandsInTheFrameContextNotTheLastView )
{
    const auto             inView  = Authored( 0.3f, 0.3f, 0.3f, 1.0f );
    const auto             outside = Authored( 0.9f, 0.0f, 0.9f, 1.0f );
    auto                   buffer  = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::ViewResources view( "viewport" );

    {
        const Graphic::ActiveViewScope scope( view );
        ASSERT_TRUE( buffer->SetData( inView.data(), kFieldSize, 0 ).IsSuccess() );
    }
    ASSERT_TRUE( buffer->SetData( outside.data(), kFieldSize, 0 ).IsSuccess() );

    const auto viewCopy     = buffer->Copy( view, 0 );
    const auto frameContext = buffer->Copy( Graphic::ViewResourceRegistry::FrameContext(), 0 );
    if ( !viewCopy || !frameContext )
    {
        ADD_FAILURE() << "a write made no copy (view: " << viewCopy.has_value()
                      << ", frame context: " << frameContext.has_value() << ")";
        return;
    }
    EXPECT_EQ( *viewCopy, inView ) << "the write outside every view landed in the last view";
    EXPECT_EQ( *frameContext, outside );
}

// Regression B: a view opened AFTER a one-shot write (field route, dirty window long over) must read the
// value, not zeroes. Red when Resolve does not seed the new copy.
TEST_F( MaterialParamUpload, AViewOpenedAfterAOneShotWriteIsSeededWithIt )
{
    const auto                     value  = Authored( 0.1f, 0.85f, 0.15f, 1.0f );
    auto                           buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::UniformBufferProperty prop( buffer );
    Graphic::ViewResources         main( "viewport" );
    {
        const Graphic::ActiveViewScope scope( main );
        RunRoute( prop, Route::ApplyOnceThenFlush, kFramesInFlight * kSlots * 2, value );
        ASSERT_FALSE( prop.HasDirtyFields() ) << "the viewport's copies must all have applied the write";
    }

    Graphic::ViewResources         preview( "preview" );
    const Graphic::ActiveViewScope scope( preview );
    for ( uint32_t f = 0; f < kFramesInFlight; ++f )
    {
        ASSERT_TRUE( buffer->EnsureMapped().IsSuccess() );
        const auto previewCopy =
             buffer->Copy( preview, Engine::FrameManager::GetInstance().GetCurrentFrameIndex() );
        if ( !previewCopy )
        {
            ADD_FAILURE() << "binding the new view made no copy (frame " << f << ")";
            return;
        }
        EXPECT_EQ( *previewCopy, value )
             << "a new view's copy started without the material's parameters (frame " << f << ")";
        Engine::FrameManager::GetInstance().NextFrame();
    }
}

// Regression A, half one: a CLEAN property still reaches the backend, which is the only place that can
// tell a set pointing at a dropped copy. Red when UniformBufferProperty::Apply gates on IsDirty().
TEST_F( MaterialParamUpload, ACleanPropertyStillAsksTheBackend )
{
    auto                           buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::UniformBufferProperty prop( buffer );
    RecordingBackend               backend;

    prop.Apply( &backend );
    EXPECT_EQ( backend.UniformApplies, 1u ) << "a clean property never asked whether its copy changed";
}

// Regression A, half two: a preview closed and another opened (the slot's set was written for the closed
// one's copy) must be rewritten although nothing was written. Red when NeedsWrite looks at the version alone.
TEST_F( MaterialParamUpload, ASetWrittenForAClosedViewsCopyIsRewrittenForTheNextView )
{
    auto                 buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    DescriptorCopyRecord set; // one [frame][slot] set, reused by both previews
    constexpr uint32_t   kBinding = 1;
    constexpr uint64_t   kVersion = 7; // unchanged throughout: nothing is written to the property

    uint64_t closedCopy = 0;
    {
        Graphic::ViewResources         first( "preview A" );
        const Graphic::ActiveViewScope scope( first );
        closedCopy = buffer->BindActiveCopy();
        ASSERT_TRUE( set.NeedsWrite( kBinding, closedCopy, kVersion ) );
        set.NoteWritten( kBinding, closedCopy, kVersion );
        EXPECT_FALSE( set.NeedsWrite( kBinding, closedCopy, kVersion ) ) << "an up-to-date set was rewritten";
    } // preview A closes: its copy is dropped (deferred-deleted in production)

    Graphic::ViewResources         second( "preview B" );
    const Graphic::ActiveViewScope scope( second );
    const uint64_t                 liveCopy = buffer->BindActiveCopy();
    EXPECT_NE( liveCopy, closedCopy ) << "copy ids were reused — the ABA a VkBuffer handle would have";
    EXPECT_TRUE( set.NeedsWrite( kBinding, liveCopy, kVersion ) )
         << "the set still points at the closed preview's freed copy and would not be rewritten";
}

// RT2f. Dirtiness is a question about one (view x frame) copy, answered by versions. The three tests below were
// red under the per-renderer-slot countdown (MaterialProperty/FieldProperty::m_DirtyCount): ActiveViewScope does
// not assign slots, so every view here shares one, exactly as views beyond the slot count and the frame
// context do in the editor.

// A view created 100 frames after a one-off write owes itself the write through the PROPERTY, not only through
// the copy's seed: the countdown had drained, so HasDirtyFields said "nothing to do" to a view that had applied
// nothing.
TEST_F( MaterialParamUpload, AViewCreatedAHundredFramesAfterAOneOffWriteStillReceivesIt )
{
    const auto                     value  = Authored( 0.3f, 0.6f, 0.9f, 1.0f );
    auto                           buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::UniformBufferProperty prop( buffer );
    Graphic::ViewResources         main( "viewport" );
    {
        const Graphic::ActiveViewScope scope( main );
        RunRoute( prop, Route::ApplyOnceThenFlush, 100, value );
    }

    Graphic::ViewResources         preview( "preview" );
    const Graphic::ActiveViewScope scope( preview );
    for ( uint32_t f = 0; f < kFramesInFlight; ++f )
    {
        EXPECT_TRUE( prop.HasDirtyFields() )
             << "a view that has applied nothing was told it is up to date (frame " << f << ")";
        prop.UpdateFields();
        EXPECT_FALSE( prop.HasDirtyFields() ) << "applying did not bring this view's copy up to date";
        const auto copy = buffer->Copy( preview, Engine::FrameManager::GetInstance().GetCurrentFrameIndex() );
        ASSERT_TRUE( copy.has_value() );
        EXPECT_EQ( copy, value ) << "frame " << f;
        Engine::FrameManager::GetInstance().NextFrame();
    }
}

// The preview made its copies, then sat idle while the viewport rendered 100 frames after a one-off write. Its
// EXISTING copies (no seed happens) must take the write when it records again.
TEST_F( MaterialParamUpload, AnIdleViewsExistingCopiesReceiveAWriteMadeWhileItWasIdle )
{
    const auto                     before = Authored( 0.2f, 0.2f, 0.2f, 1.0f );
    const auto                     after  = Authored( 0.9f, 0.1f, 0.1f, 1.0f );
    auto                           buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::UniformBufferProperty prop( buffer );
    Graphic::ViewResources         main( "viewport" );
    Graphic::ViewResources         preview( "preview" );
    {
        const Graphic::ActiveViewScope scope( preview );
        RunRoute( prop, Route::ApplyOnceThenFlush, kFramesInFlight, before );
    }
    {
        const Graphic::ActiveViewScope scope( main );
        RunRoute( prop, Route::ApplyOnceThenFlush, 100, after );
    }

    const Graphic::ActiveViewScope scope( preview );
    for ( uint32_t f = 0; f < kFramesInFlight; ++f )
    {
        if ( prop.HasDirtyFields() )
            prop.UpdateFields();
        const auto copy = buffer->Copy( preview, Engine::FrameManager::GetInstance().GetCurrentFrameIndex() );
        ASSERT_TRUE( copy.has_value() );
        EXPECT_EQ( copy, after ) << "the idle view kept the old value in frame copy " << f;
        Engine::FrameManager::GetInstance().NextFrame();
    }
}

// A write after the view has applied everything reaches each of its frame copies EXACTLY once: not zero (the
// value would be missing) and not once per frame of some window (the countdown rewrote every frame it lasted).
TEST_F( MaterialParamUpload, AWriteAfterTheViewAppliedIsWrittenOnceIntoEachFrameCopy )
{
    const auto                     first  = Authored( 0.1f, 0.1f, 0.1f, 1.0f );
    const auto                     second = Authored( 0.5f, 0.4f, 0.3f, 1.0f );
    auto                           buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
    Graphic::UniformBufferProperty prop( buffer );
    Graphic::ViewResources         view( "viewport" );
    const Graphic::ActiveViewScope scope( view );
    RunRoute( prop, Route::ApplyOnceThenFlush, kFramesInFlight * 2, first );

    std::vector<uint32_t> writesBefore;
    for ( uint32_t f = 0; f < kFramesInFlight; ++f )
        writesBefore.push_back( buffer->Writes( view, f ) );

    ASSERT_TRUE( prop.WriteField( prop.GetField( "Tint" ), second.data(), second.size() ) );
    for ( uint32_t i = 0; i < kFramesInFlight * 4; ++i )
    {
        if ( prop.HasDirtyFields() )
            prop.UpdateFields();
        Engine::FrameManager::GetInstance().NextFrame();
    }

    for ( uint32_t f = 0; f < kFramesInFlight; ++f )
    {
        EXPECT_EQ( buffer->Writes( view, f ) - writesBefore[f], 1u ) << "frame copy " << f;
        EXPECT_EQ( buffer->Copy( view, f ), std::optional( second ) ) << "frame copy " << f;
    }
}

TEST_F( MaterialParamUpload, DestroyingTheBufferTakesItsCopiesBackFromEveryView )
{
    Graphic::ViewResources a( "viewport" );
    Graphic::ViewResources b( "preview" );
    {
        auto buffer = std::make_shared<RecordingUniformBuffer>( MakeModel() );
        for ( Graphic::ViewResources* view : { &a, &b } )
        {
            const Graphic::ActiveViewScope scope( *view );
            ASSERT_TRUE( buffer->EnsureMapped().IsSuccess() );
        }
        ASSERT_EQ( a.CopyCount() + b.CopyCount(), 2u );
    }
    EXPECT_EQ( a.CopyCount(), 0u );
    EXPECT_EQ( b.CopyCount(), 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
