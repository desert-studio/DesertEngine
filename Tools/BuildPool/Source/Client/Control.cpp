// BuildPool client: `buildpool status|window|stop`. Status and window start a daemon when none runs (the
// window of an idle pool is still a number a build tool needs); stop never does - no daemon is already stopped.
#include "Client.hpp"

#include <cstdio>
#include <variant>

namespace buildpool::client
{
    namespace
    {
        constexpr uint32_t kAnswerWaitMs = 60'000;
    } // namespace

    int RunControl( Query::What what, const std::filesystem::path& home ) noexcept
    {
        const bool stop = what == Query::What::Stop;
        Hello      hello;
        hello.stage   = Stage::Control;
        hello.process = SelfProcess();
        auto opened   = DaemonLink::Open( home, std::move( hello ),
                                        stop ? DaemonLink::Start::Never : DaemonLink::Start::IfMissing );
        if ( !opened )
        {
            if ( stop && opened.error().message.find( "no daemon" ) != std::string::npos )
            {
                std::printf( "no daemon runs for %s\n", PathText( home ).c_str() );
                return 0;
            }
            return Report( opened.error().message );
        }
        DaemonLink& link = opened.value();

        if ( !link.Send( Message{ Query{ what } } ) )
            return stop ? 0 : Report( "the daemon of " + PathText( home ) + " closed before the query" );
        for ( ;; )
        {
            auto received = link.Receive( kAnswerWaitMs );
            if ( !received )
            {
                // A stopping daemon may close without its Text: it is gone, which is what stop asked for.
                if ( stop && !link.Connected() )
                    return 0;
                return Report( received.error().message );
            }
            // Stop waits for the last lease, however long it runs.
            if ( !received.value() )
            {
                if ( stop )
                    continue;
                return Report( "the daemon of " + PathText( home ) + " did not answer within " +
                               std::to_string( kAnswerWaitMs / 1000 ) + " s" );
            }
            const Message& message = *received.value();
            if ( const auto* text = std::get_if<Text>( &message ) )
            {
                std::fputs( text->text.c_str(), stdout );
                if ( text->text.empty() || text->text.back() != '\n' )
                    std::fputc( '\n', stdout );
                std::fflush( stdout );
                return 0;
            }
            if ( const auto* refused = std::get_if<Refused>( &message ) )
                return Report( "the daemon refused the query: " + refused->reason );
        }
    }
} // namespace buildpool::client
