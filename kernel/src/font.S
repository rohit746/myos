// The console font, a PSF2 file (see vendor/spleen/README), embedded in
// .rodata. console.c parses it at startup.

    .section .rodata
    .balign 16
    .global console_font, console_font_end
console_font:
    .incbin "spleen/spleen-8x16.psf"
console_font_end:
