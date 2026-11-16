#ifndef HD_FMV_H
#define HD_FMV_H
/* Generic enhanced-FMV module (Supreme Warrior and later games); Sewer Shark
   stays in hd_sewer.c. libretro.c tries hd_sewer first, then hd_fmv. */
#include <stdint.h>
void hd_fmv_init(const char *system_dir_, const char *game_path_);
void hd_fmv_deinit(void);
void hd_fmv_set_disc_key(uint64_t key_);
const uint32_t *hd_fmv_apply(uint32_t *buf_, int width_, int height_, int frame_, int *ow_, int *oh_);
int  hd_fmv_scale(void);
int  hd_fmv_active(void);
void hd_fmv_set_opts(int cutscenes_, int gameplay_, int graphics_, int grain_);
#endif
