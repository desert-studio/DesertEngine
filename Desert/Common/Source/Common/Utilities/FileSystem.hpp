#pragma once
#include <optional>

#include <iostream>

#if defined( DESERT_PLATFORM_WINDOWS )
#include <windows.h>

// NOTE: This is a workaround for Microsoft macros so that
// we can use names like CreateDirectory, etc
#ifdef CreateDirectory
#undef CreateDirectory
#undef DeleteFile
#undef MoveFile
#undef CopyFile
#undef CreateFile
#undef SetEnvironmentVariable
#undef GetEnvironmentVariable
#endif
#endif // DESERT_PLATFORM_WINDOWS

#include <cstdint>
#include <functional>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <Common/Core/ResultStr.hpp>

namespace Common::Utils
{
    class FileSystem
    {
    public:
        [[nodiscard]] static const std::string GetFileName( const std::filesystem::path& filepath );
        [[nodiscard]] static const std::string GetFileName( const std::string& filepath );
        [[nodiscard]] static const std::string
        GetFileNameWithoutExtension( const std::filesystem::path& filepath );
        [[nodiscard]] static const std::filesystem::path
        GetFileNameWithoutExtension_PATH( const std::filesystem::path& filepath );

    public:
        // THE READ PRIMITIVES ARE SOFT ON PURPOSE, AND THE SOFTNESS IS GUARDED BY THE TYPE. A path
        // that resolves neither on disk nor in a mounted .dpak logs the path (LOG_ERROR) and returns
        // a NAMED error carrying that path — it never terminates the process. A primitive cannot
        // know whether the missing file is fatal to its caller, so the policy lives at the call
        // site: every loader answers a failed read through its own error channel (Common::MakeError
        // / LOG_ERROR / a defaults branch), and in a packaged game an abort down here is a
        // guaranteed crash on the player's machine over a single missing asset.
        //
        // WHAT THE RESULT RETURN ACTUALLY BUYS, stated exactly, because an earlier version of this
        // comment promised more than the type delivers. What it buys is that the OLD shape does not
        // compile: `std::string s = ReadFileContent(p)` is rejected outright, so every one of the
        // ~30 call sites in the engine was rewritten by the COMPILER rather than by eye, and a clean
        // full build is the proof that the migration is complete.
        //
        // What it does NOT buy is a guarantee that the caller decided anything. `GetValue()` and
        // `ExtractValue()` hand back a default-constructed T when the result is an error, so an
        // unchecked unwrap still compiles and still yields the silent emptiness §1.4 forbids — one
        // method call away, with no diagnostic. Do not read "returns a Result" as "the compiler has
        // checked this for you"; the check is still yours to write. `[[nodiscard]]` below catches
        // only a WHOLLY discarded call — but it does now catch that one: this comment used to end
        // "and even that is silent in this workspace, which builds every target with -w", which
        // stopped being true on 2026-09-06 when Workspace.lua replaced `warnings "Off"` with
        // `warnings "Extra"`. Verified 2026-09-07 by discarding a NO_DISCARD result on purpose and
        // watching -Wunused-result fire, so the attribute is a real gate now and worth reaching for.
        //
        // It does end the old ambiguity this comment used to have to explain away — a genuinely
        // zero-byte file is a SUCCESS holding an empty value, a missing file is an error, and the
        // two are different values instead of one emptiness that only an up-front Exists() could
        // tell apart.
        [[nodiscard]] static Common::ResultStr<std::string>
        ReadFileContent( const std::filesystem::path& filepath );

        // FOR CALLERS WHOSE NORMAL PATH INCLUDES "NOT THERE YET": a cache probe, a freshness check
        // against a cooked file that may not exist. ReadFileContent logs every miss as an ERROR, which is
        // right when the file was promised and wrong here: a first run of the environment cache printed
        // three "[error] Could not read file" lines for a miss the caller handles and explains itself,
        // and real errors drowned in them (owner, 2026-09-23). Absent on disk AND in every mounted pak ->
        // success holding nullopt, nothing logged. Present but unreadable is still an error, logged by
        // the read it delegates to. Use this ONLY where absence is expected; a promised file keeps the
        // loud read.
        [[nodiscard]] static Common::ResultStr<std::optional<std::string>>
        ReadFileContentIfExists( const std::filesystem::path& filepath );

        // THE FIRST @p maxBytes BYTES, and never more. Added when `.tex` grew a pixel payload (B17):
        // `TextureAsset::LoadFromFile` needs a texture's header and mip table — 64 bytes plus a small
        // row per level — and `TextureService` promises in its own comment that that load is "cheap:
        // reads the metadata, not pixels". Whole-file reads made that comment false the day a `.tex`
        // went from 133 bytes to megabytes, and a comment that promises a guarantee the tree does not
        // honour is the defect shape this codebase has now closed a dozen instances of.
        //
        // A SHORT ANSWER IS A SUCCESS, NOT A FAILURE: a file smaller than @p maxBytes yields all of
        // it. The caller is asking for AT MOST that many bytes, so "how many did I get" is its own
        // question and the string's size answers it. A missing file is still an error, exactly as
        // above.
        //
        // AN ARCHIVED FILE IS READ WHOLE AND THEN TRIMMED, deliberately and visibly: a `.dpak` entry
        // is a compressed run with no random access inside it, so there is nothing to seek to. The
        // saving here is the DISK path, which is the one every editor session and every cook takes.
        // Ranged reads inside an archive are a property the archive would have to grow, and that
        // belongs with mip streaming (`Docs/World/PROGRAMME.md` §5, the resident-tail step), not here.
        [[nodiscard]] static Common::ResultStr<std::string>
        ReadFileContentPrefix( const std::filesystem::path& filepath, std::size_t maxBytes );

