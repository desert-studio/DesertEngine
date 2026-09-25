#pragma once

#include <Common/Content/AssetEnvelope.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Common::Content
{
    // ── THE FILE A MOVE LEAVES BEHIND (AF10b) ─────────────────────────────────────────────────────
    //
    // UE's UObjectRedirector (CoreUObject/Public/UObject/ObjectRedirector.h): an empty asset at the old
    // path that names the object now living elsewhere. Here a reference already carries the asset's GUID
    // beside its path, and a GUID survives a move, so a redirector is needed only by what names content
    // by PATH ALONE (a scene opened by path, a string table named by a text key). It is therefore a
    // header and nothing else:
    //
    //   * the DAST header, kind `Redirector`, GUID = the redirector's OWN identity (so one redirector
    //     can name another, and a chain has links to follow), and exactly ONE dependency: the target;
    //   * a Meta section whose Name is the stable key the redirector was written for. A redirector that
    //     is copied or moved would otherwise answer for a path it was never left at; the registry scan
    //     refuses one whose key and location disagree (ContentScan's RegistryRowFor).
    //
    // No Payload: there is no body, and a file that carries one is refused rather than ignored.
    struct AssetRedirector
    {
        AssetGuid   Self;   // the redirector's own identity
        AssetGuid   Target; // the asset (or the next redirector) the old path now means
        std::string OldKey; // AssetHandle::StableKeyForPath of the path it sits at

        bool operator==( const AssetRedirector& ) const = default;
    };

    [[nodiscard]] ResultStr<std::vector<std::byte>> EncodeRedirector( const AssetRedirector& redirector );
    [[nodiscard]] BoolResultStr                     WriteRedirectorFile( const std::filesystem::path& file,
                                                                         const AssetRedirector&       redirector );

    [[nodiscard]] ResultStr<AssetRedirector> DecodeRedirector( std::span<const std::byte> file );
    [[nodiscard]] ResultStr<AssetRedirector> ReadRedirectorFile( const std::filesystem::path& file );
} // namespace Common::Content
