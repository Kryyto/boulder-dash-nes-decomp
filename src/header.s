; iNES header and CHR ROM. Both are taken from the user's ROM by
; tools/disasm.py into assets/ (never committed).

.segment "HEADER"
    .incbin "../assets/header.bin"

.segment "CHR"
    .incbin "../assets/chr.bin"
