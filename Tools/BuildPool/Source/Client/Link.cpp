// BuildPool client: a link or archive step (`buildpool link`). It has no cache in front of it and no session
// of its own beyond this one step: one lease, the tool, Released.
#include "Client.hpp"

namespace buildpool::client
{
    int RunLink( const Options& options ) noexcept
    {
        auto parsed = ParseInvocation( options.command );
        if ( !parsed )
            return Report( parsed.error().message );
        const Invocation& invocation = parsed.value();

        auto opened =
             DaemonLink::Open( options.home, FrontHello( options.roots.build ), DaemonLink::Start::IfMissing );
        if ( !opened )
            return Report( opened.error().message );

        Acquire acquire;
        acquire.kind        = invocation.action;
        acquire.costKey     = CostKey( invocation, options.roots );
        acquire.restartable = invocation.restartable;
        ChildSpec spec;
        spec.argv = options.command;
        return RunLeased( opened.value(), acquire, spec, nullptr );
    }
} // namespace buildpool::client
