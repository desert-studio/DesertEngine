#include "ShaderGraphDocumentOpen.hpp"

// THE ONE TRANSLATION UNIT THAT MAY SEE Desert::Editor::Core, and the reason this file exists at all — see
// the note at the top of the header, and the identical one in CloudDocumentOpen.cpp.
#include <Editor/Core/SubjectOpenRequest.hpp>

namespace Desert::Editor
{
    void QueueShaderGraphSubjectOpen( const Assets::AssetHandle& subject )
    {
        Core::SubjectOpenRequests::Request(
             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::ShaderGraph ) ) );
    }
} // namespace Desert::Editor
