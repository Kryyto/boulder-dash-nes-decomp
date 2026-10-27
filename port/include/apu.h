/* NES APU emulation (pulse x2, triangle, noise) for bdview. */
#ifndef BD_APU_H
#define BD_APU_H

#include <stdint.h>

void apu_reset(int sample_rate);
void apu_write(uint16_t addr, uint8_t value);
/* Run the APU for cpu_cycles CPU cycles (29780.5 per NTSC frame) and write
 * the resulting signed 16-bit mono samples; returns how many were written. */
int apu_run(uint32_t cpu_cycles, int16_t *out, int max_samples);

#endif
