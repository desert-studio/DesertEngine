// THE PLAYER'S SETTINGS have three verbs and each one is asserted against what it must reach:
//   Load  — the player's file over the project's, the project's when the player has none, a refusal naming
//           BOTH paths when neither exists, and a broken player file refused rather than quietly replaced;
//   Save  — what is written is what the next Load returns (the round trip), and an invalid value is not written;
//   Apply — every value arrives at its consumer: the window, the frame pacer, the audio mix, the language,
//           the scalability apply point.

#include <gtest/gtest.h>

#include <Engine/Audio/AudioMix.hpp>
#include <Engine/Core/FramePacer.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Settings/GameUserSettings.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../../TestSupport/scratch_dir.hpp"

using namespace Desert;
using namespace Desert::Settings;
namespace fs = std::filesystem;

namespace
{
    // A window that records what it was told; nothing else.
    class RecordingWindow final : public Window
    {
    public:
        WindowMode Mode = WindowMode::Maximized;
        uint32_t   Width = 800, Height = 600;
        int        ModeCalls = 0;
        int        VSyncCalls = 0;
        bool       VSync = false;

        Common::ResultStr<bool> Init() override { return Common::MakeSuccess( true ); }
        void ProcessEvents() override {}
        const std::string& GetTitle() const override { return m_Title; }
        void SetTitle( const std::string& title ) override { m_Title = title; }
        void SetWindowSize( uint32_t, uint32_t ) override {}
        void SetWindowPos( int, int ) override {}
        void GetWindowPos( int& x, int& y ) const override { x = y = 0; }
        void Maximize() override {}
        void Restore() override {}
        void Minimize() override {}
        bool IsWindowMaximized() const override { return Mode == WindowMode::Maximized; }
        Common::BoolResultStr SetWindowMode( WindowMode mode, uint32_t width, uint32_t height ) override
        {
            ++ModeCalls;
            Mode = mode;
            if ( mode == WindowMode::Windowed || mode == WindowMode::Fullscreen )
            {
                Width  = width;
                Height = height;
            }
            return Common::MakeSuccess( true );
        }
        WindowMode GetWindowMode() const override { return Mode; }
        bool IsDecorated() const override { return true; }
        bool HasDrawableArea() const override { return true; }
        void Show() override {}
        void SetVSync( bool enabled ) override { ++VSyncCalls; VSync = enabled; }
        uint32_t GetWidth() const override { return Width; }
        uint32_t GetHeight() const override { return Height; }
        const void* GetNativeWindow() const override { return nullptr; }
        Common::BoolResultStr PrepareNextFrame() const override { return Common::MakeSuccess( true ); }
        Common::BoolResultStr PresentFinalImage() const override { return Common::MakeSuccess( true ); }
        std::shared_ptr<Graphic::SwapChain> GetWindowSwapChain() override { return nullptr; }
        void SetEventTree( Common::EventTree& ) override {}
        Common::EventTree* GetEventTree() const override { return nullptr; }
        Common::ResultStr<bool> SetupSwapChain() override { return Common::MakeSuccess( true ); }

    private:
        std::string m_Title;
    };

    GameUserSettings Sample()
    {
        GameUserSettings s;
        s.Display          = { .Mode = WindowMode::Windowed, .ResolutionX = 1280, .ResolutionY = 720, .VSync = true,
                               .FrameRateLimit = 60 };
        s.Scalability      = { .ViewDistance = 0, .Shadows = 1, .PostProcess = 2, .Textures = 3, .Effects = 1, .Foliage = 0 };
        s.Audio            = { .Master = 0.8f, .Music = 0.25f, .Effects = 0.5f, .Voice = 0.75f };
        s.MouseSensitivity = 1.5f;
        s.Language         = "ru";
        return s;
    }

    class GameUserSettingsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Root = fs::temp_directory_path() /
                     ( "desert-gus-" + std::string( ::testing::UnitTest::GetInstance()->current_test_info()->name() ) );
            fs::remove_all( m_Root );
            fs::create_directories( m_Root / "Project" );
        }
        void TearDown() override { fs::remove_all( m_Root ); }

        fs::path Project() const { return m_Root / "Project"; }
        fs::path User() const { return m_Root / "User"; }

        fs::path m_Root;
    };
} // namespace

TEST_F( GameUserSettingsTest, SaveThenLoadIsTheSameSettings )
{
    const GameUserSettings saved = Sample();
    ASSERT_TRUE( SaveGameUserSettings( saved, User() ) );
    const auto loaded = LoadGameUserSettings( Project(), User() );
    ASSERT_TRUE( loaded ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue(), saved );

    // and a second save of what was loaded writes the same bytes: nothing drifts across rounds
    std::ifstream first( UserGameUserSettingsFile( User() ) );
    const std::string before( ( std::istreambuf_iterator<char>( first ) ), {} );
    ASSERT_TRUE( SaveGameUserSettings( loaded.GetValue(), User() ) );
    std::ifstream second( UserGameUserSettingsFile( User() ) );
    EXPECT_EQ( std::string( ( std::istreambuf_iterator<char>( second ) ), {} ), before );
}

