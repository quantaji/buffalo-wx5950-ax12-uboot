#!/usr/bin/env python3

import argparse
import hashlib
import os
import stat
import struct


ELF_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIIIIIII")
MBN_V3_HEADER = struct.Struct("<10I")

ELF_HEADER_FIELDS = (
    "e_ident",
    "e_type",
    "e_machine",
    "e_version",
    "e_entry",
    "e_phoff",
    "e_shoff",
    "e_flags",
    "e_ehsize",
    "e_phentsize",
    "e_phnum",
    "e_shentsize",
    "e_shnum",
    "e_shstrndx",
)
PROGRAM_HEADER_FIELDS = (
    "p_type",
    "p_offset",
    "p_vaddr",
    "p_paddr",
    "p_filesz",
    "p_memsz",
    "p_flags",
    "p_align",
)

ET_DYN = 3
EM_ARM = 40
PT_NULL = 0
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_RWX = 7

ENTRY_ADDRESS = 0x4A900000
RAW_LOAD_OFFSET = 0x10000
FINAL_LOAD_OFFSET = 0x12000
STACK_OFFSET = 0x2000
PHDR_FILE_SIZE = 0xB4
HASH_OFFSET = 0x1000
HASH_ADDRESS = 0x4A990000
HASH_FILE_SIZE = 0xA8
HASH_MEMORY_SIZE = 0x1000
TLB_ADDRESS = 0x4A9A0000
PARTITION_SIZE = 0x100000


def read_elf32(path):
    with open(path, "rb") as source:
        data = source.read()

    if len(data) < ELF_HEADER.size:
        raise ValueError("{} is shorter than an ELF32 header".format(path))

    header = dict(zip(ELF_HEADER_FIELDS, ELF_HEADER.unpack_from(data, 0)))
    ident = header["e_ident"]
    if ident[:4] != b"\x7fELF" or ident[4:7] != b"\x01\x01\x01":
        raise ValueError("{} is not little-endian ELF32 version 1".format(path))
    if header["e_type"] != ET_DYN or header["e_machine"] != EM_ARM:
        raise ValueError("{} is not an ARM ET_DYN ELF".format(path))
    if header["e_version"] != 1:
        raise ValueError("{} has an unexpected ELF header version".format(path))
    if header["e_ehsize"] != ELF_HEADER.size:
        raise ValueError("{} has an unexpected ELF header size".format(path))
    if header["e_phentsize"] != PROGRAM_HEADER.size:
        raise ValueError("{} has an unexpected program-header size".format(path))

    phdr_end = header["e_phoff"] + header["e_phnum"] * PROGRAM_HEADER.size
    if phdr_end > len(data):
        raise ValueError("{} has a truncated program-header table".format(path))

    program_headers = []
    for index in range(header["e_phnum"]):
        offset = header["e_phoff"] + index * PROGRAM_HEADER.size
        values = PROGRAM_HEADER.unpack_from(data, offset)
        program_headers.append(dict(zip(PROGRAM_HEADER_FIELDS, values)))

    return data, header, program_headers


