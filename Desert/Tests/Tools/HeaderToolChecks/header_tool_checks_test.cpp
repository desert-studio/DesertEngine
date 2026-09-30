#include <gtest/gtest.h>

#include <HeaderScan.hpp>

#include <string>
#include <vector>

using namespace Desert::HeaderTool;

namespace
{
    const char* const kEvents = R"(
namespace Common
{
    class KeyPressedEvent
    {
    public:
        DESERT_ROUTED_EVENT( KeyPressedEvent, KeyPressed, Focus )
    };
    class EventWindowResize
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowResize, WindowResized, Broadcast )
    };
}
)";

    const char* const kLayer = R"(
namespace Common
{
    class Layer
    {
    public:
        void JoinEvents( EventNodeLink link );
    };
}
)";

    HeaderModel Scan( const std::string& checked, const std::string& extraChecked = {} )
    {
        std::vector<ScannedFile> files{
             { "Common/Events.hpp", kEvents, false, "Common/Events.hpp" },
             { "Common/Layer.hpp", kLayer, false, "Common/Layer.hpp" },
             { "Sample/Sample.hpp", checked, true, "Sample/Sample.hpp" },
        };
        if ( !extraChecked.empty() )
            files.push_back( { "Sample/Sample.cpp", extraChecked, true, "Sample/Sample.cpp" } );
        return ScanHeaders( files );
    }

    bool MentionsInOrder( const Diagnostic& diagnostic, const std::vector<std::string>& words )
    {
        size_t from = 0;
        for ( const std::string& word : words )
        {
            from = diagnostic.Message.find( word, from );
            if ( from == std::string::npos )
                return false;
        }
        return true;
    }
} // namespace

TEST( HeaderToolChecks, TheEventCatalogueIsReadFromDesertRoutedEvent )
{
    const HeaderModel model = Scan( "" );
    ASSERT_EQ( model.Events.size(), 2u );
    EXPECT_EQ( model.Events[0].EventClass, "KeyPressedEvent" );
    EXPECT_EQ( model.Events[0].HandlerSuffix, "KeyPressed" );
    EXPECT_EQ( model.Events[1].HandlerSuffix, "WindowResized" );
}

TEST( HeaderToolChecks, APublicHandlerOfALayerPasses )
{
    const HeaderModel model = Scan( R"(
class Viewport : public Common::Layer
{
public:
    bool OnKeyPressed( Common::KeyPressedEvent& event );
    bool OnPreviewKeyPressed( KeyPressedEvent& );
};
)" );
    EXPECT_TRUE( model.Errors.empty() );
}

