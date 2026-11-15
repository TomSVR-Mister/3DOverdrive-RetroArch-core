/*
  Sewer Shark HD: replaces the game's FMV with pre-made 2x (AI-cleaned) frames.

  The game decodes its video with the CPU straight into VRAM; cockpit, crosshair
  and other overlays are drawn by MADAM (cels). libopera keeps a per-pixel
  "drawn by MADAM" mask (HD_SCRMASK). Each frame, the visible video window is
  fingerprinted (12x8 grey thumbnail), matched against the pack index, and every
  pixel that was not drawn by MADAM is replaced by the matching 2x HD pixel.

  Pack layout (in the RetroArch system folder, or $SEWERSHARK_HD):
    SewerSharkHD/index.bin
    SewerSharkHD/<seq>/<track>/fNNNNN.png   (2x Topaz output, one per frame)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO_OVERRIDE
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"
#include "src/webp/decode.h"
#include <rthreads/rthreads.h>

/* load <base>.webp, else <base>.png, as RGBA; free() with stbi_image_free */
static uint8_t*
load_img(const char *base_, int *w_, int *h_)
{
  char p[1500];
  FILE *f;
  int n;
  snprintf(p,sizeof(p),"%s.webp",base_);
  f = fopen(p,"rb");
  if(f)
    {
      long sz; uint8_t *buf, *out = NULL; int w, h;
      fseek(f,0,SEEK_END); sz = ftell(f); fseek(f,0,SEEK_SET);
      buf = (uint8_t*)malloc(sz > 0 ? sz : 1);
      if(buf && fread(buf,1,sz,f) == (size_t)sz && WebPGetInfo(buf,sz,&w,&h))
        {
          out = (uint8_t*)malloc((size_t)w * h * 4);
          if(out && WebPDecodeRGBAInto(buf,sz,out,(size_t)w * h * 4,w * 4))
            { *w_ = w; *h_ = h; }
          else
            { free(out); out = NULL; }
        }
      free(buf);
      fclose(f);
      return out;
    }
  snprintf(p,sizeof(p),"%s.png",base_);
  return stbi_load(p,w_,h_,&n,4);
}

/* pack scale (2 or 4) and the 4x output buffer */
static int       SCALE = 2;
static uint32_t *OUT4 = NULL;

#include "hd_sewer.h"
#include "opera_lr_callbacks.h"
#include <libretro.h>

extern uint8_t *DRAM;

extern uint8_t HD_SCRMASK[];

#pragma pack(push,1)
typedef struct
{
  char     seq[8];
  char     trk[4];
  uint16_t j;
  uint16_t w,h;     /* native frame size */
  uint16_t cw,ch;   /* canvas the HD file was made on (frame centred) */
  uint8_t  th[96];  /* 12x8 grey thumbnail of the original frame */
} hd_ent_t;
#pragma pack(pop)

typedef struct { int x,y,w,h; } hd_win_t;

static hd_win_t const WINS[] =
  {
    { 40, 20, 240, 160 },   /* in-game tunnel view */
    { 16, 32, 288, 176 },   /* cut scenes */
    { 40, 40, 240, 160 },   /* briefing screens (framed) */
    { 48, 14, 224, 216 },   /* end credits */
    { 16, 12, 288, 216 },   /* (unverified) */
  };
#define NWIN ((int)(sizeof(WINS)/sizeof(WINS[0])))

static hd_ent_t *E      = NULL;
static int       N      = 0;
static char      ROOT[1024];
static int       ACTIVE = 0;

static float     LAST_TH[NWIN][96];
static int       LAST_M[NWIN];
static int       LAST_SEARCHED[NWIN];
static int       LAST_HIT_FRAME[NWIN];
static int       GOOD_M[NWIN] = {-1,-1,-1,-1,-1};   /* last frame shown enhanced, per window */
static int       LAST_FULL[NWIN];

/* small cache of decoded HD frames */
#define CACHE 4
static int      C_IDX[CACHE] = {-1,-1,-1,-1};
static uint8_t *C_PIX[CACHE];
static int      C_W[CACHE],C_H[CACHE];
static int      C_NEXT = 0;

static FILE *LOG = NULL;

/* user options (core options, see opera_lr_opts.c) */
static int OPT_CUT = 1, OPT_GAME = 1, OPT_GFX = 1, OPT_GRAIN = 0, OPT_AUTOSTEER = 0, OPT_AUTOFLARE = 0, OPT_AIMASSIST = 0, OPT_AUTOFIRE = 0;
static int OPT_INFENERGY = 0;
static int AA_X = 0, AA_Y = 0, AA_F = -100, AA_N = 0;
static int FLY_FRAME = -1000, FLY_WIN = 0;   /* last frame tunnel footage was on screen, and in which window */   /* aim-assist target (low-res screen) and frame found */
static int CUR_FRAME = 0;

static const uint8_t *hd_get(int i_, int *w_, int *h_);
static void pf_start(void);
void hd_sewer_deinit(void);

void
hd_sewer_set_autosteer(int on_)
{
  OPT_AUTOSTEER = on_;
}

void
hd_sewer_set_autoflare(int on_)
{
  OPT_AUTOFLARE = on_;
}

void
hd_sewer_set_aimassist(int on_)
{
  OPT_AIMASSIST = on_;
}

void
hd_sewer_set_infenergy(int on_)
{
  OPT_INFENERGY = on_;
}

void
hd_sewer_set_autofire(int on_)
{
  OPT_AUTOFIRE = on_;
}

void
hd_sewer_set_opts(int cutscenes_, int gameplay_, int graphics_, int grain_)
{
  OPT_CUT = cutscenes_; OPT_GAME = gameplay_; OPT_GFX = graphics_; OPT_GRAIN = grain_;
}

/* sequences that are gameplay (tunnel) video rather than cut scenes */
static const char *const GAMEPLAY_SEQ[] =
  { "05301521","10570911","13182311","15471911","18210111","19351111",
    "21041111","23181321","25110711","26332311","29551111","31531511", NULL };

static int
is_gameplay(int m_)
{
  int i;
  for(i = 0; GAMEPLAY_SEQ[i]; i++)
    if(!memcmp(E[m_].seq,GAMEPLAY_SEQ[i],8)) return 1;
  return 0;
}

/* film grain: luma-only, changes every frame, roughly triangular distribution */
static inline uint32_t
grain_hash(uint32_t x_)
{
  x_ ^= x_ >> 16; x_ *= 0x7feb352dU; x_ ^= x_ >> 15; x_ *= 0x846ca68bU; x_ ^= x_ >> 16;
  return x_;
}

