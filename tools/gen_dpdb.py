#!/usr/bin/env python3
"""Build the on-device datapoint database from the open3e sources.

Produces a single container file `o3edb.bin` that is placed on the device's
LittleFS partition.  Layout:

    Header (64 B)
        magic        char[8]   "O3EDB\0\0\0"
        version      u32       container format version
        db_version   char[16]  open3e datapoints "Version" string
        n_sections   u32
        (padding to 64)
    Section table    n_sections x { char name[12]; u32 off; u32 len; }
    Sections ...

Sections:
    dp_idx    u16 did, u16 dlen, u32 json_off, u32 json_len, u32 name_off
              sorted by did -> binary search
    dp_name   NUL-terminated datapoint id strings
    dp_de     NUL-terminated German labels, in the same order as dp_idx
    dp_de_off u32 per datapoint: offset into dp_de, parallel to dp_idx
    dp_json   the per-DID codec descriptions, verbatim from Open3Edatapoints.json
    var_idx   u16 did, u16 dlen, u32 json_off, u32 json_len   sorted by (did, dlen)
    var_json  as dp_json, for length-dependent variants
    enum_idx  char name[40], u32 ent_off, u32 ent_count
    enum_ent  i32 value, u32 str_off  sorted by value -> binary search
    enum_str  NUL-terminated enum texts
    em_idx    u16 can_id, u16 dlen, u32 json_off, u32 json_len  sorted by can_id
    em_json   codec descriptions for the energy meters' broadcast frames

Enums are pre-resolved into sorted binary tables rather than kept as JSON: they
sit on the decode hot path, so a lookup must not cost a parse.  The datapoint
codec descriptions stay JSON because the web UI streams them to the browser
verbatim, and they are parsed at most once per selected DID (see codec_compile).
"""
import argparse
import importlib.util
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import db_overrides  # noqa: E402
import glossary  # noqa: E402

# 2 added the energy-meter sections. The reader requires an exact match, so a
# firmware paired with an older database fails loudly instead of silently
# missing datapoints.
# 3 added the German labels.
CONTAINER_VERSION = 3
HEADER_SIZE = 64
SECTION_ENTRY = struct.Struct("<12sII")
DP_IDX = struct.Struct("<HHIII")
VAR_IDX = struct.Struct("<HHII")
EM_IDX = struct.Struct("<HHII")
ENUM_IDX = struct.Struct("<40sII")
ENUM_ENT = struct.Struct("<iI")
ENUM_NAME_MAX = 40


class Blob:
    """Append-only byte blob that de-duplicates identical entries."""

    def __init__(self):
        self.buf = bytearray()
        self._seen: dict[bytes, int] = {}

    def add(self, data: bytes) -> int:
        off = self._seen.get(data)
        if off is None:
            off = len(self.buf)
            self.buf += data
            self._seen[data] = off
        return off

    def add_str(self, s: str) -> int:
        return self.add(s.encode("utf-8") + b"\0")


