#include "GizmoIconSet.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Icon/IconService.hpp>

namespace Desert::Editor
{
    namespace
    {
        // Per-role handle cache. 0 = not resolved yet; the service's own map negative-caches a failed
        // import, so a missing file costs one log line and one lookup per frame, not one bake per frame.
        std::array<uint64_t, kGizmoIconCount> g_Handles{};
    } // namespace

    const Runtime::Icon* ResolveGizmoIcon( GizmoIcon role )
    {
        auto* service = Runtime::ResourceRegistry::GetIconService();
        if ( !service )
            return nullptr; // no graphics yet (headless tools, early boot) — the caller draws without art

        const size_t index = static_cast<size_t>( role );
        if ( g_Handles[index] == 0 )
        {
            // Path-derived and idempotent, so this is a lookup and not an import. The import happens in
            // Get() below, once, and a file that is missing or unparseable is negative-cached THERE with
            // the path in the log line — which is why nothing is reported here.
            g_Handles[index] = service->RegisterIcon( GizmoIconPath( role ).generic_string() );
        }

        const Runtime::Icon* icon = service->Get( g_Handles[index] );
        return ( icon && icon->Valid() ) ? icon : nullptr;
    }
} // namespace Desert::Editor