static inline uint32_t
grain_px(uint32_t c_, int n_)
{
  int r = (int)((c_ >> 16) & 0xFF), g = (int)((c_ >> 8) & 0xFF), b = (int)(c_ & 0xFF);
  int v = r > g ? (r > b ? r : b) : (g > b ? g : b);
  if(v < 24) n_ = n_ * v / 24;              /* fade grain out towards black so blacks stay black */
  r += n_; g += n_; b += n_;
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static void art_load(void);
static const uint8_t *hd_get(int i_, int *w_, int *h_);
static void spr_load(void);

int
hd_sewer_active(void)
{
  return ACTIVE;
}

int
hd_sewer_scale(void)
{
  return ACTIVE ? SCALE : 2;
}

static int
ci_contains(const char *h_, const char *n_)
{
  size_t i,j,ln = strlen(n_);
  if(!h_) return 0;
  for(i = 0; h_[i]; i++)
    {
      for(j = 0; j < ln && h_[i+j]; j++)
        {
          char a = h_[i+j], b = n_[j];
          if(a >= 'A' && a <= 'Z') a += 32;
          if(a != b) break;
        }
      if(j == ln) return 1;
    }
  return 0;
}

static int
try_root(const char *root_)
{
  char path[1200];
  FILE *f;
  long sz;
  size_t n;

  snprintf(path,sizeof(path),"%s/index.bin",root_);
  f = fopen(path,"rb");
  if(!f)
    return 0;
  fseek(f,0,SEEK_END); sz = ftell(f); fseek(f,0,SEEK_SET);
  hd_sewer_deinit();          /* the decoder thread reads E */
  free(E);
  E = (hd_ent_t*)malloc(sz);
  N = (int)(sz / sizeof(hd_ent_t));
  n = E ? fread(E,sizeof(hd_ent_t),N,f) : 0;
  fclose(f);
  if(N <= 0 || n != (size_t)N)
    { N = 0; return 0; }
  snprintf(ROOT,sizeof(ROOT),"%s",root_);
  return 1;
}

static void
chomp(char *s_)
{
  size_t l = strlen(s_);
  while(l && (s_[l-1] == '\n' || s_[l-1] == '\r' || s_[l-1] == ' ' ||
              s_[l-1] == '"' || s_[l-1] == '/' || s_[l-1] == '\\'))
    s_[--l] = 0;
  if(s_[0] == '"') memmove(s_,s_+1,l);
}

void
hd_sewer_init(const char *system_dir_, const char *game_path_)
{
  char path[1200];
  const char *env = getenv("SEWERSHARK_HD");
  int ok = 0;

  ACTIVE = 0;
  if(!ci_contains(game_path_,"sewer") && !env)
    return;

  /* 1: environment variable (testing) */
  if(env)
    ok = try_root(env);
  /* 2: <system>/SewerSharkHD.txt holding the folder path */
  if(!ok && system_dir_)
    {
      FILE *f;
      snprintf(path,sizeof(path),"%s/SewerSharkHD.txt",system_dir_);
      f = fopen(path,"r");
      if(f)
        {
          char line[1024] = {0};
          if(fgets(line,sizeof(line),f))
            { chomp(line); ok = try_root(line); }
          fclose(f);
        }
    }
  /* 3: <system>/SewerSharkHD */
  if(!ok && system_dir_)
    {
      snprintf(path,sizeof(path),"%s/SewerSharkHD",system_dir_);
      ok = try_root(path);
    }
  /* 4: SewerSharkHD next to the game */
  if(!ok && game_path_)
    {
      char *p;
      snprintf(path,sizeof(path),"%s",game_path_);
      p = strrchr(path,'/');
      { char *q = strrchr(path,'\\'); if(q > p) p = q; }
      if(p)
        {
          snprintf(p + 1,sizeof(path) - (p + 1 - path),"SewerSharkHD");
          ok = try_root(path);
        }
    }
  if(!ok)
    return;

  memset(LAST_TH,0,sizeof(LAST_TH));
  { int q; for(q = 0; q < NWIN; q++) { LAST_M[q] = -1; LAST_HIT_FRAME[q] = -1000; LAST_FULL[q] = -1000; LAST_SEARCHED[q] = 0; } }
  if(getenv("SEWERSHARK_HD_LOG"))
    LOG = fopen(getenv("SEWERSHARK_HD_LOG"),"w");
  art_load();
  spr_load();
  SCALE = 2;
  snprintf(path,sizeof(path),"%s/scale.txt",ROOT);   /* "4" for a 4x pack */
  {
    FILE *sf = fopen(path,"r");
    if(sf) { int v = 0; if(fscanf(sf,"%d",&v) == 1 && v == 4) SCALE = 4; fclose(sf); }
  }
  if(SCALE == 2 && N > 0)
    {
      int w, h;
      if(hd_get(0,&w,&h) && E[0].cw && w >= 4 * E[0].cw)
        SCALE = 4;
    }
  if(SCALE == 4 && !OUT4)
    OUT4 = (uint32_t*)malloc(1280 * 960 * 4);
  if(!OUT4) SCALE = 2;
  if(LOG) fprintf(LOG,"pack scale %d\n",SCALE);
  pf_start();
  ACTIVE = 1;
}

/*
  Background decoding: when frame i is shown, the next frames of the same
  shot are read and decoded on a worker thread, so the game never waits on
  the drive or the WebP decoder.
*/
#define PF 6
static struct { int idx; uint8_t *pix; int w, h; } PFC[PF];
static int        PF_NEXT = 0;
static int        PF_Q[4], PF_QN = 0;
static slock_t   *PF_LOCK = NULL;
static scond_t   *PF_COND = NULL;
static sthread_t *PF_TH   = NULL;
static int        PF_QUIT = 0;

static void
hd_path(int i_, char *path_, size_t n_)
{
  snprintf(path_,n_,"%s/%.8s/%.3s/f%05u",ROOT,E[i_].seq,E[i_].trk,(unsigned)E[i_].j);
}

static void
pf_worker(void *arg_)
{
  (void)arg_;
  for(;;)
    {
      int i, k, have = 0, w = 0, h = 0;
      char path[1400];
      uint8_t *p;

      slock_lock(PF_LOCK);
      while(!PF_QUIT && PF_QN == 0)
        scond_wait(PF_COND,PF_LOCK);
      if(PF_QUIT)
        { slock_unlock(PF_LOCK); return; }
      i = PF_Q[0];
      memmove(PF_Q,PF_Q + 1,sizeof(int) * --PF_QN);
      for(k = 0; k < PF; k++)
        have |= PFC[k].idx == i;
      slock_unlock(PF_LOCK);
      if(have)
        continue;

      hd_path(i,path,sizeof(path));
      p = load_img(path,&w,&h);
      if(!p)
        continue;
      slock_lock(PF_LOCK);
      k = PF_NEXT; PF_NEXT = (PF_NEXT + 1) % PF;
      free(PFC[k].pix);
      PFC[k].pix = p; PFC[k].idx = i; PFC[k].w = w; PFC[k].h = h;
      slock_unlock(PF_LOCK);
    }
}

static void
pf_start(void)
{
  int k;
  if(PF_TH)
    return;
  for(k = 0; k < PF; k++)
    { PFC[k].idx = -1; PFC[k].pix = NULL; }
  PF_LOCK = slock_new();
  PF_COND = scond_new();
  PF_QUIT = 0;
  if(PF_LOCK && PF_COND)
    PF_TH = sthread_create(pf_worker,NULL);
}

void
hd_sewer_deinit(void)
{
  int k;
  if(!PF_TH)
    return;
  slock_lock(PF_LOCK);
  PF_QUIT = 1;
  scond_signal(PF_COND);
  slock_unlock(PF_LOCK);
  sthread_join(PF_TH);
  PF_TH = NULL;
  for(k = 0; k < PF; k++)
    { free(PFC[k].pix); PFC[k].pix = NULL; PFC[k].idx = -1; }
  scond_free(PF_COND); PF_COND = NULL;
  slock_free(PF_LOCK); PF_LOCK = NULL;
}

/* ask the worker for the frames after i_ in the same shot (newest first) */
static void
pf_request(int i_)
{
  int d;
  if(!PF_TH)
    return;
  slock_lock(PF_LOCK);
  PF_QN = 0;
  for(d = 1; d <= 3 && i_ + d < N; d++)
    {
      int k, have = 0;
      if(memcmp(E[i_ + d].seq,E[i_].seq,8) || memcmp(E[i_ + d].trk,E[i_].trk,4))
        break;
      for(k = 0; k < PF; k++)
        have |= PFC[k].idx == i_ + d;
      if(!have)
        PF_Q[PF_QN++] = i_ + d;
    }
  if(PF_QN)
    scond_signal(PF_COND);
  slock_unlock(PF_LOCK);
}

static const uint8_t*
hd_get(int i_, int *w_, int *h_)
{
  int k,w = 0,h = 0;
  char path[1400];
  uint8_t *p = NULL;

  for(k = 0; k < CACHE; k++)
    if(C_IDX[k] == i_)
      {
        *w_ = C_W[k]; *h_ = C_H[k];
        return C_PIX[k];
      }

  if(PF_TH)
    {
      slock_lock(PF_LOCK);
      for(k = 0; k < PF; k++)
        if(PFC[k].idx == i_)
          {
            p = PFC[k].pix; w = PFC[k].w; h = PFC[k].h;
            PFC[k].pix = NULL; PFC[k].idx = -1;
            break;
          }
      slock_unlock(PF_LOCK);
    }
  if(!p)
    {
      hd_path(i_,path,sizeof(path));
      p = load_img(path,&w,&h);
    }
  pf_request(i_);
  if(!p)
    return NULL;
  k = C_NEXT; C_NEXT = (C_NEXT + 1) % CACHE;
  free(C_PIX[k]);
  C_PIX[k] = p; C_IDX[k] = i_; C_W[k] = w; C_H[k] = h;
  *w_ = w; *h_ = h;
  return p;
}

static void
thumb(const uint32_t *buf_, int pitch_, hd_win_t const *w_, float *th_, float *wt_)
{
  int by,bx,y,x;
  int bw = w_->w / 12, bh = w_->h / 8;
  for(by = 0; by < 8; by++)
    for(bx = 0; bx < 12; bx++)
      {
        uint32_t s = 0, n = 0;
        for(y = 0; y < bh; y++)
          {
            int sy = w_->y + by*bh + y;
            const uint32_t *row = &buf_[(size_t)(sy * 2) * pitch_];
            const uint8_t  *mrow = &HD_SCRMASK[sy * 384];
            for(x = 0; x < bw; x++)
              {
                int sx = w_->x + bx*bw + x;
                uint32_t p;
                if(mrow[sx]) continue;
                p = row[sx * 2];
                /* the emulator expands 5-bit colour as v<<3; the pack uses (v<<3)|(v>>2) */
                p |= (p >> 5) & 0x070707;
                s += ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF);
                n++;
              }
          }
        th_[by*12 + bx] = n ? (float)s / (float)(3 * n) : 0.0f;
        wt_[by*12 + bx] = (n == (uint32_t)(bw * bh)) ? 1.0f : 0.0f;
      }
}

