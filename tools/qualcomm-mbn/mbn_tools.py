#!/usr/bin/env python3
# ==========================================================================
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only
# ==========================================================================
#
# Source: 1980490718/u-boot-2016, tools/mbn_tools_py3.py
# Commit: 0d9019b33427755c17cb77e849a90806b9f434b9
# Author: Willem Lee <1980490718@qq.com>
# Date: 2026-04-11 19:05:42 +0800
# Subject: feat: Add Python 3 support with Python 2 backward compatibility
# Original-Signed-off-by: Willem Lee <1980490718@qq.com>
#
# This project retains only the ELF32 ARM, non-secure Qualcomm MBN v3 path.
# The target layout is fixed to the two known-working WXR-5950AX12 APPSBL
# images: 4 KiB hash alignment, hash address 0x4a990000, and code offset
# 0x12000. Unsupported formats are rejected instead of falling back.
#
# Signed-off-by: Codex <codex@openai.com>

import hashlib
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
RAW_LOAD_ALIGN = 0x10000
FINAL_LOAD_OFFSET = 0x12000
STACK_OFFSET = 0x2000

PHDR_SEGMENT_FLAGS = 0x07000000
HASH_SEGMENT_FLAGS = 0x02200000
HASH_OFFSET = 0x1000
HASH_ADDRESS = 0x4A990000
HASH_ALIGNMENT = 0x1000
TLB_ADDRESS = 0x4A9A0000

MBN_IMAGE_ID = 21
MBN_VERSION = 3
HASH_ENTRY_SIZE = 32
OUTPUT_PROGRAM_HEADERS = 4
HASH_TABLE_SIZE = OUTPUT_PROGRAM_HEADERS * HASH_ENTRY_SIZE
PHDR_FILE_SIZE = ELF_HEADER.size + OUTPUT_PROGRAM_HEADERS * PROGRAM_HEADER.size
HASH_FILE_SIZE = MBN_V3_HEADER.size + HASH_TABLE_SIZE


def read_elf32(path):
    with open(path, "rb") as source:
        data = source.read()

    if len(data) < ELF_HEADER.size:
        raise ValueError("ELF file is shorter than its 52-byte header")

    header = dict(zip(ELF_HEADER_FIELDS, ELF_HEADER.unpack_from(data, 0)))
    ident = header["e_ident"]

    if ident[:4] != b"\x7fELF":
        raise ValueError("input is not an ELF file")
    if ident[4] != 1:
        raise ValueError("input is not ELF32")
    if ident[5] != 1:
        raise ValueError("input is not little-endian")
    if ident[6] != 1:
        raise ValueError("input ELF version is not 1")
    if header["e_type"] != ET_DYN:
        raise ValueError("input ELF type is not ET_DYN")
    if header["e_machine"] != EM_ARM:
        raise ValueError("input ELF machine is not ARM")
    if header["e_version"] != 1:
        raise ValueError("input ELF header version is not 1")
    if header["e_ehsize"] != ELF_HEADER.size:
        raise ValueError("input ELF header size is not 52 bytes")
    if header["e_phentsize"] != PROGRAM_HEADER.size:
        raise ValueError("input program-header size is not 32 bytes")

    phdr_end = header["e_phoff"] + header["e_phnum"] * PROGRAM_HEADER.size
    if phdr_end > len(data):
        raise ValueError("input program-header table extends past end of file")

    program_headers = []
    for index in range(header["e_phnum"]):
        offset = header["e_phoff"] + index * PROGRAM_HEADER.size
        values = PROGRAM_HEADER.unpack_from(data, offset)
        program_headers.append(dict(zip(PROGRAM_HEADER_FIELDS, values)))

    return data, header, program_headers


