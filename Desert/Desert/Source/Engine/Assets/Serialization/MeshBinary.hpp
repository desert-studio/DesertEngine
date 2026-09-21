#pragma once

// THE COOKED MESH CONTAINER — why it is bytes and not JSON, and what it has to be able to refuse.
//
// WHAT WAS MEASURED, IN THIS TREE, BEFORE A LINE OF THIS WAS WRITTEN. `Cooked/Meshes/base.stmesh` is
// 40 185 992 bytes of JSON for 105 317 vertices and 120 000 triangles — one submesh, no blendshapes,
// no baked LODs. Every float in it is spelled out in decimal ("-0.5762509107589722" is 19 characters
// for the four bytes it came from) and every vertex repeats the five field names. The same payload as
// fixed-width little-endian records is 7 337 940 bytes. The interesting half is not the disk: it is
// that `rfl::json::read` has to walk all 40 MB, and `MeshService::Get` runs it on the frame that first
// touches the mesh. `Docs/World/PROGRAMME.md` §5 puts this ahead of the texture work for that reason.
//
// ── THE FORMAT ───────────────────────────────────────────────────────────────────────────────────
//
//   [Header, 64 B][SectionTable, SectionCount * 24 B][section payloads, each 8-byte aligned]
//
// Every payload is an ARRAY of fixed-width records, so decoding a section is one `memcpy` into an
// already-sized vector rather than a per-element parse. The jagged parts of `MeshAssetData` — submesh
// names, the LOD chains, the blendshape deltas — are flattened into their own sections and referenced
// by (first, count) pairs from the fixed records, which is what keeps every record fixed-width.
//
// ── WINDOWS IS THE TARGET AND THIS MACHINE IS NOT WINDOWS ────────────────────────────────────────
//
// A binary format is byte order, alignment and type width, and all three are places where a file
// written by clang on arm64 can be read as garbage by MSVC on x64 without anything failing. So:
//
//  * TYPE WIDTH IS IN THE FILE. Each section table row carries the `ElementSize` the WRITER used. The
//    reader compares it against its own `sizeof` and refuses by name on a mismatch. A record that
//    grows a field, or a compiler that pads one differently, stops the load with a sentence naming
//    the section and both numbers instead of producing a mesh made of shifted floats.
//  * BYTE ORDER IS IN THE FILE. `ByteOrder` is written as the integer 0x04030201, which reads back as
//    0x01020304 on a big-endian host. A mismatch is refused. It is NOT byte-swapped: both shipping
//    targets are little-endian, and a swapper is a second code path that neither the developer's
//    machine nor CI could ever execute — the same argument `GAP_ANALYSIS.md` §4 makes for sparse
//    residency, and the same conclusion.
//  * NO GLM AND NO `std::array` CROSS THE BOUNDARY. The wire records below are plain `float`/`uint32_t`
//    arrays with a `static_assert` on every size, because `glm`'s alignment is a compile-time option
//    (`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`) and a header that defines it in one translation unit and
//    not another would silently change the file layout.
//
// ── THE FORMAT MUST BE ABLE TO EXPRESS ITS OWN FAILURE ───────────────────────────────────────────
//
// A truncated file must be distinguishable from an empty one, and this is the whole reason `FileSize`
// is in the header. A mesh with no vertices is a legal container — 64 bytes of header, a table, and
// nothing after it — and it decodes successfully into an empty `MeshAssetData`, which the asset layer
// then refuses for carrying zero submeshes, by name, exactly as it refuses an empty JSON one. A file
// whose bytes stop early does NOT decode: the declared size does not match the bytes in hand and the
// refusal says both numbers. Without that field the two cases are the same file with different
// counts in the table, and the truncated one would come back as a valid mesh with fewer vertices.
//
// THERE IS NO PAYLOAD CHECKSUM, DELIBERATELY. Hashing 7 MB on every load is a second full pass over
// every byte read — which is the exact defect `PROGRAMME.md` §6 books against `.dpak` ("each read
// re-hashes the entry"). Truncation is the failure this format actually sees (an interrupted cook, a
// partial copy) and the declared size catches it for free; silent bit rot inside an intact file is
// not, and paying a full pass per load to catch it would give back a good part of what the format
// was built to win.
//
// ── OLD FILES ────────────────────────────────────────────────────────────────────────────────────
//
// `ReadMeshAssetData` is the migration, and it is a pure function of bytes. A payload that does not
// start with the magic is read as the retired JSON form — that is version 0 of this format and the
// only reason the JSON reader still exists. Nothing writes JSON meshes any more.

#include "Mesh.hpp"

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Desert::Assets::Serialization
{
    /// The container's own version sequence. It is NOT `kSceneVersion` (21) and not any of the asset
    /// versions beside it (`kCloudTypeFormatVersion`, `kUIThemeFormatVersion`, the registry's and the
    /// manifest's): those count changes to a `.desce`, a `.dcloud`, a `.duitheme`, a `.dreg` and a
    /// `.dman` respectively, and none of them is bumped by anything that happens to a cooked mesh. A
    /// `.desce` names a mesh by PATH and carries none of its bytes, so a scene written yesterday opens
    /// a mesh cooked today without either file knowing about the other's number.
    ///
    /// Version 0 is not a value that appears in any file: it is the name this code gives to the JSON
    /// form that predates the container, which is recognised by the ABSENCE of the magic.
    inline constexpr uint32_t kMeshBinaryVersion = 1;

    /// "DESTMESH". Eight ASCII bytes, so the sequence on disk is the same whatever the host's word
    /// order — a magic written as an integer would itself need a byte-order rule to be read.
    inline constexpr char kMeshBinaryMagic[8] = { 'D', 'E', 'S', 'T', 'M', 'E', 'S', 'H' };

    /// Does @p bytes begin with the container magic? The one question that routes a file to the binary
    /// reader or to the JSON migration, and the only thing either of them agrees to answer about a
    /// payload it has not parsed.
    [[nodiscard]] bool LooksLikeMeshBinary( std::string_view bytes );

    /// `MeshAssetData` -> container bytes. Total, and total on purpose: every field of the struct is
    /// written, so a round trip is an identity and the suite can assert it as one.
    [[nodiscard]] std::string EncodeMeshBinary( const MeshAssetData& data );

    /// Container bytes -> `MeshAssetData`, or a refusal that names the file, the section and the two
    /// numbers that disagreed. @p whatFor is the path (or any label) the refusal quotes; it is not
    /// read from the payload, because a corrupt payload cannot be trusted to name itself.
    [[nodiscard]] Common::ResultStr<MeshAssetData> DecodeMeshBinary( std::string_view  bytes,
                                                                     std::string_view  whatFor );

    /// THE ONE ENTRY POINT BOTH MESH ASSET CLASSES READ THROUGH, and the migration itself: binary when
    /// the magic is there, the retired JSON form when it is not. Pure — bytes in, data out — so the
    /// suite can drive both arms of it without a file, a filesystem or an asset manager.
    [[nodiscard]] Common::ResultStr<MeshAssetData> ReadMeshAssetData( std::string_view bytes,
                                                                      std::string_view whatFor );
} // namespace Desert::Assets::Serialization
