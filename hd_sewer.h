#ifndef HD_SEWER_H
#define HD_SEWER_H
#include <stdint.h>
void hd_sewer_init(const char *system_dir_, const char *game_path_);
void hd_sewer_deinit(void);
void hd_sewer_set_disc_key(uint64_t key_);
const uint32_t *hd_sewer_apply(uint32_t *buf_, int width_, int height_, int frame_, int *ow_, int *oh_);
int  hd_sewer_scale(void);
int  hd_sewer_active(void);
void hd_sewer_aim(int frame_);
int  hd_sewer_turn_filter(int b_, int *u_, int *d_, int *l_, int *r_);
void hd_sewer_set_opts(int cutscenes_, int gameplay_, int graphics_, int grain_);
void hd_sewer_autosteer(int *b_, int *u_, int *d_, int *l_, int *r_);
void hd_sewer_set_autosteer(int on_);
int  hd_sewer_autoflare(void);
void hd_sewer_set_autoflare(int on_);
void hd_sewer_set_aimassist(int on_);
int  hd_sewer_autofire(void);
void hd_sewer_set_autofire(int on_);
void hd_sewer_set_infenergy(int on_);
void hd_sewer_log_input(int b_, int u_, int d_, int l_, int r_);
#endif