def verify_layout(appsbl_path, raw_path):
    appsbl_data, appsbl_header, appsbl_phdrs = read_elf32(appsbl_path)
    raw_data, raw_header, raw_phdrs = read_elf32(raw_path)

    if raw_header["e_entry"] != ENTRY_ADDRESS:
        raise ValueError("stripped ELF entry is not 0x4a900000")
    if raw_header["e_phoff"] != ELF_HEADER.size or raw_header["e_phnum"] != 2:
        raise ValueError("stripped ELF does not have the expected two-PHDR layout")

    raw_load = raw_phdrs[0]
    raw_stack = raw_phdrs[1]
    expected_raw_load = {
        "p_type": PT_LOAD,
        "p_offset": RAW_LOAD_OFFSET,
        "p_vaddr": ENTRY_ADDRESS,
        "p_paddr": ENTRY_ADDRESS,
        "p_flags": PF_RWX,
        "p_align": 0x10000,
    }
    for field, expected in expected_raw_load.items():
        if raw_load[field] != expected:
            raise ValueError("stripped ELF LOAD {} is incorrect".format(field))
    if raw_load["p_filesz"] == 0 or raw_load["p_filesz"] != raw_load["p_memsz"]:
        raise ValueError("stripped ELF LOAD sizes are invalid")

    expected_raw_stack = {
        "p_type": PT_GNU_STACK,
        "p_offset": 0,
        "p_vaddr": 0,
        "p_paddr": 0,
        "p_filesz": 0,
        "p_memsz": 0,
        "p_flags": PF_RWX,
        "p_align": 0x10,
    }
    for field, expected in expected_raw_stack.items():
        if raw_stack[field] != expected:
            raise ValueError("stripped ELF GNU_STACK {} is incorrect".format(field))

    raw_load_end = raw_load["p_offset"] + raw_load["p_filesz"]
    if raw_load_end > len(raw_data):
        raise ValueError("stripped ELF LOAD extends past end of file")

    expected_header = {
        "e_entry": ENTRY_ADDRESS,
        "e_phoff": ELF_HEADER.size,
        "e_shoff": 0,
        "e_ehsize": ELF_HEADER.size,
        "e_phentsize": PROGRAM_HEADER.size,
        "e_phnum": 4,
        "e_shnum": 0,
        "e_shstrndx": 0,
    }
    for field, expected in expected_header.items():
        if appsbl_header[field] != expected:
            raise ValueError(
                "APPSBL {} is 0x{:x}, expected 0x{:x}".format(
                    field, appsbl_header[field], expected
                )
            )

    phdr_segment, hash_segment, appsbl_load, appsbl_stack = appsbl_phdrs
    expected_phdr_segment = {
        "p_type": PT_NULL,
        "p_offset": 0,
        "p_vaddr": 0,
        "p_paddr": 0,
        "p_filesz": PHDR_FILE_SIZE,
        "p_memsz": 0,
        "p_flags": 0x07000000,
        "p_align": 0,
    }
    expected_hash_segment = {
        "p_type": PT_NULL,
        "p_offset": HASH_OFFSET,
        "p_vaddr": HASH_ADDRESS,
        "p_paddr": HASH_ADDRESS,
        "p_filesz": HASH_FILE_SIZE,
        "p_memsz": HASH_MEMORY_SIZE,
        "p_flags": 0x02200000,
        "p_align": HASH_MEMORY_SIZE,
    }
    for field, expected in expected_phdr_segment.items():
        if phdr_segment[field] != expected:
            raise ValueError("APPSBL PHDR segment {} is incorrect".format(field))
    for field, expected in expected_hash_segment.items():
        if hash_segment[field] != expected:
            raise ValueError("APPSBL HASH segment {} is incorrect".format(field))

    expected_appsbl_load = dict(raw_load)
    expected_appsbl_load["p_offset"] = FINAL_LOAD_OFFSET
    expected_appsbl_stack = dict(raw_stack)
    expected_appsbl_stack["p_offset"] = STACK_OFFSET
    if appsbl_load != expected_appsbl_load:
        raise ValueError("APPSBL LOAD program header differs from the stripped ELF contract")
    if appsbl_stack != expected_appsbl_stack:
        raise ValueError("APPSBL GNU_STACK program header differs from the stripped ELF contract")

    memory_load_end = appsbl_load["p_paddr"] + appsbl_load["p_memsz"]
    if memory_load_end > HASH_ADDRESS:
        raise ValueError("APPSBL LOAD overlaps the fixed HASH address")
    if HASH_ADDRESS + HASH_MEMORY_SIZE > TLB_ADDRESS:
        raise ValueError("APPSBL HASH segment overlaps the known TLB address")

    appsbl_load_end = appsbl_load["p_offset"] + appsbl_load["p_filesz"]
    if appsbl_load_end != len(appsbl_data):
        raise ValueError("APPSBL file length does not equal the LOAD segment end")
    if len(appsbl_data) > PARTITION_SIZE:
        raise ValueError("APPSBL exceeds the 1 MiB partition")

    raw_load_data = raw_data[raw_load["p_offset"]:raw_load_end]
    appsbl_load_data = appsbl_data[appsbl_load["p_offset"]:appsbl_load_end]
    if appsbl_load_data != raw_load_data:
        raise ValueError("APPSBL LOAD bytes differ from the stripped ELF")

    if any(appsbl_data[PHDR_FILE_SIZE:HASH_OFFSET]):
        raise ValueError("APPSBL padding before the HASH segment is not zero")
    if any(appsbl_data[HASH_OFFSET + HASH_FILE_SIZE:FINAL_LOAD_OFFSET]):
        raise ValueError("APPSBL padding between HASH and LOAD is not zero")

    mode = stat.S_IMODE(os.stat(appsbl_path).st_mode)
    if mode != 0o644:
        raise ValueError(
            "APPSBL permission is {:04o}, expected 0644".format(mode)
        )

    return {
        "appsbl_data": appsbl_data,
        "appsbl_header": appsbl_header,
        "appsbl_phdrs": appsbl_phdrs,
        "load_end": memory_load_end,
        "mode": mode,
    }


