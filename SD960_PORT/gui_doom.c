// DOOM launcher - CHDK module for the PowerShot SD1000 (PLAY mode).
// Loads the standalone DOOM blob (A/DOOM.BIN) into the raw buffer's uncached alias, wires a
// callback table (timing/input/draw/file IO/arena), and jumps into it. The blob runs uncached
// (no I-cache invalidation needed). DOOM reads A/DOOM1.WAD (shareware) via the file callbacks.
//
// First boot = proof of execution: you should see "DOOM BLOB ENTERED" then DOOM's own startup
// log (or its IWAD-not-found error if DOOM1.WAD isn't on the card). Power-cycle to exit.
#include "camera_info.h"
#include "keyboard.h"
#include "lang.h"
#include "conf.h"
#include "gui.h"
#include "gui_draw.h"
#include "gui_lang.h"
#include "gui_mbox.h"
#include "modes.h"
#include "clock.h"
#include "stdlib.h"
#include "module_def.h"
#include "doom_chdk_abi.h"

extern void *hook_raw_image_addr(void);
extern void  dcache_clean_all(void);
extern unsigned char *vid_get_bitmap_fb(void);
extern void  vid_bitmap_refresh(void);     // repaint Canon's bitmap/overlay (restore screen on exit)
extern void  gui_set_need_restore(void);   // tell CHDK its OSD needs a full redraw
extern void  exit_alt(void);               // leave ALT mode -> unload module + back to clean Playback

void gui_game_menu_kbd_process();
int  gui_doom_kbd_process();
void gui_doom_draw();

// The MENU button is DOOM's own menu (we poll it as ESC). The framework's default game "menu
// button" handler would instead EXIT the module + drop out of ALT mode the instant MENU is
// pressed - which un-masks the keys to Canon (photo browser) and exposes the CHDK games menu
// behind DOOM (so SET then launches Doodle/Flappy). Override it to a no-op so MENU stays DOOM's.
// We exit DOOM only via the PRINT button (handled explicitly after the blob returns).
static void gui_doom_menu_btn(void) { }

gui_handler GUI_MODE_DOOM =
    { GUI_MODE_MODULE, gui_doom_draw, gui_doom_kbd_process, gui_doom_menu_btn, 0, GUI_MODE_FLAG_NODRAWRESTORE };

extern unsigned char *vid_get_viewport_live_fb(void);
extern int vid_get_viewport_width(void);
extern int vid_get_viewport_height(void);

#define RAW_SIZE  0xF00000u           // ~15MB for SD960


// Invalidate the entire instruction cache (ARMv5 CP15 c7,c5,0). The module runs privileged, so we
// issue this directly (icache_flush_all isn't exported to modules; only dcache_clean_all is). MCR
// is ARM-only on ARMv5, but modules build as Thumb, so force this one function to ARM and make it
// naked with its own BX LR. Must run after dcache_clean_all and before jumping into new code.
static void __attribute__((naked, target("arm"))) icache_invalidate_all(void) {
    asm volatile (
        "mov r0, #0\n\t"
        "mcr p15, 0, r0, c7, c5, 0\n\t"
        "bx  lr\n\t"
    );
}

static int launched = 0;
static int logy = 4;
static doom_cb_t cb;                  // must outlive the call (we never return)

// --- on-screen log (DOOM printf + our markers land here) ---------------------
static void scr_log(const char *s) {
    if (logy > camera_screen.height - 16) {       // wrap: clear + restart at top
        draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                       camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
        logy = 4;
    }
    draw_string_scaled(camera_screen.disp_left + 4, logy, s, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE), 1, 1);
    logy += 16;
}

// --- callbacks the blob calls -----------------------------------------------
static unsigned int cb_ticks(void)            { return (unsigned int)get_tick_count(); }
static void         cb_sleep(unsigned int ms) { msleep(ms); }

// Color: render DOOM into the camera's VIDEO/viewport layer (real YUV).
// SD960 (DIGIC 4) PLAY viewport is UYVY (4 bytes per 2 pixels: U, Y0, V, Y1).
// We dynamically fetch the viewport dimensions using CHDK API.

static unsigned char g_y[256];           // luma per index (0..255)
static signed char   g_u[256], g_v[256]; // chroma per index (signed, centered 0)
static unsigned short g_hx[1280];        // viewport column -> DOOM column (up to 1280 wide)
static unsigned char  g_vy[480];         // viewport row    -> DOOM row (up to 480 high)
static int g_cleared = 0;

