// THE TRANSPORT NOTICES A CLIENT THAT HAS GONE, BEFORE IT JUDGES THE NEXT ONE.
//
// Everything else in the control channel is a pure header and is asserted as one. This is the socket, and
// it is here because of a defect that reached a live editor: the client slot was still occupied by a peer
// that had already closed, so the next connection was refused with "another client already holds this
// editor's control channel". It was not a race in the client. A disconnected peer is only noticed by
// READING from it -- recv returning zero is the whole of the signal -- and the accept loop ran first, so
// the editor simply had not looked yet.
//
// It mattered because of how the channel is actually used: one request per invocation of desertctl, which
// connects, asks, reads and closes. Every second command failed. The fix is an ordering -- service the
// client we have, then judge a new one -- and an ordering is exactly the kind of thing that gets undone by
// somebody tidying a function six months from now.
//
// The relations:
//   1. A CLIENT THAT CLOSED FREES THE SLOT, and the next connection is accepted rather than refused.
//   2. Sequential clients work, over and over -- the real usage pattern, not a single round trip.
//   3. A request arrives on the poll it was sent to, not a poll later.
//   4. ONE LINE PER POLL, so a client cannot hand itself two commands in flight and take the channel's
//      ordering guarantee away from it.
//   5. A SECOND SIMULTANEOUS CLIENT IS REFUSED IN WORDS, not left hanging -- silence would look exactly
//      like an editor that had frozen.
//   6. A socket path another process is serving is REFUSED; a leftover from a dead one is cleared.

#include <Editor/Core/Control/ControlSocket.hpp>

#include <Common/Core/LocalSocket.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using Desert::Editor::Control::ControlSocket;

namespace
{
    namespace Socket = Common::LocalSocket;

    /// The process id, for a socket path no other run of this suite can collide with. The one thing the
    /// two platforms do not spell alike that Common/Core/LocalSocket.hpp has no reason to carry: it is
    /// scaffolding for a test, not part of a transport.
    [[nodiscard]] unsigned long OwnProcessId()
    {
#if defined( _WIN32 )
        return ::GetCurrentProcessId();
#else
        return static_cast<unsigned long>( ::getpid() );
#endif
    }

    /// A path in the machine's own temp directory, unique to this process so two runs of the suite cannot
    /// collide. Asked of <filesystem> rather than written as "/tmp/...": the sockets in this suite are real
    /// files, and Windows has no /tmp.
    std::string TempSocketPath( const char* tag )
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path();
        return ( directory /
                 ( std::string( "desert_ctl_" ) + tag + "_" + std::to_string( OwnProcessId() ) + ".sock" ) )
             .string();
    }

    /// A bare client: connect, send a line, read a line, close. Deliberately not DesertCtl — the point is
    /// to exercise the SERVER against an ordinary socket peer.
    class Client
    {
    public:
        explicit Client( const std::string& path )
        {
            m_Fd = Socket::Open();
            if ( m_Fd < 0 )
                return;

            sockaddr_un address{};
            if ( !Socket::FillAddress( address, path ) ||
                 ::connect( Socket::Raw( m_Fd ), reinterpret_cast<const sockaddr*>( &address ),
                            sizeof( address ) ) != 0 )
            {
                Socket::Close( m_Fd );
                m_Fd = Socket::kInvalid;
            }
        }

        ~Client()
        {
            Close();
        }

        Client( const Client& )            = delete;
        Client& operator=( const Client& ) = delete;

        [[nodiscard]] bool Connected() const
        {
            return m_Fd >= 0;
        }

        void Send( const std::string& line )
        {
            const std::string framed = line + "\n";
            (void)::send( Socket::Raw( m_Fd ), framed.data(), static_cast<int>( framed.size() ), 0 );
        }

        /// Read until a newline, or give up. Blocking: the server is driven by the test itself, so there
        /// is nobody to wait for that the test has not already asked to run.
        [[nodiscard]] std::string ReadLine()
        {
            std::string received;
            char        buffer[512];
            for ( int attempt = 0; attempt < 64; ++attempt )
            {
                const auto newline = received.find( '\n' );
                if ( newline != std::string::npos )
                    return received.substr( 0, newline );

                const auto got = static_cast<std::ptrdiff_t>(
                     ::recv( Socket::Raw( m_Fd ), buffer, static_cast<int>( sizeof( buffer ) ), 0 ) );
                if ( got <= 0 )
                    break;
                received.append( buffer, static_cast<std::size_t>( got ) );
            }
            const auto newline = received.find( '\n' );
            return newline == std::string::npos ? received : received.substr( 0, newline );
        }

        void Close()
        {
            if ( m_Fd >= 0 )
                Socket::Close( m_Fd );
            m_Fd = Socket::kInvalid;
        }

    private:
        Socket::Handle m_Fd = Socket::kInvalid;
    };
} // namespace

