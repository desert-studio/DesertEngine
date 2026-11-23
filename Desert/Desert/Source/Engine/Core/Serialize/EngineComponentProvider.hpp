#pragma once

namespace Desert::Core::Serialize
{
    /// Installs Engine's component provider (idempotent; also done at static initialisation). Scene calls it, so
    /// every executable with a Scene links the provider (EngineComponentProvider.cpp).
    void LinkEngineComponentProvider();
} // namespace Desert::Core::Serialize
