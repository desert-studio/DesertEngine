#pragma once

#include <map>
#include <optional>
#include <string>

namespace Desert::Migration
{
    // What following one material's shader locator came to: a refusal (Error), the rewritten text (Text), or
    // neither (the locator already names where its GUID lives, or the material states no engine shader).
    struct ShaderLocatorFollow
    {
        std::string                Error;
        std::optional<std::string> Text;
    };

    // THE LOCATOR FOLLOWS THE GUID (NO-PBR; UE's redirector fix-up, in the file). A `.demat` names its shader by
    // the header GUID and its `Path` is only a locator, so when an engine shader moves the GUID still resolves
    // and the locator goes stale. Content-detected (MATL has no generation for a locator): the `Shader` block's
    // `Guid` decides, and an `engine:` locator whose GUID no engine shader states is REFUSED, never guessed from
    // the path. @p engineShaders maps each engine shader's GUID to its `engine:` key (EngineShaderLocatorsByGuid).
    ShaderLocatorFollow FollowShaderLocator( const std::string&                        source,
                                             const std::map<std::string, std::string>& engineShaders );
} // namespace Desert::Migration