TEST( ControlTransport, ListeningCreatesTheSocketAndClosingRemovesIt )
{
    const std::string path = TempSocketPath( "lifecycle" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );
    EXPECT_TRUE( socket.IsListening() );
    EXPECT_TRUE( Socket::FileExists( path ) );

    socket.Close();
    EXPECT_FALSE( socket.IsListening() );
    // A leftover path is not harmless: the next editor given it PROBES what is there, and a file left
    // behind on every exit would train everybody to ignore the line that says so.
    EXPECT_FALSE( Socket::FileExists( path ) ) << "the socket file outlived the socket";
}

// A path another editor is serving must be REFUSED. Two editors sharing one socket would answer each
// other's clients, and the client could not tell which one it was talking to.
TEST( ControlTransport, APathAnotherEditorIsServingIsRefused )
{
    const std::string path = TempSocketPath( "contested" );
    (void)Socket::RemoveFile( path );

    ControlSocket first;
    ASSERT_TRUE( first.Listen( path ).IsSuccess() );

    ControlSocket second;
    const auto    refused = second.Listen( path );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( path ), std::string::npos ) << "the refusal must name the path";
    EXPECT_FALSE( second.IsListening() );

    first.Close();
}

// A path left behind by a DEAD editor is cleared and reused. The distinction from the test above is the
// whole reason the transport probes rather than looking at the file: a socket file outlives its process,
// so "the file is there" says nothing at all about whether anybody is serving it.
TEST( ControlTransport, ALeftoverSocketFromADeadEditorIsCleared )
{
    const std::string path = TempSocketPath( "stale" );
    (void)Socket::RemoveFile( path );

    // A CRASHED editor leaves the path behind. ~ControlSocket unlinks on a clean exit, so the leftover is
    // built directly: a socket bound to the path and then dropped, which is what the filesystem is left
    // holding when a process dies without running a destructor.
    {
        const Socket::Handle fd = Socket::Open();
        ASSERT_GE( fd, 0 );
        sockaddr_un address{};
        ASSERT_TRUE( Socket::FillAddress( address, path ) );
        ASSERT_EQ( ::bind( Socket::Raw( fd ), reinterpret_cast<const sockaddr*>( &address ), sizeof( address ) ),
                   0 )
             << Socket::LastErrorText();
        Socket::Close( fd ); // bound, never listened, descriptor gone: nothing is serving this path
    }
    ASSERT_TRUE( Socket::FileExists( path ) );

    ControlSocket fresh;
    EXPECT_TRUE( fresh.Listen( path ).IsSuccess() )
         << "a path nothing is serving must be cleared, not treated as a live editor";
    fresh.Close();
}

// A path longer than the kernel's sun_path would be TRUNCATED and bound somewhere else, and the editor
// would report success while listening where the client never looks.
TEST( ControlTransport, AnOverlongPathIsRefusedRatherThanTruncated )
{
    ControlSocket socket;
    const auto    refused = socket.Listen( std::string( 200, 'x' ) );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "truncated" ), std::string::npos );
}

// ---------------------------------------------------------------------------------------------------
// 1-3. The defect that reached a live editor.
// ---------------------------------------------------------------------------------------------------

TEST( ControlTransport, ARequestArrivesOnThePollItWasSentTo )
{
    const std::string path = TempSocketPath( "oneshot" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    Client client( path );
    ASSERT_TRUE( client.Connected() );
    client.Send( R"({"id":1,"op":"commands"})" );

    // ONE poll. Accepting on this poll and reading on the next would cost every command a frame for a
    // reason nobody watching the editor could see.
    const auto line = socket.PollRequestLine();
    ASSERT_TRUE( line.has_value() ) << "the connection and its first line must both land on one poll";
    EXPECT_EQ( *line, R"({"id":1,"op":"commands"})" );

    socket.SendResponseLine( R"({"id":1,"ok":true})" );
    EXPECT_EQ( client.ReadLine(), R"({"id":1,"ok":true})" );

    socket.Close();
}

// THE DEFECT ITSELF. The first client closed; the slot must be free by the time the second one is judged.
// Before the fix this returned the "another client already holds this channel" refusal, because a peer
// that has closed is only noticed by reading from it and the accept loop ran first.
TEST( ControlTransport, AClientThatClosedFreesTheSlotBeforeTheNextIsJudged )
{
    const std::string path = TempSocketPath( "sequential" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    {
        Client first( path );
        ASSERT_TRUE( first.Connected() );
        first.Send( R"({"id":1,"op":"state"})" );
        ASSERT_TRUE( socket.PollRequestLine().has_value() );
        socket.SendResponseLine( R"({"id":1,"ok":true})" );
        EXPECT_EQ( first.ReadLine(), R"({"id":1,"ok":true})" );
    } // first closes here, exactly as desertctl does after reading its reply

    Client second( path );
    ASSERT_TRUE( second.Connected() );
    second.Send( R"({"id":2,"op":"state"})" );

    const auto line = socket.PollRequestLine();
    ASSERT_TRUE( line.has_value() )
         << "the previous client had already closed and its slot must have been reaped first";
    EXPECT_EQ( *line, R"({"id":2,"op":"state"})" );

    socket.SendResponseLine( R"({"id":2,"ok":true})" );
    const std::string reply = second.ReadLine();
    EXPECT_EQ( reply, R"({"id":2,"ok":true})" )
         << "the second client was refused as a duplicate; that is the defect this test exists for";

    socket.Close();
}

// The real usage pattern, repeated. One round trip working proves less than it looks: the defect above
// appeared on the SECOND connection and would have been invisible to a single-exchange test.
TEST( ControlTransport, SequentialClientsWorkOverAndOver )
{
    const std::string path = TempSocketPath( "repeated" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    for ( int i = 0; i < 8; ++i )
    {
        const std::string request = R"({"id":)" + std::to_string( i ) + R"(,"op":"state"})";
        const std::string answer  = R"({"id":)" + std::to_string( i ) + R"(,"ok":true})";

        Client client( path );
        ASSERT_TRUE( client.Connected() ) << "connection " << i;
        client.Send( request );

        const auto line = socket.PollRequestLine();
        ASSERT_TRUE( line.has_value() ) << "request " << i << " was not read";
        EXPECT_EQ( *line, request );

        socket.SendResponseLine( answer );
        EXPECT_EQ( client.ReadLine(), answer ) << "reply " << i;
    }

    socket.Close();
}

// ---------------------------------------------------------------------------------------------------
// 4-5. One line per poll, and a second simultaneous client.
// ---------------------------------------------------------------------------------------------------

// A client that pipelines must not get two commands in flight. The channel promises that a reply follows a
// frame reflecting ITS command; two commands running before either was answered would hand that promise to
// whoever wrote the client.
TEST( ControlTransport, OnlyOneLineIsHandedBackPerPoll )
{
    const std::string path = TempSocketPath( "pipelined" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    Client client( path );
    ASSERT_TRUE( client.Connected() );
    client.Send( R"({"id":1,"op":"state"})" );
    client.Send( R"({"id":2,"op":"state"})" );

    const auto first = socket.PollRequestLine();
    ASSERT_TRUE( first.has_value() );
    EXPECT_EQ( *first, R"({"id":1,"op":"state"})" );

    // The second is BUFFERED, not lost: it is offered on a later poll, once the editor is ready for it.
    const auto second = socket.PollRequestLine();
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( *second, R"({"id":2,"op":"state"})" );

    socket.Close();
}

// A second client while one is live is refused IN WORDS. Left hanging, it would look exactly like an
// editor that had frozen, and the whole point of this channel is that a failure says what it is.
TEST( ControlTransport, ASecondSimultaneousClientIsRefusedInWords )
{
    const std::string path = TempSocketPath( "twoclients" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    Client holder( path );
    ASSERT_TRUE( holder.Connected() );
    holder.Send( R"({"id":1,"op":"state"})" );
    ASSERT_TRUE( socket.PollRequestLine().has_value() );

    Client intruder( path );
    ASSERT_TRUE( intruder.Connected() );
    (void)socket.PollRequestLine(); // the poll on which the second connection is judged

    const std::string refusal = intruder.ReadLine();
    EXPECT_FALSE( refusal.empty() ) << "a refused client must be told, not left waiting";
    EXPECT_NE( refusal.find( "\"ok\":false" ), std::string::npos );
    EXPECT_NE( refusal.find( "another client" ), std::string::npos );

    socket.Close();
}

// Nothing to read is not an error, and it must not drop the client. This is the ordinary case: the editor
// polls sixty times a second and the client is usually thinking.
TEST( ControlTransport, AnIdlePollKeepsTheClient )
{
    const std::string path = TempSocketPath( "idle" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    Client client( path );
    ASSERT_TRUE( client.Connected() );
    (void)socket.PollRequestLine(); // accept
    ASSERT_TRUE( socket.HasClient() );

    for ( int i = 0; i < 10; ++i )
        EXPECT_FALSE( socket.PollRequestLine().has_value() );
    EXPECT_TRUE( socket.HasClient() ) << "an idle client was dropped for having nothing to say";

    client.Send( R"({"id":9,"op":"state"})" );
    const auto line = socket.PollRequestLine();
    ASSERT_TRUE( line.has_value() );
    EXPECT_EQ( *line, R"({"id":9,"op":"state"})" );

    socket.Close();
}

// ── 7. WHICH CONNECTION IS ON THE OTHER END, AND WHY "SOMEBODY IS" IS NOT THE SAME QUESTION ────────
//
// A6-1 gave the channel a wait that can last a whole boot: a request that arrives before the editor has
// read the project is PARKED rather than answered from a half-built one. That turned a four-second window
// into a thirty-second one, and it is what made the following reachable.
//
// MEASURED ON THE REAL EDITOR. Client A parked a `commands` during the boot and was killed. The editor
// noticed the disconnect and, in the SAME service call, accepted client B into the freed slot — the accept
// loop runs immediately after the read that detects a loss. `HasClient()` was true again, the parked
// request went on, and A's answer of 311 commands was written to B's socket. Both had sent id 1, so the
// reply was indistinguishable from B's own at both ends: a completely convincing answer to a question
// nobody asked.
//
// So the transport carries a GENERATION. Not "is somebody connected" — that was true throughout — but
// "is it the SAME somebody".

TEST( ControlTransport, EveryAcceptedConnectionIsANewGeneration )
{
    const std::string path = TempSocketPath( "generation" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    // Before anyone has ever connected. Zero cannot be a live connection's generation, so a caller that
    // recorded one here could never mistake a later client for it.
    EXPECT_EQ( socket.ClientGeneration(), 0u );

    uint64_t previous = 0;
    for ( int i = 0; i < 4; ++i )
    {
        Client client( path );
        ASSERT_TRUE( client.Connected() ) << "connection " << i;
        client.Send( R"({"id":1,"op":"state"})" );
        ASSERT_TRUE( socket.PollRequestLine().has_value() ) << "connection " << i;

        EXPECT_GT( socket.ClientGeneration(), previous ) << "connection " << i << " reused a generation";
        previous = socket.ClientGeneration();

        socket.SendResponseLine( R"({"id":1,"ok":true})" );
        (void)client.ReadLine();
    }

    socket.Close();
}

// AN IDLE CLIENT KEEPS ITS GENERATION. The whole value of the number is that it moves ONLY on a new
// connection: one that drifted per poll would abandon every request that waited a frame, which is every
// request the channel makes a promise about.
TEST( ControlTransport, AGenerationDoesNotMoveWhileOneClientStays )
{
    const std::string path = TempSocketPath( "generation-stable" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    Client client( path );
    ASSERT_TRUE( client.Connected() );
    (void)socket.PollRequestLine(); // accept
    ASSERT_TRUE( socket.HasClient() );

    const uint64_t generation = socket.ClientGeneration();
    for ( int i = 0; i < 10; ++i )
    {
        socket.ServiceConnection();
        EXPECT_EQ( socket.ClientGeneration(), generation ) << "poll " << i;
    }

    socket.Close();
}

// THE DEFECT, IN ITS OWN SHAPE. The first client goes away and the second arrives; `HasClient()` says yes
// both before and after, and only the generation distinguishes them. A caller holding the old number is
// what stops one client's answer reaching another's socket.
TEST( ControlTransport, AReplacementClientIsDistinguishableFromTheOneItReplaced )
{
    const std::string path = TempSocketPath( "generation-swap" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    uint64_t asker = 0;
    {
        Client first( path );
        ASSERT_TRUE( first.Connected() );
        first.Send( R"({"id":1,"op":"commands"})" );
        ASSERT_TRUE( socket.PollRequestLine().has_value() );
        ASSERT_TRUE( socket.HasClient() );
        asker = socket.ClientGeneration();
        // ...and the reply is NOT sent: this is a request parked for the readiness wait.
    } // the asker goes away, exactly as a client killed mid-wait does

    Client second( path );
    ASSERT_TRUE( second.Connected() );

    // ONE service call, which is where the loss and the accept both happen. This is the call that used to
    // leave the editor believing its asker was still there.
    socket.ServiceConnection();

    EXPECT_TRUE( socket.HasClient() ) << "the question 'is somebody connected' answers YES here, which is "
                                         "exactly why it is the wrong question";
    EXPECT_NE( socket.ClientGeneration(), asker )
         << "the parked request's reply would have been written to a client that never asked for it";

    socket.Close();
}

// ServiceConnection does everything PollRequestLine's first half does and takes NO request. The editor
// needs that while a request is in flight: reading a second one would hand the channel's ordering
// guarantee to whoever wrote the client, but not looking at all leaves it holding a dead connection.
TEST( ControlTransport, ServicingTheConnectionDoesNotConsumeARequest )
{
    const std::string path = TempSocketPath( "service-only" );
    (void)Socket::RemoveFile( path );

    ControlSocket socket;
    ASSERT_TRUE( socket.Listen( path ).IsSuccess() );

    Client client( path );
    ASSERT_TRUE( client.Connected() );
    client.Send( R"({"id":7,"op":"state"})" );

    for ( int i = 0; i < 5; ++i )
        socket.ServiceConnection();
    EXPECT_TRUE( socket.HasClient() );

    // Still there, whole, and offered on the poll that does ask for it.
    const auto line = socket.PollRequestLine();
    ASSERT_TRUE( line.has_value() ) << "servicing the connection swallowed the request";
    EXPECT_EQ( *line, R"({"id":7,"op":"state"})" );

    socket.Close();
}

int main( int argc, char** argv )
{
    // The suite opens sockets of its own — the bare client, and the leftover a dead editor would have
    // left — and on Windows the first socket call in a process fails until the library is up. The editor
    // does this inside Listen; nothing here would have, and every test would fail as "socket() failed".
    if ( const std::string unavailable = Common::LocalSocket::EnsureLibraryReady(); !unavailable.empty() )
    {
        std::fprintf( stderr, "the platform's socket library could not be started: %s\n", unavailable.c_str() );
        return 1;
    }

    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
