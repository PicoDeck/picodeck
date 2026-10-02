/*
 * BTstack's static state in QMI PSRAM (CMakeLists.txt adds this script after
 * the SDK's memmap_default.ld).
 *
 * The .bss of BTstack, the SDK's BTstack glue (run loop, HCI transport and
 * its incoming packet buffer) and the CYW43's BT shared-bus driver is ~720
 * bytes of SRAM, most of the ~900 bytes the link had to spare over the 2 KB
 * heap reserve. Here it goes into the low 2 MB of the QMI PSRAM, which
 * nothing uses (the umm heap starts at 0x11200000; the boot self-test only
 * touches 0x3E0-0x4E0), through the cached alias like the heap.
 *
 * NOLOAD and outside .bss: crt0 does not zero it, and the PSRAM is not up
 * when crt0 runs anyway. bt_pad.c zeroes it (__bt_psram_bss_start__ ..
 * __bt_psram_bss_end__) before the first BTstack call; nothing reads it
 * before then (the driver reaches the transport only once the BT firmware
 * is loaded, cyw43_t.bt_loaded).
 */
MEMORY
{
    BT_PSRAM (rw) : ORIGIN = 0x11100000, LENGTH = 64k
}

SECTIONS
{
    .bt_psram_bss (NOLOAD) :
    {
        . = ALIGN(8);
        __bt_psram_bss_start__ = .;
        *lib/btstack/*(.bss .bss.* COMMON)
        *pico_btstack/*(.bss .bss.* COMMON)
        *pico_cyw43_driver/btstack_*(.bss .bss.* COMMON)
        *pico_cyw43_driver/cybt_shared_bus/*(.bss .bss.* COMMON)
        . = ALIGN(8);
        __bt_psram_bss_end__ = .;
    } > BT_PSRAM
}
INSERT BEFORE .bss;
