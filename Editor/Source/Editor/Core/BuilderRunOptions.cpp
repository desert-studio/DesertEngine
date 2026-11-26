#include "BuilderRunOptions.hpp"

namespace Desert::Editor
{
    auto BuilderRunOptions::Get() -> BuilderRunOptions&
    {
        static BuilderRunOptions options;
        return options;
    }
}
