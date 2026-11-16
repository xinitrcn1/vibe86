/* A flat image for vsim's flash: offset 0 is the image's first byte; the
   board maps the image so its last byte is at FFFF_FFFFh (the reset vector,
   FFFF_FFF0h, is 16 bytes before the end). */
OUTPUT_FORMAT(binary)
SECTIONS
{
	. = 0;
	.text : { *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) }
	/DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