        // The prefix read with the same "absence is an answer" contract as ReadFileContentIfExists.
        [[nodiscard]] static Common::ResultStr<std::optional<std::string>>
        ReadFileContentPrefixIfExists( const std::filesystem::path& filepath, std::size_t maxBytes );

        // THE WRITE PRIMITIVE. There is exactly one body, and this is it; the `Content` spelling below
        // is the same call with the bytes taken from a string, and exists only so ~30 text call sites
        // do not each have to spell `std::as_bytes`.
        //
        // WRITE-THEN-RENAME, so a file's PREVIOUS contents survive a failed write. It writes
        // `<filepath>.tmp` BESIDE the destination (same directory — rename is only atomic within one
        // filesystem, and the system temp dir can be another volume), verifies the stream after the
        // write AND after close (close() is where a buffered failure finally surfaces), and only then
        // renames over the original. Interruption at any step leaves the original untouched; the worst
        // a failure costs is a stray .tmp, which is removed on the way out. The temp name is
        // deliberately FIXED rather than unique-per-process: two concurrent writers then race to a
        // whole file from one of them instead of interleaving into a torn one, and a test can block
        // the temp path to drive the failure branch.
        //
        // WHY THERE IS NO PLAIN `WriteContentToFile` ANY MORE. There used to be one three lines above
        // this, returning `const void`. It detected a failed open, logged it and returned nothing, and
        // it checked neither `<<` nor `close()` at all, so a full disk never reached even the log. It
        // opened the destination with trunc, so the old file was already gone before the first byte
        // landed — Tools/SceneMigrator destroyed scenes exactly that way. Twenty-eight call sites used
        // it, among them the whole Ctrl+S chain, which then cleared the "unsaved changes" mark and
        // showed a green "Saved 'X'" toast for a scene that had not been written. Two write primitives
        // meant every new call site was a coin toss between the safe one and the silent one, so the
        // silent one is gone rather than deprecated, and its NAME is gone with it: changing only the
        // return type would have left every old call site compiling, because at the time this
        // workspace built with -w and even [[nodiscard]] was mute. (That has since changed —
        // `warnings "Extra"` landed 2026-09-06 — so the same migration today would also have had the
        // attribute behind it. Removing the name is still what made the COMPILER, rather than the
        // eye, find all twenty-eight, and a warning would not have been an error.)
        //
        // On failure the result names which step failed and where, and the file on disk is unchanged —
        // the caller owns the policy (a tool counts the file as failed and exits non-zero; the editor
        // leaves its unsaved-changes mark standing and says why).
        //
        // WHY A BYTE SPAN AND NOT ONLY A STRING (Д35). The four cloud formats, the two bakers and
        // PakTool all hold their payload as `std::vector<unsigned char>` — a `.dcnv` is 64 MB of RGBA8
        // voxels — and the only way to reach a string-only primitive was to copy the whole buffer into
        // a `std::string` first. That copy is what kept five of the fifteen Д31-D sites on their own
        // hand-rolled `std::ofstream`, so the primitive grew the shape they already had instead of
        // asking them to pay a copy to use it.
        [[nodiscard]] static Common::BoolResultStr WriteBytesToFileAtomic( const std::filesystem::path& filepath,
                                                                           std::span<const std::byte>   content );

        [[nodiscard]] static Common::BoolResultStr WriteContentToFileAtomic( const std::filesystem::path& filepath,
                                                                             const std::string& content );

        [[nodiscard]] static Common::ResultStr<std::vector<uint8_t>>
        ReadByteFileContent( const std::filesystem::path& filepath );

        // Every regular file under `root`, from BOTH halves of the content world: the loose files on
        // disk and everything a mounted .dpak holds under that root, deduplicated by absolute
        // normalized path (a loose file overrides its pak twin). A missing root contributes nothing.
        // Every scanner that enumerates content must go through this: the font and icon services each
        // used to walk only the disk half, so a packaged game — where the loose directories do not
        // exist at all — scanned nothing and no text could resolve its font.
        [[nodiscard]] static std::vector<std::filesystem::path>
        ListFilesRecursive( const std::filesystem::path& root );

    public:
        [[nodiscard]] static const std::filesystem::path GetParentPath( const std::filesystem::path& filepath );
        [[nodiscard]] static const std::string           GetFileExtension( const std::filesystem::path& filepath );
        [[nodiscard]] static uint32_t                    GetFileSize( const std::filesystem::path& filepath );
        static bool                                      CreateDirectory( const std::filesystem::path& directory );
        static bool                                      CreateDirectory( const std::string& directory );
        static void                                      CreateFile( const std::string& path );
        static void                                      CreateFile( const std::filesystem::path& path );
        static bool                                      Exists( const std::filesystem::path& filepath );
        static bool                                      Exists( const std::string& filepath );
        static std::string           GetFileDirectoryString( const std::filesystem::path& filepath );
        static std::filesystem::path GetFileDirectory( const std::filesystem::path& filepath );

        // Absolute path of the running executable — for locating content (a .dpak) packaged next to it.
        [[nodiscard]] static std::filesystem::path ExecutablePath();

    public:
        static std::filesystem::path OpenFileDialog( const char* filter = "All\0*.*\0" );
        static std::filesystem::path OpenFolderDialog( const char* initialFolder = "" );
        static std::filesystem::path SaveFileDialog( const char* filter = "All\0*.*\0" );

    public:
        static bool        HasEnvironmentVariable( const std::string& key );
        static bool        SetEnvironmentVariable( const std::string& key, const std::string& value );
        static std::string GetEnvironmentVariable( const std::string& key );
    };
} // namespace Common::Utils