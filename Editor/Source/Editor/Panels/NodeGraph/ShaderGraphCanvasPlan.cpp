#include "ShaderGraphCanvasPlan.hpp"

#include <string>

namespace Desert::Editor::Graph
{
    CanvasPlan PlanShaderGraph( const SGF::Document& doc, ElementLedger& ledger )
    {
        ledger.BeginFrame();

        CanvasPlan plan;
        plan.Nodes.reserve( doc.Nodes.size() );
        plan.Links.reserve( doc.Links.size() );

        for ( const auto& node : doc.Nodes )
        {
            const auto id = static_cast<ElementId>( node.Id );

            PlannedNode planned;
            planned.Id  = id;
            planned.Key = std::to_string( node.Id );
            planned.X   = node.X;
            planned.Y   = node.Y;
            // ORDER MATTERS: the pins must be declared too, or the ledger forgets them at EndFrame and
            // reports every pin as fresh on the next one. They carry no position, so nothing visible
            // would break — but "the ledger holds exactly what is on the canvas" is the property the
            // whole layer rests on, and a half-declared canvas is a ledger that means nothing.
            planned.PushPosition = ledger.See( id );
            plan.Nodes.push_back( std::move( planned ) );

            for ( const auto& pin : node.Inputs )
                ( void )ledger.See( static_cast<ElementId>( pin.Id ) );
            for ( const auto& pin : node.Outputs )
                ( void )ledger.See( static_cast<ElementId>( pin.Id ) );
        }

        for ( const auto& link : doc.Links )
        {
            const auto id = static_cast<ElementId>( link.Id );
            ( void )ledger.See( id );

            PlannedLink planned;
            planned.Id      = id;
            planned.Key     = std::to_string( link.Id );
            planned.FromPin = link.From;
            planned.ToPin   = link.To;
            plan.Links.push_back( std::move( planned ) );
        }

        ledger.EndFrame();
        return plan;
    }
} // namespace Desert::Editor::Graph