TEST_F( GameUserSettingsTest, TheProjectDefaultsAnswerWhenThePlayerHasNoFile )
{
    GameUserSettings defaults = Sample();
    defaults.Language         = "en";
    ASSERT_TRUE( SaveGameUserSettings( defaults, Project() / "Config" ) );
    fs::rename( Project() / "Config" / "GameUserSettings.json", ProjectGameUserSettingsFile( Project() ) );

    const auto fromProject = LoadGameUserSettings( Project(), User() );
    ASSERT_TRUE( fromProject ) << fromProject.GetError();
    EXPECT_EQ( fromProject.GetValue().Language, "en" );

    ASSERT_TRUE( SaveGameUserSettings( Sample(), User() ) ); // the player's own file now wins
    const auto fromPlayer = LoadGameUserSettings( Project(), User() );
    ASSERT_TRUE( fromPlayer ) << fromPlayer.GetError();
    EXPECT_EQ( fromPlayer.GetValue().Language, "ru" );
}

TEST_F( GameUserSettingsTest, NoFileAnywhereIsARefusalNamingBothPaths )
{
    const auto loaded = LoadGameUserSettings( Project(), User() );
    ASSERT_FALSE( loaded );
    EXPECT_NE( loaded.GetError().find( UserGameUserSettingsFile( User() ).string() ), std::string::npos );
    EXPECT_NE( loaded.GetError().find( ProjectGameUserSettingsFile( Project() ).string() ), std::string::npos );
}

TEST_F( GameUserSettingsTest, ABrokenPlayerFileIsRefusedNotReplacedByTheProjects )
{
    ASSERT_TRUE( SaveGameUserSettings( Sample(), Project() / "Config" ) );
    fs::rename( Project() / "Config" / "GameUserSettings.json", ProjectGameUserSettingsFile( Project() ) );
    fs::create_directories( User() );
    std::ofstream( UserGameUserSettingsFile( User() ) ) << R"({"Display":{}})";
    EXPECT_FALSE( LoadGameUserSettings( Project(), User() ) );
}

TEST_F( GameUserSettingsTest, AnOutOfRangeValueIsNotSaved )
{
    GameUserSettings bad    = Sample();
    bad.Scalability.Shadows = 4;
    EXPECT_FALSE( SaveGameUserSettings( bad, User() ) );
    EXPECT_FALSE( fs::exists( UserGameUserSettingsFile( User() ) ) );
    bad          = Sample();
    bad.Language = "xx-unknown";
    EXPECT_FALSE( SaveGameUserSettings( bad, User() ) );
}

TEST( GameUserSettingsProject, TheGamesOwnDefaultsLoadAndValidate )
{
    const fs::path project = Desert::TestSupport::RepositoryRoot() / "Projects" / "Desert";
    const auto     loaded  = LoadGameUserSettings( project, fs::temp_directory_path() / "desert-gus-no-player" );
    ASSERT_TRUE( loaded ) << loaded.GetError();
}

TEST( GameUserSettingsApply, EveryValueReachesItsConsumer )
{
    RecordingWindow    window;
    Engine::FramePacer pacer;
    const GameUserSettings settings = Sample();
    const auto applied = ApplyGameUserSettings( settings, window, pacer );
    ASSERT_TRUE( applied ) << applied.GetError();

    EXPECT_EQ( window.Mode, WindowMode::Windowed );
    EXPECT_EQ( window.Width, 1280u );
    EXPECT_EQ( window.Height, 720u );
    EXPECT_TRUE( window.VSync );
    EXPECT_EQ( pacer.Limit(), 60u );

    const Audio::AudioMix& mix = Audio::AudioMix::Get();
    EXPECT_FLOAT_EQ( mix.MasterVolume(), 0.8f );
    EXPECT_FLOAT_EQ( mix.ClassVolume( Audio::SoundClass::Music ), 0.25f );
    EXPECT_FLOAT_EQ( mix.ClassVolume( Audio::SoundClass::Effects ), 0.5f );
    EXPECT_FLOAT_EQ( mix.ClassVolume( Audio::SoundClass::Voice ), 0.75f );

    EXPECT_EQ( Localization::Localization::Get().RequestedLanguage().Tag, "ru" );
    EXPECT_EQ( AppliedScalability(), settings.Scalability );
    EXPECT_FLOAT_EQ( AppliedMouseSensitivity(), 1.5f );

    // Applying the same settings again does not move a window that is already there.
    ASSERT_TRUE( ApplyGameUserSettings( settings, window, pacer ) );
    EXPECT_EQ( window.ModeCalls, 1 );
}

TEST( GameUserSettingsApply, AnInvalidValueTouchesNothing )
{
    RecordingWindow    window;
    Engine::FramePacer pacer;
    GameUserSettings   bad = Sample();
    bad.Audio.Master       = 2.0f;
    EXPECT_FALSE( ApplyGameUserSettings( bad, window, pacer ) );
    EXPECT_EQ( window.ModeCalls, 0 );
    EXPECT_EQ( window.VSyncCalls, 0 );
    EXPECT_EQ( pacer.Limit(), 0u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