static float
dist(const float *a_, const float *w_, const uint8_t *b_)
{
  int i;
  float s = 0, n = 0;
  for(i = 0; i < 96; i++)
    {
      float d = a_[i] - (float)b_[i];
      s += w_[i] * d*d;
      n += w_[i];
    }
  return n > 0 ? sqrtf(s / n) : 1e9f;
}

static int
same_track(int a_, int b_)
{
  return (a_ >= 0 && b_ >= 0 && b_ < N &&
          !memcmp(E[a_].seq,E[b_].seq,8) && !memcmp(E[a_].trk,E[b_].trk,4));
}

static float
dist_cut(const float *a_, const float *w_, float n_, const uint8_t *b_, float cut_)
{
  int i;
  float s = 0, lim = cut_ * cut_ * n_;
  for(i = 0; i < 96; i++)
    {
      float d = a_[i] - (float)b_[i];
      s += w_[i] * d*d;
      if(s > lim) return 1e9f;
    }
  return sqrtf(s / n_);
}

static int
search(hd_win_t const *w_, const float *th_, const float *wt_, int prev_, int full_, float *best_d_)
{
  int i, best = -1, c, cb = -1;
  float bd = 1e9f, cd = 1e9f, n = 0;
  for(i = 0; i < 96; i++) n += wt_[i];

  /* first try simply continuing the clip that was playing */
  if(prev_ >= 0)
    for(c = prev_; c <= prev_ + 3; c++)
      {
        float d;
        if(!same_track(prev_,c)) continue;
        d = dist(th_,wt_,E[c].th);
        if(d < cd) { cd = d; cb = c; }
      }
  if(cb >= 0 && cd < 1.0f)
    { *best_d_ = cd; return cb; }
  if(!full_)
    { *best_d_ = cd; return (cd < 3.0f) ? cb : -1; }

  for(i = 0; i < N; i++)
    {
      float d;
      if(E[i].w != w_->w || E[i].h != w_->h)
        continue;
      d = dist_cut(th_,wt_,n,E[i].th,bd);
      if(d < bd) { bd = d; best = i; }
    }
  if(cb >= 0 && cd <= bd + 0.25f)
    { best = cb; bd = cd; }
  *best_d_ = bd;
  return best;
}


/* ---- HD replacements for the game's fixed artwork (cockpit, frames, logos) ----
   <pack>/art/art.txt lists "name x y"; <pack>/art/<name>_lo.png is the original
   art (RGBA, as stored on the disc) and <name>_hd.png the 2x version. Every
   frame, an item counts as on screen when most of its opaque pixels match the
   original exactly; then each matching pixel becomes the HD 2x2 block, and
   anything drawn over the art (digits, gauges, crosshair) stays untouched. */
#define MAXART 32
typedef struct { int x,y,w,h,f,pause; uint8_t *lo,*hd; } art_t;   /* f: hd scale 2 or 4 */
static art_t ART[MAXART];
static int   NART = 0;

static void
art_load(void)
{
  char path[1400], name[256];
  FILE *f;
  int x,y,i,n,w2,h2;
  for(i = 0; i < NART; i++) { stbi_image_free(ART[i].lo); stbi_image_free(ART[i].hd); }
  NART = 0;
  snprintf(path,sizeof(path),"%s/art/art.txt",ROOT);
  f = fopen(path,"r");
  if(!f) return;
  while(NART < MAXART && fscanf(f,"%255s %d %d",name,&x,&y) == 3)
    {
      art_t *a = &ART[NART];
      snprintf(path,sizeof(path),"%s/art/%s_lo",ROOT,name);
      a->lo = load_img(path,&a->w,&a->h); (void)n;
      snprintf(path,sizeof(path),"%s/art/%s_hd",ROOT,name);
      a->hd = load_img(path,&w2,&h2);
      a->f = (a->lo && w2 == 4*a->w && h2 == 4*a->h) ? 4 : 2;
      if(!a->lo || !a->hd || w2 != a->f*a->w || h2 != a->f*a->h ||
         x < 0 || y < 0 || x + a->w > 320 || y + a->h > 240)
        {
          if(a->lo) stbi_image_free(a->lo);
          if(a->hd) stbi_image_free(a->hd);
          continue;
        }
      a->x = x; a->y = y;
      a->pause = !strcmp(name,"paused_sign");
      NART++;
    }
  fclose(f);
}