static void cb_setpal(const unsigned char *rgb) {
    int i;
    for (i = 0; i < 256; i++) {
        int r = rgb[i*3], g = rgb[i*3+1], b = rgb[i*3+2];
        int y = (77*r + 150*g + 29*b) >> 8;     // 0.299R + 0.587G + 0.114B
        int u = ((b - y) * 144) >> 8;            // (B-Y)/1.772
        int v = ((r - y) * 183) >> 8;            // (R-Y)/1.402
        if (y < 0) y = 0; else if (y > 255) y = 255;
        if (u < -128) u = -128; else if (u > 127) u = 127;
        if (v < -128) v = -128; else if (v > 127) v = 127;
        g_y[i] = (unsigned char)y; g_u[i] = (signed char)u; g_v[i] = (signed char)v;
    }
}
static void cb_draw(const unsigned char *fb, int w, int h) {
    unsigned char *vp = vid_get_viewport_live_fb();
    if (!vp) return;
    int vpw = vid_get_viewport_width();
    int vph = vid_get_viewport_height();
    int vpbw = vpw * 2; // UYVY is 2 bytes per pixel
    
    if (!g_cleared) {                            // first frame: hide the UI overlay, build scale maps
        unsigned char *osd = vid_get_bitmap_fb();
        if (osd) { int n = camera_screen.buffer_width * camera_screen.height, k; for (k = 0; k < n; k++) osd[k] = 0; }
        int c; for (c = 0; c < vpw; c++) g_hx[c] = (unsigned short)(c * w / vpw);
        int r; for (r = 0; r < vph; r++) g_vy[r] = (unsigned char)(r * h / vph);
        g_cleared = 1;
    }
    int r;
    for (r = 0; r < vph; r++) {
        const unsigned char *s = fb + g_vy[r] * w;
        unsigned char *d = vp + r * vpbw;
        int g;
        for (g = 0; g < vpw; g += 2) {          // 2 viewport px = one UYVY group = 4 bytes
            unsigned int q0 = s[g_hx[g]], q1 = s[g_hx[g+1]];
            d[0] = (unsigned char)(((int)g_u[q0] + g_u[q1]) >> 1);  // shared U
            d[1] = g_y[q0];
            d[2] = (unsigned char)(((int)g_v[q0] + g_v[q1]) >> 1);  // shared V
            d[3] = g_y[q1];
            d += 4;
        }
    }
}

// Input: poll the physical keys (Canon's kbd task keeps these live even while we sit in the
// blob's loop) and emit DOOM key transitions one per call. Round-robin scan so no key starves.
#define DK_RIGHT 0xae
#define DK_LEFT  0xac
#define DK_UP    0xad
#define DK_DOWN  0xaf
#define DK_STRL  0xa0
#define DK_STRR  0xa1
#define DK_USE   0xa2
#define DK_FIRE  0xa3
#define DK_ESC   27
#define DK_ENTER 13
#define DK_RUN   (0x80 + 0x36)
static const struct { long ck; unsigned char dk; } KEYMAP[] = {
    // SD960 / DIGIC 4 keymap (Wheel + Playback)
    { KEY_UP,         DK_UP    },   // wheel up -> forward
    { KEY_DOWN,       DK_DOWN  },   // wheel down -> back
    { KEY_LEFT,       DK_STRL  },   // wheel left -> strafe left
    { KEY_RIGHT,      DK_STRR  },   // wheel right -> strafe right
    { KEY_SET,        DK_USE   },   // center button -> use / open doors
    { KEY_SET,        DK_ENTER },   // center button -> confirm (in menus)
    { KEY_SHOOT_FULL, DK_FIRE  },   // fire the gun
    { KEY_SHOOT_HALF, DK_RUN   },   // light squeeze = run
    { KEY_ZOOM_IN,    DK_RIGHT },   // turn right
    { KEY_ZOOM_OUT,   DK_LEFT  },   // turn left
    { KEY_MENU,       DK_ESC   },   // DOOM menu
};
#define NKEYS (int)(sizeof(KEYMAP) / sizeof(KEYMAP[0]))
static unsigned char g_prev[NKEYS];
static int g_scan = 0;
static int cb_getkey(int *pressed, unsigned char *key) {
    int n;
    for (n = 0; n < NKEYS; n++) {
        int i = g_scan; g_scan = (g_scan + 1) % NKEYS;
        unsigned char now = kbd_is_key_pressed(KEYMAP[i].ck) ? 1 : 0;
        if (now != g_prev[i]) {
            g_prev[i] = now;
            *pressed = now;
            *key = KEYMAP[i].dk;
            return 1;
        }
    }
    return 0;
}
// Quit: PLAYBACK button is our "get me out" on the SD960.
// Blob polls this each tick and returns from doom_main when it goes nonzero.
static int cb_should_quit(void) { return kbd_is_key_pressed(KEY_PLAYBACK) ? 1 : 0; }
static int cb_open(const char *path, int wr) {
    // Flatten every DOOM path to the card root: "A/<basename>". DOOM builds save/config paths that
    // can include a subdir (e.g. ".savegame/doomsav0.dsg") which can't exist on the card (mkdir is a
    // stub), so opening them fails and DOOM I_Errors to a blank screen. Drop the directory, keep the
    // filename. Writes get O_CREAT|O_TRUNC so the save file is actually created.
    const char *base = path, *q;
    for (q = path; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    char buf[80];
    int i = 0; buf[0] = 'A'; buf[1] = '/';
    while (base[i] && i < 76) { buf[i + 2] = base[i]; i++; }
    buf[i + 2] = 0;
    return open(buf, wr ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0777);
}
static int cb_read(int fd, void *b, int n)   { return read(fd, b, n); }
static int cb_write(int fd, const void *b, int n) { return write(fd, (void *)b, n); }
static int cb_lseek(int fd, int off, int wh) { return lseek(fd, off, wh); }
static int cb_close(int fd)                  { return close(fd); }

// --- the launch sequence -----------------------------------------------------
static void launch(void) {
    char msg[48];
    draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                   camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
    scr_log("DOOM LAUNCHER");

    // hook_raw_image_addr() hands back the UNCACHED alias (0x10xxxxxx); strip the alias bit so
    // every address we compute (blob load, arena) lives in the CACHED alias we now execute from.
    unsigned int raw = (unsigned int)hook_raw_image_addr() & ~0x10000000u;
    if (raw == 0) { sprintf(msg, "bad raw 0x%08x - abort", raw); scr_log(msg); return; }

    unsigned int blob_addr = raw;

    scr_log("loading A/DOOM.BIN ...");
    int fd = open("A/DOOM.BIN", O_RDONLY, 0777);
    if (fd < 0) { 
        scr_log("DOOM.BIN NOT FOUND on card!"); 
        sprintf(msg, "PROBED ADDR: 0x%08x", blob_addr); scr_log(msg);
        scr_log("Use this addr in build script:");
        scr_log("./build-blob.sh 0x[ADDR]");
        return; 
    }
    
    unsigned char *blob = (unsigned char *)blob_addr;
    int total = 0, r;
    while ((r = read(fd, blob + total, 65536)) > 0) total += r;
    close(fd);
    if (total < 100000) { sprintf(msg, "blob too small (%d) - abort", total); scr_log(msg); return; }

    unsigned int entry    = *(volatile unsigned int *)(blob_addr);       // header word0 = entry
    unsigned int blob_end = *(volatile unsigned int *)(blob_addr + 4);   // header word1 = arena start
    
    // sanity: entry + arena must sit inside the loaded blob / raw buffer, else a corrupt load would
    // jump into garbage and hang. Bail with a visible message instead.
    if (entry < blob_addr || entry >= blob_addr + (unsigned)total ||
        blob_end <= blob_addr || blob_end >= raw + RAW_SIZE) {
        sprintf(msg, "bad blob hdr e=%08x - abort", entry); scr_log(msg); return;
    }
    sprintf(msg, "loaded %d, entry=0x%08x", total, entry); scr_log(msg);

    cb.magic        = DOOM_ABI_MAGIC;
    cb.get_ticks_ms = cb_ticks;
    cb.sleep_ms     = cb_sleep;
    cb.get_key      = cb_getkey;
    cb.draw_frame   = cb_draw;
    cb.set_palette  = cb_setpal;
    cb.f_open = cb_open; cb.f_read = cb_read; cb.f_write = cb_write;
    cb.f_lseek = cb_lseek; cb.f_close = cb_close;
    cb.arena      = (char *)blob_end;
    cb.arena_size = (raw + RAW_SIZE) - blob_end;
    cb.log        = scr_log;
    cb.should_quit = cb_should_quit;
    sprintf(msg, "arena=%u KB", cb.arena_size / 1024); scr_log(msg);

    scr_log("flush + JUMP ->");
    dcache_clean_all();          // push freshly written code from D-cache to RAM
    icache_invalidate_all();     // drop stale I-cache lines so the new code is fetched
    ((int (*)(doom_cb_t *))entry)(&cb);     // returns when the user presses PRINT to quit
}

//-------------------------------------------------------------------
int gui_doom_kbd_process(void) { kbd_get_clicked_key(); return 0; }

void gui_doom_draw(void) {
    if (launched) return;
    launched = 1;
    launch();                      // blocks in the DOOM tick loop; returns only on PRINT (quit) or a boot abort
    // Clean exit: leave ALT mode entirely. The core loop's gui_activate_alt_mode() then unloads the
    // module (running=0) and returns to defaultGuiHandler, so Canon repaints the real Playback photo
    // instead of leaving DOOM's last frame on screen. Refresh the overlay too so nothing lingers.
    vid_bitmap_refresh();
    gui_set_need_restore();
    exit_alt();
}

int gui_doom_init(void) {
    launched = 0; logy = 4; g_cleared = 0;
    gui_set_mode(&GUI_MODE_DOOM);
    return 1;
}
int basic_module_init(void) { return gui_doom_init(); }

#include "simple_game.c"

/******************** Module Information structure ******************/

ModuleInfo _module_info =
{
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    SIMPLE_MODULE_VERSION,

    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,
    ANY_PLATFORM_ALLOWED,

    (int)"DOOM",
    MTYPE_GAME,

    &_librun.base,

    ANY_VERSION,
    CAM_SCREEN_VERSION,
    ANY_VERSION,
    ANY_VERSION,

    0,
};

/*************** END OF AUXILARY PART *******************/
