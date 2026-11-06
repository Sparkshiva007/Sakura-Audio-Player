#include <stdint.h>

void flasher(uint32_t flash_addr, const uint32_t *ram_buf, uint32_t num_128) {
    volatile uint32_t *flc_addr   = (uint32_t *)0x40029000;
    volatile uint32_t *flc_clkdiv = (uint32_t *)0x40029004;
    volatile uint32_t *flc_ctrl   = (uint32_t *)0x40029008;
    volatile uint32_t *flc_data   = (uint32_t *)0x40029030;

    *flc_clkdiv = 100;
    *flc_ctrl = (*flc_ctrl & ~0xF0000000) | (2 << 28);

    for (uint32_t i = 0; i < num_128; i++) {
        while (*flc_ctrl & 1);

        *flc_addr = flash_addr + (i * 16);
        flc_data[0] = ram_buf[i * 4 + 0];
        flc_data[1] = ram_buf[i * 4 + 1];
        flc_data[2] = ram_buf[i * 4 + 2];
        flc_data[3] = ram_buf[i * 4 + 3];

        *flc_ctrl |= 1;
        while (*flc_ctrl & 1);
    }

    *flc_ctrl &= ~0xF0000000;
    __asm volatile("bkpt #0");
}