static inline int
iabs(int v_) { return v_ < 0 ? -v_ : v_; }

/* does screen pixel scr_ look like art pixel lo_ dimmed by gains g_ (x256)? */
static inline int
art_match(uint32_t scr_, const uint8_t *lo_, const int *g_, int exact_)
{
  int r = (scr_ >> 16) & 0xFF, gg = (scr_ >> 8) & 0xFF, b = scr_ & 0xFF;
  if(exact_)   /* screen colours are v<<3, art is (v<<3)|(v>>2): compare 5-bit values */
    return ((r >> 3) == (lo_[0] >> 3) && (gg >> 3) == (lo_[1] >> 3) && (b >> 3) == (lo_[2] >> 3));
  return (iabs(r  - ((lo_[0] * g_[0]) >> 8)) <= 12 &&
          iabs(gg - ((lo_[1] * g_[1]) >> 8)) <= 12 &&
          iabs(b  - ((lo_[2] * g_[2]) >> 8)) <= 12);
}

static inline void
blend(uint32_t *d_, const uint8_t *s_, const int *g_)
{
  int a = s_[3], r, gg, b;
  uint32_t p = *d_;
  if(!a) return;
  r  = (s_[0] * g_[0]) >> 8; if(r  > 255) r  = 255;
  gg = (s_[1] * g_[1]) >> 8; if(gg > 255) gg = 255;
  b  = (s_[2] * g_[2]) >> 8; if(b  > 255) b  = 255;
  if(a < 255)
    {
      r  = (r  * a + (int)((p >> 16) & 0xFF) * (255 - a)) / 255;
      gg = (gg * a + (int)((p >>  8) & 0xFF) * (255 - a)) / 255;
      b  = (b  * a + (int)( p        & 0xFF) * (255 - a)) / 255;
    }
  *d_ = ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
}

/* is art item a_ on screen? fills g_ (per channel gain, 256 = full) */
static int
art_test(const art_t *a, const uint32_t *buf_, int *g, int *exact_)
{
  int x,y,c;
      int64_t ss[3] = {0,0,0}, sl[3] = {0,0,0};
      int tot = 0, hit = 0, exact;

      /* estimate the brightness of each colour channel (fades) on a sparse grid */
      for(y = 0; y < a->h; y += 3)
        for(x = 0; x < a->w; x += 3)
          {
            const uint8_t *lo = &a->lo[(y * a->w + x) * 4];
            uint32_t p;
            if(lo[3] < 128) continue;
            p = buf_[(size_t)((a->y + y) * 2) * 640 + (a->x + x) * 2];
            ss[0] += (p >> 16) & 0xFF; ss[1] += (p >> 8) & 0xFF; ss[2] += p & 0xFF;
            sl[0] += lo[0]; sl[1] += lo[1]; sl[2] += lo[2];
          }
      for(c = 0; c < 3; c++)
        {
          g[c] = sl[c] > 0 ? (int)((ss[c] * 256) / sl[c]) : 256;
          if(g[c] > 272) g[c] = 272;
        }
      /* very dark: black screens would "match" dark art, so require some light */
      if(g[0] < 64 || g[1] < 64 || g[2] < 64)
        return 0;
      /* close to full brightness: sprites over the art skew the estimate, so snap */
      if(iabs(g[0] - 256) <= 24 && iabs(g[1] - 256) <= 24 && iabs(g[2] - 256) <= 24)
        g[0] = g[1] = g[2] = 256;
      exact = 0;

      for(y = 0; y < a->h; y += 3)
        for(x = 0; x < a->w; x += 3)
          {
            const uint8_t *lo = &a->lo[(y * a->w + x) * 4];
            if(lo[3] < 128) continue;
            /* only pixels bright enough to tell art from black count */
            if(((lo[0] * g[0]) >> 8) < 40 && ((lo[1] * g[1]) >> 8) < 40 && ((lo[2] * g[2]) >> 8) < 40) continue;
            tot++;
            hit += art_match(buf_[(size_t)((a->y + y) * 2) * 640 + (a->x + x) * 2],lo,g,exact);
          }
      if(tot < 8 || hit * 3 < tot * 2)
        return 0;
      *exact_ = exact;
      return 1;
}

static void
art_apply(const uint32_t *buf_, uint32_t *out_, int F_)
{
  int i,x,y;
  for(i = 0; i < NART; i++)
    {
      art_t *a = &ART[i];
      int g[3], exact;
      int one[3] = {256,256,256};
      if(!art_test(a,buf_,g,&exact))
        continue;
      if(LOG) fprintf(LOG,"art %d g=%d,%d,%d exact=%d\n",i,g[0],g[1],g[2],exact);
      if(exact) memcpy(g,one,sizeof(g));

      {
        const int OW = 320 * F_, AF = a->f, HW = a->w * a->f;
        for(y = 0; y < a->h; y++)
          {
            const uint8_t *lo = &a->lo[(size_t)y * a->w * 4];
            const uint32_t *b0 = &buf_[(size_t)((a->y + y) * 2) * 640 + a->x * 2];
            for(x = 0; x < a->w; x++, lo += 4)
              {
                int i2, j2;
                if(lo[3] >= 128)
                  {
                    /* art pixel: replace only where nothing was drawn over it */
                    if(!art_match(b0[2*x],lo,g,exact)) continue;
                  }
                else
                  {
                    int any = 0;
                    for(i2 = 0; i2 < AF && !any; i2++)
                      for(j2 = 0; j2 < AF; j2++)
                        if(a->hd[((size_t)(y * AF + i2) * HW + x * AF + j2) * 4 + 3]) { any = 1; break; }
                    if(!any) continue;   /* fully transparent: leave video/background alone */
                  }
                /* (soft HD edges are blended over whatever is underneath) */
                for(i2 = 0; i2 < F_; i2++)
                  {
                    uint32_t *d = &out_[(size_t)((a->y + y) * F_ + i2) * OW + (a->x + x) * F_];
                    const uint8_t *srow = &a->hd[(size_t)((y * F_ + i2) * AF / F_) * HW * 4];
                    for(j2 = 0; j2 < F_; j2++)
                      blend(&d[j2],&srow[(size_t)(((x * F_ + j2) * AF) / F_) * 4],g);
                  }
              }
          }
      }
    }
}


/* ---- HD sprites: <pack>/sprites/sprites.txt
   "<first 16 bytes hex> <length> <fnv1a32> <w> <h> <file>" per cel image; the
   file is the HD (2x) RGBA version. Cels are recognised by their pixel data. */
#include "libopera/opera_mem.h"

typedef struct { uint8_t pre[16]; uint32_t len, fnv; int w, h; uint8_t *img; } spr_t;
static spr_t *SPR = NULL;
static int    NSPR = 0;

typedef const uint8_t *(*opera_hd_lookup_fn)(uint32_t, uint32_t, uint32_t, uint32_t, int*, int*);
extern opera_hd_lookup_fn opera_hd_cel_lookup;

#define SCACHE 512
static struct { uint32_t addr, sig; int idx; } SC[SCACHE];

static uint32_t
mem_sig(uint32_t a_, uint32_t n_)
{
  uint32_t h = 2166136261u, i;
  for(i = 0; i < n_; i += 4)
    {
      uint32_t w = opera_mem_read32(a_ + i);
      int k;
      for(k = 3; k >= 0; k--)
        {
          if(i + (3 - k) >= n_) break;
          h = (h ^ ((w >> (k * 8)) & 0xFF)) * 16777619u;
        }
    }
  return h;
}

static const uint8_t*
hd_cel_lookup(uint32_t pdata_, uint32_t pre0_, uint32_t pre1_, uint32_t flags_, int *w_, int *h_)
{
  uint8_t pre[16];
  uint32_t sig, slot;
  int i, k, found = -1;

  /* an unpacked 16-bit cel exactly the size of a video window: a copy of a video frame */
  if(!(flags_ & 0x200) && (pre0_ & 7) == 6)
    {
      int w = (pre1_ & 0x7FF) + 1, h = ((pre0_ >> 6) & 0x3FF) + 1;
      for(k = 0; k < NWIN; k++)
        if(WINS[k].w == w && WINS[k].h == h)
          return (const uint8_t*)1;
    }
  if(!NSPR || !OPT_GFX)
    return NULL;

  for(i = 0; i < 16; i += 4)
    {
      uint32_t w = opera_mem_read32(pdata_ + i);
      pre[i] = w >> 24; pre[i+1] = w >> 16; pre[i+2] = w >> 8; pre[i+3] = w;
    }
  sig  = mem_sig(pdata_,64);
  slot = (pdata_ >> 2) % SCACHE;
  if(SC[slot].addr == pdata_ && SC[slot].sig == sig)
    found = SC[slot].idx;
  else
    {
      for(i = 0; i < NSPR; i++)
        if(!memcmp(SPR[i].pre,pre,16) && mem_sig(pdata_,SPR[i].len) == SPR[i].fnv)
          { found = i; break; }
      SC[slot].addr = pdata_; SC[slot].sig = sig; SC[slot].idx = found;
    }
  if(found < 0)
    {
      if(LOG) fprintf(LOG,"cel? pdata=%06x flags=%08x pre0=%08x pre1=%08x pre=%02x%02x%02x%02x%02x%02x%02x%02x\n",pdata_,flags_,pre0_,pre1_,pre[0],pre[1],pre[2],pre[3],pre[4],pre[5],pre[6],pre[7]);
      return NULL;
    }
  *w_ = SPR[found].w * 2;
  *h_ = SPR[found].h * 2;
  return SPR[found].img;
}

static void
spr_load(void)
{
  char path[1400], hex[64], file[256];
  unsigned len, fnv;
  int w, h, i, n, w2, h2;
  FILE *f;

  for(i = 0; i < NSPR; i++) stbi_image_free(SPR[i].img);
  free(SPR); SPR = NULL; NSPR = 0;
  memset(SC,0xFF,sizeof(SC));
  opera_hd_cel_lookup = hd_cel_lookup;   /* also handles video-frame copies */

  snprintf(path,sizeof(path),"%s/sprites/sprites.txt",ROOT);
  f = fopen(path,"r");
  if(!f) return;
  SPR = (spr_t*)calloc(1024,sizeof(spr_t));
  while(NSPR < 1024 && fscanf(f,"%63s %u %x %d %d %255s",hex,&len,&fnv,&w,&h,file) == 6)
    {
      spr_t *s = &SPR[NSPR];
      if(strlen(hex) != 32) continue;
      for(i = 0; i < 16; i++) { unsigned b; sscanf(hex + 2*i,"%2x",&b); s->pre[i] = b; }
      snprintf(path,sizeof(path),"%s/sprites/%s",ROOT,file);
      s->img = stbi_load(path,&w2,&h2,&n,4);
      if(!s->img || w2 != 2*w || h2 != 2*h) { if(s->img) stbi_image_free(s->img); continue; }
      s->len = len; s->fnv = fnv; s->w = w; s->h = h;
      NSPR++;
    }
  fclose(f);
}

/* fit  screen = a*hd + b  per channel between the on-screen (tinted) low-res
   frame and HD frame m_; robust: refit on the best 60% of samples (the
   PAUSED sign, crosshair etc. are outliers). Returns median error or 1e9. */
static double
pause_fit(const uint32_t *buf_, int F_, const hd_win_t *w_, int m_, double *A, double *B)
{
  enum { MAXS = 4096 };
  static float P[MAXS][3], H[MAXS][3], R[MAXS];
  int hw, hh, ox, oy, x, y, c, n = 0, pass, i;
  const uint8_t *hd = hd_get(m_,&hw,&hh);
  if(!hd) return 1e9;
  ox = (E[m_].cw - E[m_].w) * F_ / 2;
  oy = (E[m_].ch - E[m_].h) * F_ / 2;
  if(hw < ox + F_*w_->w || hh < oy + F_*w_->h) return 1e9;
  for(y = 1; y < w_->h && n < MAXS; y += 4)
    for(x = 1; x < w_->w && n < MAXS; x += 4)
      {
        uint32_t p = buf_[(size_t)((w_->y + y) * 2) * 640 + (w_->x + x) * 2];
        const uint8_t *h = &hd[((size_t)(oy + F_*y) * hw + ox + F_*x) * 4];
        for(c = 0; c < 3; c++) { P[n][c] = (p >> (16 - 8*c)) & 0xFF; H[n][c] = h[c]; }
        n++;
      }
  if(n < 100) return 1e9;
  for(pass = 0; pass < 2; pass++)
    {
      float thr = 1e9f;
      if(pass)
        {
          /* keep the best 60% */
          static float S[MAXS];
          memcpy(S,R,n*sizeof(float));
          for(i = 0; i < n; i++) ; 
          {
            /* nth_element by simple selection on a copy */
            int k = n * 6 / 10, lo = 0, hi = n - 1;
            while(lo < hi)
              {
                float piv = S[(lo + hi) / 2]; int a = lo, b = hi;
                while(a <= b) { while(S[a] < piv) a++; while(S[b] > piv) b--; if(a <= b) { float t = S[a]; S[a] = S[b]; S[b] = t; a++; b--; } }
                if(k <= b) hi = b; else if(k >= a) lo = a; else break;
              }
            thr = S[k];
          }
        }
      for(c = 0; c < 3; c++)
        {
          double sh = 0, sp = 0, shh = 0, shp = 0; int m = 0;
          for(i = 0; i < n; i++)
            if(R[i] <= thr || !pass)
              { sh += H[i][c]; sp += P[i][c]; shh += (double)H[i][c]*H[i][c]; shp += (double)H[i][c]*P[i][c]; m++; }
          if(m < 50) return 1e9;
          {
            double var = shh/m - (sh/m)*(sh/m), cov = shp/m - (sh/m)*(sp/m);
            A[c] = var > 1 ? cov / var : 1.0;
            if(A[c] < 0.2) A[c] = 0.2;
            if(A[c] > 1.5) A[c] = 1.5;
            B[c] = sp/m - A[c] * sh/m;
          }
        }
      for(i = 0; i < n; i++)
        {
          float e = 0;
          for(c = 0; c < 3; c++) e += fabsf(P[i][c] - (float)(A[c] * H[i][c] + B[c]));
          R[i] = e / 3;
        }
    }
  {
    /* median residual */
    static float S[MAXS]; int k = n / 2, lo = 0, hi = n - 1;
    memcpy(S,R,n*sizeof(float));
    while(lo < hi)
      {
        float piv = S[(lo + hi) / 2]; int a = lo, b = hi;
        while(a <= b) { while(S[a] < piv) a++; while(S[b] > piv) b--; if(a <= b) { float t = S[a]; S[a] = S[b]; S[b] = t; a++; b--; } }
        if(k <= b) hi = b; else if(k >= a) lo = a; else break;
      }
    return S[k];
  }
}

