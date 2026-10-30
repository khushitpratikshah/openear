MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 1M }
ENTRY(bench_entry)
SECTIONS { .text : { *(.text*) *(.rodata*) } > RAM
 .data : { *(.data*) } > RAM
 .bss : { __bss_start__ = .; *(.bss*) *(COMMON) __bss_end__ = .; } > RAM
 end = .; _end = .; __exidx_start = .; __exidx_end = .; }
