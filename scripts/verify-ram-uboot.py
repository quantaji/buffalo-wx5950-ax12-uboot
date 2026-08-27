#!/usr/bin/env python3

import argparse
import gzip
import hashlib
import pathlib
import re
import stat
import struct
import subprocess
import tempfile
import zlib


ELF_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")

ET_DYN = 3
EM_ARM = 40
FDT_MAGIC = 0xD00DFEED
AARCH64_IMAGE_MAGIC = 0x644D5241

FIT_INPUT_ADDRESS = 0x44000000
RAM_LOAD_ADDRESS = 0x4A000000
RAM_UPPER_BOUND = 0x4A400000
ARM_PAGE_TABLE_SIZE = 0x10000
FDT_RELOCATION_PADDING = 0x3000

CONFIG_SYS_MALLOC_LEN = 0x140000
CONFIG_ENV_SIZE = 0x40000
GENERATED_BD_INFO_SIZE = 80
INITIAL_STACK_ADDRESS = (
    RAM_LOAD_ADDRESS
    - CONFIG_SYS_MALLOC_LEN
    - CONFIG_ENV_SIZE
    - GENERATED_BD_INFO_SIZE
)

EXPECTED_IMAGES = ("kernel@1", "fdt@hk01")
EXPECTED_CONFIGURATIONS = ("config@hk01",)
REQUIRED_MAP_SYMBOLS = (
    "_start",
    "reset",
    "_image_binary_end",
    "__bss_end",
)


def read_elf_entry(path):
    with path.open("rb") as source:
        header_data = source.read(ELF_HEADER.size)

    if len(header_data) != ELF_HEADER.size:
        raise ValueError("{} is shorter than an ELF32 header".format(path))

    header = ELF_HEADER.unpack(header_data)
    ident = header[0]
    elf_type = header[1]
    machine = header[2]
    version = header[3]
    entry = header[4]
    header_size = header[8]

    if ident[:4] != b"\x7fELF" or ident[4:7] != b"\x01\x01\x01":
        raise ValueError("{} is not little-endian ELF32 version 1".format(path))
    if elf_type != ET_DYN or machine != EM_ARM:
        raise ValueError("{} is not an ARM ET_DYN ELF".format(path))
    if version != 1 or header_size != ELF_HEADER.size:
        raise ValueError("{} has an invalid ELF header".format(path))

    return entry


def read_map_symbols(path):
    symbol_pattern = re.compile(
        r"^\s*(0x[0-9a-fA-F]+)\s+"
        r"(_start|reset|_image_binary_end|__bss_end)(?:\s|$)"
    )
    symbols = {}

    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            match = symbol_pattern.match(line)
            if not match:
                continue

            address = int(match.group(1), 16)
            name = match.group(2)
            if name in symbols and symbols[name] != address:
                raise ValueError(
                    "{} has conflicting addresses for {}".format(path, name)
                )
            symbols[name] = address

    missing = [name for name in REQUIRED_MAP_SYMBOLS if name not in symbols]
    if missing:
        raise ValueError(
            "{} is missing map symbols: {}".format(path, ", ".join(missing))
        )

    return symbols


def read_fdt_property(fdtget, fit_path, node, property_name, value_type):
    output = subprocess.check_output(
        [
            fdtget,
            "-t",
            value_type,
            str(fit_path),
            node,
            property_name,
        ],
        universal_newlines=True,
    )
    return output.strip()


def verify_hash_node(
    fdtget,
    fit_path,
    node,
    expected_algorithm,
    payload,
):
    algorithm = read_fdt_property(
        fdtget,
        fit_path,
        node,
        "algo",
        "s",
    )
    if algorithm != expected_algorithm:
        raise ValueError(
            "{} uses {}, expected {}".format(
                node,
                algorithm,
                expected_algorithm,
            )
        )

    value_text = read_fdt_property(
        fdtget,
        fit_path,
        node,
        "value",
        "bx",
    )
    stored_value = bytes(int(token, 16) for token in value_text.split())

    if expected_algorithm == "crc32":
        calculated_value = struct.pack(
            ">I",
            zlib.crc32(payload) & 0xFFFFFFFF,
        )
    elif expected_algorithm == "sha1":
        calculated_value = hashlib.sha1(payload).digest()
    else:
        raise ValueError(
            "unsupported RAM FIT hash algorithm: {}".format(expected_algorithm)
        )

    if stored_value != calculated_value:
        raise ValueError("{} does not match its payload".format(node))