/* redraw the frozen video of window w_ in HD with the game's pause tint.
   m_ is the last frame shown enhanced; the frozen frame may be a neighbour. */
static void
pause_fill(const uint32_t *buf_, uint32_t *out_, int F_, int OW_, const hd_win_t *w_, int m_)
{
  static int cm = -1, cw = -1; static double cA[3], cB[3]; static uint32_t csum = 0;
  int hw, hh, ox, oy, x, y, c, i2, d, best = -1;
  double A[3], B[3], be = 1e9;
  const uint8_t *hd;
  uint32_t sum = 0;

  /* the frozen screen does not change: only refit when it does */
  for(y = 0; y < w_->h; y += 7)
    for(x = 0; x < w_->w; x += 7)
      sum = sum * 31 + buf_[(size_t)((w_->y + y) * 2) * 640 + (w_->x + x) * 2];
  if(sum == csum && cw == (int)(w_ - WINS) && cm >= 0)
    { best = cm; memcpy(A,cA,sizeof(A)); memcpy(B,cB,sizeof(B)); be = 0; }
  else
    {
      for(d = -2; d <= 3; d++)
        {
          int m = m_ + d; double a2[3], b2[3], e;
          if(m < 0 || m >= N || memcmp(E[m].seq,E[m_].seq,8) || memcmp(E[m].trk,E[m_].trk,3)) continue;
          e = pause_fit(buf_,F_,w_,m,a2,b2);
          if(e < be) { be = e; best = m; memcpy(A,a2,sizeof(A)); memcpy(B,b2,sizeof(B)); }
        }
      if(LOG) fprintf(LOG,"pause fit m=%d a=%.2f,%.2f,%.2f b=%.0f,%.0f,%.0f med=%.1f\n",best,A[0],A[1],A[2],B[0],B[1],B[2],be);
      csum = sum; cw = (int)(w_ - WINS);
      cm = (be <= 8.0) ? best : -1;
      memcpy(cA,A,sizeof(A)); memcpy(cB,B,sizeof(B));
      if(cm < 0) return;
    }
  if(best < 0) return;

  hd = hd_get(best,&hw,&hh);
  if(!hd) return;
  ox = (E[best].cw - E[best].w) * F_ / 2;
  oy = (E[best].ch - E[best].h) * F_ / 2;
  {
    /* keep what the game drew over the video (sign, crosshair), unless it
       tinted the whole picture, in which case everything is "drawn over" */
    int masked = 0, all;
    for(y = 0; y < w_->h; y++)
      for(x = 0; x < w_->w; x++)
        masked += HD_SCRMASK[(w_->y + y) * 384 + w_->x + x] != 0;
    all = masked * 10 > w_->w * w_->h * 9;
    if(LOG && be != 0) fprintf(LOG,"pause masked %d/%d all=%d\n",masked,w_->w*w_->h,all);
  for(y = 0; y < w_->h; y++)
    for(i2 = 0; i2 < F_; i2++)
      {
        uint32_t *dd = &out_[(size_t)((w_->y + y) * F_ + i2) * OW_ + w_->x * F_];
        const uint8_t *h = &hd[((size_t)(oy + F_*y + i2) * hw + ox) * 4];
        const uint8_t *mrow = &HD_SCRMASK[(w_->y + y) * 384 + w_->x];
        for(x = 0; x < w_->w * F_; x++, h += 4)
          {
            uint32_t o = 0;
            if(!all && mrow[x / F_]) continue;
            for(c = 0; c < 3; c++)
              {
                int v = (int)(A[c] * h[c] + B[c] + 0.5);
                v = v < 0 ? 0 : v > 255 ? 255 : v;
                o |= (uint32_t)v << (16 - 8*c);
              }
            dd[x] = o;
          }
      }
  }
}

/*
  Aim assist, part 1: the game keeps the creature filmed into the current
  shot in one slot: centre x,y (16.16) at 0xA0900/0xA0904 and hit radius at
  0xA0908 (0 = none), in video coordinates. The crosshair hits it at
  (x+30, y-24). Result in AA_X/AA_Y (crosshair coordinates).
*/
static int
aa_ram(int frame_)
{
  int rad = *(const int32_t*)&DRAM[0xA0908] >> 16;
  if(rad <= 0 || rad > 80)
    return 0;
  AA_X = (*(const int32_t*)&DRAM[0xA0900] >> 16) + 30;
  AA_Y = (*(const int32_t*)&DRAM[0xA0904] >> 16) - 24;
  AA_N = rad;
  AA_F = frame_;
  return 1;
}

