#pragma once

#include <cstddef>
#include <cstdint>

namespace Common::Utils
{
    // The LZ4 BLOCK format — the archive's one compression codec.
    //
    // WHY A CODEC AT ALL, AND WHY THIS ONE, measured rather than chosen by reputation (2026-09-22,
    // this machine, Release -O2). Over the whole shipping corpus (368 434 518 bytes, 1513 files) the
    // ratios are lz4 1.49x and zstd-3 1.88x, but that average is a fiction: the win lives entirely in
    // two kinds — .stmesh 1.72x and .spv 1.95x, 190 MB of the 368 — while 130 MB of .png/.gif/.fbx and
    // baked volumes compress 1.00-1.03x. A whole-archive codec would therefore pay decompression on a
    // third of the bytes to save nothing on them, which is why the codec is a PER-ENTRY column and the
    // packer's rule refuses it per entry (PakFile.cpp, kCompressionThreshold).
    //
    // WHY LZ4 AND NOT ZSTD, in the one number that decides it. Time to get S bytes into memory is
    // S/D uncompressed and S/(r*D) + S/P compressed, so a codec pays exactly when the device is slower
    // than P*(r-1)/r. Measured on the largest real .stmesh (40 185 993 bytes): lz4 r=1.72 at
    // P=4289 MB/s, zstd-1 r=3.42 at P=1499 MB/s. That puts lz4's break-even device at 1797 MB/s and
    // zstd-1's at 1061 MB/s — and this machine's storage reads at 1670 MB/s (F_NOCACHE, 2 GiB). So on
    // the machine this is written on lz4 is a 3 % win and zstd-1 is a 40 % LOSS, while on the SATA-SSD
    // and spinning media a shipped game actually meets, both win and lz4 keeps winning by more the
    // faster the target gets. A decompressor that is four times the storage bandwidth is the property
    // worth having; a better ratio that is BELOW it is not.
    //
    // This is the block format only — no frame header, no frame checksum, no dictionary. The archive
    // already knows every byte the frame header would carry (the stored size, the original size and
    // which entry it belongs to), and the frame's checksum would be a second, weaker integrity check
    // beside the index's CRC32C. What is emitted is nevertheless the real LZ4 block format, and
    // Desert/Tests/Common/Lz4Block decodes a block produced by the reference implementation to prove
    // it: an independent decoder is the only witness that this is a format and not a private scheme.

    // Largest output Lz4BlockCompress can produce for `size` input bytes — worst case is a block with
    // no match at all, which is the input plus one token per 255 literals plus the final token.
    size_t Lz4BlockBound( size_t size );

    // Compresses into `dst`. Returns the number of bytes written, or 0 when the input does not fit the
    // format's limits or the output would not fit `dstCapacity`. A 0 is never an error the caller has
    // to diagnose: the packer stores the entry raw instead.
    size_t Lz4BlockCompress( const void* src, size_t srcSize, void* dst, size_t dstCapacity );

    // Decompresses exactly `dstSize` bytes, which the caller knows from the index. Returns false when
    // the block is malformed OR when it does not produce precisely that many bytes — a short or long
    // result is a damaged block, never a shorter file.
    //
    // EVERY read from `src` AND every write to `dst` is bounds-checked, including the back-reference
    // offset. The archive verifies an entry's CRC before this is called, so a block that reaches here
    // has already been proved to be the bytes that were packed; the checks are for the case where it
    // has not, because a decompressor that trusts its input is a decompressor that writes outside its
    // buffer when a check is skipped somewhere else.
    bool Lz4BlockDecompress( const void* src, size_t srcSize, void* dst, size_t dstSize );
} // namespace Common::Utils
