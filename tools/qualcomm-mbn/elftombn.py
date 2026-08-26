#!/usr/bin/env python3
# ==========================================================================
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only
# ==========================================================================
#
# Source: 1980490718/u-boot-2016, tools/elftombn_py3.py
# Commit: 0d9019b33427755c17cb77e849a90806b9f434b9
# Author: Willem Lee <1980490718@qq.com>
# Date: 2026-04-11 19:05:42 +0800
# Subject: feat: Add Python 3 support with Python 2 backward compatibility
# Original-Signed-off-by: Willem Lee <1980490718@qq.com>
#
# Local changes restrict the interface to the WXR-5950AX12 non-secure MBN v3
# format and use the focused mbn_tools module in this directory.
#
# Signed-off-by: Codex <codex@openai.com>

import argparse

import mbn_tools


def main():
    parser = argparse.ArgumentParser(
        description="Package a WXR-5950AX12 U-Boot ELF as Qualcomm MBN v3"
    )
    parser.add_argument("-f", "--first-filepath", required=True, dest="input_path")
    parser.add_argument("-o", "--output-filepath", required=True, dest="output_path")
    parser.add_argument("-v", "--mbn-version", required=True, dest="mbn_version")
    args = parser.parse_args()

    if args.mbn_version != "3":
        parser.error("MBN version must be exactly 3")

    try:
        mbn_tools.package_appsbl(args.input_path, args.output_path)
    except (OSError, ValueError) as error:
        parser.exit(1, "elftombn.py: error: {}\n".format(error))

    return 0


if __name__ == "__main__":
    main()
