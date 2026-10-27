# Spleen 5x8

`spleen-5x8.bdf` is from Spleen 2.1.0 by Frederic Cambus
(https://github.com/fcambus/spleen), BSD 2-Clause (see `LICENSE`).
`system/lib/fonts/spleen-5x8.pfn` is built from it:

    python3 tools/mkfont.py sdk/fonts/spleen/spleen-5x8.bdf system/lib/fonts/spleen-5x8.pfn --range 0x20-0x7E

A 5x8 cell gives a 64x40 full-screen picocalc.chargrid (docs/API-Chargrid.md).
