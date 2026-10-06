#include "CloudDocumentOpen.hpp"

// THE ONE TRANSLATION UNIT THAT MAY SEE Desert::Editor::Core, and the reason this file exists at all.
// CloudDocumentOpen.hpp is included by the four cloud panels, which spell Desert::Core::Formats as an
// unqualified `::Desert::Core::Formats`; a header that made Desert::Editor::Core visible ahead of those uses
// rebinds every one of them. Here there is no such use, so the include is safe — see the note at the top of the
// header, and the identical one in Editor/Core/SubjectEditorRegistry.hpp.
#include <Editor/Core/SubjectOpenRequest.hpp>

namespace Desert::Editor
{
    void QueueCloudSubjectOpen( const Assets::AssetHandle& subject, Assets::AssetTypeID type )
    {
        Core::SubjectOpenRequests::Request( AssetSubject( subject, static_cast<uint32_t>( type ) ) );
    }
} // namespace Desert::Editor