const uint32_t*
hd_sewer_apply(uint32_t *buf_, int width_, int height_, int frame_, int *ow_, int *oh_)
{
  int k, F = 2, OW = 640;
  uint32_t *out = buf_;
  *ow_ = width_; *oh_ = height_;
  if(!ACTIVE || width_ != 640 || height_ != 480)
    return buf_;
  if(SCALE == 4)
    {
      /* 4x pack: present a 1280x960 frame, the emulated picture doubled */
      int y, x;
      F = 4; OW = 1280; out = OUT4;
      for(y = 0; y < 480; y++)
        {
          const uint32_t *s = &buf_[(size_t)y * 640];
          uint32_t *d0 = &OUT4[(size_t)(2 * y) * 1280], *d1 = d0 + 1280;
          for(x = 0; x < 640; x++) { uint32_t c = s[x]; d0[2*x] = d0[2*x+1] = d1[2*x] = d1[2*x+1] = c; }
        }
      *ow_ = 1280; *oh_ = 960;
    }

  for(k = 0; k < NWIN; k++)
    {
      hd_win_t const *w = &WINS[k];
      float th[96], wt[96], mean = 0, md = 0, d = 0, wsum = 0;
      int i, m, x, y, hw, hh, ox, oy;
      const uint8_t *hd;

      thumb(buf_,640,w,th,wt);
      for(i = 0; i < 96; i++)
        {
          float t = wt[i] * fabsf(th[i] - LAST_TH[k][i]);
          mean += wt[i] * th[i];
          wsum += wt[i];
          if(t > md) md = t;
        }
      mean = wsum > 0 ? mean / wsum : 0;
      if(wsum < 24.0f || mean < 3.0f)          /* black or nearly black: nothing to gain */
        {
          LAST_M[k] = -1;
          LAST_SEARCHED[k] = 0;
          memcpy(LAST_TH[k],th,sizeof(th));
          continue;
        }

      if(md < 0.5f && LAST_SEARCHED[k])
        m = LAST_M[k];
      else
        {
          int other = 0, full, q;
          for(q = 0; q < NWIN; q++)
            if(q != k && frame_ - LAST_HIT_FRAME[q] < 8) other = 1;
          /* while another window is showing video, look here only now and then */
          full = !other || (frame_ - LAST_FULL[k] >= 30);
          if(full) LAST_FULL[k] = frame_;
          m = search(w,th,wt,LAST_M[k],full,&d);
          LAST_SEARCHED[k] = 1;
          if(d > 4.0f || (mean < 12.0f && d > 1.5f))
            m = -1;
          LAST_M[k] = m;
          memcpy(LAST_TH[k],th,sizeof(th));
          if(LOG)
            {
              if(m >= 0)
                fprintf(LOG,"%d %d %.8s %.3s %u %.2f\n",frame_,k,E[m].seq,E[m].trk,(unsigned)E[m].j,d);
              else
                fprintf(LOG,"%d %d - - - %.2f\n",frame_,k,d);
            }
        }
      if(m < 0)
        continue;
      LAST_HIT_FRAME[k] = frame_;
      if(k == 0 || is_gameplay(m)) { FLY_FRAME = frame_; FLY_WIN = k; }   /* the game sometimes shifts the tunnel down */
      if(!(is_gameplay(m) ? OPT_GAME : OPT_CUT))
        continue;

      hd = hd_get(m,&hw,&hh);
      if(!hd)
        continue;
      ox = (E[m].cw - E[m].w) * F / 2;   /* (canvas - frame)/2 * F */
      oy = (E[m].ch - E[m].h) * F / 2;
      if(hw < ox + F*w->w || hh < oy + F*w->h)
        continue;
      GOOD_M[k] = m;

      for(y = 0; y < w->h; y++)
        {
          const uint8_t *mrow = &HD_SCRMASK[(w->y + y) * 384 + w->x];
          int i2, j2;
          for(x = 0; x < w->w; x++)
            {
              if(mrow[x])
                continue;
              for(i2 = 0; i2 < F; i2++)
                {
                  int py = (w->y + y) * F + i2, px = (w->x + x) * F;
                  uint32_t *d = &out[(size_t)py * OW + px];
                  const uint8_t *sp = &hd[((size_t)(oy + F*y + i2) * hw + ox + F*x) * 4];
                  for(j2 = 0; j2 < F; j2++, sp += 4)
                    {
                      uint32_t c = ((uint32_t)sp[0] << 16) | ((uint32_t)sp[1] << 8) | sp[2];
                      if(OPT_GRAIN)
                        {
                          /* sum of two uniform taps 0..a-1, centred.
                             light a=4 (sigma ~1.6), medium a=7 (sigma ~2.8) */
                          int a = OPT_GRAIN == 1 ? 4 : 7;
                          uint32_t h0 = grain_hash(((uint32_t)frame_ * 0x9E3779B1U) ^ (uint32_t)((py * 2 / F) * 8192 + (px + j2) * 2 / F));   /* same grain size at 2x and 4x */
                          c = grain_px(c,(int)(((h0 & 0xFF) * (uint32_t)a) >> 8) + (int)((((h0 >> 8) & 0xFF) * (uint32_t)a) >> 8) - (a - 1));
                        }
                      d[j2] = c;
                    }
                }
            }
        }
    }

  /* Pause: the game lays a see-through tint over the frozen video, so the
     video can no longer be recognised. Redraw the last enhanced frame of
     the most recently active window with the same tint (fitted per channel). */
  if(OPT_GFX)
    {
      int i, paused = (DRAM && DRAM[0xA09DF] == 1);   /* game's pause flag */
      for(i = 0; i < NART && !paused; i++)
        {
          int g[3], ex;
          if(ART[i].pause && art_test(&ART[i],buf_,g,&ex)) paused = 1;
        }
      if(paused)
        {
          int kk = -1, best = -1000000;
          for(k = 0; k < NWIN; k++)
            if(GOOD_M[k] >= 0 && LAST_HIT_FRAME[k] > best) { best = LAST_HIT_FRAME[k]; kk = k; }
          if(kk >= 0 && best != frame_ && (is_gameplay(GOOD_M[kk]) ? OPT_GAME : OPT_CUT))
            pause_fill(buf_,out,F,OW,&WINS[kk],GOOD_M[kk]);
        }
    }

  if(OPT_GFX)
    art_apply(buf_,out,F);
  return out;
}

/*
  Analog aiming for Sewer Shark. The game keeps the crosshair in RAM as
  int32 x,y (low-res screen pixels) at 0xA08DC/0xA08E0 followed by its
  d-pad velocity. While a tunnel is on screen, the left analog stick moves
  the crosshair directly: speed grows with how far the stick is pushed.
  The d-pad keeps working as before.
*/
#define SS_X   0xA08DC
#define SS_Y   0xA08E0
#define SS_VX  0xA08E4
#define SS_VY  0xA08E8
#define SS_XMIN 56
#define SS_XMAX 264
#define SS_YMIN 26
#define SS_YMAX 166

static float AIM_FX = 0, AIM_FY = 0;

static float
stick_speed(int v_)
{
  const float dz = 7000.0f, top = 5.0f;   /* px per frame at full tilt */
  float a = (float)(v_ < 0 ? -v_ : v_), n;
  if(a <= dz) return 0.0f;
  n = (a - dz) / (32767.0f - dz);
  if(n > 1.0f) n = 1.0f;
  n = n * sqrtf(n);                       /* gentle near centre, fast at the edge */
  return (v_ < 0 ? -1.0f : 1.0f) * n * top;
}