def package_appsbl(input_path, output_path):
    data, header, program_headers = read_elf32(input_path)

    if header["e_entry"] != ENTRY_ADDRESS:
        raise ValueError("input entry address is not 0x4a900000")
    if header["e_phoff"] != ELF_HEADER.size:
        raise ValueError("input program-header table does not start at offset 52")
    if header["e_phnum"] != 2:
        raise ValueError("input must contain exactly two program headers")

    load = program_headers[0]
    stack = program_headers[1]

    expected_load = {
        "p_type": PT_LOAD,
        "p_offset": RAW_LOAD_OFFSET,
        "p_vaddr": ENTRY_ADDRESS,
        "p_paddr": ENTRY_ADDRESS,
        "p_flags": PF_RWX,
        "p_align": RAW_LOAD_ALIGN,
    }
    for field, expected in expected_load.items():
        if load[field] != expected:
            raise ValueError(
                "input LOAD {} is 0x{:x}, expected 0x{:x}".format(
                    field, load[field], expected
                )
            )
    if load["p_filesz"] == 0:
        raise ValueError("input LOAD segment is empty")
    if load["p_filesz"] != load["p_memsz"]:
        raise ValueError("input LOAD file and memory sizes differ")

    expected_stack = {
        "p_type": PT_GNU_STACK,
        "p_offset": 0,
        "p_vaddr": 0,
        "p_paddr": 0,
        "p_filesz": 0,
        "p_memsz": 0,
        "p_flags": PF_RWX,
        "p_align": 0x10,
    }
    for field, expected in expected_stack.items():
        if stack[field] != expected:
            raise ValueError(
                "input GNU_STACK {} is 0x{:x}, expected 0x{:x}".format(
                    field, stack[field], expected
                )
            )

    raw_load_end = load["p_offset"] + load["p_filesz"]
    if raw_load_end > len(data):
        raise ValueError("input LOAD data extends past end of file")
    memory_load_end = load["p_paddr"] + load["p_memsz"]
    if memory_load_end > HASH_ADDRESS:
        raise ValueError(
            "input LOAD ends at 0x{:x}, beyond hash address 0x{:x}".format(
                memory_load_end, HASH_ADDRESS
            )
        )
    if HASH_ADDRESS + HASH_ALIGNMENT > TLB_ADDRESS:
        raise ValueError("hash segment overlaps the known TLB address")

    load_data = data[load["p_offset"]:raw_load_end]

    output_header = dict(header)
    output_header["e_phnum"] = OUTPUT_PROGRAM_HEADERS
    output_header["e_shoff"] = 0
    output_header["e_shnum"] = 0
    output_header["e_shstrndx"] = 0

    phdr_segment = {
        "p_type": PT_NULL,
        "p_offset": 0,
        "p_vaddr": 0,
        "p_paddr": 0,
        "p_filesz": PHDR_FILE_SIZE,
        "p_memsz": 0,
        "p_flags": PHDR_SEGMENT_FLAGS,
        "p_align": 0,
    }
    hash_segment = {
        "p_type": PT_NULL,
        "p_offset": HASH_OFFSET,
        "p_vaddr": HASH_ADDRESS,
        "p_paddr": HASH_ADDRESS,
        "p_filesz": HASH_FILE_SIZE,
        "p_memsz": HASH_ALIGNMENT,
        "p_flags": HASH_SEGMENT_FLAGS,
        "p_align": HASH_ALIGNMENT,
    }
    output_load = dict(load)
    output_load["p_offset"] = FINAL_LOAD_OFFSET
    output_stack = dict(stack)
    output_stack["p_offset"] = STACK_OFFSET
    output_program_headers = (
        phdr_segment,
        hash_segment,
        output_load,
        output_stack,
    )

    header_bytes = ELF_HEADER.pack(
        *(output_header[field] for field in ELF_HEADER_FIELDS)
    )
    phdr_bytes = b"".join(
        PROGRAM_HEADER.pack(*(entry[field] for field in PROGRAM_HEADER_FIELDS))
        for entry in output_program_headers
    )
    header_hash = hashlib.sha256(header_bytes + phdr_bytes).digest()
    load_hash = hashlib.sha256(load_data).digest()
    zero_hash = b"\0" * HASH_ENTRY_SIZE
    hash_table = header_hash + zero_hash + load_hash + zero_hash

    image_destination = HASH_ADDRESS + HASH_ALIGNMENT + MBN_V3_HEADER.size
    signature_pointer = image_destination + HASH_TABLE_SIZE
    mbn_header = MBN_V3_HEADER.pack(
        MBN_IMAGE_ID,
        MBN_VERSION,
        0,
        image_destination,
        HASH_TABLE_SIZE,
        HASH_TABLE_SIZE,
        signature_pointer,
        0,
        signature_pointer,
        0,
    )

    output_size = FINAL_LOAD_OFFSET + len(load_data)
    if output_size > 0x100000:
        raise ValueError("packaged APPSBL exceeds the 1 MiB partition")

    output = bytearray(output_size)
    output[:ELF_HEADER.size] = header_bytes
    output[ELF_HEADER.size:PHDR_FILE_SIZE] = phdr_bytes
    output[HASH_OFFSET:HASH_OFFSET + HASH_FILE_SIZE] = mbn_header + hash_table
    output[FINAL_LOAD_OFFSET:output_size] = load_data

    with open(output_path, "wb") as target:
        target.write(output)