def verify_hashes(context):
    data = context["appsbl_data"]
    header = context["appsbl_header"]
    load = context["appsbl_phdrs"][2]

    mbn_fields = MBN_V3_HEADER.unpack_from(data, HASH_OFFSET)
    expected_mbn_fields = (
        21,
        3,
        0,
        0x4A991028,
        0x80,
        0x80,
        0x4A9910A8,
        0,
        0x4A9910A8,
        0,
    )
    if mbn_fields != expected_mbn_fields:
        raise ValueError("APPSBL MBN v3 header fields are incorrect")

    table_offset = HASH_OFFSET + MBN_V3_HEADER.size
    hash_table = data[table_offset:table_offset + 0x80]
    if len(hash_table) != 0x80:
        raise ValueError("APPSBL hash table is truncated")

    expected_header_hash = hashlib.sha256(data[:PHDR_FILE_SIZE]).digest()
    load_start = load["p_offset"]
    load_end = load_start + load["p_filesz"]
    expected_load_hash = hashlib.sha256(data[load_start:load_end]).digest()
    zero_hash = b"\0" * 32
    expected_hash_table = (
        expected_header_hash + zero_hash + expected_load_hash + zero_hash
    )
    if hash_table != expected_hash_table:
        raise ValueError("APPSBL SHA-256 hash table does not match its final contents")

    if header["e_phnum"] != 4:
        raise ValueError("APPSBL hash count does not match its program-header count")


def main():
    parser = argparse.ArgumentParser(
        description="Verify a WXR-5950AX12 Qualcomm MBN v3 APPSBL"
    )
    parser.add_argument("appsbl", help="packaged APPSBL path")
    parser.add_argument("stripped_elf", help="corresponding stripped U-Boot ELF")
    args = parser.parse_args()

    try:
        context = verify_layout(args.appsbl, args.stripped_elf)
        verify_hashes(context)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, "verify-appsbl.py: error: {}\n".format(error))

    digest = hashlib.sha256(context["appsbl_data"]).hexdigest()
    print("APPSBL verification passed")
    print("size: {} bytes".format(len(context["appsbl_data"])))
    print("sha256: {}".format(digest))
    print("mode: {:04o}".format(context["mode"]))
    print("entry: 0x{:08x}".format(context["appsbl_header"]["e_entry"]))
    print("load: 0x{:08x}-0x{:08x}".format(ENTRY_ADDRESS, context["load_end"]))
    print("hash: 0x{:08x}-0x{:08x}".format(HASH_ADDRESS, HASH_ADDRESS + HASH_MEMORY_SIZE))
    return 0


if __name__ == "__main__":
    main()