void
hd_sewer_aim(int frame_)
{
  CUR_FRAME = frame_;
  int32_t *px, *py, x, y, vx, vy;
  int16_t lx, ly;
  float dx, dy;

  if(!ACTIVE || !DRAM || !retro_input_state_cb)
    return;

  /* Infinite energy (cheat): top up while flying a tunnel */
  if(OPT_INFENERGY && frame_ - FLY_FRAME <= 30)
    {
      int32_t *e = (int32_t*)&DRAM[0xA0880];
      if(*e > 0 && *e < 25000) *e = 25000;
    }

  if(frame_ - FLY_FRAME > 30)     /* only while the tunnel view is showing */
    { AIM_FX = AIM_FY = 0; return; }

  px = (int32_t*)&DRAM[SS_X];
  py = (int32_t*)&DRAM[SS_Y];
  x  = *px; y = *py;
  vx = *(int32_t*)&DRAM[SS_VX];
  vy = *(int32_t*)&DRAM[SS_VY];
  if(x < SS_XMIN || x > SS_XMAX || y < SS_YMIN || y > SS_YMAX ||
     vx < -32 || vx > 32 || vy < -32 || vy > 32)
    return;                               /* not the crosshair: leave memory alone */

  lx = retro_input_state_cb(0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X);
  ly = retro_input_state_cb(0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y);
  dx = stick_speed(lx);
  dy = stick_speed(ly);

  /* Aim assist, part 2: ease toward the creature found last frame, unless
     the player is steering the crosshair with the stick */
  if(OPT_AIMASSIST && aa_ram(frame_) && dx == 0.0f && dy == 0.0f &&
     !retro_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP) &&
     !retro_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN) &&
     !retro_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT) &&
     !retro_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT))
    {
      int tx = AA_X - x, ty = AA_Y - y;
      int sx = tx * 3 / 5, sy = ty * 3 / 5;
      if(sx == 0 && tx) sx = tx > 0 ? 1 : -1;
      if(sy == 0 && ty) sy = ty > 0 ? 1 : -1;
      if(sx > 14) sx = 14; if(sx < -14) sx = -14;
      if(sy > 14) sy = 14; if(sy < -14) sy = -14;
      x += sx; y += sy;
      if(x < SS_XMIN) x = SS_XMIN; if(x > SS_XMAX) x = SS_XMAX;
      if(y < SS_YMIN) y = SS_YMIN; if(y > SS_YMAX) y = SS_YMAX;
      *px = x; *py = y;
      return;
    }

  if(dx == 0.0f && dy == 0.0f)
    { AIM_FX = AIM_FY = 0; return; }

  AIM_FX += dx; AIM_FY += dy;
  x += (int32_t)AIM_FX; AIM_FX -= (int32_t)AIM_FX;
  y += (int32_t)AIM_FY; AIM_FY -= (int32_t)AIM_FY;
  if(x < SS_XMIN) x = SS_XMIN; if(x > SS_XMAX) x = SS_XMAX;
  if(y < SS_YMIN) y = SS_YMIN; if(y > SS_YMAX) y = SS_YMAX;
  *px = x; *py = y;
}

/*
  Turning with the stick: Sewer Shark locks in a turn when B is pressed while
  a direction is held. While B is down, a firmly pushed left stick counts as
  the matching d-pad direction (strongest axis wins), and the B press itself
  reaches the game 3 frames later so the direction is already registered.
*/
int
hd_sewer_turn_filter(int b_, int *u_, int *d_, int *l_, int *r_)
{
  static int held = 0;
  int16_t lx, ly;
  int ax, ay;

  if(!ACTIVE || !retro_input_state_cb)
    return b_;
  if(!b_)
    { held = 0; return 0; }

  held++;
  lx = retro_input_state_cb(0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X);
  ly = retro_input_state_cb(0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y);
  ax = lx < 0 ? -lx : lx;
  ay = ly < 0 ? -ly : ly;
  if(!(*u_ | *d_ | *l_ | *r_) && (ax > 16000 || ay > 16000))
    {
      if(ax > ay) { if(lx < 0) *l_ = 1; else *r_ = 1; }
      else        { if(ly < 0) *u_ = 1; else *d_ = 1; }
      if(held <= 3)
        return 0;     /* let the direction land before the button */
    }
  return 1;
}

/*
  Auto-steer: Catfish's called route lives in the game's RAM (byte order
  within each 32-bit word is swapped in DRAM):
    0xA09A3  direction required at the next junction (1=left 2=right 4=up 8=down)
    0xA099B  jumps open right now (same bits; matches the flashing green arrows)
    0xA09B7  1 while a recharge station is offered; 0xA0987 = its green side
  When the required jump is open, hold steer (3DO B) and then the direction,
  exactly like a player would.
*/
void
hd_sewer_autosteer(int *b_, int *u_, int *d_, int *l_, int *r_)
{
  static int held = 0, last = -1;
  int cur, open;

  if(!ACTIVE || !DRAM || !OPT_AUTOSTEER)
    return;

  cur  = DRAM[0xA09A3];
  open = DRAM[0xA099B];

  /* Recharge station: 0xA09B7 = 1 while one is offered, 0xA0987 = the side
     with the green light (1=left 2=right). Only that side recharges. */
  if(DRAM[0xA09B7] == 1 && (DRAM[0xA0987] == 1 || DRAM[0xA0987] == 2) && (open & DRAM[0xA0987]))
    cur = DRAM[0xA0987];

  if(!(cur == 1 || cur == 2 || cur == 4 || cur == 8) || !(open & cur))
    {
      held = 0;
      return;
    }

  if(CUR_FRAME != last)
    {
      held++;
      last = CUR_FRAME;
    }
  if(held > 14)
    return;          /* done: the jump is locked in (yellow) */

  *b_ = 1;
  if(LOG) fprintf(LOG,"autosteer f=%d cur=%d open=%d held=%d\n",CUR_FRAME,cur,open,held);
  if(held > 2)
    {
      *u_ = (cur == 4); *d_ = (cur == 8);
      *l_ = (cur == 1); *r_ = (cur == 2);
    }
}

/*
  Auto-flare: hydrogen level is a 16.16 value at 0xA0954 (gauge = 12
  segments of ~50; segments 8-12, level >= ~400, are the red danger zone).
  Flares cost nothing when not needed, so fire as soon as the level is well
  above its normal range (>= 150), and again every 2 s while it stays high.
*/
int
hd_sewer_autoflare(void)
{
  static int last = -1, fired = -100000, held = 0;
  int level;

  if(!ACTIVE || !DRAM || !OPT_AUTOFLARE)
    return 0;
  if(CUR_FRAME == last)
    return held > 0;
  last = CUR_FRAME;
  if(held > 0)
    return --held > 0;
  if(CUR_FRAME - FLY_FRAME > 30)     /* only while flying a tunnel */
    return 0;
  level = (*(const int32_t*)&DRAM[0xA0954]) >> 16;
  if(level >= 150 && CUR_FRAME - fired > (level >= 200 ? 20 : 60))    /* gas builds up fast late in the game: keep flaring until it drops */
    {
      fired = CUR_FRAME;
      held  = 8;
      if(LOG) fprintf(LOG,"autoflare level=%d\n",level);
      return 1;
    }
  return 0;
}

/*
  Auto-fire: every shot costs energy, so shoot only while the crosshair is
  on the creature (see aa_ram). Taps the button 2 frames in 4.
  Returns 1 when the fire button should be down this frame.
*/
int
hd_sewer_autofire(void)
{
  int x, y, dx, dy, r;
  if(!ACTIVE || !DRAM || !OPT_AUTOFIRE)
    return 0;
  if(CUR_FRAME - FLY_FRAME > 30 || !aa_ram(CUR_FRAME))
    return 0;
  x = *(const int32_t*)&DRAM[0xA08DC];
  y = *(const int32_t*)&DRAM[0xA08E0];
  dx = AA_X - x; dy = AA_Y - y;
  r = AA_N < 8 ? 8 : AA_N;
  if(dx * dx + dy * dy > r * r)
    return 0;
  return (CUR_FRAME & 3) < 2;
}

void
hd_sewer_log_input(int b_, int u_, int d_, int l_, int r_)
{
  if(LOG && (b_ || u_ || d_ || l_ || r_))
    fprintf(LOG,"input f=%d B=%d U=%d D=%d L=%d R=%d\n",CUR_FRAME,b_,u_,d_,l_,r_);
}
