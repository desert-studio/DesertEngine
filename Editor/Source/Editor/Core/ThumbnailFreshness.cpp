#include "ThumbnailFreshness.hpp"

namespace Desert::Editor::ThumbnailFreshness::Detail
{
    MemoTable& Memos()
    {
        static MemoTable table;
        return table;
    }
}