TEST( HeaderToolChecks, AMisspeltHandlerIsAnErrorAtItsLine )
{
    const HeaderModel model = Scan( R"(
class Viewport : public Common::Layer
{
public:
    bool OnKeyPresed( KeyPressedEvent& event );
};
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_EQ( model.Errors[0].Line, 5 );
    EXPECT_TRUE(
         MentionsInOrder( model.Errors[0], { "Viewport::OnKeyPresed", "OnKeyPressed", "OnPreviewKeyPressed" } ) );
}

TEST( HeaderToolChecks, AMisspeltPreviewHandlerIsAnError )
{
    const HeaderModel model = Scan( R"(
class Viewport : public Common::Layer
{
public:
    bool OnPreviewKeyPress( KeyPressedEvent& event );
};
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_TRUE( MentionsInOrder( model.Errors[0], { "OnPreviewKeyPress" } ) );
}

TEST( HeaderToolChecks, AHandlerNamedForAnotherEventIsAnError )
{
    const HeaderModel model = Scan( R"(
class Viewport : public Common::Layer
{
public:
    bool OnWindowResized( KeyPressedEvent& event );
};
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_TRUE( MentionsInOrder( model.Errors[0], { "OnWindowResized", "OnKeyPressed" } ) );
}

TEST( HeaderToolChecks, APrivateHandlerIsAnError )
{
    const HeaderModel model = Scan( R"(
class Viewport : public Common::Layer
{
    bool OnKeyPressed( KeyPressedEvent& event );
};
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_EQ( model.Errors[0].Line, 4 );
    EXPECT_TRUE( MentionsInOrder( model.Errors[0], { "Viewport::OnKeyPressed", "private" } ) );
}

TEST( HeaderToolChecks, AProtectedHandlerIsAnError )
{
    const HeaderModel model = Scan( R"(
struct Viewport : Common::Layer
{
protected:
    bool OnKeyPressed( KeyPressedEvent& event );
};
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_TRUE( MentionsInOrder( model.Errors[0], { "protected" } ) );
}

TEST( HeaderToolChecks, AHandlerOutsideTheTreeIsAnErrorAtItsClass )
{
    const HeaderModel model = Scan( R"(
namespace Tools
{
    class Stray
    {
    public:
        bool OnKeyPressed( KeyPressedEvent& event );
    };
}
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_EQ( model.Errors[0].Line, 4 );
    EXPECT_TRUE( MentionsInOrder( model.Errors[0], { "Tools::Stray", "not a node" } ) );
}

TEST( HeaderToolChecks, AClassDerivedFromANodeThroughAnotherClassIsInTheTree )
{
    const HeaderModel model = Scan( R"(
class PanelBase : public Common::Layer {};
class Outliner final : public PanelBase
{
public:
    bool OnKeyPressed( KeyPressedEvent& event );
};
)" );
    EXPECT_TRUE( model.Errors.empty() );
}

TEST( HeaderToolChecks, ASubsystemIsInTheTree )
{
    const HeaderModel model = Scan( R"(
class Hotkeys
{
    DESERT_SUBSYSTEM( Editor )
public:
    bool OnKeyPressed( KeyPressedEvent& event );
};
)" );
    EXPECT_TRUE( model.Errors.empty() );
}

TEST( HeaderToolChecks, AClassAttachedByNameIsInTheTree )
{
    const HeaderModel model = Scan( R"(
class Window
{
public:
    bool OnWindowResized( EventWindowResize& resize );
};
)",
                                    R"(
void Build( EventTree& events, Window& window )
{
    events.Attach<Engine::Window>( events.Root(), window );
}
)" );
    EXPECT_TRUE( model.Errors.empty() );
}

TEST( HeaderToolChecks, AMethodTakingAnUnroutedTypeIsNotAHandler )
{
    const HeaderModel model = Scan( R"(
class Importer
{
    bool OnKeyPresed( ImportJob& job );
    bool OnDone( const KeyPressedEvent& event );
};
)" );
    EXPECT_TRUE( model.Errors.empty() );
}

TEST( HeaderToolChecks, CommentedAndQuotedHandlersAreNotRead )
{
    const HeaderModel model = Scan( R"SAMPLE(
class Stray
{
public:
    // bool OnKeyPressed( KeyPressedEvent& event );
    /* bool OnKeyPresed( KeyPressedEvent& event ); */
    const char* Text = "bool OnKeyPresed( KeyPressedEvent& event ); {";
};
)SAMPLE" );
    EXPECT_TRUE( model.Errors.empty() );
}

TEST( HeaderToolChecks, LinesAreCountedThroughBlockComments )
{
    const HeaderModel model = Scan( R"(
/* one
   two
   three */
class Stray
{
public:
    bool OnKeyPressed( KeyPressedEvent& event );
};
)" );
    ASSERT_EQ( model.Errors.size(), 1u );
    EXPECT_EQ( model.Errors[0].Line, 5 );
}

TEST( HeaderToolChecks, ContextFilesAreReadButNotDiagnosed )
{
    std::vector<ScannedFile> files{
         { "Common/Events.hpp", kEvents, false, "Common/Events.hpp" },
         { "Other/Stray.hpp", "class Stray { public: bool OnKeyPresed( KeyPressedEvent& e ); };", false,
           "Other/Stray.hpp" },
    };
    EXPECT_TRUE( ScanHeaders( files ).Errors.empty() );
}

TEST( HeaderToolChecks, SubsystemsAreListedWithOwnerQualifiedNameAndInclude )
{
    const HeaderModel model = Scan( R"(
namespace Desert::Editor
{
    class Hotkeys
    {
        DESERT_SUBSYSTEM( Editor )
    };
    namespace Detail
    {
        struct Autosave
        {
            DESERT_SUBSYSTEM( Engine )
        };
    }
}
)" );
    ASSERT_EQ( model.Subsystems.size(), 2u );
    EXPECT_EQ( model.Subsystems[0].QualifiedName, "Desert::Editor::Detail::Autosave" );
    EXPECT_EQ( model.Subsystems[0].Owner, "Engine" );
    EXPECT_EQ( model.Subsystems[1].QualifiedName, "Desert::Editor::Hotkeys" );
    EXPECT_EQ( model.Subsystems[1].Owner, "Editor" );
    EXPECT_EQ( model.Subsystems[1].Include, "Sample/Sample.hpp" );
}

TEST( HeaderToolChecks, TheDiagnosticReadsFileLineError )
{
    EXPECT_EQ( FormatDiagnostic( { "Editor/Source/Panel.hpp", 42, "message" } ),
               "Editor/Source/Panel.hpp:42: error: message" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
