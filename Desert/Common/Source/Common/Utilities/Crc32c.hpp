#pragma once

#include <cstddef>
#include <cstdint>

namespace Common::Utils
{
    // CRC-32C (Castagnoli, polynomial 0x1EDC6F41) — the archive's INTEGRITY primitive.
    //
    // WHY THIS EXISTS AT ALL, in numbers rather than in taste. Measured 2026-09-22 on the shipping
    // cooked tree (1069 entries, 229 187 752 bytes, Release -O2, 7 repeats): reading every entry out
    // of a .dpak cost 373.7 ms, of which the byte transfer was 67.2 ms and the per-entry FNV-1a
    // verification was 297.4 ms — EIGHTY PER CENT of the read path, and 2.2x the cost of fetching the
    // same bytes off this machine's storage cold (137 ms at the 1.67 GB/s an F_NOCACHE read measures).
    // FNV-1a cannot be made faster: each byte multiplies the previous state, so it is a serial
    // dependency chain and 0.77 GB/s is its ceiling. PakFile.hpp had already written down what to do
    // when that number showed up in a load profile — "a hardware CRC32C or xxHash ... and that means a
    // v3 magic, not a silent skip of the check" — and this is that number showing up.
    //
    // Measured on the same machine over 256 MiB, 5 repeats: FNV-1a 0.77 GB/s, this function 8.17 GB/s
    // on the hardware path and 1.48 GB/s on the portable one. So the integrity phase falls from
    // 297 ms to ~28 ms, and stops being the archive's bottleneck at all.
    //
    // WHAT CHANGED ABOUT WHAT IS CHECKED — because "cheaper" must never be allowed to mean "less".
    // A 32-bit CRC and a 64-bit FNV differ in two directions, and only one of them favours FNV:
    //   * for UNIFORMLY RANDOM corruption the false-accept rate is 2^-32 against 2^-64. Over an
    //     archive of ~1500 entries, missing a damaged one has probability 2.3e-10;
    //   * for the corruption that storage ACTUALLY produces — a burst inside one sector — a CRC is a
    //     guarantee and FNV is a coin flip: CRC-32C detects every single-bit error, every burst of up
    //     to 32 bits, and every odd number of flipped bits, none of which FNV-1a promises.
    // Neither is a defence against deliberate modification, and neither ever was; PakFile.hpp states
    // the trigger for a cryptographic hash (a content set distributed PER ENTRY) and it has not fired.
    //
    // THE PATH-DERIVED CONTENT HASH IS NOT REPLACED BY THIS. PakContentHash stays exactly as it is and
    // keeps its index column: it is the archive's IDENTITY — what ContentManifest records per file and
    // what patch generation diffs — and changing it would invalidate every manifest ever recorded for
    // a release. Identity is computed once at cook; integrity is computed at every read. They are two
    // questions with two costs, and the v3 index carries one column for each.
    //
    // The result is bit-identical on both paths and on both platforms: hardware where the CPU has the
    // instruction (ARMv8 CRC, SSE4.2), a slice-by-8 table otherwise. The two are asserted equal over
    // the same bytes by Desert/Tests/Common/Crc32c, which is what makes the x86 path — the one this
    // development machine cannot execute — checked on the machine that can.
    uint32_t Crc32c( const void* data, size_t size );

    // The same function with the portable table forced, so a test can compare the two implementations
    // on whichever host it runs on. Not a fallback the caller chooses: Crc32c picks for itself.
    uint32_t Crc32cPortable( const void* data, size_t size );

    // True when Crc32c is running on the CPU instruction rather than the table. Reported in the test's
    // output so a green run on a machine without the instruction cannot be mistaken for a green run of
    // the hardware path.
    bool Crc32cUsesHardware();
} // namespace Common::Utils
