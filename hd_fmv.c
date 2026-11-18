/*
  3DOverdrive generic FMV module: replaces a game's FMV with pre-made 2x/4x
  (AI-cleaned) frames, for every game that has a profile in GAMES[] below.
  Sewer Shark is NOT handled here: it keeps its own module, hd_sewer.c, which
  the core tries first. This file started as a copy of hd_sewer.c with the
  Sewer Shark specifics (names, windows, sequences, RAM addresses, play aids)
  moved into per-game profiles.

  Each frame, every video window of the active profile is fingerprinted
  (12x8 grey thumbnail), matched against the pack index, and every pixel not
  drawn by MADAM is replaced by the matching enhanced pixel.

  Pack: <base>.3dov in the RetroArch system folder (or next to the game), or
  for building/testing an unpacked folder <base>/ with index.bin, scale.txt,
  art/, sprites/, <seq>/<track>/fNNNNN.webp|png. A profile may list several
  pack names (one per disc); the one whose disc check passes is used.
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
#include <sys/types.h>

/* pack scale (2 or 4) and the 4x output buffer */
static int       SCALE = 2;
static uint32_t *OUT4 = NULL;

#include "hd_fmv.h"
#ifdef SW_HANDDUMP
#include <features/features_cpu.h>
static int64_t PT_SEARCH, PT_GET, PT_HANDS, PT_TINT;
#define PT_NOW() ((int64_t)cpu_features_get_time_usec())
#endif
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

#define HD_MAXWIN 8

/* ---- per-game profiles ---- */
typedef struct
{
  const char        *name;          /* for the log */
  const char *const *path_keys;     /* lower-case substrings of the game path; any one matches */
  const char        *env_pack;      /* test override: pack folder or .3dov */
  const char        *env_log;       /* test: match log file */
  const char *const *packs;         /* pack base names, tried in order (one per disc) */
  const hd_win_t    *wins;          /* video windows (low-res screen coordinates) */
  int                nwin;
  const char *const *gameplay_seq;  /* seq ids that count as gameplay video, else cut scene */
  uint32_t           pause_addr;    /* DRAM byte that is pause_val while paused; 0 = none */
  int                pause_val;
  int                overlay_cel;   /* 1: smaller 16-bit frame-buffer cels from VRAM are
                                       overlays drawn over the video (see hd_cel_lookup) */
  int                overlay;       /* >0: the game draws things into the video with the CPU
                                       (not MADAM), e.g. the player's hands in Supreme Warrior.
                                       Matching then ignores the worst-fitting quarter of the
                                       thumbnail, and a pixel that differs from the matched
                                       frame by more than this value is kept as drawn. */
  /* Auto-Win play aid for fighting games (0 = not available): per disc (the
     order of packs[]), DRAM addresses of the player's health, its maximum,
     the opponent's health and its maximum (int32 each). Each disc of
     Supreme Warrior has its own program, so the addresses differ. */
  uint32_t           hp[2][4];
} hd_game_t;

/* Supreme Warrior (Digital Pictures, 1995, 2 discs). All video, cut scenes and
   fights, plays in one 256x160 window at 32,16. One pack per disc. */
static const char *const SW_KEYS[]  = { "supreme warrior", "supremewarrior", "supreme_warrior", NULL };
static const char *const SW_PACKS[] = { "SupremeWarriorHD1", "SupremeWarriorHD2", NULL };
static const hd_win_t    SW_WINS[]  = { { 32, 16, 256, 160 } };
static const char *const SW_PLAY[]  = { NULL };   /* to fill in: fight sequences */

static const hd_game_t GAMES[] =
  {
    { "Supreme Warrior", SW_KEYS, "SUPREMEWARRIOR_HD", "SUPREMEWARRIOR_HD_LOG",
      SW_PACKS, SW_WINS, 1, SW_PLAY, 0, 0, 1, 0,
      { { 0xA8228, 0xA822C, 0xA824C, 0xA8250 },      /* disc 1 */
        { 0xA816C, 0xA8170, 0xA8190, 0xA8194 } } },  /* disc 2 */
  };
#define NGAMES ((int)(sizeof(GAMES)/sizeof(GAMES[0])))

static const hd_game_t *G    = NULL;   /* active profile */
static int              PACK_K = 0;     /* which of G->packs[] was loaded (= disc) */
static const hd_win_t  *WINS = NULL;
static int              NWIN = 0;

static hd_ent_t *E      = NULL;
static int       N      = 0;
static char      ROOT[1024];
static int       ACTIVE = 0;

/*
  Pack access. A pack is either a folder (for building/testing) or one
  .3dov file: header, table of contents, then every file of the folder.
  The table and the files are XOR-ed with a keystream derived from sectors
  of the user's own game disc (hd_fmv_set_disc_key), so the pack is only
  readable together with the game.

  .3dov header (little endian):
    char magic[8] "3DOVPAK1", u32 count, u32 toc_size, u64 toc_off, u64 check
  toc (encrypted, stream id 0xFFFFFFFF): per file u64 off, u32 size, u16 name
    length, name ("index.bin", "art/cockpit_hd.png", "26332311/t01/f00843.webp")
  file k: size bytes at off, encrypted with stream id k.
*/
#if defined(_WIN32)
#define PK_SEEK(f_,o_) _fseeki64((f_),(__int64)(o_),SEEK_SET)
#else
#define PK_SEEK(f_,o_) fseeko((f_),(off_t)(o_),SEEK_SET)
#endif

typedef struct { uint64_t off; uint32_t size; uint32_t name; } pk_ent_t;
static FILE     *PK      = NULL;
static slock_t  *PK_LOCK = NULL;
static pk_ent_t *PKE     = NULL;
static char     *PKNAMES = NULL;
static uint32_t  PKN     = 0;
static uint32_t *PKH     = NULL;    /* hash -> entry index + 1 */
static uint32_t  PKHN    = 0;
static uint64_t  PK_KEY  = 0;
static uint64_t  DISC_KEY = 0;

void
hd_fmv_set_disc_key(uint64_t key_)
{
  DISC_KEY = key_;
}

