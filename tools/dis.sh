#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
# dis.sh fn_XXXXXXXX  -> clean disassembly of one function from dtk asm
# DECOMP_DIR: a decomp-toolkit project for GEAP8P that has been split (build/GEAP8P/asm)
: "${DECOMP_DIR:?set DECOMP_DIR to your decomp-toolkit project}"
cd "$DECOMP_DIR/build/GEAP8P/asm" || exit 1
F=$(grep -l "^.fn $1," *.s | head -1)
awk -v f="$1" '$0 ~ "^.fn "f"," {p=1} p {print} $0 ~ "^.endfn "f"$" {exit}' $F | sed -E 's#^/\* ([0-9A-F]+) [0-9A-F]+ +([0-9A-F]{2} ){4}\*/\t#\1  #'
