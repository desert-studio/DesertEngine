"""Shared by the content scripts here: the text form the engine writes, the asset envelope of a texture (.detex), guids."""
import hashlib, json, os, re, struct

CONTENT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Projects", "Desert",
                                        "Content"))


def content(path):
    return os.path.join(CONTENT, path)


def guid_of(path):
    """A stable guid for an asset this repository's scripts author, from its content path."""
    return hashlib.md5(("desert:" + path).encode()).hexdigest()


def dump_text(path, value):
    """JSON, indent 4, an array of plain values on one line - as the engine writes it."""
    text = json.dumps(value, indent=4, ensure_ascii=False)
    text = re.sub(r"\[\s+([^\[\]{}]*?)\s+\]", lambda m: "[" + re.sub(r"\s*\n\s*", " ", m.group(1)) + "]", text)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text + "\n")


def load_text(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _crc_table(poly):
    table = []
    for i in range(256):
        for _ in range(8):
            i = (i >> 1) ^ poly if i & 1 else i >> 1
        table.append(i)
    return table


_CRC32C = _crc_table(0x82F63B78)


def crc32c(data):
    c = 0xFFFFFFFF
    t = _CRC32C
    for x in data:
        c = t[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for x in data:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def envelope_guid(path):
    """The guid an asset envelope (DAST) carries."""
    with open(path, "rb") as f:
        head = f.read(32)
    if head[:4] != b"DAST":
        raise SystemExit("%s is not an asset envelope" % path)
    return "%016x%016x" % struct.unpack("<QQ", head[16:32])


def write_texture(dest, source_bytes, source_ext, like):
    """Writes the texture asset @p dest (content path, .detex) over @p source_bytes with the import settings of the
    existing texture asset @p like. Returns {Guid, Path}."""
    with open(content(like), "rb") as f:
        model = f.read()
    header_size, = struct.unpack("<I", model[8:12])
    header = bytearray(model[:header_size])
    toc = header.index(b"META")
    off, size = struct.unpack("<QQ", header[toc + 8:toc + 24])
    old_meta = model[off:off + size]
    off, size = struct.unpack("<QQ", header[toc + 40:toc + 56])
    old_import = model[off:off + size]

    name = os.path.basename(dest)[:-len(".detex")].encode()
    old_name, = struct.unpack("<H", old_meta[:2])
    meta = struct.pack("<H", len(name)) + name + old_meta[2 + old_name:]
    source_path = ("assets:" + dest[:-len(".detex")] + source_ext).encode()
    version, old_path = struct.unpack("<II", old_import[:8])
    imported = struct.pack("<II", version, len(source_path)) + source_path + struct.pack("<Q", fnv1a64(source_bytes)) + \
        old_import[8 + old_path + 8:]

    guid = guid_of(dest)
    header[16:32] = struct.pack("<QQ", int(guid[:16], 16), int(guid[16:], 16))
    at = header_size
    for i, section in enumerate((meta, imported, source_bytes)):
        row = toc + 32 * i
        header[row + 8:row + 32] = struct.pack("<QQQ", at, len(section), fnv1a64(section))
        at += len(section)
    header[12:16] = b"\0\0\0\0"
    header[12:16] = struct.pack("<I", crc32c(header))
    os.makedirs(os.path.dirname(content(dest)), exist_ok=True)
    with open(content(dest), "wb") as f:
        f.write(bytes(header) + meta + imported + source_bytes)
    return {"Guid": guid, "Path": "assets:" + dest}
