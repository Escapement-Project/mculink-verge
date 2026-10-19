/* CPU 1's image, linked on its own, in the flash CPU 0's link script reserves for it.
 *
 * It starts with CPU 1's vector table, so core1_image is what CPU 0 writes to CPBOOT. CORE1_BIN is
 * the path of the binary, from CMakeLists.txt; tools/image.py checks these bytes against CPU 1's ELF.
 */
    .section .core1_code, "a", %progbits
    .align 9
    .global core1_image
    .type core1_image, %object
core1_image:
    .incbin CORE1_BIN
    .size core1_image, . - core1_image

    .section .note.GNU-stack,"",%progbits