def verify_build_layout(build_dir):
    elf_path = build_dir / "u-boot"
    raw_path = build_dir / "u-boot.bin"
    map_path = build_dir / "u-boot.map"
    config_path = build_dir / ".config"
    control_dtb = build_dir / "arch/arm/dts/ipq807x-wxr5950ax12.dtb"
    dumpimage = build_dir / "tools/dumpimage"

    for required_path in (
        elf_path,
        raw_path,
        map_path,
        config_path,
        control_dtb,
        dumpimage,
    ):
        if not required_path.is_file() or required_path.stat().st_size == 0:
            raise ValueError(
                "RAM build input is missing or empty: {}".format(required_path)
            )

    config = config_path.read_text(encoding="utf-8", errors="replace")
    if "CONFIG_BUFFALO_WXR5950AX12_RAM_UBOOT=y\n" not in config:
        raise ValueError("RAM U-Boot configuration is not enabled")
    if "CONFIG_CMD_ELF=y\n" not in config:
        raise ValueError("bootelf support is not enabled in RAM U-Boot")

    entry = read_elf_entry(elf_path)
    symbols = read_map_symbols(map_path)
    raw = raw_path.read_bytes()

    if entry != RAM_LOAD_ADDRESS:
        raise ValueError("RAM U-Boot ELF entry is not 0x4a000000")
    if symbols["_start"] != RAM_LOAD_ADDRESS:
        raise ValueError("RAM U-Boot _start is not 0x4a000000")
    if len(raw) < 0x3C:
        raise ValueError("RAM U-Boot raw binary is too short for its entry checks")

    raw_end = RAM_LOAD_ADDRESS + len(raw)
    if raw_end != symbols["_image_binary_end"]:
        raise ValueError("u-boot.bin length differs from _image_binary_end")
    if symbols["__bss_end"] < raw_end:
        raise ValueError("RAM U-Boot BSS ends below its raw binary")

    instruction = struct.unpack_from("<I", raw, 0)[0]
    if instruction & 0xFF000000 != 0xEA000000:
        raise ValueError("RAM U-Boot first instruction is not an ARM branch")

    immediate = instruction & 0x00FFFFFF
    if immediate & 0x00800000:
        immediate -= 0x01000000
    branch_target = RAM_LOAD_ADDRESS + 8 + (immediate << 2)
    if branch_target != symbols["reset"]:
        raise ValueError("RAM U-Boot first instruction does not branch to reset")

    image_magic = struct.unpack_from("<I", raw, 0x38)[0]
    if image_magic == AARCH64_IMAGE_MAGIC:
        raise ValueError("RAM U-Boot would be mistaken for an AArch64 image")

    tlb_start = (symbols["__bss_end"] + 0xFFFF) & ~0xFFFF
    tlb_end = tlb_start + ARM_PAGE_TABLE_SIZE
    if tlb_end > RAM_UPPER_BOUND:
        raise ValueError("RAM U-Boot image, BSS, or TLB exceeds 0x4a400000")

    dtb_size = control_dtb.stat().st_size
    if tlb_end + dtb_size + FDT_RELOCATION_PADDING > RAM_UPPER_BOUND:
        raise ValueError("RAM U-Boot and relocated FIT DTB cannot remain disjoint")

    return {
        "raw": raw,
        "raw_end": raw_end,
        "bss_end": symbols["__bss_end"],
        "tlb_start": tlb_start,
        "tlb_end": tlb_end,
        "control_dtb": control_dtb,
        "dumpimage": dumpimage,
    }


def verify_fit(fit_path, build_context):
    fdtget = "fdtget"
    fit_data = fit_path.read_bytes()
    if len(fit_data) < 8:
        raise ValueError("RAM FIT is shorter than an FDT header")

    magic, total_size = struct.unpack_from(">II", fit_data, 0)
    if magic != FDT_MAGIC:
        raise ValueError("RAM FIT has an invalid FDT magic")
    if total_size != len(fit_data):
        raise ValueError("RAM FIT total size differs from its file length")

    image_nodes = tuple(
        line.strip()
        for line in subprocess.check_output(
            [fdtget, "-l", str(fit_path), "/images"],
            universal_newlines=True,
        ).splitlines()
        if line.strip()
    )
    if image_nodes != EXPECTED_IMAGES:
        raise ValueError("RAM FIT image nodes are incorrect: {}".format(image_nodes))

    configuration_nodes = tuple(
        line.strip()
        for line in subprocess.check_output(
            [fdtget, "-l", str(fit_path), "/configurations"],
            universal_newlines=True,
        ).splitlines()
        if line.strip()
    )
    if configuration_nodes != EXPECTED_CONFIGURATIONS:
        raise ValueError(
            "RAM FIT configuration nodes are incorrect: {}".format(
                configuration_nodes
            )
        )

    expected_properties = (
        ("/configurations", "default", "s", "config@hk01"),
        ("/configurations/config@hk01", "kernel", "s", "kernel@1"),
        ("/configurations/config@hk01", "fdt", "s", "fdt@hk01"),
        ("/images/kernel@1", "type", "s", "kernel"),
        ("/images/kernel@1", "arch", "s", "arm"),
        ("/images/kernel@1", "os", "s", "linux"),
        ("/images/kernel@1", "compression", "s", "gzip"),
        ("/images/kernel@1", "load", "x", "4a000000"),
        ("/images/kernel@1", "entry", "x", "4a000000"),
        ("/images/fdt@hk01", "type", "s", "flat_dt"),
        ("/images/fdt@hk01", "arch", "s", "arm"),
        ("/images/fdt@hk01", "compression", "s", "none"),
    )
    for node, property_name, value_type, expected_value in expected_properties:
        value = read_fdt_property(
            fdtget,
            fit_path,
            node,
            property_name,
            value_type,
        )
        if value.lower() != expected_value:
            raise ValueError(
                "RAM FIT {}:{} is {}, expected {}".format(
                    node,
                    property_name,
                    value,
                    expected_value,
                )
            )

    for image_node in EXPECTED_IMAGES:
        hash_nodes = tuple(
            line.strip()
            for line in subprocess.check_output(
                [fdtget, "-l", str(fit_path), "/images/{}".format(image_node)],
                universal_newlines=True,
            ).splitlines()
            if line.strip()
        )
        if hash_nodes != ("hash@1", "hash@2"):
            raise ValueError(
                "RAM FIT {} hash nodes are incorrect: {}".format(
                    image_node,
                    hash_nodes,
                )
            )

    with tempfile.TemporaryDirectory(prefix="verify-ram-uboot.") as temp_name:
        temp_dir = pathlib.Path(temp_name)
        extracted_kernel = temp_dir / "u-boot.bin.gz"
        extracted_dtb = temp_dir / "ipq807x-wxr5950ax12.dtb"

        subprocess.check_call(
            [
                str(build_context["dumpimage"]),
                "-i",
                str(fit_path),
                "-T",
                "flat_dt",
                "-p",
                "0",
                "-o",
                str(extracted_kernel),
                str(fit_path),
            ]
        )
        subprocess.check_call(
            [
                str(build_context["dumpimage"]),
                "-i",
                str(fit_path),
                "-T",
                "flat_dt",
                "-p",
                "1",
                "-o",
                str(extracted_dtb),
                str(fit_path),
            ]
        )

        compressed_payload = extracted_kernel.read_bytes()
        dtb_payload = extracted_dtb.read_bytes()

        if gzip.decompress(compressed_payload) != build_context["raw"]:
            raise ValueError("RAM FIT kernel payload differs from u-boot.bin")
        if dtb_payload != build_context["control_dtb"].read_bytes():
            raise ValueError("RAM FIT DTB differs from the same-build control DTB")

        verify_hash_node(
            fdtget,
            fit_path,
            "/images/kernel@1/hash@1",
            "crc32",
            compressed_payload,
        )
        verify_hash_node(
            fdtget,
            fit_path,
            "/images/kernel@1/hash@2",
            "sha1",
            compressed_payload,
        )
        verify_hash_node(
            fdtget,
            fit_path,
            "/images/fdt@hk01/hash@1",
            "crc32",
            dtb_payload,
        )
        verify_hash_node(
            fdtget,
            fit_path,
            "/images/fdt@hk01/hash@2",
            "sha1",
            dtb_payload,
        )

    fit_input_end = FIT_INPUT_ADDRESS + len(fit_data)
    if fit_input_end > INITIAL_STACK_ADDRESS:
        raise ValueError("RAM FIT input overlaps the RAM U-Boot stack region")

    mode = stat.S_IMODE(fit_path.stat().st_mode)
    if mode != 0o644:
        raise ValueError(
            "RAM FIT permission is {:04o}, expected 0644".format(mode)
        )

    return {
        "fit_data": fit_data,
        "fit_input_end": fit_input_end,
        "mode": mode,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Verify a WXR-5950AX12 recovery-compatible RAM U-Boot FIT"
    )
    parser.add_argument("fit", help="packaged RAM U-Boot FIT path")
    parser.add_argument("build_dir", help="corresponding RAM U-Boot build directory")
    args = parser.parse_args()

    fit_path = pathlib.Path(args.fit)
    build_dir = pathlib.Path(args.build_dir)

    try:
        build_context = verify_build_layout(build_dir)
        result = verify_fit(fit_path, build_context)
    except (
        OSError,
        ValueError,
        struct.error,
        subprocess.CalledProcessError,
    ) as error:
        parser.exit(1, "verify-ram-uboot.py: error: {}\n".format(error))

    digest = hashlib.sha256(result["fit_data"]).hexdigest()
    print("RAM U-Boot FIT verification passed")
    print("size: {} bytes".format(len(result["fit_data"])))
    print("sha256: {}".format(digest))
    print("mode: {:04o}".format(result["mode"]))
    print("entry: 0x{:08x}".format(RAM_LOAD_ADDRESS))
    print(
        "raw: 0x{:08x}-0x{:08x}".format(
            RAM_LOAD_ADDRESS,
            build_context["raw_end"],
        )
    )
    print("bss end: 0x{:08x}".format(build_context["bss_end"]))
    print(
        "tlb: 0x{:08x}-0x{:08x}".format(
            build_context["tlb_start"],
            build_context["tlb_end"],
        )
    )
    print(
        "fit input: 0x{:08x}-0x{:08x}".format(
            FIT_INPUT_ADDRESS,
            result["fit_input_end"],
        )
    )
    return 0


if __name__ == "__main__":
    main()
