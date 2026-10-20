#include <pnes/apu.hpp>

#ifdef TARGET_NES

tech::wo_register<0x4000> apu::sq1_vol;
tech::wo_register<0x4001> apu::sq1_sweep;
tech::wo_register<0x4002> apu::sq1_lo;
tech::wo_register<0x4003> apu::sq1_hi;
tech::wo_register<0x4004> apu::sq2_vol;
tech::wo_register<0x4005> apu::sq2_sweep;
tech::wo_register<0x4006> apu::sq2_lo;
tech::wo_register<0x4007> apu::sq2_hi;
tech::wo_register<0x4008> apu::tri_linear;
tech::wo_register<0x400A> apu::tri_lo;
tech::wo_register<0x400B> apu::tri_hi;
tech::wo_register<0x400C> apu::noise_vol;
tech::wo_register<0x400E> apu::noise_lo;
tech::wo_register<0x400F> apu::noise_hi;
tech::wo_register<0x4010> apu::dmc_freq;
tech::wo_register<0x4011> apu::dmc_raw;
tech::wo_register<0x4012> apu::dmc_start;
tech::wo_register<0x4013> apu::dmc_len;
tech::wo_register<0x4015> apu::snd_chn;
tech::wo_register<0x4017> apu::frame_counter;

u8 APU_REGISTERS_snapshot[15];

#endif