static uint64_t
pk_mix(uint64_t z_)
{
  z_ += 0x9E3779B97F4A7C15ULL;
  z_ = (z_ ^ (z_ >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z_ = (z_ ^ (z_ >> 27)) * 0x94D049BB133111EBULL;
  return z_ ^ (z_ >> 31);
}

static void
pk_crypt(uint8_t *p_, size_t n_, uint32_t stream_)
{
  uint64_t base = PK_KEY ^ ((uint64_t)stream_ << 32);
  size_t i, j;
  for(i = 0; i * 8 < n_; i++)
    {
      uint64_t k = pk_mix(base ^ (uint64_t)i);
      size_t m = (n_ - i * 8 < 8) ? n_ - i * 8 : 8;
      for(j = 0; j < m; j++)
        p_[i * 8 + j] ^= (uint8_t)(k >> (8 * j));
    }
}

static uint32_t
pk_hash(const char *s_, size_t n_)
{
  uint32_t h = 2166136261u;
  while(n_--) { h ^= (uint8_t)*s_++; h *= 16777619u; }
  return h;
}

static void
pk_close(void)
{
  if(PK) fclose(PK);
  PK = NULL;
  free(PKE); PKE = NULL;
  free(PKNAMES); PKNAMES = NULL;
  free(PKH); PKH = NULL;
  PKN = PKHN = 0;
  if(PK_LOCK) { slock_free(PK_LOCK); PK_LOCK = NULL; }
}

static uint32_t rd32(const uint8_t *p_) { return p_[0] | (p_[1] << 8) | (p_[2] << 16) | ((uint32_t)p_[3] << 24); }
static uint64_t rd64(const uint8_t *p_) { return rd32(p_) | ((uint64_t)rd32(p_ + 4) << 32); }

/* open a .3dov pack; 1 = ok, 0 = not a pack, -1 = pack for another disc */
static int
pk_open(const char *path_)
{
  uint8_t hd[32], *toc = NULL;
  uint32_t count, tsz, i, pos, nm = 0;
  uint64_t toff;
  FILE *f = fopen(path_,"rb");
  if(!f)
    return 0;
  if(fread(hd,1,32,f) != 32 || memcmp(hd,"3DOVPAK1",8))
    { fclose(f); return 0; }
  count = rd32(hd + 8); tsz = rd32(hd + 12); toff = rd64(hd + 16);
  if(rd64(hd + 24) != pk_mix(DISC_KEY ^ 0x3D0F00DULL))
    { fclose(f); return -1; }
  pk_close();
  PK_KEY = DISC_KEY;
  toc = (uint8_t*)malloc(tsz ? tsz : 1);
  PKE = (pk_ent_t*)calloc(count ? count : 1,sizeof(pk_ent_t));
  PKNAMES = (char*)malloc(tsz + count + 1);
  for(PKHN = 1; PKHN < count * 2; PKHN <<= 1) ;
  PKH = (uint32_t*)calloc(PKHN,sizeof(uint32_t));
  if(!toc || !PKE || !PKNAMES || !PKH || PK_SEEK(f,toff) || fread(toc,1,tsz,f) != tsz)
    { free(toc); fclose(f); pk_close(); return 0; }
  pk_crypt(toc,tsz,0xFFFFFFFFu);
  for(i = 0, pos = 0; i < count && pos + 14 <= tsz; i++)
    {
      uint32_t ln = toc[pos + 12] | (toc[pos + 13] << 8), h;
      if(pos + 14 + ln > tsz) break;
      PKE[i].off = rd64(toc + pos); PKE[i].size = rd32(toc + pos + 8); PKE[i].name = nm;
      memcpy(PKNAMES + nm,toc + pos + 14,ln); PKNAMES[nm + ln] = 0;
      h = pk_hash(PKNAMES + nm,ln) & (PKHN - 1);
      while(PKH[h]) h = (h + 1) & (PKHN - 1);
      PKH[h] = i + 1;
      nm += ln + 1; pos += 14 + ln;
    }
  free(toc);
  PKN = i;
  PK = f;
  PK_LOCK = slock_new();
  return PKN ? 1 : 0;
}

/* read a pack file (pack or folder) into malloc'd memory */
static uint8_t*
pk_get(const char *rel_, size_t *n_)
{
  uint8_t *p = NULL;
  if(PK)
    {
      uint32_t h = pk_hash(rel_,strlen(rel_)) & (PKHN - 1), i;
      while((i = PKH[h]) != 0 && strcmp(PKNAMES + PKE[i - 1].name,rel_))
        h = (h + 1) & (PKHN - 1);
      if(!i) return NULL;
      i--;
      p = (uint8_t*)malloc(PKE[i].size + 1);
      if(!p) return NULL;
      slock_lock(PK_LOCK);
      if(PK_SEEK(PK,PKE[i].off) || fread(p,1,PKE[i].size,PK) != PKE[i].size)
        { slock_unlock(PK_LOCK); free(p); return NULL; }
      slock_unlock(PK_LOCK);
      pk_crypt(p,PKE[i].size,i);
      p[PKE[i].size] = 0;
      *n_ = PKE[i].size;
      return p;
    }
  else
    {
      char path[1500];
      FILE *f;
      long sz;
      snprintf(path,sizeof(path),"%s/%s",ROOT,rel_);
      f = fopen(path,"rb");
      if(!f) return NULL;
      fseek(f,0,SEEK_END); sz = ftell(f); fseek(f,0,SEEK_SET);
      p = (uint8_t*)malloc(sz > 0 ? sz + 1 : 1);
      if(p && fread(p,1,sz,f) == (size_t)sz) { p[sz] = 0; *n_ = sz; }
      else { free(p); p = NULL; }
      fclose(f);
      return p;
    }
}

/* decode a WebP or PNG file to RGBA; free() the result */
static uint8_t*
img_decode(const uint8_t *buf_, size_t n_, int *w_, int *h_)
{
  int w, h, c;
  if(n_ >= 12 && !memcmp(buf_,"RIFF",4) && !memcmp(buf_ + 8,"WEBP",4))
    {
      uint8_t *out;
      if(!WebPGetInfo(buf_,n_,&w,&h)) return NULL;
      out = (uint8_t*)malloc((size_t)w * h * 4);
      if(out && WebPDecodeRGBAInto(buf_,n_,out,(size_t)w * h * 4,w * 4))
        { *w_ = w; *h_ = h; return out; }
      free(out);
      return NULL;
    }
  return stbi_load_from_memory(buf_,(int)n_,w_,h_,&c,4);
}

/* load pack file <rel> exactly as named, as RGBA */
static uint8_t*
load_file_img(const char *rel_, int *w_, int *h_)
{
  size_t n = 0;
  uint8_t *b = pk_get(rel_,&n), *img;
  if(!b) return NULL;
  img = img_decode(b,n,w_,h_);
  free(b);
  return img;
}

/* load <rel>.webp, else <rel>.png, as RGBA (the file's content decides the
   decoder, not its extension) */
static uint8_t*
load_img(const char *rel_, int *w_, int *h_)
{
  char p[1400];
  uint8_t *img;
  snprintf(p,sizeof(p),"%s.webp",rel_);
  if((img = load_file_img(p,w_,h_)) == NULL)
    {
      snprintf(p,sizeof(p),"%s.png",rel_);
      img = load_file_img(p,w_,h_);
    }
  return img;
}

/* Bring a pack frame to the pack's scale (canvas * SCALE). Packs normally
   hold exactly that size and nothing happens. Test packs may mix sizes:
   original 1x frames (doubled/quadrupled, nearest) and frames straight out
   of Topaz at another size such as 5x (resampled, bilinear). */
static uint8_t*
fit_frame(uint8_t *img_, int *w_, int *h_, int cw_, int ch_)
{
  int sw = *w_, sh = *h_, tw = cw_ * SCALE, th = ch_ * SCALE, x, y;
  const uint32_t *s = (const uint32_t*)img_;
  uint32_t *d;
  if(!img_ || cw_ <= 0 || ch_ <= 0 || (sw == tw && sh == th))
    return img_;
  d = (uint32_t*)malloc((size_t)tw * th * 4);
  if(!d) { free(img_); return NULL; }
  if(tw % sw == 0 && th % sh == 0)
    {
      int fx = tw / sw, fy = th / sh;
      for(y = 0; y < th; y++)
        for(x = 0; x < tw; x++)
          d[(size_t)y * tw + x] = s[(size_t)(y / fy) * sw + x / fx];
    }
  else
    {
      for(y = 0; y < th; y++)
        {
          float fy = ((float)y + 0.5f) * sh / th - 0.5f;
          int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
          float wy = fy - y0; if(wy < 0) wy = 0;
          for(x = 0; x < tw; x++)
            {
              float fx = ((float)x + 0.5f) * sw / tw - 0.5f;
              int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1, k;
              float wx = fx - x0;
              uint32_t o = 0;
              const uint8_t *p00, *p01, *p10, *p11;
              if(wx < 0) wx = 0;
              p00 = (const uint8_t*)&s[(size_t)y0 * sw + x0]; p01 = (const uint8_t*)&s[(size_t)y0 * sw + x1];
              p10 = (const uint8_t*)&s[(size_t)y1 * sw + x0]; p11 = (const uint8_t*)&s[(size_t)y1 * sw + x1];
              for(k = 0; k < 4; k++)
                {
                  float v = (p00[k] * (1 - wx) + p01[k] * wx) * (1 - wy) + (p10[k] * (1 - wx) + p11[k] * wx) * wy;
                  o |= (uint32_t)(v + 0.5f) << (8 * k);
                }
              d[(size_t)y * tw + x] = o;
            }
        }
    }
  free(img_);
  *w_ = tw; *h_ = th;
  return (uint8_t*)d;
}

/* next line of a text buffer (modifies it); NULL at end */
static char*
next_line(char **pp_)
{
  char *s = *pp_, *e;
  if(!s || !*s) return NULL;
  e = strchr(s,'\n');
  if(e) { *e = 0; *pp_ = e + 1; } else *pp_ = s + strlen(s);
  return s;
}

static float     LAST_TH[HD_MAXWIN][96];
static int       LAST_M[HD_MAXWIN];
static int       LAST_SEARCHED[HD_MAXWIN];
static int       LAST_HIT_FRAME[HD_MAXWIN];
static int       LAST_MATCH_M[HD_MAXWIN];   /* frame matched at LAST_HIT_FRAME (shown or not) */
static int       GOOD_M[HD_MAXWIN];   /* last frame shown enhanced, per window */
static int       LAST_FULL[HD_MAXWIN];

/* small cache of decoded HD frames */
#define CACHE 4
static int      C_IDX[CACHE] = {-1,-1,-1,-1};
static uint8_t *C_PIX[CACHE];
static int      C_W[CACHE],C_H[CACHE];
static int      C_NEXT = 0;

static FILE *LOG = NULL;

/* user options (core options, see opera_lr_opts.c) */
static int OPT_CUT = 1, OPT_GAME = 1, OPT_GFX = 1, OPT_GRAIN = 0;
static int OPT_AUTOWIN = 0;

static const uint8_t *hd_get(int i_, int *w_, int *h_);
static void pf_start(void);
void hd_fmv_deinit(void);

void
hd_fmv_set_opts(int cutscenes_, int gameplay_, int graphics_, int grain_)
{
  OPT_CUT = cutscenes_; OPT_GAME = gameplay_; OPT_GFX = graphics_; OPT_GRAIN = grain_;
}

/* gameplay video (vs cut scenes): per profile */
static int
is_gameplay(int m_)
{
  int i;
  for(i = 0; G->gameplay_seq[i]; i++)
    if(!strncmp(E[m_].seq,G->gameplay_seq[i],8)) return 1;
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
static void hands_load(void);
static const uint8_t *hd_get(int i_, int *w_, int *h_);
static void spr_load(void);

int
hd_fmv_active(void)
{
  return ACTIVE;
}

int
hd_fmv_scale(void)
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

/* use pack <root_>: a .3dov file or a folder; 1 = ok */
static int
try_root(const char *root_)
{
  uint8_t *b;
  size_t sz = 0;
  int r;

  hd_fmv_deinit();          /* the decoder thread reads E and the pack */
  pk_close();
  r = pk_open(root_);
  if(r < 0)
    {
      if(retro_log_printf_cb)
        retro_log_printf_cb(RETRO_LOG_WARN,"[3DOverdrive] %s is for a different disc; enhanced video off\n",root_);
      return 0;
    }
  if(r == 0)
    snprintf(ROOT,sizeof(ROOT),"%s",root_);
  b = pk_get("index.bin",&sz);
  if(!b)
    { pk_close(); return 0; }
  free(E);
  E = (hd_ent_t*)b;
  N = (int)(sz / sizeof(hd_ent_t));
  if(N <= 0)
    { pk_close(); N = 0; return 0; }
  if(retro_log_printf_cb)
    retro_log_printf_cb(RETRO_LOG_INFO,"[3DOverdrive] enhanced pack: %s (%d frames)\n",root_,N);
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

/* try pack <base> in folder <dir_>: <base>.txt (holding a path), <base>.3dov, <base>/ */
static int
try_base(const char *dir_, const char *base_, int txt_)
{
  char path[1200];
  int ok = 0;
  if(txt_)
    {
      FILE *f;
      snprintf(path,sizeof(path),"%s/%s.txt",dir_,base_);
      f = fopen(path,"r");
      if(f)
        {
          char line[1024] = {0};
          if(fgets(line,sizeof(line),f))
            { chomp(line); ok = try_root(line); }
          fclose(f);
        }
    }
  if(!ok)
    {
      snprintf(path,sizeof(path),"%s/%s.3dov",dir_,base_);
      ok = try_root(path);
    }
  if(!ok)
    {
      snprintf(path,sizeof(path),"%s/%s",dir_,base_);
      ok = try_root(path);
    }
  return ok;
}

void
hd_fmv_init(const char *system_dir_, const char *game_path_)
{
  char gdir[1200];
  const char *env = NULL;
  int ok = 0, g, k;

  ACTIVE = 0;
  G = NULL;
  for(g = 0; g < NGAMES && !G; g++)
    {
      if(getenv(GAMES[g].env_pack))
        G = &GAMES[g];
      for(k = 0; GAMES[g].path_keys[k] && !G; k++)
        if(ci_contains(game_path_,GAMES[g].path_keys[k]))
          G = &GAMES[g];
    }
  if(!G)
    return;
  WINS = G->wins;
  NWIN = G->nwin < HD_MAXWIN ? G->nwin : HD_MAXWIN;
  env  = getenv(G->env_pack);

  /* folder of the game, for packs placed next to it */
  gdir[0] = 0;
  if(game_path_)
    {
      char *p;
      snprintf(gdir,sizeof(gdir),"%s",game_path_);
      p = strrchr(gdir,'/');
      { char *q = strrchr(gdir,'\\'); if(q > p) p = q; }
      if(p) *p = 0; else gdir[0] = 0;
    }

  /* 1: environment variable (testing) */
  if(env)
    ok = try_root(env);
  /* 2-4: each pack name (one per disc) in the system folder, then next to the
     game; a pack made for another disc is skipped, so the right one wins */
  {
    /* "Disc N" in the file name: try the pack ending in N first (unpacked
       test folders have no disc check, so the order matters for them) */
    char dd = 0;
    int pass;
    for(k = 1; k <= 9 && !dd; k++)
      {
        char key[8];
        snprintf(key,sizeof(key),"disc %d",k);
        if(ci_contains(game_path_,key)) dd = (char)('0' + k);
      }
    for(pass = 0; pass < 2 && !ok; pass++)
      for(k = 0; G->packs[k] && !ok; k++)
        {
          const char *pn = G->packs[k];
          int mine = dd && pn[0] && pn[strlen(pn) - 1] == dd;
          if(pass == 0 ? !mine : mine)
            continue;
          if(system_dir_)
            ok = try_base(system_dir_,pn,1);
          if(!ok && gdir[0])
            ok = try_base(gdir,pn,0);
          if(ok) PACK_K = k;
        }
  }
  if(!ok)
    return;
  if(retro_log_printf_cb)
    retro_log_printf_cb(RETRO_LOG_INFO,"[3DOverdrive] game profile: %s\n",G->name);

  memset(LAST_TH,0,sizeof(LAST_TH));
  { int q; for(q = 0; q < HD_MAXWIN; q++) { LAST_M[q] = -1; LAST_HIT_FRAME[q] = -1000; LAST_FULL[q] = -1000; LAST_SEARCHED[q] = 0; GOOD_M[q] = -1; LAST_MATCH_M[q] = -1; } }
  if(G->env_log && getenv(G->env_log) && !LOG)
    LOG = fopen(getenv(G->env_log),"w");
  art_load();
  spr_load();
  hands_load();
  SCALE = 2;
  {                                                  /* "4" for a 4x pack */
    size_t n = 0;
    char *t = (char*)pk_get("scale.txt",&n);
    if(t) { if(atoi(t) == 4) SCALE = 4; free(t); }   /* "1" (test pack of 1x frames) works as 2 */
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
static sthread_t *PF_TH2  = NULL;   /* second decoder: a new shot's first frame need not wait */
static int        PF_QUIT = 0;

static void
hd_path(int i_, char *path_, size_t n_)
{
  snprintf(path_,n_,"%.8s/%.3s/f%05u",E[i_].seq,E[i_].trk,(unsigned)E[i_].j);
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
      p = fit_frame(load_img(path,&w,&h),&w,&h,E[i].cw,E[i].ch);
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
    {
      PF_TH = sthread_create(pf_worker,NULL);
      PF_TH2 = sthread_create(pf_worker,NULL);
    }
}

void
hd_fmv_deinit(void)
{
  int k;
  if(PF_TH)
    {
      slock_lock(PF_LOCK);
      PF_QUIT = 1;
      scond_broadcast(PF_COND);
      slock_unlock(PF_LOCK);
      sthread_join(PF_TH);
      PF_TH = NULL;
      if(PF_TH2) { sthread_join(PF_TH2); PF_TH2 = NULL; }
      for(k = 0; k < PF; k++)
        { free(PFC[k].pix); PFC[k].pix = NULL; PFC[k].idx = -1; }
      scond_free(PF_COND); PF_COND = NULL;
      slock_free(PF_LOCK); PF_LOCK = NULL;
    }
  for(k = 0; k < CACHE; k++)
    { free(C_PIX[k]); C_PIX[k] = NULL; C_IDX[k] = -1; }
  pk_close();
}

/* ask the worker for frames i_+first_ .. i_+3 of the same shot (nearest first) */
static void
pf_queue(int i_, int first_)
{
  int d;
  if(!PF_TH)
    return;
  slock_lock(PF_LOCK);
  PF_QN = 0;
  for(d = first_; d <= 3 && i_ + d < N; d++)
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
    scond_broadcast(PF_COND);
  slock_unlock(PF_LOCK);
}

/* the frames after i_ in the same shot */
static void
pf_request(int i_)
{
  pf_queue(i_,1);
}

/* frame i_ already decoded (cache or prefetch buffer)? */
static int
hd_have(int i_)
{
  int k, have = 0;
  for(k = 0; k < CACHE; k++)
    if(C_IDX[k] == i_) return 1;
  if(PF_TH)
    {
      slock_lock(PF_LOCK);
      for(k = 0; k < PF; k++)
        have |= PFC[k].idx == i_ && PFC[k].pix != NULL;
      slock_unlock(PF_LOCK);
    }
  return have;
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
      p = fit_frame(load_img(path,&w,&h),&w,&h,E[i_].cw,E[i_].ch);
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
        /* blocks partly covered by an overlay (e.g. a big hand pose) still
           count, with less weight; mostly covered ones do not */
        wt_[by*12 + bx] = (n == (uint32_t)(bw * bh)) ? 1.0f : (4 * n >= (uint32_t)(3 * bw * bh)) ? 0.5f : 0.0f;
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

/* distance over the best-fitting 3/4 of the weighted blocks (overlay games):
   blocks covered by something the game drew into the video do not count */
#define TRIM 24
static float
dist_trim(const float *a_, const float *w_, const uint8_t *b_, float n_, float cut_)
{
  /* n_ = number of weighted blocks; cut_ = stop once the distance must exceed it.
     top[] holds the TRIM largest squared differences seen so far; the sum of
     the others (kept in s) only grows, so it can end the search early. */
  float top[TRIM], s = 0, lim;
  int i, nt = 0, mi = 0;
  if(n_ <= TRIM + 8) return 1e9f;
  lim = cut_ * cut_ * (n_ - TRIM);
  for(i = 0; i < 96; i++)
    {
      float d;
      if(w_[i] <= 0) continue;
      d = a_[i] - (float)b_[i]; d *= d;
      if(nt < TRIM)
        {
          top[nt] = d;
          if(d < top[mi]) mi = nt;
          nt++;
          continue;
        }
      if(d > top[mi])
        {
          int j;
          s += top[mi]; top[mi] = d;
          for(j = 0, mi = 0; j < TRIM; j++) if(top[j] < top[mi]) mi = j;
        }
      else
        s += d;
      if(s > lim) return 1e9f;
    }
  return sqrtf(s / (n_ - TRIM));
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
        d = G->overlay ? dist_trim(th_,wt_,E[c].th,n,1e9f) : dist(th_,wt_,E[c].th);
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
      d = G->overlay ? dist_trim(th_,wt_,E[i].th,n,bd) : dist_cut(th_,wt_,n,E[i].th,bd);
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
  char path[1400], name[256], *txt, *cur, *ln;
  size_t tn = 0;
  int x,y,i,w2,h2;
  for(i = 0; i < NART; i++) { stbi_image_free(ART[i].lo); stbi_image_free(ART[i].hd); }
  NART = 0;
  txt = (char*)pk_get("art/art.txt",&tn);
  if(!txt) return;
  cur = txt;
  while(NART < MAXART && (ln = next_line(&cur)) != NULL)
    {
      art_t *a = &ART[NART];
      if(sscanf(ln,"%255s %d %d",name,&x,&y) != 3) continue;
      snprintf(path,sizeof(path),"art/%s_lo",name);
      a->lo = load_img(path,&a->w,&a->h);
      snprintf(path,sizeof(path),"art/%s_hd",name);
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
  free(txt);
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
      /* a smaller one from VRAM in frame-buffer form, drawn over the video
         (Supreme Warrior's hands and legs): an overlay, keep it on top */
      if(G && G->overlay_cel && pdata_ >= 0x200000 && (pre1_ & 0x800) && h < WINS[0].h)
        return (const uint8_t*)2;
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
  char path[1400], hex[64], file[256], *txt, *cur, *ln;
  size_t tn = 0;
  unsigned len, fnv;
  int w, h, i, w2, h2;

  for(i = 0; i < NSPR; i++) stbi_image_free(SPR[i].img);
  free(SPR); SPR = NULL; NSPR = 0;
  memset(SC,0xFF,sizeof(SC));
  opera_hd_cel_lookup = hd_cel_lookup;   /* also handles video-frame copies */

  txt = (char*)pk_get("sprites/sprites.txt",&tn);
  if(!txt) return;
  cur = txt;
  SPR = (spr_t*)calloc(1024,sizeof(spr_t));
  while(SPR && NSPR < 1024 && (ln = next_line(&cur)) != NULL)
    {
      spr_t *s = &SPR[NSPR];
      if(sscanf(ln,"%63s %u %x %d %d %255s",hex,&len,&fnv,&w,&h,file) != 6) continue;
      if(strlen(hex) != 32) continue;
      for(i = 0; i < 16; i++) { unsigned b; sscanf(hex + 2*i,"%2x",&b); s->pre[i] = b; }
      snprintf(path,sizeof(path),"sprites/%s",file);
      s->img = load_file_img(path,&w2,&h2);
      if(!s->img || w2 != 2*w || h2 != 2*h) { if(s->img) stbi_image_free(s->img); continue; }
      s->len = len; s->fnv = fnv; s->w = w; s->h = h;
      NSPR++;
    }
  free(txt);
}

/* ---- Enhanced overlay poses (Supreme Warrior's hands and legs) ------------
   <pack>/hands/hands.txt: "<key> <w> <h> ..." per pose, with hands/<key>.png
   (4x RGBA, smooth alpha) and hands/<key>_1x.png (the game's own pose).
   key = FNV-1a 32 over the cel's 32-bit words, high byte first. MADAM reports
   every overlay cel it draws (opera_hd_overlay_draw); at output time each
   reported pose is drawn at 4x over the doubled 1x one, but only where the
   1x pose is really still visible on screen (anything drawn over it wins).
   Behind the pose's smooth edge goes the enhanced video, which the video
   swap keeps in VID4 for the screen pixels the 1x pose covers. */
typedef void (*opera_hd_overlay_fn)(uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t);
extern opera_hd_overlay_fn opera_hd_overlay_draw;

typedef struct { uint32_t key; int w, h; uint8_t *hd, *one; } hand_t;
static hand_t *HAND = NULL;
static int     NHAND = 0;
typedef struct { int idx, x, y, mirror; } hand_draw_t;
#define HD_MAXDRAW 16
#define HD_HIST 4                        /* [0] = this frame; 1.. = last frames that drew a pose */
static hand_draw_t HDRAW[HD_HIST][HD_MAXDRAW];
static int         NHDRAW[HD_HIST] = {0,0,0,0};
static uint32_t   *VID4 = NULL;          /* enhanced video under overlay pixels (1280x960) */
static int        *VIDF = NULL;          /* per 1x pixel (384 wide): frame VID4 was written */

static void
hd_ov_draw(uint32_t pdata_, uint32_t pre0_, uint32_t pre1_, int32_t xpos_, int32_t ypos_, int32_t hdx_, int32_t vdy_)
{
  int w = (pre1_ & 0x7FF) + 1, rp = ((pre0_ >> 6) & 0x3FF) + 1;
  uint32_t stride = ((pre1_ >> 16) & 0x3FF) + 2, h = 2166136261u;
  int i, p, x, k;
#ifdef SW_HANDDUMP
  {
    /* debug: write every distinct overlay cel once (rp row pairs = 2*rp lines) */
    static uint32_t seen[4096]; static int nseen = 0;
    const char *dir = getenv("SUPREMEWARRIOR_HANDDUMP");
    if(dir && rp > 0 && w > 0 && rp * w <= 512 * 1024)
      {
        uint32_t fh = 2166136261u, *buf = (uint32_t*)malloc((size_t)rp * w * 4);
        int n = 0, j;
        if(buf)
          {
            for(p = 0; p < rp; p++)
              for(x = 0; x < w; x++)
                {
                  uint32_t v = opera_mem_read32(pdata_ + 4 * (p * stride + x));
                  buf[n++] = v;
                  for(k = 24; k >= 0; k -= 8) fh = (fh ^ ((v >> k) & 0xFF)) * 16777619u;
                }
            for(j = 0; j < nseen; j++) if(seen[j] == fh) break;
            if(j == nseen && nseen < 4096)
              {
                char fp[1400]; FILE *f;
                seen[nseen++] = fh;
                snprintf(fp,sizeof(fp),"%s\\%08x.raw",dir,fh);
                if((f = fopen(fp,"wb")) != NULL) { fwrite(buf,4,(size_t)n,f); fclose(f); }
                snprintf(fp,sizeof(fp),"%s\\index.txt",dir);
                if((f = fopen(fp,"a")) != NULL)
                  { fprintf(f,"%08x %d %d %d %d %d %d\n",fh,w,2 * rp,xpos_,ypos_,hdx_,vdy_); fclose(f); }
              }
            free(buf);
          }
      }
  }
#endif
  if(!NHAND || NHDRAW[0] >= HD_MAXDRAW || vdy_ != 65536 || (hdx_ != 65536 && hdx_ != -65536))
    return;
  for(i = 0; i < NHAND; i++)
    if(HAND[i].w == w && HAND[i].h == 2 * rp) break;
  if(i == NHAND) return;
  for(p = 0; p < rp; p++)
    for(x = 0; x < w; x++)
      {
        uint32_t v = opera_mem_read32(pdata_ + 4 * (p * stride + x));
        for(k = 24; k >= 0; k -= 8) h = (h ^ ((v >> k) & 0xFF)) * 16777619u;
      }
  for(i = 0; i < NHAND; i++)
    if(HAND[i].key == h && HAND[i].w == w) break;
  if(LOG) fprintf(LOG,"hand %08x w=%d rp=%d x=%d y=%d hdx=%d %s\n",h,w,rp,xpos_ >> 16,ypos_ >> 16,hdx_,i < NHAND ? "hit" : "miss");
  if(i == NHAND) return;
  HDRAW[0][NHDRAW[0]].idx = i;
  HDRAW[0][NHDRAW[0]].x = xpos_ >> 16;
  HDRAW[0][NHDRAW[0]].y = ypos_ >> 16;
  HDRAW[0][NHDRAW[0]].mirror = hdx_ < 0;
  NHDRAW[0]++;
}

static void
hands_load0(void)
{
  char *txt, *cur, *ln, path[1400], key[32];
  size_t tn = 0;
  int i, w, h, w2, h2;
  for(i = 0; i < NHAND; i++) { free(HAND[i].hd); free(HAND[i].one); }
  free(HAND); HAND = NULL; NHAND = 0; memset(NHDRAW,0,sizeof(NHDRAW));
  opera_hd_overlay_draw = NULL;
  txt = (char*)pk_get("hands/hands.txt",&tn);
  if(!txt) return;
  HAND = (hand_t*)calloc(256,sizeof(hand_t));
  cur = txt;
  while(HAND && NHAND < 256 && (ln = next_line(&cur)) != NULL)
    {
      hand_t *s = &HAND[NHAND];
      if(sscanf(ln,"%31s %d %d",key,&w,&h) != 3) continue;
      s->key = (uint32_t)strtoul(key,NULL,16); s->w = w; s->h = h;
      snprintf(path,sizeof(path),"hands/%s.png",key);
      s->hd = load_file_img(path,&w2,&h2);
      if(!s->hd || w2 != 4 * w || h2 != 4 * h) { free(s->hd); continue; }
      snprintf(path,sizeof(path),"hands/%s_1x.png",key);
      s->one = load_file_img(path,&w2,&h2);
      if(!s->one || w2 != w || h2 != h) { free(s->hd); free(s->one); continue; }
      NHAND++;
    }
  free(txt);
  if(NHAND)
    {
      if(!VID4) VID4 = (uint32_t*)malloc(1280 * 960 * 4);
      if(!VIDF) VIDF = (int*)calloc(384 * 288,sizeof(int));
      if(VID4 && VIDF) opera_hd_overlay_draw = hd_ov_draw;
    }
  if(LOG) fprintf(LOG,"hands %d\n",NHAND);
}

static void
hands_load(void)
{
#ifdef SW_HANDDUMP
  /* debug: SUPREMEWARRIOR_HANDS=<dir> loads <dir>/hands/... instead of the pack's hands */
  const char *ov = getenv("SUPREMEWARRIOR_HANDS");
  if(ov)
    {
      static char saved[sizeof(ROOT)];
      FILE *pk = PK;
      memcpy(saved,ROOT,sizeof(ROOT));
      snprintf(ROOT,sizeof(ROOT),"%s",ov); PK = NULL;
      hands_load0();
      memcpy(ROOT,saved,sizeof(ROOT)); PK = pk;
      return;
    }
#endif
  hands_load0();
}

static INLINE int
c5near(uint32_t p_, const uint8_t *o_)
{
  int dr = (int)((p_ >> 19) & 31) - (o_[0] >> 3);
  int dg = (int)((p_ >> 11) & 31) - (o_[1] >> 3);
  int db = (int)((p_ >> 3) & 31) - (o_[2] >> 3);
  return dr >= -1 && dr <= 1 && dg >= -1 && dg <= 1 && db >= -1 && db <= 1;
}

static int
hand_vis(const hand_draw_t *d_, const uint32_t *buf_, int *opq_)
{
  const hand_t *P = &HAND[d_->idx];
  int x0 = d_->mirror ? d_->x - P->w : d_->x, cx, cy, vis = 0;
  for(cy = 0; cy < P->h; cy++)
    for(cx = 0; cx < P->w; cx++)
      {
        int sx = x0 + cx, sy = d_->y + cy, ux = d_->mirror ? P->w - 1 - cx : cx;
        const uint8_t *o = &P->one[(cy * P->w + ux) * 4];
        if(!o[3] || sx < 0 || sy < 0 || sx >= 320 || sy >= 240) continue;
        (*opq_)++;
        vis += c5near(buf_[(size_t)(sy * 2) * 640 + sx * 2],o);
      }
  return vis;
}

/* no enhanced video stored under pose pixel sx,sy: the colour of the
   nearest screen pixel (within 4) outside the pose that nothing else was
   drawn on - enhanced video - so a soft 4x edge never blends over the
   doubled 1x hand colour (staircase) */
static uint32_t
hand_bg(const uint32_t *out_, const hand_t *P, const hand_draw_t *d_, int x0_, int sx_, int sy_, int *ok_)
{
  int r, dx, dy;
  for(r = 1; r <= 4; r++)
    for(dy = -r; dy <= r; dy++)
      for(dx = -r; dx <= r; dx++)
        {
          int x = sx_ + dx, y = sy_ + dy, cx = x - x0_, cy = y - d_->y;
          if((dx != -r && dx != r && dy != -r && dy != r) || x < 0 || y < 0 || x >= 320 || y >= 240)
            continue;
          if(cx >= 0 && cy >= 0 && cx < P->w && cy < P->h &&
             P->one[(cy * P->w + (d_->mirror ? P->w - 1 - cx : cx)) * 4 + 3])
            continue;                                    /* inside the pose */
          if(HD_SCRMASK[y * 384 + x])
            continue;                                    /* something drawn there */
          *ok_ = 1;
          return out_[(size_t)(y * 4 + 2) * 1280 + x * 4 + 2];
        }
  *ok_ = 0;
  return 0;
}

static void
hands_apply(const uint32_t *buf_, uint32_t *out_, int F_, int frame_)
{
  int r, n = 0, set = 0, k;
  /* the screen shows what the game drew a frame or two earlier: use the
     pose set (this frame, or one of the last frames that drew one) that
     best matches what is on screen now */
  if(F_ == 4 && OPT_GFX && NHAND)
    {
      double best = -1;
      for(k = 0; k < HD_HIST; k++)
        {
          int o = 0, v = 0;
          for(r = 0; r < NHDRAW[k]; r++) v += hand_vis(&HDRAW[k][r],buf_,&o);
          if(o && (double)v / o > best) { best = (double)v / o; set = k; n = NHDRAW[k]; }
        }
    }
  if(F_ == 4 && OPT_GFX && NHAND)
    for(r = 0; r < n; r++)
      {
        const hand_draw_t *d = &HDRAW[set][r];
        const hand_t *P = &HAND[d->idx];
        int x0 = d->mirror ? d->x - P->w : d->x, cx, cy, opq = 0, vis = 0;   /* HDX = -1: the cel covers x-w .. x-1 */
        for(cy = 0; cy < P->h; cy++)
          for(cx = 0; cx < P->w; cx++)
            {
              int sx = x0 + cx, sy = d->y + cy, ux = d->mirror ? P->w - 1 - cx : cx;
              const uint8_t *o = &P->one[(cy * P->w + ux) * 4];
              if(!o[3] || sx < 0 || sy < 0 || sx >= 320 || sy >= 240) continue;
              opq++;
              vis += c5near(buf_[(size_t)(sy * 2) * 640 + sx * 2],o);
            }
#ifdef SW_HANDDUMP
        if(LOG) fprintf(LOG,"apply frame=%d set=%d idx=%d x=%d y=%d opq=%d vis=%d\n",frame_,set,d->idx,x0,d->y,opq,vis);
#endif
        if(!opq || vis * 10 < opq * 3)
          continue;                                     /* not (or hardly) on screen */
        for(cy = 0; cy < P->h; cy++)
          for(cx = 0; cx < P->w; cx++)
            {
              int sx = x0 + cx, sy = d->y + cy, ux = d->mirror ? P->w - 1 - cx : cx, i2, j2, under, hasbg = 0;
              uint32_t bg = 0;
              const uint8_t *o = &P->one[(cy * P->w + ux) * 4];
              if(sx < 0 || sy < 0 || sx >= 320 || sy >= 240) continue;
              if(o[3])
                {
                  if(!c5near(buf_[(size_t)(sy * 2) * 640 + sx * 2],o)) continue;   /* covered */
                  /* enhanced video under the pose: this frame's, or one up to
                     3 frames old (the video under a hand changes little) */
                  under = VIDF[sy * 384 + sx] > 0 && frame_ - VIDF[sy * 384 + sx] >= 0 && frame_ - VIDF[sy * 384 + sx] <= 3;
                  if(!under) bg = hand_bg(out_,P,d,x0,sx,sy,&hasbg);
                }
              else
                {
                  if(HD_SCRMASK[sy * 384 + sx]) continue;   /* something else drawn here */
                  under = 0;                                 /* already enhanced video */
                }
              for(i2 = 0; i2 < 4; i2++)
                {
                  uint32_t *dp = &out_[(size_t)(sy * 4 + i2) * 1280 + sx * 4];
                  const uint32_t *vp = &VID4[(size_t)(sy * 4 + i2) * 1280 + sx * 4];
                  for(j2 = 0; j2 < 4; j2++)
                    {
                      int hx = 4 * ux + (d->mirror ? 3 - j2 : j2);
                      const uint8_t *s = &P->hd[((size_t)(4 * cy + i2) * 4 * P->w + hx) * 4];
                      uint32_t b = under ? vp[j2] : hasbg ? bg : dp[j2], a = s[3];
                      if(!a && !under && !hasbg) continue;
                      dp[j2] = ((((b >> 16) & 0xFF) * (255 - a) + s[0] * a + 127) / 255) << 16
                             | ((((b >> 8) & 0xFF) * (255 - a) + s[1] * a + 127) / 255) << 8
                             | (((b & 0xFF) * (255 - a) + s[2] * a + 127) / 255);
                    }
                }
            }
      }
  if(NHDRAW[0])
    {
      for(k = HD_HIST - 1; k > 0; k--)
        { memcpy(HDRAW[k],HDRAW[k-1],sizeof(HDRAW[0])); NHDRAW[k] = NHDRAW[k-1]; }
    }
  NHDRAW[0] = 0;
}

/* fit  screen = a*hd + b  per channel between the on-screen (tinted) low-res
   frame and HD frame m_; robust: refit on the best 60% of samples (the
   PAUSED sign, crosshair etc. are outliers). Returns median error or 1e9. */
static double
pause_fit(const uint32_t *buf_, int F_, const hd_win_t *w_, int m_, double *A, double *B)
{
  enum { MAXS = 4096 };
  static float P[MAXS][3], H[MAXS][3], R[MAXS];
  int hw, hh, ox, oy, x, y, c, n = 0, pass, i, skipmask;
  const uint8_t *hd = hd_get(m_,&hw,&hh);
  if(!hd) return 1e9;
  ox = (E[m_].cw - E[m_].w) * F_ / 2;
  oy = (E[m_].ch - E[m_].h) * F_ / 2;
  if(hw < ox + F_*w_->w || hh < oy + F_*w_->h) return 1e9;
  {
    /* leave out what the game drew over the video (hands, HUD), unless it
       covers nearly everything (the pause tint is drawn over the whole window) */
    int msk = 0, tot = 0;
    for(y = 1; y < w_->h; y += 4)
      for(x = 1; x < w_->w; x += 4, tot++)
        msk += HD_SCRMASK[(w_->y + y) * 384 + w_->x + x] != 0;
    skipmask = msk * 10 < tot * 9;
  }
  for(y = 1; y < w_->h && n < MAXS; y += 4)
    for(x = 1; x < w_->w && n < MAXS; x += 4)
      {
        uint32_t p = buf_[(size_t)((w_->y + y) * 2) * 640 + (w_->x + x) * 2];
        if(skipmask && HD_SCRMASK[(w_->y + y) * 384 + w_->x + x]) continue;
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

/* the video did not match because the game tinted it (hit flash: magenta,
   yellow, darkened): fit  screen = a*hd + b  per channel for the frames
   around m0_ (same clip) and, if one fits, draw it enhanced with that tint.
   Pixels that do not fit the tinted video (hands, HUD, sprites) keep the
   game's. Returns the frame drawn or -1. */
static int
tint_try(const uint32_t *buf_, uint32_t *out_, int F_, int OW_, const hd_win_t *w_, int m0_, int lo_, int hi_, int frame_)
{
  int d, best = -1, hw, hh, ox, oy, x, y, c, i2, j2, tried = 0, keepmask;
  double A[3] = {1,1,1}, B[3] = {0,0,0}, be = 1e9;
  const uint8_t *hd;
  {
    /* a flat window (the flash at full strength) has nothing to enhance */
    int n = 0; double s1 = 0, s2 = 0;
    for(y = 2; y < w_->h; y += 8)
      for(x = 2; x < w_->w; x += 8)
        {
          uint32_t p = buf_[(size_t)((w_->y + y) * 2) * 640 + (w_->x + x) * 2];
          double l = (((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) / 3.0;
          s1 += l; s2 += l * l; n++;
        }
    if(!n || s2 / n - (s1 / n) * (s1 / n) < 36.0) return -1;
  }
  /* only frames already decoded (cache or prefetch): never decode here,
     that cost up to ~40 ms per frame. Ask the prefetcher for the frame
     the video should be at, for the next game frames. */
  for(i2 = 0; i2 <= hi_ - lo_ && tried < 3; i2++)
    {
      /* nearest the expected frame (middle of lo_..hi_) first */
      int mid = (lo_ + hi_ + 1) / 2, dd = (i2 + 1) / 2;
      double a2[3], b2[3], e; int m;
      d = (i2 & 1) ? mid - dd : mid + dd;
      if(d < lo_ || d > hi_) continue;
      m = m0_ + d;
      if(m < 0 || m >= N || memcmp(E[m].seq,E[m0_].seq,8) || memcmp(E[m].trk,E[m0_].trk,3)) continue;
      if(!hd_have(m)) { if(LOG) fprintf(LOG,"%d tint skip m=%d (not decoded)%c",frame_,m,10); continue; }
      tried++;
      e = pause_fit(buf_,F_,w_,m,a2,b2);
      if(e < be) { be = e; best = m; memcpy(A,a2,sizeof(A)); memcpy(B,b2,sizeof(B)); }
    }
  if(m0_ + (lo_ + hi_) / 2 >= 0 && m0_ + (lo_ + hi_) / 2 < N)
    pf_queue(m0_ + (lo_ + hi_) / 2,0);          /* the expected frame first */
  if(LOG) fprintf(LOG,"%d tint m=%d a=%.2f,%.2f,%.2f b=%.0f,%.0f,%.0f med=%.1f\n",frame_,best,A[0],A[1],A[2],B[0],B[1],B[2],be);
  if(best < 0) return -1;
  if(A[0] < 0.5 && A[1] < 0.5 && A[2] < 0.5) return -1;   /* no channel carries the picture */
  /* only a close fit is drawn pixel by pixel; a looser one (the hit
     effect also blurs) goes to detail_try, which keeps the game's picture
     and adds only fine detail, so no patchwork of the two can show */
  if(be > 8.0) return -1;
  hd = hd_get(best,&hw,&hh);
  if(!hd) return -1;
  ox = (E[best].cw - E[best].w) * F_ / 2;
  oy = (E[best].ch - E[best].h) * F_ / 2;
  if(hw < ox + F_*w_->w || hh < oy + F_*w_->h) return -1;
  {
    int msk = 0;
    for(y = 0; y < w_->h; y++)
      for(x = 0; x < w_->w; x++)
        msk += HD_SCRMASK[(w_->y + y) * 384 + w_->x + x] != 0;
    keepmask = msk * 10 < w_->w * w_->h * 9;   /* overlays (menu, hands) stay the game's */
  }
  for(y = 0; y < w_->h; y++)
    for(x = 0; x < w_->w; x++)
      {
        uint32_t p = buf_[(size_t)((w_->y + y) * 2) * 640 + (w_->x + x) * 2];
        const uint8_t *hc = &hd[((size_t)(oy + F_*y + F_/2) * hw + ox + F_*x + F_/2) * 4];
        int e = 0;
        if(keepmask && HD_SCRMASK[(w_->y + y) * 384 + w_->x + x]) continue;
        for(c = 0; c < 3; c++)
          {
            int v = (int)(A[c] * hc[c] + B[c] + 0.5);
            e += abs((int)((p >> (16 - 8*c)) & 0xFF) - (v < 0 ? 0 : v > 255 ? 255 : v));
          }
        if(e > 3 * 28) continue;                    /* not the video: keep the game's pixel */
        for(i2 = 0; i2 < F_; i2++)
          {
            uint32_t *dd = &out_[(size_t)((w_->y + y) * F_ + i2) * OW_ + (w_->x + x) * F_];
            const uint8_t *h = &hd[((size_t)(oy + F_*y + i2) * hw + ox + F_*x) * 4];
            for(j2 = 0; j2 < F_; j2++, h += 4)
              {
                uint32_t o = 0;
                for(c = 0; c < 3; c++)
                  {
                    int v = (int)(A[c] * h[c] + B[c] + 0.5);
                    v = v < 0 ? 0 : v > 255 ? 255 : v;
                    o |= (uint32_t)v << (16 - 8*c);
                  }
                dd[j2] = o;
              }
          }
      }
  return best;
}

/* hit effect the tint fit cannot follow (the game darkens AND blurs the
   picture for a few frames): keep the low frequencies (colour, brightness,
   blur) from the game's own screen and add only the fine detail of the
   expected enhanced frame:
     out = smooth 4x of screen + conf * a * (hd - smooth 4x of hd at 1x)
   conf fades to 0 as the expected frame fits the screen worse, so a wrong
   guess looks like a smooth upscale, never like a wrong picture. Only frames
   already decoded are used. Overlay pixels (hands, HUD) keep the game's. */
static int
detail_try(const uint32_t *buf_, uint32_t *out_, int F_, int OW_, const hd_win_t *w_, int m0_, int lo_, int hi_, int frame_)
{
  static float SC[384 * 288 * 3], HM[384 * 288 * 3];
  int d, best = -1, hw, hh, ox, oy, x, y, c, i2, j2, tried = 0, W = w_->w, H = w_->h;
  double A[3] = {1,1,1}, B[3] = {0,0,0}, be = 1e9, conf;
  const uint8_t *hd;
  for(i2 = 0; i2 <= hi_ - lo_ && tried < 3; i2++)
    {
      /* nearest the expected frame (middle of lo_..hi_) first */
      int mid = (lo_ + hi_ + 1) / 2, dd = (i2 + 1) / 2;
      double a2[3], b2[3], e; int m;
      d = (i2 & 1) ? mid - dd : mid + dd;
      if(d < lo_ || d > hi_) continue;
      m = m0_ + d;
      if(m < 0 || m >= N || memcmp(E[m].seq,E[m0_].seq,8) || memcmp(E[m].trk,E[m0_].trk,3)) continue;
      if(!hd_have(m)) continue;
      tried++;
      e = pause_fit(buf_,F_,w_,m,a2,b2);
      if(e < be) { be = e; best = m; memcpy(A,a2,sizeof(A)); memcpy(B,b2,sizeof(B)); }
    }
  conf = (40.0 - be) / 25.0;                   /* fit 15 or better: full detail; 40: none */
  if(conf > 1) conf = 1;
  if(LOG) fprintf(LOG,"%d detail m=%d a=%.2f,%.2f,%.2f med=%.1f conf=%.2f\n",frame_,best,A[0],A[1],A[2],be,conf);
  if(best < 0 || conf <= 0.05) return -1;
  for(c = 0; c < 3; c++) if(A[c] < 0.5 || A[c] > 1.5) return -1;   /* colour flashes, menus: not this */
  hd = hd_get(best,&hw,&hh);
  if(!hd) return -1;
  ox = (E[best].cw - E[best].w) * F_ / 2;
  oy = (E[best].ch - E[best].h) * F_ / 2;
  if(hw < ox + F_*W || hh < oy + F_*H) return -1;
  /* 1x grids: the screen, and the enhanced frame averaged down to 1x */
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++)
      {
        uint32_t p = buf_[(size_t)((w_->y + y) * 2) * 640 + (w_->x + x) * 2];
        int s3[3] = {0,0,0};
        for(i2 = 0; i2 < F_; i2++)
          {
            const uint8_t *h = &hd[((size_t)(oy + F_*y + i2) * hw + ox + F_*x) * 4];
            for(j2 = 0; j2 < F_; j2++, h += 4) { s3[0] += h[0]; s3[1] += h[1]; s3[2] += h[2]; }
          }
        for(c = 0; c < 3; c++)
          {
            SC[(y * W + x) * 3 + c] = (float)((p >> (16 - 8*c)) & 0xFF);
            HM[(y * W + x) * 3 + c] = (float)s3[c] / (F_ * F_);
          }
      }
  {
    /* 3x3 box on both grids: the game's dither must not survive in the
       low frequencies (the fine detail comes from the enhanced frame) */
    static float T[384 * 288 * 3];
    float *G2[2] = { SC, HM }; int g;
    for(g = 0; g < 2; g++)
      {
        float *Gp = G2[g];
        for(y = 0; y < H; y++)
          for(x = 0; x < W; x++)
            for(c = 0; c < 3; c++)
              {
                float sum = 0; int n = 0, yy, xx;
                for(yy = y - 1; yy <= y + 1; yy++)
                  for(xx = x - 1; xx <= x + 1; xx++)
                    if(yy >= 0 && yy < H && xx >= 0 && xx < W) { sum += Gp[(yy * W + xx) * 3 + c]; n++; }
                T[(y * W + x) * 3 + c] = sum / n;
              }
        memcpy(Gp,T,sizeof(float) * W * H * 3);
      }
  }
  for(y = 0; y < H; y++)
    {
      const uint8_t *mrow = &HD_SCRMASK[(w_->y + y) * 384 + w_->x];
      for(i2 = 0; i2 < F_; i2++)
        {
          float fy = y + (i2 + 0.5f) / F_ - 0.5f;
          int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < H ? y0 + 1 : H - 1;
          float ty = fy < 0 ? 0 : fy - y0;
          uint32_t *dd = &out_[(size_t)((w_->y + y) * F_ + i2) * OW_ + w_->x * F_];
          const uint8_t *h = &hd[((size_t)(oy + F_*y + i2) * hw + ox) * 4];
          for(x = 0; x < W; x++)
            {
              if(mrow[x]) { h += 4 * F_; continue; }          /* hands, HUD: the game's */
              for(j2 = 0; j2 < F_; j2++, h += 4)
                {
                  float fx = x + (j2 + 0.5f) / F_ - 0.5f;
                  int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < W ? x0 + 1 : W - 1;
                  float tx = fx < 0 ? 0 : fx - x0;
                  uint32_t o = 0;
                  for(c = 0; c < 3; c++)
                    {
                      const float *S0 = &SC[(y0 * W) * 3 + c], *S1 = &SC[(y1 * W) * 3 + c];
                      const float *M0 = &HM[(y0 * W) * 3 + c], *M1 = &HM[(y1 * W) * 3 + c];
                      float lo = (S0[x0*3] * (1 - tx) + S0[x1*3] * tx) * (1 - ty) + (S1[x0*3] * (1 - tx) + S1[x1*3] * tx) * ty;
                      float hm = (M0[x0*3] * (1 - tx) + M0[x1*3] * tx) * (1 - ty) + (M1[x0*3] * (1 - tx) + M1[x1*3] * tx) * ty;
                      int v = (int)(lo + conf * A[c] * (h[c] - hm) + 0.5f);
                      v = v < 0 ? 0 : v > 255 ? 255 : v;
                      o |= (uint32_t)v << (16 - 8*c);
                    }
                  dd[x * F_ + j2] = o;
                }
            }
        }
    }
  return best;
}

const uint32_t*
hd_fmv_apply(uint32_t *buf_, int width_, int height_, int frame_, int *ow_, int *oh_)
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

  /* nothing to swap while both video options are Original: skip the matching */
  for(k = 0; k < NWIN && (OPT_CUT || OPT_GAME); k++)
    {
      hd_win_t const *w = &WINS[k];
      float th[96], wt[96], mean = 0, md = 0, d = 0, wsum = 0;
      int i, m, x, y, hw, hh, ox, oy, cand = -1;
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
      if(wsum < 6.0f || mean < 3.0f)           /* black, or covered almost entirely: nothing to gain */
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
#ifdef SW_HANDDUMP
          { int64_t t0 = PT_NOW();
#endif
          m = search(w,th,wt,LAST_M[k],full,&d);
#ifdef SW_HANDDUMP
          PT_SEARCH += PT_NOW() - t0; }
#endif
          LAST_SEARCHED[k] = 1;
          cand = m;
          if(d > 4.0f || (mean < 12.0f && d > 1.5f &&
                          !(GOOD_M[k] >= 0 && m >= GOOD_M[k] && m <= GOOD_M[k] + 4 && same_track(GOOD_M[k],m))))
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
        {
          /* tinted (hit flash) or darkened video shortly after a match:
             the video goes on, about one frame per 5 game frames */
          int g = LAST_MATCH_M[k] >= 0 ? LAST_MATCH_M[k] : GOOD_M[k], el = frame_ - LAST_HIT_FRAME[k], r = -1;
          if(OPT_GFX && g >= 0 && el > 0 && el <= 30 && (is_gameplay(g) ? OPT_GAME : OPT_CUT))
            {
              r = tint_try(buf_,out,F,OW,w,g,el / 5 - 1,el / 5 + 2,frame_);
              /* still nothing just after a match (hit: darkened and blurred):
                 the game's picture with the enhanced frame's fine detail */
              if(r < 0 && !(DRAM && G->pause_addr && DRAM[G->pause_addr] == G->pause_val))
                {
                  r = detail_try(buf_,out,F,OW,w,g,el / 5 - 1,el / 5 + 2,frame_);
                  if(r >= 0) r = -2 - r;               /* drawn, but not a match: do not advance */
                }
            }
          if(r >= 0) { GOOD_M[k] = r; LAST_HIT_FRAME[k] = frame_; LAST_MATCH_M[k] = r; }
          else if(r <= -2) GOOD_M[k] = -2 - r;          /* detail transfer: follow the clip, keep the timer */
          continue;
        }
      LAST_HIT_FRAME[k] = frame_;
      LAST_MATCH_M[k] = m;
      if(!(is_gameplay(m) ? OPT_GAME : OPT_CUT))
        continue;

      if(!hd_have(m))
        {
          /* not decoded yet (a new shot): decoding here would stall the
             game for 30-40 ms. Let the worker do it; this frame stays as
             the game drew it */
          pf_queue(m,0);
          if(LOG) fprintf(LOG,"%d wait m=%d\n",frame_,m);
          continue;
        }
#ifdef SW_HANDDUMP
      { int64_t t0 = PT_NOW();
#endif
      hd = hd_get(m,&hw,&hh);
#ifdef SW_HANDDUMP
      PT_GET += PT_NOW() - t0; }
#endif
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
              uint32_t *tgt = out;
              if(mrow[x])
                {
                  /* overlay pixel: keep the game's, but remember the enhanced
                     video under it for an enhanced overlay pose (hands_apply) */
                  if(F != 4 || !opera_hd_overlay_draw)
                    continue;
                  tgt = VID4;
                  VIDF[(w->y + y) * 384 + w->x + x] = frame_;
                }
              else if(G->overlay)
                {
                  /* keep what the game drew into the video: compare the emulated
                     pixel with the average of the enhanced block */
                  uint32_t p = buf_[(size_t)((w->y + y) * 2) * 640 + (w->x + x) * 2];
                  int sr = 0, sg = 0, sb = 0, dr, dg, db;
                  p |= (p >> 5) & 0x070707;
                  for(i2 = 0; i2 < F; i2++)
                    {
                      const uint8_t *q = &hd[((size_t)(oy + F*y + i2) * hw + ox + F*x) * 4];
                      for(j2 = 0; j2 < F; j2++, q += 4) { sr += q[0]; sg += q[1]; sb += q[2]; }
                    }
                  dr = (int)((p >> 16) & 0xFF) - sr / (F*F);
                  dg = (int)((p >> 8) & 0xFF) - sg / (F*F);
                  db = (int)(p & 0xFF) - sb / (F*F);
                  if(iabs(dr) > G->overlay || iabs(dg) > G->overlay || iabs(db) > G->overlay)
                    continue;
                }
              for(i2 = 0; i2 < F; i2++)
                {
                  int py = (w->y + y) * F + i2, px = (w->x + x) * F;
                  uint32_t *d = &tgt[(size_t)py * OW + px];
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
      int i, paused = (DRAM && G->pause_addr && DRAM[G->pause_addr] == G->pause_val);   /* game's pause flag */
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

#ifdef SW_HANDDUMP
  { int64_t t0 = PT_NOW();
#endif
  hands_apply(buf_,out,F,frame_);
#ifdef SW_HANDDUMP
  PT_HANDS += PT_NOW() - t0; }
  if(LOG && PT_SEARCH + PT_GET + PT_HANDS + PT_TINT > 8000)
    fprintf(LOG,"%d slow search=%lldus get=%lldus hands=%lldus tint=%lldus\n",frame_,(long long)PT_SEARCH,(long long)PT_GET,(long long)PT_HANDS,(long long)PT_TINT);
  PT_SEARCH = PT_GET = PT_HANDS = PT_TINT = 0;
#endif
  if(OPT_GFX)
    art_apply(buf_,out,F);
  return out;
}

/*
  Auto-Win (cheat, off by default) for fighting games such as Supreme Warrior.
  Learned in the lab: emptying the opponent's health bar knocks him down; he
  gets up and the fight goes on until he has been beaten enough times. So
  every frame of a fight the player's health is kept full, the opponent's is
  capped at 1 (the next hit that lands knocks him down; the cap leaves 0
  alone so the game sees the knockdown), and while the player presses
  nothing the core throws random direction + button moves.
*/
void
hd_fmv_set_autowin(int on_)
{
  OPT_AUTOWIN = on_;
}

int
hd_fmv_has_autowin(void)
{
  return ACTIVE && G && G->hp[0][0];
}

/* the health addresses for this disc, or NULL unless all four values look
   like a fight in progress (so nothing is written anywhere else) */
static const uint32_t*
fight_hp(void)
{
  const uint32_t *a = G->hp[PACK_K < 2 ? PACK_K : 0];
  int32_t me, mm, op, om;
  if(!DRAM || !a[0]) return NULL;
  me = *(const int32_t*)&DRAM[a[0]]; mm = *(const int32_t*)&DRAM[a[1]];
  op = *(const int32_t*)&DRAM[a[2]]; om = *(const int32_t*)&DRAM[a[3]];
  if(mm < 100 || mm > 2000 || om < 100 || om > 2000 || me < 0 || me > mm || op < 0 || op > om)
    return NULL;
  return a;
}

static int
in_fight(void)
{
  return fight_hp() != NULL;
}

void
hd_fmv_frame(void)
{
  int32_t *me, *opp;
  const uint32_t *a;
  if(!OPT_AUTOWIN || !hd_fmv_has_autowin() || (a = fight_hp()) == NULL)
    return;
  me  = (int32_t*)&DRAM[a[0]];
  opp = (int32_t*)&DRAM[a[2]];
  *me = *(const int32_t*)&DRAM[a[1]];
  if(*opp > 1) *opp = 1;
}

void
hd_fmv_autofight(int *a_, int *b_, int *c_, int *u_, int *d_, int *l_, int *r_)
{
  static uint32_t seed = 12345, n = 0, cur = 0;
  if(!OPT_AUTOWIN || !hd_fmv_has_autowin() || !in_fight())
    { n = 0; return; }
  if(*a_ || *b_ || *c_ || *u_ || *d_ || *l_ || *r_)
    { n = 0; return; }                      /* the player is playing: leave it */
  /* one move every 20 polls, held for 6: a direction plus A, B or C */
  if(n % 20 == 0)
    {
      seed = seed * 1103515245u + 12345u;
      cur = (seed >> 16) % 12;
    }
  if(n % 20 < 6)
    {
      int dir = (int)(cur / 3), btn = (int)(cur % 3);
      if(dir == 0) *u_ = 1; else if(dir == 1) *d_ = 1; else if(dir == 2) *l_ = 1; else *r_ = 1;
      if(btn == 0) *a_ = 1; else if(btn == 1) *b_ = 1; else *c_ = 1;
    }
  n++;
}
