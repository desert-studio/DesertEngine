#pragma once

#include <Engine/Assets/GrassCoverLook.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>

namespace Desert::Editor
{
    // The editor's grass cover look builder (registered with Assets::SetGrassCoverLookBuilder): the albedo
    // and mask maps' platform data decoded at the largest level within kGrassCoverLookEdge, averaged by
    // Assets::AverageGrassCoverLook. The runtime calls it on a DDC miss. It lives beside the texture
    // importer, not in it: the importer cooks any texture and knows nothing of grass, so the shell
    // TextureCook (which compiles the importer) does not have to compile the material chain the cover look
    // resolves.
    Common::ResultStr<std::string> BuildGrassCoverLook( const Assets::GrassCoverLookInput& input );
} // namespace Desert::Editor