def load_enums(path: Path) -> dict:
    """Import Open3Eenums.py as a module and return its E3Enums dict.

    The file is a plain literal dict with no imports of its own, so importing it
    is both safe and considerably simpler than parsing it.
    """
    spec = importlib.util.spec_from_file_location("Open3Eenums", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.E3Enums


def build_enums(enums: dict):
    idx = bytearray()
    ent = bytearray()
    strings = Blob()
    count = 0

    for name, values in sorted(enums.items()):
        if not isinstance(values, dict):
            continue  # "name" / "Version" metadata keys
        raw = name.encode("utf-8")
        if len(raw) >= ENUM_NAME_MAX:
            raise ValueError(f"enum name too long for index: {name}")
        ent_off = len(ent)
        items = sorted(values.items(), key=lambda kv: int(kv[0]))
        for value, text in items:
            ent += ENUM_ENT.pack(int(value), strings.add_str(str(text)))
        idx += ENUM_IDX.pack(raw, ent_off // ENUM_ENT.size, len(items))
        count += 1

    return idx, ent, bytes(strings.buf), count


def build_datapoints(dps: dict):
    """Index, names, German labels and the codec descriptions.

    The German labels are a separate parallel array rather than another field
    in the index entry, so the entry layout the firmware reads stays as it was.
    """
    idx = bytearray()
    names = Blob()
    german = Blob()
    de_off = bytearray()
    payload = bytearray()

    for did in sorted(int(k) for k, v in dps.items() if isinstance(v, dict)):
        rec = dps[str(did)]
        blob = json.dumps(rec, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        off = len(payload)
        payload += blob
        idx += DP_IDX.pack(did, rec["len"], off, len(blob), names.add_str(rec["id"]))
        de_off += struct.pack("<I", german.add_str(glossary.translate(rec["id"])))

    return idx, bytes(names.buf), bytes(payload), bytes(german.buf), bytes(de_off)


def load_e3oncan_e380(src: Path):
    """Read E3onCAN's E380 table and re-shape it into the same codec JSON.

    The E380 energy meter never answers a UDS request; it broadcasts eight
    bytes on a fixed CAN-ID and that is the whole protocol. E3onCAN describes
    those frames with codecs that are compatible with open3e's, bar two
    details handled in main/o3e_codec.c: its O3EFloat32 divides by a scale, and
    it adds O3EcosPhi.
    """
    if not src.is_dir():
        return {}

    sys.path.insert(0, str(src))
    try:
        import E3onCANdatapointsE380 as e380
    except Exception as exc:  # noqa: BLE001
        print(f"warning: could not load the E380 table: {exc}", file=sys.stderr)
        return {}
    finally:
        sys.path.pop(0)

    out = {}
    for can_id, codec in e380.dataIdentifiersE380.items():
        info = codec.getCodecInfo()
        # E3onCAN's Float32 does not round; open3e's defaults to two decimals,
        # and our compiler follows open3e. Pin it so the E380 keeps full
        # precision on its cumulated energy counters.
        def pin_decimals(node):
            if isinstance(node, dict):
                if node.get("codec") in ("O3EFloat32", "O3EcosPhi"):
                    node.setdefault("args", {})["decimals"] = 0
                for v in node.values():
                    pin_decimals(v)
            elif isinstance(node, list):
                for v in node:
                    pin_decimals(v)
        pin_decimals(info)
        info.setdefault("args", {})["acc"] = "ro"   # passive: never writable
        out[int(can_id)] = info
    return out


def build_energy_meters(table: dict):
    idx = bytearray()
    payload = bytearray()
    for can_id in sorted(table):
        rec = table[can_id]
        blob = json.dumps(rec, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        off = len(payload)
        payload += blob
        idx += EM_IDX.pack(can_id, rec["len"], off, len(blob))
    return bytes(idx), bytes(payload)


def build_variants(vars_: dict):
    idx = bytearray()
    payload = bytearray()

    keys = []
    for did, by_len in vars_.items():
        if not isinstance(by_len, dict):
            continue
        for dlen in by_len:
            keys.append((int(did), int(dlen)))
    keys.sort()

    for did, dlen in keys:
        rec = vars_[str(did)][str(dlen)]
        blob = json.dumps(rec, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        off = len(payload)
        payload += blob
        idx += VAR_IDX.pack(did, dlen, off, len(blob))

    return idx, bytes(payload)


def pack(db_version: str, sections: list[tuple[str, bytes]]) -> bytes:
    table_size = SECTION_ENTRY.size * len(sections)
    body_off = HEADER_SIZE + table_size

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<8sI16sI", header, 0, b"O3EDB", CONTAINER_VERSION,
                     db_version.encode("utf-8"), len(sections))

    table = bytearray()
    body = bytearray()
    for name, data in sections:
        table += SECTION_ENTRY.pack(name.encode("utf-8"), body_off + len(body), len(data))
        body += data
        # keep every section 4-byte aligned so the device can cast index arrays
        while len(body) % 4:
            body += b"\0"

    return bytes(header) + bytes(table) + bytes(body)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    ap.add_argument("--open3e", type=Path, default=root / ".cache" / "open3e")
    ap.add_argument("--e3oncan", type=Path, default=root / ".cache" / "e3oncan")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    src = args.open3e / "src" / "open3e"
    if not src.is_dir():
        print(f"open3e sources not found at {src}; run tools/fetch_open3e.py first",
              file=sys.stderr)
        return 1

    dps = json.loads((src / "Open3Edatapoints.json").read_text(encoding="utf-8"))
    fixed = db_overrides.apply_to_json(dps)
    print(f"  overrides      {fixed['units']} units, "
          f"{fixed['unit_fixes']} corrected units, "
          f"{fixed['access']} made writable, "
          f"{fixed['sensor_error']} sensor error enums")
    vars_ = json.loads((src / "Open3EdatapointsVariants.json").read_text(encoding="utf-8"))
    enums = load_enums(src / "Open3Eenums.py")

    db_version = dps.get("Version", "unknown")

    e380 = load_e3oncan_e380(args.e3oncan)
    em_idx, em_json = build_energy_meters(e380)

    dp_idx, dp_name, dp_json, dp_de, dp_de_off = build_datapoints(dps)
    var_idx, var_json = build_variants(vars_)
    enum_idx, enum_ent, enum_str, n_enums = build_enums(enums)

    blob = pack(db_version, [
        ("dp_idx", bytes(dp_idx)),
        ("dp_name", dp_name),
        ("dp_de", dp_de),
        ("dp_de_off", dp_de_off),
        ("dp_json", dp_json),
        ("var_idx", bytes(var_idx)),
        ("var_json", var_json),
        ("enum_idx", bytes(enum_idx)),
        ("enum_ent", bytes(enum_ent)),
        ("enum_str", enum_str),
        ("em_idx", em_idx),
        ("em_json", em_json),
    ])

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)

    print(f"open3e datapoints version {db_version}")
    n_de = sum(1 for did in sorted(int(k) for k, v in dps.items() if isinstance(v, dict))
               if glossary.translate(dps[str(did)]["id"]))
    print(f"  {len(dp_idx) // DP_IDX.size:5d} datapoints   ({len(dp_json) / 1024:.0f} KiB json)")
    print(f"  {n_de:5d} German labels ({len(dp_de) / 1024:.0f} KiB)")
    print(f"  {len(var_idx) // VAR_IDX.size:5d} variants     ({len(var_json) / 1024:.0f} KiB json)")
    print(f"  {n_enums:5d} enum lists   ({len(enum_ent) // ENUM_ENT.size} entries, "
          f"{len(enum_str) / 1024:.0f} KiB strings)")
    print(f"  {len(em_idx) // EM_IDX.size:5d} E380 frames  (E3onCAN)")
    print(f"  -> {args.output}  ({len(blob) / 1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
