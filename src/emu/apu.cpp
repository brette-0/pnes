#include <pnes/apu.hpp>

u8 apu::sq1_vol, apu::sq1_sweep, apu::sq1_lo, apu::sq1_hi;
u8 apu::sq2_vol, apu::sq2_sweep, apu::sq2_lo, apu::sq2_hi;
u8 apu::tri_linear, apu::tri_lo, apu::tri_hi;
u8 apu::noise_vol, apu::noise_lo, apu::noise_hi;
u8 apu::dmc_freq, apu::dmc_raw, apu::dmc_start, apu::dmc_len;
u8 apu::snd_chn, apu::frame_counter;

u8 APU_REGISTERS_snapshot[15];
