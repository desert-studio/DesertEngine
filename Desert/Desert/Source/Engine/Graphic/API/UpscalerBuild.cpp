#include <Engine/Graphic/API/UpscalerBuild.hpp>

namespace Desert::Graphic
{
#if defined( DESERT_UPSCALER_FSR )
    const bool kFsrBuilt = true;
#else
    const bool kFsrBuilt = false;
#endif
#if defined( DESERT_UPSCALER_DLSS )
    const bool kDlssBuilt = true;
#else
    const bool kDlssBuilt = false;
#endif
} // namespace Desert::Graphic
