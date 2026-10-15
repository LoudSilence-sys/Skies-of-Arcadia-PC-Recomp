#!/bin/sh
# dis.sh fn_XXXXXXXX  -> clean disassembly of one function from dtk asm
cd /home/claude/decomp/soa/build/GEAP8P/asm
F=$(grep -l "^.fn $1," *.s | head -1)
awk -v f="$1" '$0 ~ "^.fn "f"," {p=1} p {print} $0 ~ "^.endfn "f"$" {exit}' $F | sed -E 's#^/\* ([0-9A-F]+) [0-9A-F]+ +([0-9A-F]{2} ){4}\*/\t#\1  #'
