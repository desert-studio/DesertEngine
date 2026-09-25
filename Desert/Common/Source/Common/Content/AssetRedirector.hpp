#pragma once

#include <Common/Content/AssetEnvelope.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
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

    // A LOADER THAT OPENS BY FILE, not through the registry (the editor opening a scene, a string table
    // loaded from its path), finds a redirector's bytes where the moved asset's text used to be. Its text
    // parser would refuse them as unreadable, which is true and useless; this refuses them by name and
    // says where the asset went. Success for anything that is not a redirector: its own parser judges it.
    // @p nameTarget returns the target's current key, or "" when it does not know the GUID; without one, or
    // on "", the message names the GUID. A callback, not the registry, so a loader that links no registry
    // (the scene version gate's suites) can still refuse by name.
    using RedirectorTargetNamer = std::function<std::string( const AssetGuid& )>;
    [[nodiscard]] BoolResultStr RefuseRedirectorBytes( std::string_view source, std::string_view bytes,
                                                       const RedirectorTargetNamer& nameTarget = {} );
} // namespace Common::Content
