#pragma once

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <sstream>
#include <string>

// The constructor half of a text asset's identity: the GUID its header states, read WITHOUT loading it.
namespace Desert::Assets
{
    // The GUID the text header of `filepath` states, or a null GUID when there is none to read: the file is
    // absent (Save is about to create it), has no header (an old version the load refuses by name), or its
    // header does not parse (the load refuses that too). Only the header object is read, through the VFS
    // first like every load, so a packaged build adopts the same identity a loose file does.
    //
    // WHY IN THE CONSTRUCTOR. The asset manager keys its handle lookup at creation (the mesh's reason,
    // d75c29df), so an identity adopted at load would be a second handle for one asset. One function for
    // every text kind, because each kind that grew its own copy of this read is one more place for the
    // VFS-first order to be forgotten.
    [[nodiscard]] inline Common::Content::AssetGuid ReadTextHeaderGuid( const Common::Filepath& filepath )
    {
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( filepath ) ? Common::Utils::VFS::ReadFile( filepath ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else if ( auto read = Common::Utils::FileSystem::ReadFileContentIfExists( filepath );
                  read && read.GetValue().has_value() )
            text = *read.GetValue();
        std::istringstream in( text );
        const auto         object = Common::Content::ReadTextHeaderObject( in );
        if ( !object )
            return {};
        const auto header = Common::Content::ParseTextHeaderObject( object.GetValue() );
        if ( !header )
            return {};
        const auto guid = Common::Content::AssetGuidFromText( header.GetValue().Guid );
        if ( !guid )
            return {};
        return guid.GetValue();
    }
} // namespace Desert::Assets
