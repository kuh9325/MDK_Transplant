#!/usr/bin/env python3
"""exe_headers.py — structural header dump for PE32 and LE/LX executables.

Evidence-first companion to binary_inventory.py: emits *metadata only*
(addresses, sizes, flags, imported symbol names, section/object maps).
It never disassembles and never emits file content beyond structural
fields and name tables.

Handles the Watcom-wlink PE quirk observed in this project (section
VirtualSize == 0; real extent in SizeOfRawData) by mapping RVAs through
max(VirtualSize, SizeOfRawData).

Usage:
    python3 tools/exe_headers.py <file> [<file> ...] [--format json|text]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys

# ---------------------------------------------------------------- PE32 ---

PE_DD_NAMES = [
    "export", "import", "resource", "exception", "certificate",
    "base_reloc", "debug", "architecture", "global_ptr", "tls",
    "load_config", "bound_import", "iat", "delay_import", "clr",
    "reserved",
]


def _cstr(d: bytes, off: int) -> str:
    end = d.index(b"\0", off)
    return d[off:end].decode("latin1")


def parse_pe(d: bytes) -> dict:
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    (machine, nsec, tstamp, _sym, _nsym, optsz,
     chars) = struct.unpack_from("<HHIIIHH", d, e_lfanew + 4)
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    if magic != 0x10B:
        raise ValueError(f"unsupported PE optional-header magic 0x{magic:x}")

    (entry_rva,) = struct.unpack_from("<I", d, opt + 16)
    codebase, database = struct.unpack_from("<II", d, opt + 20)
    image_base, sec_align, file_align = struct.unpack_from("<III", d, opt + 28)
    (osmaj, osmin, imgmaj, imgmin, submaj, submin,
     win32ver, size_img, size_hdr, _cksum, subsystem,
     dll_chars) = struct.unpack_from("<HHHHHHIIIIHH", d, opt + 40)
    n_rva = struct.unpack_from("<I", d, opt + 92)[0]
    dd = opt + 96
    dirs = {}
    for i in range(min(n_rva, 16)):
        rva, sz = struct.unpack_from("<II", d, dd + 8 * i)
        if rva:
            dirs[PE_DD_NAMES[i]] = {"rva": rva, "size": sz}

    secs = []
    soff = opt + optsz
    for i in range(nsec):
        s = d[soff + 40 * i:soff + 40 * i + 40]
        nm = s[:8].rstrip(b"\0").decode("latin1")
        vsz, va, rawsz, rawoff = struct.unpack_from("<IIII", s, 8)
        _r, _p, _nrel, _nln, schars = struct.unpack_from("<IIHHI", s, 16)
        secs.append({"name": nm, "va": va, "virtual_size": vsz,
                     "raw_size": rawsz, "raw_offset": rawoff,
                     "characteristics": schars})

    def r2o(rva: int):
        for s in secs:
            span = max(s["virtual_size"], s["raw_size"])
            if s["va"] <= rva < s["va"] + span:
                return s["raw_offset"] + (rva - s["va"])
        return None

    imports = []
    if "import" in dirs:
        off = r2o(dirs["import"]["rva"])
        while off is not None:
            desc = d[off:off + 20]
            if len(desc) < 20:
                break
            oft, _ts, _fwd, name_rva, ft = struct.unpack("<IIIII", desc)
            if oft == 0 and name_rva == 0 and ft == 0:
                break
            dll = _cstr(d, r2o(name_rva))
            funcs = []
            toff = r2o(oft if oft else ft)
            if toff is not None:
                while True:
                    val = struct.unpack_from("<I", d, toff)[0]
                    if val == 0:
                        break
                    if val & 0x80000000:
                        funcs.append(f"ord{val & 0xffff}")
                    else:
                        hn = r2o(val)
                        if hn is not None:
                            funcs.append(_cstr(d, hn + 2))
                    toff += 4
            imports.append({"dll": dll, "functions": funcs})
            off += 20

    exports = []
    if "export" in dirs:
        eo = r2o(dirs["export"]["rva"])
        if eo is not None:
            (_f, _t, _vmaj, _vmin, name_rva, base, nfunc, nname,
             ftab, ntab, otab) = struct.unpack_from("<IIHHIIIIIII", d, eo)
            noff = r2o(ntab)
            for i in range(nname):
                nrva = struct.unpack_from("<I", d, noff + 4 * i)[0]
                exports.append(_cstr(d, r2o(nrva)))

    return {
        "format": "PE32",
        "machine": f"0x{machine:04x}",
        "timestamp_unix": tstamp,
        "characteristics": chars,
        "entry_point_rva": entry_rva,
        "entry_point_va": image_base + entry_rva,
        "image_base": image_base,
        "base_of_code": codebase,
        "base_of_data": database,
        "section_alignment": sec_align,
        "file_alignment": file_align,
        "os_version": f"{osmaj}.{osmin}",
        "subsystem_version": f"{submaj}.{submin}",
        "subsystem": subsystem,
        "size_of_image": size_img,
        "size_of_headers": size_hdr,
        "sections": secs,
        "data_directories": dirs,
        "imports": imports,
        "exports": exports,
    }


# ---------------------------------------------------------------- LE/LX ---

LE_CPU = {1: "80286", 2: "80386", 3: "80486", 4: "80586",
          0x20: "intel i860", 0x21: "intel n10/n11", 0x40: "mips mark I",
          0x41: "mips mark II", 0x42: "mips mark III",
          0x60: "ppc-1", 0x61: "ppc-2", 0x62: "ppc-3"}
LE_OS = {0: "unknown", 1: "os/2", 2: "windows", 3: "dos 4.x",
         4: "windows 386", 5: "unknown"}

# LE object flags (per LE spec; subset most relevant for loader evidence)
LE_OBJ_FLAGS = {
    0x0001: "readable", 0x0002: "writable", 0x0004: "executable",
    0x0008: "resource", 0x0010: "discardable", 0x0020: "shared",
    0x0040: "preload", 0x0080: "invalid/zerofill", 0x0100: "permanent",
    0x0200: "permanent", 0x0400: "resident", 0x0800: "res/contiguous",
    0x1000: "res/longlock", 0x2000: "16:16-alias", 0x4000: "big/32bit",
    0x8000: "conforming",
}

_LE_HEADER_FMT = "<BBIHH" + "I" * 43
_LE_HEADER_NAMES = [
    "byte_order", "word_order", "fmt_level", "cpu", "os", "mod_ver",
    "mod_flags", "npages", "eip_obj", "eip", "esp_obj", "esp",
    "page_size", "last_page_sz", "fixup_sz", "fixup_ck", "loader_sz",
    "loader_ck", "objtab_off", "nobj", "objpagetab_off", "objiter_off",
    "restab_off", "nres", "rnamentab_off", "entrytab_off", "moddir_off",
    "nmoddir", "fixpagetab_off", "fixrectab_off", "impmodtab_off",
    "nimpmod", "impproctab_off", "pgcksum_off", "datapages_off",
    "npreload", "nonrestab_off", "nonrestab_sz", "nonres_ck",
    "autodata_obj", "dbg_off", "dbg_len", "npreinst", "ndeminst",
    "heap_sz", "stack_sz",
]


def _le_flags_str(flags: int) -> list[str]:
    return [name for bit, name in LE_OBJ_FLAGS.items() if flags & bit]


def parse_le(d: bytes) -> dict:
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    sig = d[e_lfanew:e_lfanew + 2]
    if sig not in (b"LE", b"LX"):
        raise ValueError("no LE/LX signature")
    h = e_lfanew
    hv = dict(zip(_LE_HEADER_NAMES,
                  struct.unpack_from(_LE_HEADER_FMT, d, h + 2)))
    byte_order, word_order, fmt_level = (hv["byte_order"],
                                         hv["word_order"],
                                         hv["fmt_level"])
    cpu, os_type = hv["cpu"], hv["os"]
    mod_ver, mod_flags = hv["mod_ver"], hv["mod_flags"]
    npages, eip_obj, eip = hv["npages"], hv["eip_obj"], hv["eip"]
    esp_obj, esp = hv["esp_obj"], hv["esp"]
    page_size, last_page_sz = hv["page_size"], hv["last_page_sz"]
    fixup_sz, fixup_ck = hv["fixup_sz"], hv["fixup_ck"]
    loader_sz, loader_ck = hv["loader_sz"], hv["loader_ck"]
    objtab_off, nobj = hv["objtab_off"], hv["nobj"]
    objpagetab_off, objiter_off = hv["objpagetab_off"], hv["objiter_off"]
    restab_off, nres = hv["restab_off"], hv["nres"]
    rnamentab_off, entrytab_off = hv["rnamentab_off"], hv["entrytab_off"]
    moddir_off, nmoddir = hv["moddir_off"], hv["nmoddir"]
    fixpagetab_off, fixrectab_off = (hv["fixpagetab_off"],
                                   hv["fixrectab_off"])
    impmodtab_off, nimpmod = hv["impmodtab_off"], hv["nimpmod"]
    impproctab_off, pgcksum_off = (hv["impproctab_off"],
                                 hv["pgcksum_off"])
    datapages_off, npreload = hv["datapages_off"], hv["npreload"]
    nonrestab_off, nonrestab_sz = hv["nonrestab_off"], hv["nonrestab_sz"]
    nonres_ck, autodata_obj = hv["nonres_ck"], hv["autodata_obj"]
    dbg_off, dbg_len = hv["dbg_off"], hv["dbg_len"]
    npreinst, ndeminst = hv["npreinst"], hv["ndeminst"]
    heap_sz, stack_sz = hv["heap_sz"], hv["stack_sz"]

    # Object table (offsets relative to LE header)
    objects = []
    for i in range(nobj):
        o = h + objtab_off + 24 * i
        vsz, reloc_base, flags, ptidx, npt, _r = struct.unpack_from(
            "<IIIIII", d, o)
        objects.append({"index": i + 1, "virtual_size": vsz,
                        "reloc_base": reloc_base, "flags": flags,
                        "flag_names": _le_flags_str(flags),
                        "page_table_index": ptidx,
                        "num_page_table_entries": npt})

    # Object page map. LE entries are 4 bytes: 24-bit big-endian page
    # index (relative to data-pages area) + 8-bit page flags.
    # LX entries are 8 bytes: 32-bit BE data offset (shifted per page
    # offset shift), 16-bit BE data size, 16-bit BE flags.
    pages = []
    is_lx = sig == b"LX"
    entsz = 8 if is_lx else 4
    for o in objects:
        for i in range(o["num_page_table_entries"]):
            p = h + objpagetab_off + entsz * (o["page_table_index"] - 1 + i)
            if is_lx:
                data_off, data_sz, flags = struct.unpack_from(">IHH",
                                                              d, p)
                file_off = data_off << (
                    0 if page_size == 0x1000 else 0)  # LX: raw units
            else:
                idx = (d[p] << 16) | (d[p + 1] << 8) | d[p + 2]
                flags = d[p + 3]
                data_sz = page_size
                file_off = datapages_off + idx * page_size
            pages.append({"object": o["index"], "page_index": idx if not
                          is_lx else None,
                          "file_offset": file_off, "data_size": data_sz,
                          "flags": flags})

    # Resident name table (offset relative to LE header):
    # [len][name][ord]... entry 0 = module name, terminated by len==0
    rnames = []
    off = h + rnamentab_off
    while off < len(d):
        ln = d[off]
        if ln == 0:
            break
        nm = d[off + 1:off + 1 + ln].decode("latin1", "replace")
        ordv = struct.unpack_from("<H", d, off + 1 + ln)[0]
        rnames.append({"name": nm, "ordinal": ordv})
        off += 3 + ln

    # Nonresident name table (absolute file offset)
    nrnames = []
    off = nonrestab_off
    end = off + nonrestab_sz
    while nonrestab_sz and off < end and off < len(d):
        ln = d[off]
        if ln == 0:
            break
        nm = d[off + 1:off + 1 + ln].decode("latin1", "replace")
        ordv = struct.unpack_from("<H", d, off + 1 + ln)[0]
        nrnames.append({"name": nm, "ordinal": ordv})
        off += 3 + ln

    # Entry table (offset relative to LE header):
    # bundles of [count][type][entries]
    entries = []
    off = h + entrytab_off
    while off < len(d):
        cnt = d[off]
        if cnt == 0:
            break
        typ = d[off + 1]
        off += 2
        if typ == 0x01:  # 16-bit entries
            for _ in range(cnt):
                flags, ent = struct.unpack_from("<BH", d, off)
                entries.append({"type": "16bit", "flags": flags,
                                "offset": ent})
                off += 3
        elif typ == 0x02:  # 286 callgate
            for _ in range(cnt):
                flags, ent, _g = struct.unpack_from("<BHH", d, off)
                entries.append({"type": "callgate286", "flags": flags,
                                "offset": ent})
                off += 5
        elif typ == 0x03:  # 32-bit entries
            for _ in range(cnt):
                flags, ent = struct.unpack_from("<BI", d, off)
                entries.append({"type": "32bit", "flags": flags,
                                "offset": ent})
                off += 5
        elif typ == 0x04:  # forwarder
            for _ in range(cnt):
                flags, modord, _r, entoff = struct.unpack_from(
                    "<BHH I", d, off)
                entries.append({"type": "fwd", "flags": flags,
                                "module_ord": modord, "offset": entoff})
                off += 7
        else:
            entries.append({"type": f"unknown-0x{typ:02x}",
                            "bundle_rest": cnt})
            break

    # Import module name table (counted strings, LE-header-relative)
    impmods = []
    off = h + impmodtab_off
    for _ in range(nimpmod):
        ln = d[off]
        nm = d[off + 1:off + 1 + ln].decode("latin1", "replace")
        impmods.append(nm)
        off += 1 + ln
    impprocs = []
    off = h + impproctab_off
    if nimpmod:
        while off < h + fixpagetab_off:
            ln = d[off]
            if ln == 0:
                off += 1
                continue
            nm = d[off + 1:off + 1 + ln].decode("latin1", "replace")
            impprocs.append(nm)
            off += 1 + ln

    return {
        "format": sig.decode(),
        "byte_order": byte_order, "word_order": word_order,
        "format_level": fmt_level,
        "cpu": LE_CPU.get(cpu, f"0x{cpu:02x}"),
        "os": LE_OS.get(os_type, f"0x{os_type:02x}"),
        "module_version": mod_ver,
        "module_flags": mod_flags,
        "num_memory_pages": npages,
        "entry": {"object": eip_obj, "offset": eip},
        "initial_stack": {"object": esp_obj, "offset": esp},
        "page_size": page_size,
        "last_page_size": last_page_sz,
        "fixup_section_size": fixup_sz,
        "loader_section_size": loader_sz,
        "num_objects": nobj,
        "objects": objects,
        "object_page_map": pages,
        "num_resources": nres,
        "num_module_directives": nmoddir,
        "resident_names": rnames,
        "nonresident_names": nrnames,
        "num_imported_modules": nimpmod,
        "import_modules": impmods,
        "import_procedures": impprocs,
        "entry_table": entries,
        "data_pages_offset": datapages_off,
        "num_preload_pages": npreload,
        "auto_data_object": autodata_obj,
        "debug_offset": dbg_off, "debug_length": dbg_len,
        "heap_size": heap_sz, "stack_size": stack_sz,
        "mz_stub_size": e_lfanew,
    }


def identify_and_parse(d: bytes) -> dict:
    if d[:2] != b"MZ":
        raise ValueError("no MZ signature")
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    sig = d[e_lfanew:e_lfanew + 4]
    if sig == b"PE\0\0":
        return parse_pe(d)
    if sig[:2] in (b"LE", b"LX"):
        return parse_le(d)
    raise ValueError(f"unrecognized signature at e_lfanew: {sig!r}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--format", choices=["json", "text"], default="json")
    args = ap.parse_args(argv)

    out = {}
    for p in args.files:
        try:
            with open(p, "rb") as f:
                out[p] = identify_and_parse(f.read())
        except (OSError, ValueError, struct.error) as e:
            out[p] = {"error": str(e)}
    if args.format == "json":
        json.dump(out, sys.stdout, indent=1)
        sys.stdout.write("\n")
    else:
        for p, r in out.items():
            print(f"== {p} ==")
            print(json.dumps(r, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
