#pragma once

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <array>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

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
    [[nodiscard]] inline std::string ReadTextForIdentity( const Common::Filepath& filepath )
    {
        if ( const auto packed =
                  Common::Utils::VFS::Exists( filepath ) ? Common::Utils::VFS::ReadFile( filepath ) : std::nullopt;
             packed.has_value() )
            return packed.value();
        if ( const auto read = Common::Utils::FileSystem::ReadFileContentIfExists( filepath ); read )
            if ( const auto& content = read.GetValue(); content.has_value() )
                return content.value();
        return {};
    }

    [[nodiscard]] inline Common::Content::AssetGuid
    GuidOfHeader( const Common::ResultStr<Common::Content::TextAssetHeaderSerialized>& header )
    {
        if ( !header )
            return {};
        const auto guid = Common::Content::AssetGuidFromText( header.GetValue().Guid );
        if ( !guid )
            return {};
        return guid.GetValue();
    }

    [[nodiscard]] inline Common::Content::AssetGuid ReadTextHeaderGuid( const Common::Filepath& filepath )
    {
        std::istringstream in( ReadTextForIdentity( filepath ) );
        const auto         object = Common::Content::ReadTextHeaderObject( in );
        if ( !object )
            return {};
        return GuidOfHeader( Common::Content::ParseTextHeaderObject( object.GetValue() ) );
    }

    // The same for a .shader, whose header is its first line's comment (ShaderAssetHeader.hpp).
    [[nodiscard]] inline Common::Content::AssetGuid ReadShaderHeaderGuid( const Common::Filepath& filepath )
    {
        return GuidOfHeader( Common::Content::ReadShaderHeader( ReadTextForIdentity( filepath ) ) );
    }

    // THE HEADER A REWRITE OF `target` STATES: the GUID the file already there states, minted only for a new
    // file. A re-import replaces the bytes, not the identity every reference names (UE keeps a package's GUID
    // on reimport), so a cook that overwrites a path reads its GUID first.
    [[nodiscard]] inline Common::Content::TextAssetHeaderSerialized
    HeaderKeepingFileGuid( const Common::Filepath& target, Common::Content::ContentKind kind,
                           std::span<const Common::Content::SubsystemVersion> subsystems )
    {
        Common::Content::AssetGuid guid = ReadTextHeaderGuid( target );
        if ( guid.IsNull() )
            guid = Common::Content::AssetGuid::Generate();
        return Common::Content::MakeTextHeader( kind, guid, subsystems );
    }

    // THE .shader A REWRITE OF `target` WRITES: the header line HeaderKeepingFileGuid would state for a
    // shader, then `source`. A shader graph's Compile overwrites its .shader on every edit; minting a GUID
    // each time would give every recompile a new handle, and a material naming the old one would lose its
    // shader. So the GUID is read from the file already there, and minted only for a first compile.
    [[nodiscard]] inline std::string ShaderSourceKeepingFileGuid( const Common::Filepath& target,
                                                                  std::string_view        source )
    {
        Common::Content::AssetGuid guid = ReadShaderHeaderGuid( target );
        if ( guid.IsNull() )
            guid = Common::Content::AssetGuid::Generate();
        const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ kShaderSchemaTag, kShaderSchemaVersion } };
        return Common::Content::WriteShaderHeaderLine(
                    Common::Content::MakeTextHeader( Common::Content::ContentKind::Shader, guid, versions ) ) +
               std::string( source );
    }
} // namespace Desert::Assets
