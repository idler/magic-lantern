/** \file
 * Zebra stripes, contrast edge detection and crop marks.
 *
 */
/*
 * Copyright (C) 2009 Trammell Hudson <hudson+ml@osresearch.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "zebra.h"
#include "dryos.h"
#include "bmp.h"
#include "version.h"
#include "config.h"
#include "menu.h"
#include "property.h"
#include "gui.h"
#include "lens.h"
#include "math.h"
#include "raw.h"

#include "imgconv.h"
#include "falsecolor.h"
#include "histogram.h"

#if defined(FEATURE_RAW_HISTOGRAM) || defined(FEATURE_RAW_ZEBRAS) || defined(FEATURE_RAW_SPOTMETER)
#define FEATURE_RAW_OVERLAYS
#endif


#define DIGIC_ZEBRA_REGISTER 0xC0F140cc
#define FAST_ZEBRA_GRID_COLOR 4 // invisible diagonal grid for zebras; must be unused and only from 0-15

// those colors will not be considered for histogram (so they should be very unlikely to appear in real situations)
#define MZ_WHITE 0xFA12FA34 
#define MZ_BLACK 0x00120034
#define MZ_GREEN 0x80808080

#ifdef CONFIG_KILL_FLICKER // this will block all Canon drawing routines when the camera is idle 
extern int kill_canon_gui_mode;
#endif                      // but it will display ML graphics




static void waveform_init();
//~ static void histo_init();
static void do_disp_mode_change();
static void show_overlay();
static void transparent_overlay_from_play();
static void transparent_overlay_offset_clear(void* priv, int delta);
//~ static void draw_histogram_and_waveform();
static void schedule_transparent_overlay();
//~ static void defish_draw();
//~ static void defish_draw_lv_color();
static int zebra_color_word_row(int c, int y);
static void spotmeter_step();
static int zebra_rgb_color(int underexposed, int clipR, int clipG, int clipB, int y);
static int zebra_rgb_solid_color(int underexposed, int clipR, int clipG, int clipB);

static void cropmark_cache_update_signature();
static int cropmark_cache_is_valid();
static void default_movie_cropmarks();
static void black_bars_16x9();
static void black_bars();
//~ static void defish_draw_play();

extern unsigned int log_length(int x);
extern void zoom_sharpen_step();
extern void bv_auto_update();

void lens_display_set_dirty();
void cropmark_clear_cache();
void draw_histogram_and_waveform(int);
void update_disp_mode_bits_from_params();
//~ void uyvy2yrgb(uint32_t , int* , int* , int* , int* );
int toggle_disp_mode();
static void toggle_disp_mode_menu(void *priv, int delta);

// in movie mode: skip the 16:9 bar when computing overlays
// in photo mode: compute the overlays on full-screen image
int FAST get_y_skip_offset_for_overlays()
{
    // in playback mode, and skip 16:9 bars for movies, but cover the entire area for photos
    if (!lv) return is_pure_play_movie_mode() ? os.off_169 : 0;

    // in liveview, try not to overlap top and bottom bars
    int off = 0;
    if (lv && is_movie_mode() && video_mode_resolution <= 1) off = os.off_169;
    int t = get_ml_topbar_pos();
    int b = get_ml_bottombar_pos();
    int mid = os.y0 + (os.y_ex >> 1);
    if (t < mid && t + 25 > os.y0 + off) off = t + 25 - os.x0;
    if (t > mid) b = MIN(b, t);
    if (b < os.y_max - off) off = os.y_max - b;
    return off;
}

int FAST get_y_skip_offset_for_histogram()
{
    // in playback mode, and skip 16:9 bars for movies, but cover the entire area for photos
    if (!lv) return is_pure_play_movie_mode() ? os.off_169 : 0;

    // in liveview, try not to overlap top and bottom bars
    int off = 0;
    if (lv && is_movie_mode() && video_mode_resolution <= 1) off = os.off_169;
    return off;
}

static int is_zoom_mode_so_no_zebras() 
{ 
    if (!lv) return 0;
    if (lv_dispsize == 1) return 0;
    #ifdef FEATURE_RAW_OVERLAYS
    if (raw_lv_is_enabled()) return 0; /* exception: in raw mode we can record crop videos */
    #endif
    
    return 1;
}

// true if LV image reflects accurate luma of the final picture / video
int lv_luma_is_accurate()
{
    if (is_movie_mode()) return 1;
    
    int digic_iso_gain_photo = get_digic_iso_gain_photo();
    return get_expsim() && digic_iso_gain_photo == 1024;
}

#ifdef FEATURE_SHOW_OVERLAY_FPS
static int show_lv_fps = 0; // for debugging
#endif

static struct bmp_file_t * cropmarks = 0;
static int _bmp_muted = false;
static int _bmp_unmuted = false;
int bmp_is_on() { return !_bmp_muted; }
void bmp_on();
void bmp_off();

#define WAVEFORM_WIDTH 180
#define WAVEFORM_HEIGHT 120
#define WAVEFORM_FACTOR (1 << waveform_size) // 1, 2 or 4
#define WAVEFORM_OFFSET (waveform_size <= 1 ? 80 : 0)

#define WAVEFORM_FULLSCREEN (waveform_draw && waveform_size == 2)

#define BVRAM_MIRROR_SIZE (BMPPITCH*540)

CONFIG_INT("lv.disp.profiles", disp_profiles_0, 0);

static CONFIG_INT("disp.mode", disp_mode, 0);
static CONFIG_INT("disp.mode.a", disp_mode_a, 1);
static CONFIG_INT("disp.mode.b", disp_mode_b, 1);
static CONFIG_INT("disp.mode.c", disp_mode_c, 1);
static CONFIG_INT("disp.mode.x", disp_mode_x, 1);

static CONFIG_INT( "transparent.overlay", transparent_overlay, 0);
static CONFIG_INT( "transparent.overlay.x", transparent_overlay_offx, 0);
static CONFIG_INT( "transparent.overlay.y", transparent_overlay_offy, 0);
static CONFIG_INT( "transparent.overlay.autoupd", transparent_overlay_auto_update, 1);
static int transparent_overlay_hidden = 0;

static CONFIG_INT( "global.draw",   global_draw, 3 );

#define ZEBRAS_IN_QUICKREVIEW (global_draw > 1)
#define ZEBRAS_IN_LIVEVIEW (global_draw & 1)

static CONFIG_INT( "zebra.draw",    zebra_draw, 1 );
static CONFIG_INT( "zebra.colorspace",    zebra_colorspace,   2 );// luma/rgb/lumafast
static CONFIG_INT( "zebra.thr.hi",    zebra_level_hi, 99 );
static CONFIG_INT( "zebra.thr.lo",    zebra_level_lo, 0 );
static CONFIG_INT( "zebra.rec", zebra_rec,  1 );
static CONFIG_INT( "crop.enable",   crop_enabled,   0 ); // index of crop file
static CONFIG_INT( "crop.index",    crop_index, 0 ); // index of crop file
static CONFIG_INT( "crop.movieonly", cropmark_movieonly, 0);
static CONFIG_INT("crop.playback", cropmarks_play, 0);

#define MZ_ZOOM_WHILE_RECORDING 1
#define MZ_ZOOMREC_N_FOCUS_RING 2
#define MZ_TAKEOVER_ZOOM_IN_BTN 3
#define MZ_ALWAYS_ON            4
static CONFIG_INT( "zoom.overlay", zoom_overlay_enabled, 0);
static CONFIG_INT( "zoom.overlay.trig", zoom_overlay_trigger_mode, MZ_TAKEOVER_ZOOM_IN_BTN);
static CONFIG_INT( "zoom.overlay.size", zoom_overlay_size, 1);
static CONFIG_INT( "zoom.overlay.x", zoom_overlay_x, 1);
#ifdef CONFIG_5D3
static CONFIG_INT( "zoom.overlay.pos", zoom_overlay_pos, 4); // less flicker when MZ is at the bottom
#else
static CONFIG_INT( "zoom.overlay.pos", zoom_overlay_pos, 1);
#endif
static CONFIG_INT( "zoom.overlay.split", zoom_overlay_split, 0);

int get_zoom_overlay_trigger_mode() 
{ 
#ifdef FEATURE_MAGIC_ZOOM
    if (!get_global_draw()) return 0;
    if (!zoom_overlay_enabled) return 0;
    return zoom_overlay_trigger_mode;
#else
    return 0;
#endif
}

int get_zoom_overlay_trigger_by_focus_ring()
{
#ifdef FEATURE_MAGIC_ZOOM
    int z = get_zoom_overlay_trigger_mode();
    #ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
    return z == 2 || z == 3;
    #else
    return z == 2;
    #endif
#else
    return 0;
#endif
}

static int get_zoom_overlay_trigger_by_halfshutter()
{
#ifdef FEATURE_MAGIC_ZOOM
    #ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
    int z = get_zoom_overlay_trigger_mode();
    return z == 1 || z == 3;
    #else
    return 0;
    #endif
#else
    return 0;
#endif
}

static int zoom_overlay_triggered_by_zoom_btn = 0;
static int zoom_overlay_triggered_by_focus_ring_countdown = 0;
static int is_zoom_overlay_triggered_by_zoom_btn() 
{ 
    if (!get_global_draw()) return 0;
    return zoom_overlay_triggered_by_zoom_btn;
}

static int zoom_overlay_dirty = 0;

int should_draw_zoom_overlay()
{
#ifdef FEATURE_MAGIC_ZOOM
    if (!lv) return 0;
    if (!zoom_overlay_enabled) return 0;
    if (!zebra_should_run()) return 0;
    if (EXT_MONITOR_RCA) return 0;
    if (hdmi_code == 5) return 0;
    #ifdef CONFIG_5D2
    if (display_broken_for_mz()) return 0;
    #endif
    
    if (zoom_overlay_size == 3 && video_mode_crop && is_movie_mode()) return 0;
    
    if (zoom_overlay_trigger_mode == 4) return true;

    #ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
    if (zoom_overlay_triggered_by_zoom_btn || zoom_overlay_triggered_by_focus_ring_countdown) return true;
    #else
    int zt = zoom_overlay_triggered_by_zoom_btn;
    int zm = get_zoom_overlay_trigger_mode();
    if (zt && (zm==1 || zm==2) && !recording) zt = 0; // in ZR and ZR+F modes, if triggered while recording, it should only work while recording
    if (zt || zoom_overlay_triggered_by_focus_ring_countdown) return true;
    #endif
#endif
    return false;
}

int digic_zoom_overlay_enabled()
{
    return zoom_overlay_size == 3 &&
        should_draw_zoom_overlay();
}

int nondigic_zoom_overlay_enabled()
{
    return zoom_overlay_size != 3 &&
        should_draw_zoom_overlay();
}

static CONFIG_INT( "focus.peaking", focus_peaking, 0);
//~ static CONFIG_INT( "focus.peaking.method", focus_peaking_method, 1);
static CONFIG_INT( "focus.peaking.filter.edges", focus_peaking_filter_edges, 0); // prefer texture details rather than strong edges
static CONFIG_INT( "focus.peaking.lowlight", focus_peaking_lores, 1); // use a low-res image buffer for better results in low light
static CONFIG_INT( "focus.peaking.thr", focus_peaking_pthr, 5); // 1%
static CONFIG_INT( "focus.peaking.color", focus_peaking_color, 7); // R,G,B,C,M,Y,cc1,cc2
CONFIG_INT( "focus.peaking.grayscale", focus_peaking_grayscale, 0); // R,G,B,C,M,Y,cc1,cc2

#if defined(CONFIG_DISPLAY_FILTERS) && defined(FEATURE_FOCUS_PEAK_DISP_FILTER)
static CONFIG_INT( "focus.peaking.disp", focus_peaking_disp, 0); // display as dots or blended
#else
#define focus_peaking_disp 0
#endif

int focus_peaking_as_display_filter() 
{
    #if defined(CONFIG_DISPLAY_FILTERS) && defined(FEATURE_FOCUS_PEAK_DISP_FILTER)
    return lv && focus_peaking && focus_peaking_disp;
    #else
    return 0;
    #endif
}

static CONFIG_INT( "waveform.draw", waveform_draw,
#ifdef CONFIG_4_3_SCREEN
1
#else
0
#endif
 );
static CONFIG_INT( "waveform.size", waveform_size,  0 );
static CONFIG_INT( "waveform.bg",   waveform_bg,    COLOR_ALMOST_BLACK ); // solid black

int histogram_or_small_waveform_enabled()
{
    return (
#ifdef FEATURE_HISTOGRAM
hist_draw ||
#endif
 (waveform_draw && !waveform_size)) && get_expsim(); }

static CONFIG_INT( "vectorscope.draw", vectorscope_draw, 0);
static CONFIG_INT( "vectorscope.gain", vectorscope_gain, 0);

/* runtime-configurable size */
#define vectorscope_width 256
#define vectorscope_height 256
/* 128 is also a good choice, but 256 is max. U and V are using that resolution */
#define VECTORSCOPE_WIDTH_MAX 256
#define VECTORSCOPE_HEIGHT_MAX 256


       CONFIG_INT( "clear.preview", clearscreen, 0);
static CONFIG_INT( "clear.preview.delay", clearscreen_delay, 1000); // ms
       //~ CONFIG_INT( "clear.preview.mode", clearscreen_mode, 0); // 2 is always

// keep old program logic
//~ #define clearscreen (clearscreen_enabled ? clearscreen_mode+1 : 0)

static CONFIG_INT( "spotmeter.size",        spotmeter_size, 5 );
static CONFIG_INT( "spotmeter.draw",        spotmeter_draw, 1 );
static CONFIG_INT( "spotmeter.formula",     spotmeter_formula, 0 ); // 0 percent, 1 IRE AJ, 2 IRE Piers
static CONFIG_INT( "spotmeter.position",        spotmeter_position, 1 ); // fixed / attached to AF frame

//~ static CONFIG_INT( "unified.loop", unified_loop, 2); // temporary; on/off/auto
//~ static CONFIG_INT( "zebra.density", zebra_density, 0); 
//~ static CONFIG_INT( "hd.vram", use_hd_vram, 0); 

CONFIG_INT("idle.display.turn_off.after", idle_display_turn_off_after, 0); // this also enables power saving for intervalometer
static CONFIG_INT("idle.display.dim.after", idle_display_dim_after, 0);
static CONFIG_INT("idle.display.gdraw_off.after", idle_display_global_draw_off_after, 0);
static CONFIG_INT("idle.rec", idle_rec, 0);
static CONFIG_INT("idle.shortcut.key", idle_shortcut_key, 0);
static CONFIG_INT("idle.blink", idle_blink, 1);

/**
 * Normal BMP VRAM has its origin in 720x480 center crop
 * But on HDMI you are allowed to go back 120x30 pixels (BMP_W_MINUS x BMP_H_MINUS).
 * 
 * For mirror VRAM we'll keep the same addressing mode:
 * allocate full size (960x540) and use the pointer to 720x480 center crop.
 */


static uint8_t* bvram_mirror_start = 0;
static uint8_t* bvram_mirror = 0;
uint8_t* get_bvram_mirror() { return bvram_mirror; }
//~ #define bvram_mirror bmp_vram_idle()


static int cropmark_cache_dirty = 1;
static int crop_dirty = 0;       // redraw cropmarks after some time (unit: 0.1s)

void crop_set_dirty(int value)
{
    crop_dirty = MAX(crop_dirty, value);
}

PROP_HANDLER(PROP_HOUTPUT_TYPE)
{
    extern int ml_started;
    if (ml_started) redraw();
}

#ifdef CONFIG_VARIANGLE_DISPLAY
static volatile int lcd_position = 0;
static volatile int display_dont_mirror_dirty;
PROP_HANDLER(PROP_LCD_POSITION)
{
    if (lcd_position != (int)buf[0]) display_dont_mirror_dirty = 1;
    lcd_position = buf[0];
    redraw_after(100);
}
#endif

static int idle_globaldraw_disable = 0;

int get_global_draw() // menu setting, or off if 
{
#ifdef FEATURE_GLOBAL_DRAW
    
    //~ PROP_HOUTPUT_TYPE handler only fires when Canon overlays are hidden, not restored.
    //~ So we update lv_disp_mode here instead.
    #ifdef CONFIG_EOSM
        lv_disp_mode = (MEM(0x8A01C + 0x7C) != 3);
    #endif
    
    extern int ml_started;
    if (!ml_started) return 0;
    if (!global_draw) return 0;
    
    if (PLAY_MODE) return 1; // exception, always draw stuff in play mode
    
    #ifdef CONFIG_CONSOLE
    extern int console_visible;
    if (console_visible) return 0;
    #endif
    
    if (lv && ZEBRAS_IN_LIVEVIEW)
    {
        return 
            lv_disp_mode == 0 &&
            !idle_globaldraw_disable && 
            bmp_is_on() &&
            DISPLAY_IS_ON && 
            recording != 1 && 
            #ifdef CONFIG_KILL_FLICKER
            !(lv && kill_canon_gui_mode && !canon_gui_front_buffer_disabled() && !gui_menu_shown()) &&
            #endif
            !LV_PAUSED && 
            #ifdef CONFIG_5D3
            !(hdmi_code==5 && video_mode_resolution>0) && // unusual VRAM parameters
            #endif
            job_state_ready_to_take_pic();
    }
    
    if (!lv && ZEBRAS_IN_QUICKREVIEW)
    {
        return DISPLAY_IS_ON;
    }
#endif
    return 0;
}

int get_global_draw_setting() // whatever is set in menu
{
    return global_draw;
}

/** Store the waveform data for each of the WAVEFORM_WIDTH bins with
 * 128 levels
 */
static uint8_t* waveform = 0;
#define WAVEFORM_UNSAFE(x,y) (waveform[(x) + (y) * WAVEFORM_WIDTH])
#define WAVEFORM(x,y) (waveform[COERCE((x), 0, WAVEFORM_WIDTH-1) + COERCE((y), 0, WAVEFORM_HEIGHT-1) * WAVEFORM_WIDTH])

#ifdef FEATURE_VECTORSCOPE

static uint8_t *vectorscope = NULL;

/* helper to draw <count> pixels at given position. no wrap checks when <count> is greater 1 */
static void 
vectorscope_putpixels(uint8_t *bmp_buf, int x_pos, int y_pos, uint8_t color, uint8_t count)
{
    int pos = x_pos + y_pos * vectorscope_width;

    while(count--)
    {
        bmp_buf[pos++] = 255 - color;
    }
}

/* another helper that draws a color dot at given position.
   <xc> and <yc> specify the center of our scope graphic.
   <frac_x> and <frac_y> are in 1/2048th units and specify the relative dot position.
 */
static void 
vectorscope_putblock(uint8_t *bmp_buf, int xc, int yc, uint8_t color, int32_t frac_x, int32_t frac_y)
{
    int x_pos = xc + (((int32_t)vectorscope_width * frac_x) >> 12);
    int y_pos = yc + ((-(int32_t)vectorscope_height * frac_y) >> 12);

    vectorscope_putpixels(bmp_buf, x_pos + 0, y_pos - 4, color, 1);
    vectorscope_putpixels(bmp_buf, x_pos + 0, y_pos + 4, color, 1);

    vectorscope_putpixels(bmp_buf, x_pos - 3, y_pos - 3, color, 7);
    vectorscope_putpixels(bmp_buf, x_pos - 3, y_pos - 2, color, 7);
    vectorscope_putpixels(bmp_buf, x_pos - 3, y_pos - 1, color, 7);
    vectorscope_putpixels(bmp_buf, x_pos - 4, y_pos + 0, color, 9);
    vectorscope_putpixels(bmp_buf, x_pos - 3, y_pos + 1, color, 7);
    vectorscope_putpixels(bmp_buf, x_pos - 3, y_pos + 2, color, 7);
    vectorscope_putpixels(bmp_buf, x_pos - 3, y_pos + 3, color, 7);
}

/* draws the overlay: circle with color dots. */
static void vectorscope_paint(uint8_t *bmp_buf, uint32_t x_origin, uint32_t y_origin)
{    
    //int r = vectorscope_height/2 - 1;
    int xc = x_origin + (vectorscope_width >> 1);
    int yc = y_origin + (vectorscope_height >> 1);

    /* red block at U=-14.7% V=61.5% => U=-304/2048th V=1259/2048th */
    vectorscope_putblock(bmp_buf, xc, yc, 8, -302, 1259);
    /* green block */
    vectorscope_putblock(bmp_buf, xc, yc, 7, -593, -1055);
    /* blue block */
    vectorscope_putblock(bmp_buf, xc, yc, 9, 895, -204);
    /* cyan block */
    vectorscope_putblock(bmp_buf, xc, yc, 5, 301, -1259);
    /* magenta block */
    vectorscope_putblock(bmp_buf, xc, yc, 14, 592, 1055);
    /* yellow block */
    vectorscope_putblock(bmp_buf, xc, yc, 15, -893, 204);
}

static void
vectorscope_clear()
{
    if(vectorscope != NULL)
    {
        bzero32(vectorscope, vectorscope_width * vectorscope_height * sizeof(uint8_t));
    }
}

static void
vectorscope_init()
{
    if(vectorscope == NULL)
    {
        vectorscope = SmallAlloc(VECTORSCOPE_WIDTH_MAX * VECTORSCOPE_HEIGHT_MAX * sizeof(uint8_t));
        vectorscope_clear();
    }
}

static int vectorscope_coord_uv_to_pos(int U, int V)
{
    /* convert YUV to vectorscope position */
    V *= vectorscope_height;
    V >>= 8;
    V += vectorscope_height >> 1;

    U *= vectorscope_width;
    U >>= 8;
    U += vectorscope_width >> 1;

    int pos = U + V * vectorscope_width;
    
    return pos;
}

static void
vectorscope_addpixel(uint8_t y, int8_t u, int8_t v)
{
    if(vectorscope == NULL)
    {
        return;
    }
    
    int V = -v << vectorscope_gain;
    int U = u << vectorscope_gain;
    
    int r = U*U + V*V;
    const int r_sqrt = (int)sqrtf(r);
    if (r > 124*124)
    {
        /* almost out of circle, mark it with red */
        for (int R = 124; R < 128; R++)
        {
            int c = U * R / r_sqrt;
            int s = V * R / r_sqrt;
            int pos = vectorscope_coord_uv_to_pos(c, s);
            vectorscope[pos] = 255 - COLOR_RED;
        }
    }
    else
    {
        if (vectorscope_gain)
        {
            /* simulate better resolution */
            U += rand()%2;
            V += rand()%2;
        }
        
        int pos = vectorscope_coord_uv_to_pos(U, V);
        
        /* increase luminance at this position. when reaching 4*0x2A, we are at maximum. */
        if(vectorscope[pos] < (0x2A << 2))
        {
            vectorscope[pos]++;
        }
    }
}

/* memcpy the second part of vectorscope buffer. uses only few resources */
static void
vectorscope_draw_image(uint32_t x_origin, uint32_t y_origin)
{    
    if(vectorscope == NULL)
    {
        return;
    }

    uint8_t * const bvram = bmp_vram();
    if (!bvram)
    {
        return;
    }

    vectorscope_paint(vectorscope, 0, 0);

    const uint32_t vsh2 = vectorscope_height >> 1;
    const int r = vsh2 - 1;
    const int r_plus1_square = (r+1)*(r+1);
    const int r_minus1_square = (r-1)*(r-1);

    for(uint32_t y = 0; y < vectorscope_height; y++)
    {
        #ifdef CONFIG_4_3_SCREEN
        uint8_t *bmp_buf = &(bvram[BM(x_origin, y_origin + (EXT_MONITOR_CONNECTED ? y : y*8/9))]);
        #else
        uint8_t *bmp_buf = &(bvram[BM(x_origin, y_origin+y)]);
        #endif

        const int yc = y - vsh2;
        const int yc_square = yc * yc;
        const int yc_663div1024 = (yc * 663) >> 10;

        for(uint32_t x = 0; x < vectorscope_width; x++)
        {
            uint8_t brightness = vectorscope[x + y*vectorscope_width];

            int xc = x - vsh2;
            int xc_square = xc * xc;
            int xc_plus_yc_square = xc_square + yc_square;
            int inside_circle = xc_plus_yc_square < r_minus1_square;
            int on_circle = !inside_circle && xc_plus_yc_square <= r_plus1_square;
            // kdenlive vectorscope:
            // center: 175,180
            // I: 83,38   => dx=-92, dy=142
            // Q: 320,87  => dx=145, dy=93
            // let's say 660/1024 is a good approximation of the slope

            // wikipedia image:
            // center: 318, 294
            // I: 171, 68  => 147,226
            // Q: 545, 147 => 227,147
            // => 663/1024 is a better approximation

            int on_axis = (x==vectorscope_width/2) || (y==vsh2) || (inside_circle && (xc==yc_663div1024 || -xc*663/1024==yc));

            if (on_circle || (on_axis && brightness==0))
            {
                //#ifdef CONFIG_4_3_SCREEN
                bmp_buf[x] = 60;
                //#else
                //bmp_buf[x] = COLOR_BLACK;
                //#endif
            }
            else if (inside_circle)
            {
                /* paint (semi)transparent when no pixels in this color range */
                if (brightness == 0)
                {
                    //#ifdef CONFIG_4_3_SCREEN
                    bmp_buf[x] = COLOR_WHITE; // semitransparent looks bad
                    //#else
                    //bmp_buf[x] = (x+y)%2 ? COLOR_WHITE : 0;
                    //#endif
                }
                else if (brightness > (0x2A << 2))
                {
                    /* some fake fixed color, for overlays */
                    bmp_buf[x] = 255 - brightness;
                }
                else if (brightness <= (0x29 << 2))
                {
                    /* 0x26 is the palette color for black plus max 0x29 until white */
                    bmp_buf[x] = 0x26 + (brightness >> 2);
                }
                else
                {   /* overflow */
                    bmp_buf[x] = COLOR_YELLOW;
                }
            }
        }
    }
}

static MENU_UPDATE_FUNC(vectorscope_update)
{
    if (vectorscope_draw && vectorscope_gain)
        MENU_SET_VALUE("ON, 2x");
}
#endif

/** Generate the histogram data from the YUV frame buffer.
 *
 * Walk the frame buffer two pixels at a time, in 32-bit chunks,
 * to avoid err70 while recording.
 *
 * Average two adjacent pixels to try to reduce noise slightly.
 *
 * Update the hist_max for the largest number of bin entries found
 * to scale the histogram to fit the display box from top to
 * bottom.
 */

#if defined(FEATURE_HISTOGRAM) || defined(FEATURE_WAVEFORM) || defined(FEATURE_VECTORSCOPE)
void // fixme: should be static
hist_build()
{
    struct vram_info * lv = get_yuv422_vram();
    uint32_t* buf = (uint32_t*)lv->vram;

    int x,y;

    #ifdef FEATURE_HISTOGRAM
    memset(&histogram, 0, sizeof(histogram));
    #endif

    #ifdef FEATURE_WAVEFORM
    if (waveform_draw)
    {
        waveform_init();
    }
    #endif
    
    #ifdef FEATURE_VECTORSCOPE
    if (vectorscope_draw)
    {
        vectorscope_init();
        vectorscope_clear();
    }
    #endif
    
    int mz = nondigic_zoom_overlay_enabled();
    int off = get_y_skip_offset_for_histogram();
    int yoffset = 0;
    for( y = os.y0 + off, yoffset = y * vram_lv.pitch; y < os.y_max - off; y += 2, yoffset += vram_lv.pitch )
    {
        for( x = os.x0 ; x < os.x_max ; x += 2 )
        {
            uint32_t pixel = buf[(yoffset + (BM2LV_X(x) << 1)) >> 2];

            // ignore magic zoom borders
            if (mz && (pixel == MZ_WHITE || pixel == MZ_BLACK || pixel == MZ_GREEN))
                continue;

            int Y;

            #ifdef FEATURE_HISTOGRAM
            if (hist_colorspace == 1 && !EXT_MONITOR_RCA) // rgb
            {
                int R, G, B;
                //~ uyvy2yrgb(pixel, &Y, &R, &G, &B);
                COMPUTE_UYVY2YRGB(pixel, Y, R, G, B);
                // YRGB range: 0-255
                uint32_t R_level = (R * HIST_WIDTH) >> 8;
                uint32_t G_level = (G * HIST_WIDTH) >> 8;
                uint32_t B_level = (B * HIST_WIDTH) >> 8;
                
                histogram.hist_r[R_level & (HIST_WIDTH-1)]++;
                histogram.hist_g[G_level & (HIST_WIDTH-1)]++;
                histogram.hist_b[B_level & (HIST_WIDTH-1)]++;
            }
            else // luma
            #endif

            #if defined(FEATURE_HISTOGRAM) || defined(FEATURE_WAVEFORM)
            {
                uint32_t p1 = ((pixel >> 16) & 0xFF00) >> 8;
                uint32_t p2 = ((pixel >>  0) & 0xFF00) >> 8;
                Y = (p1+p2) >> 1; 
            }
            #endif

            #ifdef FEATURE_HISTOGRAM
            histogram.total_px++;
            uint32_t hist_level = (Y * HIST_WIDTH) >> 8;

            // Ignore the 0 bin.  It generates too much noise
            unsigned count = ++ (histogram.hist[ hist_level & (HIST_WIDTH-1)]);
            if( hist_level && count > histogram.max )
                histogram.max = count;
            #endif
            
            #ifdef FEATURE_WAVEFORM
            // Update the waveform plot
            if (waveform_draw) 
            {
                uint8_t* w = &WAVEFORM(((x-os.x0) * WAVEFORM_WIDTH) / os.x_ex, (Y * WAVEFORM_HEIGHT) >> 8);
                if ((*w) < 250) (*w)++;
            }
            #endif
            
            #ifdef FEATURE_VECTORSCOPE
            if (vectorscope_draw)
            {
                int8_t U = (pixel >>  0) & 0xFF;
                int8_t V = (pixel >> 16) & 0xFF;
                vectorscope_addpixel(Y, U, V);
            }
            #endif
        }
    }
}
#endif

#ifdef FEATURE_RAW_OVERLAYS

int can_use_raw_overlays_photo()
{
    // MRAW/SRAW are causing trouble, figure out why
    // RAW and RAW+JPEG are OK
    if ((pic_quality & 0xFE00FF) == (PICQ_RAW & 0xFE00FF))
        return 1;

    return 0;
}

int can_use_raw_overlays()
{
    if (QR_MODE && can_use_raw_overlays_photo())
        return 1;
    
    if (lv && raw_lv_is_enabled())
        return 1;
    
    return 0;
}

int can_use_raw_overlays_menu()
{
    if (can_use_raw_overlays_photo())
        return 1;

    if (lv && raw_lv_is_enabled())
        return 1;

    int raw = pic_quality & 0x60000;
    if (lv && !is_movie_mode() && raw)
        return 1;

    return 0;
}

#endif

#ifdef FEATURE_RAW_ZEBRAS

static CONFIG_INT("raw.zebra", raw_zebra_enable, 1);

static void FAST draw_zebras_raw()
{
    if (!raw_update_params()) return;

    uint8_t * bvram = bmp_vram();
    if (!bvram) return;

    int white = raw_info.white_level;
    int underexposed = ev_to_raw(- (raw_info.dynamic_range - 100) / 100.0);

    for (int i = os.y0; i < os.y_max; i ++)
    {
        int y = BM2RAW_Y(i);

        for (int j = os.x0; j < os.x_max; j ++)
        {
            int x = BM2RAW_X(j);

            /* for dual ISO: show solid zebras if both sub-images are overexposed */
            /* show semitransparent zebras if only one is overexposed */
            /* impact on normal ISOs should be minimal */
            int dark = (i + j) % 4;
            int r = dark ? raw_red_pixel_dark(x, y) : raw_red_pixel_bright(x, y);
            int g = dark ? raw_green_pixel_dark(x, y) : raw_green_pixel_bright(x, y);
            int b = dark ? raw_blue_pixel_dark(x, y) : raw_blue_pixel_bright(x, y);
            int u = raw_green_pixel_bright(x, y);

            /* define this to check if color channels are identified correctly */
            #undef RAW_ZEBRA_TEST
            
            #ifdef RAW_ZEBRA_TEST
            {
                uint32_t* lv = get_yuv422_vram()->vram;
                int R = r > raw_info.black_level+16 ? (int)(log2f((r - raw_info.black_level) / 16.0f) * 255 / 10) : 1;
                int G = g > raw_info.black_level+16 ? (int)(log2f((g - raw_info.black_level) / 32.0f) * 255 / 10) : 1;
                int B = b > raw_info.black_level+16 ? (int)(log2f((b - raw_info.black_level) / 16.0f) * 255 / 10) : 1;
                int Y =  (0.257 * R) + (0.504 * G) + (0.098 * B);
                int U = -(0.148 * R) - (0.291 * G) + (0.439 * B);
                int V =  (0.439 * R) - (0.368 * G) - (0.071 * B);
                lv[BM2LV(j,i)/4] = UYVY_PACK(U,Y,V,Y);
                continue;
            }
            #endif
            
            int c = zebra_rgb_solid_color(u <= underexposed, r > white, g > white, b > white);
            if (c)
            {
                uint8_t* bp = (uint8_t*) &bvram[BM(j,i)];
                uint8_t* mp = (uint8_t*) &bvram_mirror[BM(j,i)];

                #define BP (*bp)
                #define MP (*mp)
                if (BP != 0 && BP != MP) continue;
                if ((MP & 0x80)) continue;
                
                BP = MP = c;
                    
                #undef MP
                #undef BP
            }
        }

        if (!PLAY_OR_QR_MODE) break;
    }
}

static void FAST draw_zebras_raw_lv()
{
    if (!raw_update_params()) return;

    uint8_t * const bvram = bmp_vram_real();
    if (!bvram) return;
    uint8_t * const bvram_mirror = get_bvram_mirror();
    if (!bvram_mirror) return;

    int white = raw_info.white_level;
    if (white > 16383) white = 15000;
    int underexposed = ev_to_raw(- (raw_info.dynamic_range - 100) / 100.0);

    int off = get_y_skip_offset_for_overlays();
    for(int i = os.y0 + off; i < os.y_max - off; i += 2 )
    {
        uint64_t * const b_row = (uint64_t*)( bvram        + BM_R(i)       );  // 2 pixels
        uint64_t * const m_row = (uint64_t*)( bvram_mirror + BM_R(i)       );  // 2 pixels
        
        uint64_t* bp;  // through bmp vram
        uint64_t* mp;  // through mirror

        int y = BM2RAW_Y(i);
        if (y < raw_info.active_area.y1 || y > raw_info.active_area.y2) continue;
        
        for (int j = os.x0; j < os.x_max; j += 8)
        {
            bp = b_row + j/8;
            mp = m_row + j/8;
            
            #define BP (*bp)
            #define MP (*mp)
            
            if (BP != 0 && BP != MP) { little_cleanup(bp, mp); continue; }
            if ((MP & 0x80808080)) continue;
            
            int x = BM2RAW_X(j);
            
            if (x < raw_info.active_area.x1 || x > raw_info.active_area.x2) continue;
            
            /* for dual ISO: use dark lines for overexposure and bright lines for underexposure */
            int r = raw_red_pixel_dark(x, y);
            int g = raw_green_pixel_dark(x, y);
            int b = raw_blue_pixel_dark(x, y);
            int u = raw_green_pixel_bright(x, y);

            uint64_t c = zebra_rgb_solid_color(u <= underexposed, r > white, g > white, b > white);
            c = c | (c << 32);

            MP = BP = c;

            #undef BP
            #undef MP
        }
    }
}

static MENU_UPDATE_FUNC(raw_zebra_update)
{
    if (!can_use_raw_overlays_menu())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Set picture quality to RAW in Canon menu.");
    else if (raw_zebra_enable)
        MENU_SET_WARNING(MENU_WARN_INFO, "Will use RAW RGB zebras in LiveView and after taking a pic.");
}
#endif

int get_under_and_over_exposure(int thr_lo, int thr_hi, int* under, int* over)
{
    *under = -1;
    *over = -1;
    struct vram_info * lv = get_yuv422_vram();
    if (!lv) return -1;

    *under = 0;
    *over = 0;
    int total = 0;
    void* vram = lv->vram;
    int x,y;
    for( y = os.y0 ; y < os.y_max; y ++ )
    {
        uint32_t * const v_row = (uint32_t*)( vram + BM2LV_R(y) );
        for( x = os.x0 ; x < os.x_max ; x += 2 )
        {
            uint32_t pixel = v_row[x >> 1];
            
            int Y, R, G, B;
            //~ uyvy2yrgb(pixel, &Y, &R, &G, &B);
            COMPUTE_UYVY2YRGB(pixel, Y, R, G, B);
            
            int M = MAX(R,G);
            M = MAX(M, B);
            if (pixel && Y < thr_lo) (*under)++; // try to ignore black bars
            if (M > thr_hi) (*over)++;
            total++;
        }
    }
    return total;
}

#ifdef FEATURE_ZEBRA
#define ZEBRA_COLOR_WORD_SOLID(x) ( (x) | (x)<<8 | (x)<<16 | (x)<<24 )
static int zebra_rgb_color(int underexposed, int clipR, int clipG, int clipB, int y)
{
    if (underexposed) return zebra_color_word_row(79, y);
    
    switch ((clipR ? 4 : 0) |
            (clipG ? 2 : 0) |
            (clipB ? 1 : 0))
    {
        case 0b111: return ZEBRA_COLOR_WORD_SOLID(COLOR_BLACK);
        case 0b110: return ZEBRA_COLOR_WORD_SOLID(COLOR_YELLOW);
        case 0b101: return ZEBRA_COLOR_WORD_SOLID(COLOR_MAGENTA);
        case 0b011: return ZEBRA_COLOR_WORD_SOLID(COLOR_CYAN);
        case 0b100: return y&2 ? 0 : ZEBRA_COLOR_WORD_SOLID(COLOR_RED);
        case 0b001: return y&2 ? 0 : ZEBRA_COLOR_WORD_SOLID(COLOR_BLUE);
        case 0b010: return y&2 ? 0 : ZEBRA_COLOR_WORD_SOLID(COLOR_GREEN2);
        default: return 0;
    }
}

static int zebra_rgb_solid_color(int underexposed, int clipR, int clipG, int clipB)
{
    if (underexposed) return ZEBRA_COLOR_WORD_SOLID(79);
    
    switch ((clipR ? 4 : 0) |
            (clipG ? 2 : 0) |
            (clipB ? 1 : 0))
    {
        case 0b111: return ZEBRA_COLOR_WORD_SOLID(COLOR_BLACK);
        case 0b110: return ZEBRA_COLOR_WORD_SOLID(COLOR_YELLOW);
        case 0b101: return ZEBRA_COLOR_WORD_SOLID(COLOR_MAGENTA);
        case 0b011: return ZEBRA_COLOR_WORD_SOLID(COLOR_CYAN);
        case 0b100: return ZEBRA_COLOR_WORD_SOLID(COLOR_RED);
        case 0b001: return ZEBRA_COLOR_WORD_SOLID(COLOR_BLUE);
        case 0b010: return ZEBRA_COLOR_WORD_SOLID(COLOR_GREEN2);
        default: return 0;
    }
}
#endif

#ifdef FEATURE_WAVEFORM
/** Draw the waveform image into the bitmap framebuffer.
 *
 * Draw one pixel at a time; it seems to be ok with err70.
 * Since there is plenty of math per pixel this doesn't
 * swamp the bitmap framebuffer hardware.
 */

static void
waveform_draw_image(
    unsigned        x_origin,
    unsigned        y_origin,
    unsigned        height
)
{
    if (!PLAY_OR_QR_MODE)
    {
        if (!lv_luma_is_accurate()) return;
    }

    // Ensure that x_origin is quad-word aligned
    x_origin &= ~3;
    
    uint8_t * const bvram = bmp_vram();
    if (!bvram) return;
    unsigned pitch = BMPPITCH;
    if( histogram.max == 0 )
        histogram.max = 1;

    int i, y;

    // vertical line up to the hist size
    for (int k = 0; k < WAVEFORM_FACTOR; k++)
    {
        for( y=WAVEFORM_HEIGHT-1 ; y>=0 ; y-- )
        {
            int y_bmp = y_origin + y * height / WAVEFORM_HEIGHT + k;
            if (y_bmp < 0) continue;
            if (y_bmp >= BMP_H_PLUS) continue;

            uint8_t * row = bvram + x_origin + y_bmp * pitch;
            //int y_next = (y-1) * height / WAVEFORM_HEIGHT;
            uint32_t pixel = 0;
            int w = WAVEFORM_WIDTH*WAVEFORM_FACTOR;
            for( i=0 ; i<w; i++ )
            {
                uint32_t count = WAVEFORM_UNSAFE( i / WAVEFORM_FACTOR, WAVEFORM_HEIGHT - y - 1);
                if (height < WAVEFORM_HEIGHT)
                { // smooth it a bit to reduce aliasing; not perfect, but works.. sort of
                    count += WAVEFORM_UNSAFE( i / WAVEFORM_FACTOR, WAVEFORM_HEIGHT - y - 1);
                    //~ count /= 2;
                }
                // Scale to a grayscale
                count = (count * 42) >> 7;
                if( count > 42 - 5 )
                    count = COLOR_RED;
                else
                if( count >  0 )
                    count += 38 + 5;
                else
                // Draw a series of colored scales
                if( y == (WAVEFORM_HEIGHT*1)>>2 )
                    count = COLOR_BLUE;
                else
                if( y == (WAVEFORM_HEIGHT*2)>>2 )
                    count = 0xE; // pink
                else
                if( y == (WAVEFORM_HEIGHT*3)>>2 )
                    count = COLOR_BLUE;
                else
                    count = waveform_bg; // transparent

                pixel |= (count << ((i & 3)<<3));

                if( (i & 3) != 3 )
                    continue;

                // Draw the pixel, rounding down to the nearest
                // quad word write (and then nop to avoid err70).
                *(uint32_t*) ALIGN32(row + i) = pixel;
                #ifdef CONFIG_500D // err70?!
                asm( "nop" );
                asm( "nop" );
                asm( "nop" );
                asm( "nop" );
                asm( "nop" );
                asm( "nop" );
                asm( "nop" );
                asm( "nop" );
                #endif
                pixel = 0;
            }
        }
        bmp_draw_rect(60, x_origin-1, y_origin-1, WAVEFORM_WIDTH*WAVEFORM_FACTOR+1, height+1);
    }
}
#endif

static FILE * g_aj_logfile = INVALID_PTR;
static unsigned int aj_create_log_file( char * name)
{
   g_aj_logfile = FIO_CreateFileEx( name );
   if ( g_aj_logfile == INVALID_PTR )
   {
      bmp_printf( FONT_SMALL, 120, 40, "FCreate: Err %s", name );
      return( 0 );  // FAILURE
   }
   return( 1 );  // SUCCESS
}

static void aj_close_log_file( void )
{
   if (g_aj_logfile == INVALID_PTR)
      return;
   FIO_CloseFile( g_aj_logfile );
   g_aj_logfile = INVALID_PTR;
}

void dump_seg(uint32_t start, uint32_t size, char* filename)
{
    DEBUG();
    aj_create_log_file(filename);
    FIO_WriteFile( g_aj_logfile, (const void *) start, size );
    aj_close_log_file();
    DEBUG();
}

void dump_big_seg(int k, char* filename)
{
    DEBUG();
    aj_create_log_file(filename);
    
    int i;
    for (i = 0; i < 16; i++)
    {
        DEBUG();
        uint32_t start = (k << 28 | i << 24);
        bmp_printf(FONT_LARGE, 50, 50, "DUMP %x %8x ", i, start);
        FIO_WriteFile( g_aj_logfile, (const void *) start, 0x1000000 );
    }
    
    aj_close_log_file();
    DEBUG();
}

static int fps_ticks = 0;

static void waveform_init()
{
#ifdef FEATURE_WAVEFORM
    if (!waveform)
        waveform = SmallAlloc(WAVEFORM_WIDTH * WAVEFORM_HEIGHT);
    bzero32(waveform, WAVEFORM_WIDTH * WAVEFORM_HEIGHT);
#endif
}

static void bvram_mirror_clear()
{
    ASSERT(bvram_mirror_start);
    BMP_LOCK( bzero32(bvram_mirror_start, BMP_VRAM_SIZE); )
    cropmark_cache_dirty = 1;
}
void bvram_mirror_init()
{
    if (!bvram_mirror_start)
    {
        // shoot_malloc is not that stable
        //~ #if defined(CONFIG_600D) || defined(CONFIG_1100D)
        //~ bvram_mirror_start = (void*)shoot_malloc(BMP_VRAM_SIZE); // there's little memory available in system pool
        //~ #else
        #if defined(RSCMGR_MEMORY_PATCH_END)
        extern unsigned int ml_reserved_mem;
        bvram_mirror_start = RESTARTSTART + ml_reserved_mem;
        #elif defined(CONFIG_EOSM)
        bvram_mirror_start = (void*)malloc(BMP_VRAM_SIZE); // malloc is big!    
        #else
        bvram_mirror_start = (void*)AllocateMemory(BMP_VRAM_SIZE);
        #endif
        //~ #endif
        if (!bvram_mirror_start) 
        {   
            while(1)
            {
                bmp_printf(FONT_MED, 30, 30, "Failed to allocate BVRAM mirror");
                msleep(100);
            }
        }
        // to keep the same addressing mode as with normal BMP VRAM - origin in 720x480 center crop
        bvram_mirror = bvram_mirror_start + BMP_HDMI_OFFSET;
        bvram_mirror_clear();
    }
}

#ifdef FEATURE_FOCUS_PEAK
static int get_focus_color(int thr, int d)
{
    return
        focus_peaking_color == 0 ? COLOR_RED :
        focus_peaking_color == 1 ? 7 :
        focus_peaking_color == 2 ? COLOR_BLUE :
        focus_peaking_color == 3 ? 5 :
        focus_peaking_color == 4 ? 14 :
        focus_peaking_color == 5 ? 15 :
        focus_peaking_color == 6 ?  (thr > 50 ? COLOR_RED :
                                     thr > 40 ? 19 /*orange*/ :
                                     thr > 30 ? 15 /*yellow*/ :
                                     thr > 20 ? 5 /*cyan*/ : 
                                     9 /*light blue*/) :
        focus_peaking_color == 7 ? ( d > 50 ? COLOR_RED :
                                     d > 40 ? 19 /*orange*/ :
                                     d > 30 ? 15 /*yellow*/ :
                                     d > 20 ? 5 /*cyan*/ : 
                                     9 /*light blue*/) : 1;
}
#endif

#ifdef FEATURE_ZEBRA
static inline int zebra_color_word_row(int c, int y)
{
    if (!c) return 0;
    
    uint32_t cw = 0;
    switch(y % 4)
    {
        case 0:
            cw  = c  | c  << 8;
            break;
        case 1:
            cw  = c << 8 | c << 16;
            break;
        case 2:
            cw = c  << 16 | c << 24;
            break;
        case 3:
            cw  = c  << 24 | c ;
            break;
    }
    return cw;
}
#endif

#ifdef FEATURE_FOCUS_PEAK

#define MAX_DIRTY_PIXELS 5000


static int* dirty_pixels = 0;
static uint32_t* dirty_pixel_values = 0;
static int dirty_pixels_num = 0;
//~ static unsigned int* bm_hd_r_cache = 0;
static uint16_t bm_hd_x_cache[BMP_W_PLUS - BMP_W_MINUS];
static int bm_hd_bm2lv_sx = 0;
static int bm_hd_lv2hd_sx = 0;
static int old_peak_lores = 0;

static void zebra_update_lut()
{
    int rebuild = 0;
        
    if(unlikely(bm_hd_bm2lv_sx != bm2lv.sx))
    {
        bm_hd_bm2lv_sx = bm2lv.sx;
        rebuild = 1;
    }
    if(unlikely(bm_hd_lv2hd_sx != lv2hd.sx))
    {
        bm_hd_lv2hd_sx = lv2hd.sx;
        rebuild = 1;
    }
    if (unlikely(focus_peaking_lores != old_peak_lores))
    {
        old_peak_lores = focus_peaking_lores;
        rebuild = 1;
    }
    
    if(unlikely(rebuild))
    {
        int xStart = os.x0 + 8;
        int xEnd = os.x_max - 8;

        for (int x = xStart; x < xEnd; x += 1)
        {
            bm_hd_x_cache[x - BMP_W_MINUS] = ((focus_peaking_lores ? BM2LV_X(x) : BM2HD_X(x)) * 2) + 1;
        }        
    }
}

#endif


static int zebra_digic_dirty = 0;

#ifdef FEATURE_ZEBRA
static void draw_zebras( int Z )
{
    uint8_t * const bvram = bmp_vram_real();
    int zd = Z && zebra_draw && (lv_luma_is_accurate() || PLAY_OR_QR_MODE) && (zebra_rec || !recording); // when to draw zebras
    if (zd)
    {
        #ifdef FEATURE_RAW_ZEBRAS
        if (raw_zebra_enable && can_use_raw_overlays())
        {
            if (lv) draw_zebras_raw_lv();
            else draw_zebras_raw();
            return;
        }
        #endif
        int zlh = zebra_level_hi * 255 / 100 - 1;
        int zll = zebra_level_lo * 255 / 100;

        int only_over  = (zebra_level_hi <= 100 && zebra_level_lo ==   0);
        int only_under = (zebra_level_lo  >   0 && zebra_level_hi  > 100);
        int only_one = only_over || only_under;

        // fast zebras
        #ifdef FEATURE_ZEBRA_FAST
        /*
            C0F140cc configurable "zebra" (actually solid color)
            -------- -------- -------- --------
                                       ******** threshold
                                  ****          bmp palette entry (0-F)
                              ****              zebra color (0-F)
                            *                   type (1=under,0=over)
                     *******                    blinking flags maybe (0=no blink)
         */
        if (zebra_colorspace == 2 && (lv || only_one)) // if both under and over are enabled, fall back to regular zebras in play mode
        {
            zebra_digic_dirty = 1;
            
            // if both zebras are enabled, alternate them (can't display both at the same time)
            // if only one is enabled, show them both
            
            int parity = (get_seconds_clock() / 2) % 2;
            
            int ov = (zebra_level_hi <= 100 && (zebra_level_lo ==   0 || parity == 0));
            int un = (zebra_level_lo  >   0 && (zebra_level_hi  > 100 || parity == 1));
            
            if (ov)
                EngDrvOut(DIGIC_ZEBRA_REGISTER, 0x08000 | (FAST_ZEBRA_GRID_COLOR<<8) | zlh);
            else if (un)
                EngDrvOut(DIGIC_ZEBRA_REGISTER, 0x1B000 | (FAST_ZEBRA_GRID_COLOR<<8) | zll);

            // make invisible diagonal strips onto which the zebras will be displayed
            // only refresh this once per second
            
            static int last_s = 0;
            int s = get_seconds_clock();
            if (s == last_s) return;
            last_s = s;
            
            alter_bitmap_palette_entry(FAST_ZEBRA_GRID_COLOR, 0, 256, 256);
            int off = get_y_skip_offset_for_overlays();
            for(int y = os.y0 + off; y < os.y_max - off; y++)
            {
                #define color_zeb           zebra_color_word_row(FAST_ZEBRA_GRID_COLOR,  y)

                uint32_t * const b_row = (uint32_t*)( bvram        + BM_R(y)       );  // 4 pixels
                uint32_t * const m_row = (uint32_t*)( bvram_mirror + BM_R(y)       );  // 4 pixels
                
                uint32_t* bp;  // through bmp vram
                uint32_t* mp;  // through mirror

                for (int x = os.x0; x < os.x_max; x += 4)
                {
                    bp = b_row + (x >> 2);
                    mp = m_row + (x >> 2);
                    #define BP (*bp)
                    #define MP (*mp)
                    if (BP != 0 && BP != MP) { little_cleanup(bp, mp); continue; }
                    if ((MP & 0x80808080)) continue;
                    
                    BP = MP = color_zeb;
                        
                    #undef MP
                    #undef BP
                }
            }

            return;
        }
        #endif
        
        uint8_t * lvram = get_yuv422_vram()->vram;

        int zlr = zlh;
        int zlg = zlh;
        int zlb = zlh;

        // draw zebra in 16:9 frame
        // y is in BM coords
        int off = get_y_skip_offset_for_overlays();
        for(int y = os.y0 + off; y < os.y_max - off; y += 2 )
        {
            #define color_over           zebra_color_word_row(COLOR_RED,  y)
            #define color_under          zebra_color_word_row(COLOR_BLUE, y)
            #define color_over_2         zebra_color_word_row(COLOR_RED,  y+1)
            #define color_under_2        zebra_color_word_row(COLOR_BLUE, y+1)
            
            #define color_rgb_under      zebra_rgb_color(1, 0, 0, 0, y)
            #define color_rgb_under_2    zebra_rgb_color(1, 0, 0, 0, y+1)
            
            #define color_rgb_clipR      zebra_rgb_color(0, 1, 0, 0, y)
            #define color_rgb_clipR_2    zebra_rgb_color(0, 1, 0, 0, y+1)
            #define color_rgb_clipG      zebra_rgb_color(0, 0, 1, 0, y)
            #define color_rgb_clipG_2    zebra_rgb_color(0, 0, 1, 0, y+1)
            #define color_rgb_clipB      zebra_rgb_color(0, 0, 0, 1, y)
            #define color_rgb_clipB_2    zebra_rgb_color(0, 0, 0, 1, y+1)
            
            #define color_rgb_clipRG     zebra_rgb_color(0, 1, 1, 0, y)
            #define color_rgb_clipRG_2   zebra_rgb_color(0, 1, 1, 0, y+1)
            #define color_rgb_clipGB     zebra_rgb_color(0, 0, 1, 1, y)
            #define color_rgb_clipGB_2   zebra_rgb_color(0, 0, 1, 1, y+1)
            #define color_rgb_clipRB     zebra_rgb_color(0, 1, 0, 1, y)
            #define color_rgb_clipRB_2   zebra_rgb_color(0, 1, 0, 1, y+1)
            
            #define color_rgb_clipRGB    zebra_rgb_color(0, 1, 1, 1, y)
            #define color_rgb_clipRGB_2  zebra_rgb_color(0, 1, 1, 1, y+1)

            uint32_t * const v_row = (uint32_t*)( lvram        + BM2LV_R(y)    );  // 2 pixels
            uint32_t * const b_row = (uint32_t*)( bvram        + BM_R(y)       );  // 4 pixels
            uint32_t * const m_row = (uint32_t*)( bvram_mirror + BM_R(y)       );  // 4 pixels
            
            uint32_t* lvp; // that's a moving pointer through lv vram
            uint32_t* bp;  // through bmp vram
            uint32_t* mp;  // through mirror

            for (int x = os.x0; x < os.x_max; x += 4)
            {
                lvp = v_row + (BM2LV_X(x) >> 1);
                bp = b_row + (x >> 2);
                mp = m_row + (x >> 2);
                #define BP (*bp)
                #define MP (*mp)
                #define BN (*(bp + BMPPITCH/4))
                #define MN (*(mp + BMPPITCH/4))
                if (BP != 0 && BP != MP) { little_cleanup(bp, mp); continue; }
                if (BN != 0 && BN != MN) { little_cleanup(bp + (BMPPITCH >> 2), mp + (BMPPITCH >> 2)); continue; }
                if ((MP & 0x80808080) || (MN & 0x80808080)) continue;
                
                if (zebra_colorspace == 1 && !EXT_MONITOR_RCA) // rgb
                {
                    int Y, R, G, B;
                    //~ uyvy2yrgb(*lvp, &Y, &R, &G, &B);
                    COMPUTE_UYVY2YRGB(*lvp, Y, R, G, B);

                    if(unlikely(Y < zll)) // underexposed
                    {
                        BP = MP = color_rgb_under;
                        BN = MN = color_rgb_under_2;
                    }
                    else
                    {
                        //~ BP = MP = zebra_rgb_color(Y < zll, R > zlh, G > zlh, B > zlh, y);
                        //~ BN = MN = zebra_rgb_color(Y < zll, R > zlh, G > zlh, B > zlh, y+1);

                        if (unlikely(R > zlr)) // R clipped
                        {
                            if (unlikely(G > zlg)) // RG clipped
                            {
                                if (B > zlb) // RGB clipped (all of them)
                                {
                                    BP = MP = color_rgb_clipRGB;
                                    BN = MN = color_rgb_clipRGB_2;
                                }
                                else // only R and G clipped
                                {
                                    BP = MP = color_rgb_clipRG;
                                    BN = MN = color_rgb_clipRG_2;
                                }
                            }
                            else // R clipped, G not clipped
                            {
                                if (unlikely(B > zlb)) // only R and B clipped
                                {
                                    BP = MP = color_rgb_clipRB;
                                    BN = MN = color_rgb_clipRB_2;
                                }
                                else // only R clipped
                                {
                                    BP = MP = color_rgb_clipR;
                                    BN = MN = color_rgb_clipR_2;
                                }
                            }
                        }
                        else // R not clipped
                        {
                            if (unlikely(G > zlg)) // R not clipped, G clipped
                            {
                                if (unlikely(B > zlb)) // only G and B clipped
                                {
                                    BP = MP = color_rgb_clipGB;
                                    BN = MN = color_rgb_clipGB_2;
                                }
                                else // only G clipped
                                {
                                    BP = MP = color_rgb_clipG;
                                    BN = MN = color_rgb_clipG_2;
                                }
                            }
                            else // R not clipped, G not clipped
                            {
                                if (unlikely(B > zlb)) // only B clipped
                                {
                                    BP = MP = color_rgb_clipB;
                                    BN = MN = color_rgb_clipB_2;
                                }
                                else // nothing clipped
                                {
                                    BN = MN = BP = MP = 0;
                                }
                            }
                        }
                    }
                }
                else // luma
                {
                    int p0 = (*lvp) >> 8 & 0xFF;
                    if (unlikely(p0 > zlh))
                    {
                        BP = MP = color_over;
                        BN = MN = color_over_2;
                    }
                    else if (unlikely(p0 < zll))
                    {
                        BP = MP = color_under;
                        BN = MN = color_under_2;
                    }
                    else
                        BN = MN = BP = MP = 0;
                }
                    
                #undef MP
                #undef BP
                #undef BN
                #undef MN
            }
        }
    }
}
#endif

#ifdef FEATURE_FOCUS_PEAK

/* superseded by the peak_d2xy algorithm (2012-09-01)
static inline int peak_d1xy(uint8_t* p8)
{
    int p_cc = (int)(*p8);
    int p_rc = (int)(*(p8 + 2));
    int p_cd = (int)(*(p8 + vram_lv.pitch));
    
    int e_dx = ABS(p_rc - p_cc);
    int e_dy = ABS(p_cd - p_cc);
    
    int e = MAX(e_dx, e_dy);
    return peak_scaling[MIN(e, 255)];
}*/

static inline int peak_d2xy_sharpen(uint8_t* p8)
{
    int orig = (int)(*p8);
    int diff = orig * 4 - (int)(*(p8 + 2));
    diff -= (int)(*(p8 - 2));
    diff -= (int)(*(p8 + vram_lv.pitch));
    diff -= (int)(*(p8 - vram_lv.pitch));
    int v = orig + (diff << 2);
    return COERCE(v, 0, 255);
}

static inline int FAST calc_peak(const uint8_t* p8, const int pitch)
{
    // approximate second derivative with a Laplacian kernel:
    //     -1
    //  -1  4 -1
    //     -1
    const int p8_xmin1 = (int)(*(p8 - 2));
    const int p8_xplus1 = (int)(*(p8 + 2));
    const int p8_ymin1 = (int)(*(p8 - pitch));
    const int p8_yplus1 = (int)(*(p8 + pitch));

    int result = ((int)(*p8) * 4);
    result -= p8_xplus1 + p8_xmin1 + p8_yplus1 + p8_ymin1;

    int e = ABS(result);

    if (focus_peaking_filter_edges)
    {
        // filter out strong edges where first derivative is strong
        // as these are usually false positives
        int d1x = ABS(p8_xplus1 - p8_xmin1);
        int d1y = ABS(p8_yplus1 - p8_ymin1);
        int d1 = MAX(d1x, d1y);
        e = MAX(e - ((d1 << focus_peaking_filter_edges) >> 2), 0) * 2;
    }
    return e;
}

static inline int FAST peak_d2xy(const uint8_t* p8)
{
    return calc_peak(p8, vram_lv.pitch);
}

static inline int FAST peak_d2xy_hd(const uint8_t* p8)
{
    return calc_peak(p8, vram_hd.pitch);
}

#ifdef FEATURE_FOCUS_PEAK_DISP_FILTER

//~ static inline int peak_blend_solid(uint32_t* s, int e, int thr) { return 0x4C7F4CD5; }
//~ static inline int peak_blend_raw(uint32_t* s, int e) { return (e << 8) | (e << 24); }
static inline int peak_blend_alpha(uint32_t* s, int e)
{
    // e=0 => cold (original color)
    // e=255 => hot (red)
    
    uint8_t* s8u = (uint8_t*)s;
    int8_t*  s8s = (int8_t*)s;

    int y_cold = *(s8u+1);
    int u_cold = *(s8s);
    int v_cold = *(s8s+2);
    
    // red (255,0,0)
    const int y_hot = 76;
    const int u_hot = -43;
    const int v_hot = 127;
    
    int er = 255-e;
    int y = (y_cold * er + y_hot * e) >> 8;
    int u = (u_cold * er + u_hot * e) >> 8;
    int v = (v_cold * er + v_hot * e) >> 8;
    
    return UYVY_PACK(u,y,v,y);
}

static int peak_scaling[256];

void FAST peak_disp_filter()
{
    uint32_t* src_buf;
    uint32_t* dst_buf;
    if (lv)
    {
        display_filter_get_buffers(&src_buf, &dst_buf);
    }
    else if (PLAY_OR_QR_MODE)
    {
        void* aux_buf = (void*)YUV422_HD_BUFFER_2;
        void* current_buf = get_yuv422_vram()->vram;
        int w = get_yuv422_vram()->width;
        int h = get_yuv422_vram()->height;
        int buf_size = w * h * 2;
        memcpy(aux_buf, current_buf, buf_size);
        
        src_buf = aux_buf;
        dst_buf = current_buf;
    }
    else return;

    static int thr = 50;
    static int thr_increment = 1;
    static int thr_delta = 0;
    
    #define FOCUSED_THR 64
    // the percentage selected in menu represents how many pixels are considered in focus
    // let's say above some FOCUSED_THR
    // so, let's scale edge value so that e=thr maps to e=FOCUSED_THR
    for (int i = 0, i_fthr = 0; i < 255; i++, i_fthr += FOCUSED_THR)
        peak_scaling[i] = MIN(i_fthr / thr, 255);
    
    int n_over = 0;
    int n_total = 720 * (os.y_max - os.y0) / 2;

    #define PEAK_LOOP for (int i = 720 * (os.y0/2), max = 720 * (os.y_max/2); i < max; i++)
    // generic loop:
    //~ for (int i = 720 * (os.y0/2); i < 720 * (os.y_max/2); i++)
    //~ {
        //~ int e = peak_compute((uint8_t*)&src_buf[i] + 1);
        //~ dst_buf[i] = peak_blend(&src_buf[i], e, blend_thr);
        //~ if (unlikely(e > FOCUSED_THR)) n_over++;
    //~ }
    
    if (focus_peaking_disp == 4) // raw
    {
        PEAK_LOOP
        {
            int e = peak_d2xy((uint8_t*)&src_buf[i] + 1);
            e = MIN(e * 4, 255);
            dst_buf[i] = (e << 8) | (e << 24);
        }
    }
    
    else if (focus_peaking_grayscale)
    {
        if (focus_peaking_disp == 1) 
        {
            PEAK_LOOP
            {
                int e = peak_d2xy((uint8_t*)&src_buf[i] + 1);
                e = peak_scaling[MIN(e, 255)];
                if (likely(e < FOCUSED_THR)) dst_buf[i] = src_buf[i] & 0xFF00FF00;
                else 
                { 
                    dst_buf[i] = 0x4C7F4CD5; // red
                    n_over++;
                }
            }
        }
        else if (focus_peaking_disp == 2) // alpha
        {
            PEAK_LOOP
            {
                int e = peak_d2xy((uint8_t*)&src_buf[i] + 1);
                e = peak_scaling[MIN(e, 255)];
                if (likely(e < 20)) dst_buf[i] = src_buf[i] & 0xFF00FF00;
                else dst_buf[i] = peak_blend_alpha(&src_buf[i], e);
                if (unlikely(e > FOCUSED_THR)) n_over++;
            }
        }
        else if (focus_peaking_disp == 3) // sharp
        {
            PEAK_LOOP
            {
                int e = peak_d2xy_sharpen((uint8_t*)&src_buf[i] + 1);
                dst_buf[i] = (src_buf[i] & 0xFF000000) | ((e & 0xFF) << 8);
            }
        }
    }
    else // color
    {
        if (focus_peaking_disp == 1) 
        {
            PEAK_LOOP
            {
                int e = peak_d2xy((uint8_t*)&src_buf[i] + 1);
                e = peak_scaling[MIN(e, 255)];
                if (likely(e < FOCUSED_THR)) dst_buf[i] = src_buf[i];
                else 
                { 
                    dst_buf[i] = 0x4C7F4CD5; // red
                    n_over++;
                }
            }
        }
        else if (focus_peaking_disp == 2) // alpha
        {
            PEAK_LOOP
            {
                int e = peak_d2xy((uint8_t*)&src_buf[i] + 1);
                e = peak_scaling[MIN(e, 255)];
                if (likely(e < 20)) dst_buf[i] = src_buf[i];
                else dst_buf[i] = peak_blend_alpha(&src_buf[i], e);
                if (unlikely(e > FOCUSED_THR)) n_over++;
            }
        }
        else if (focus_peaking_disp == 3) // sharp
        {
            PEAK_LOOP
            {
                int e = peak_d2xy_sharpen((uint8_t*)&src_buf[i] + 1);
                dst_buf[i] = (src_buf[i] & 0xFFFF00FF) | ((e & 0xFF) << 8);
            }
        }
    }

    // update threshold for next iteration
    if (1000 * n_over / n_total > (int)focus_peaking_pthr)
    {
        if (thr_delta > 0) thr_increment++; else thr_increment = 1;
        thr += thr_increment;
    }
    else
    {
        if (thr_delta < 0) thr_increment++; else thr_increment = 1;
        thr -= thr_increment;
    }

    thr_increment = COERCE(thr_increment, -10, 10);
    thr = COERCE(thr, 10, 255);
    
    if (focus_peaking_disp == 3) thr = 64;
}
#endif

static void FAST focus_found_pixel(int x, int y, int e, int thr, uint8_t * const bvram)
{    
    int color = get_focus_color(thr, e);
    //~ int color = COLOR_RED;
    color = (color << 8) | color;
    
#ifdef FEATURE_ANAMORPHIC_PREVIEW
    y = anamorphic_squeeze_bmp_y(y);
#endif
    
    uint16_t * const b_row = (uint16_t*)( bvram + BM_R(y) );   // 2 pixels
    uint16_t * const m_row = (uint16_t*)( bvram_mirror + BM_R(y) );   // 2 pixels

    const int x_half = x >> 1;
    uint32_t pixel = b_row[x_half];
    uint32_t mirror = m_row[x_half];
    const int pos = x_half + (BMPPITCH >> 1);
    uint32_t pixel2 = b_row[pos];
    uint32_t mirror2 = m_row[pos];
    if (mirror  & 0x8080) 
        return;
    if (mirror2 & 0x8080)
        return;
    if (pixel  != 0 && pixel  != mirror)
        return;
    if (pixel2 != 0 && pixel2 != mirror2)
        return;

    if (dirty_pixels_num < MAX_DIRTY_PIXELS)
    {
        dirty_pixel_values[dirty_pixels_num] = pixel + (pixel2 << 16);
        dirty_pixels[dirty_pixels_num++] = (void*)&b_row[x_half] - (void*)bvram;
    }

    b_row[x_half] = b_row[pos] = 
    m_row[x_half] = m_row[pos] = color;
}

static void focus_found_pixel_playback(int x, int y, int e, int thr, uint8_t * const bvram)
{    
    int color = get_focus_color(thr, e);

    uint16_t * const b_row = (uint16_t*)( bvram + BM_R(y) );   // 2 pixels
    uint16_t * const m_row = (uint16_t*)( bvram_mirror + BM_R(y) );   // 2 pixels

    const int x_half = x >> 1;
    uint16_t pixel = b_row[x_half];
    uint16_t mirror = m_row[x_half];
    if (mirror  & 0x8080) 
        return;
    if (pixel  != 0 && pixel  != mirror)
        return;

    bmp_putpixel_fast(bvram, x, y, color);
    bmp_putpixel_fast(bvram, x+1, y, color);
}
#endif

// returns how the focus peaking threshold changed
static int FAST
draw_zebra_and_focus( int Z, int F )
{
    if (unlikely(!get_global_draw())) return 0;

    uint8_t * const bvram = bmp_vram_real();
    if (unlikely(!bvram)) return 0;
    if (unlikely(!bvram_mirror)) return 0;
    
    #ifdef FEATURE_ZEBRA
    draw_zebras(Z);
    #endif
    
    #ifdef FEATURE_FOCUS_PEAK
    if (focus_peaking && focus_peaking_disp && !EXT_MONITOR_CONNECTED)
    {
        if (lv) 
        {
            return 0; // it's drawn from display filters routine
        }
        else  // display filters are not called in playback
        {
            if (F != 1) return 0; // problem: we need to update the threshold somehow
            peak_disp_filter(); return 0; 
        }
    }

    static int thr = 50;
    static int thr_increment = 1;
    static int prev_thr = 50;
    static int thr_delta = 0;

    if (F && focus_peaking)
    {
        // clear previously written pixels
        if (unlikely(!dirty_pixels)) dirty_pixels = SmallAlloc(MAX_DIRTY_PIXELS * sizeof(int));
        if (unlikely(!dirty_pixels)) return -1;
        if (unlikely(!dirty_pixel_values)) dirty_pixel_values = SmallAlloc(MAX_DIRTY_PIXELS * sizeof(int));
        if (unlikely(!dirty_pixel_values)) return -1;
        int i;
        for (i = 0; i < dirty_pixels_num; i++)
        {
            #define B1 *(uint16_t*)(bvram + dirty_pixels[i])
            #define B2 *(uint16_t*)(bvram + dirty_pixels[i] + BMPPITCH)
            #define M1 *(uint16_t*)(bvram_mirror + dirty_pixels[i])
            #define M2 *(uint16_t*)(bvram_mirror + dirty_pixels[i] + BMPPITCH)

            if (likely((B1 == 0 || B1 == M1)) && likely((B2 == 0 || B2 == M2)))
            {
                B1 = M1 = dirty_pixel_values[i] & 0xFFFF;
                B2 = M2 = dirty_pixel_values[i] >> 16;
            }
            #undef B1
            #undef B2
            #undef M1
            #undef M2
        }
        dirty_pixels_num = 0;
        
        struct vram_info *hd_vram = focus_peaking_lores ? get_yuv422_vram() : get_yuv422_hd_vram();
        uint32_t hdvram = (uint32_t)hd_vram->vram;
        if (focus_peaking_lores) hdvram = (uint32_t)CACHEABLE(YUV422_LV_BUFFER_DISPLAY_ADDR);
        
        int off = get_y_skip_offset_for_overlays();
        int yStart = os.y0 + off + 8;
        int yEnd = os.y_max - off - 8;
        int xStart = os.x0 + 8;
        int xEnd = os.x_max - 8;
        int n_over = 0;
        
        const uint8_t* p8; // that's a moving pointer
        
        zebra_update_lut();

        /** simple Laplacian filter
         *     -1
         *  -1  4 -1
         *     -1
         * 
         * Big endian:
         *  uyvy uyvy uyvy
         *  uyvy uYvy uyvy
         *  uyvy uyvy uyvy
         */

        int n_total = 0;
        if (lv) // fast, realtime
        {
            n_total = ((yEnd - yStart) * (xEnd - xStart)) / 6;
            for(int y = yStart; y < yEnd; y += 3)
            {
                uint32_t hd_row = hdvram + (focus_peaking_lores ? BM2LV_R(y) : BM2HD_R(y));
                
                if (focus_peaking_lores) // use LV buffer
                {
                    for (int x = xStart; x < xEnd; x += 2)
                    {
                        p8 = (uint8_t *)(hd_row + bm_hd_x_cache[x - BMP_W_MINUS]); // this was adjusted to hold LV offsets instead of HD
                         
                        int e = peak_d2xy(p8);
                        
                        /* executed for 1% of pixels */
                        if (unlikely(e >= thr))
                        {
                            n_over++;
                            if (unlikely(dirty_pixels_num >= MAX_DIRTY_PIXELS)) break; // threshold too low, abort
                            focus_found_pixel(x, y, e, thr, bvram);
                        }
                    }
                }
                else // hi-res, use HD buffer
                {
                    for (int x = xStart; x < xEnd; x += 2)
                    {
                        p8 = (uint8_t *)(hd_row + bm_hd_x_cache[x - BMP_W_MINUS]);
                         
                        int e = peak_d2xy_hd(p8);
                        
                        /* executed for 1% of pixels */
                        if (unlikely(e >= thr))
                        {
                            n_over++;
                            if (unlikely(dirty_pixels_num >= MAX_DIRTY_PIXELS)) break; // threshold too low, abort
                            focus_found_pixel(x, y, e, thr, bvram);
                        }
                    }
                }
            }
        }
        else // playback - can be slower and more accurate
        {
            n_total = ((yEnd - yStart) * (xEnd - xStart));
            for(int y = yStart; y < yEnd; y ++)
            {
                uint32_t hd_row = hdvram + BM2HD_R(y);
                
                for (int x = xStart; x < xEnd; x ++)
                {
                    p8 = (uint8_t *)(hd_row + bm_hd_x_cache[x - BMP_W_MINUS]);
                    int e = peak_d2xy_hd(p8);
                    
                    /* executed for 1% of pixels */
                    if (unlikely(e >= thr))
                    {
                        n_over++;

                        if (unlikely(dirty_pixels_num >= MAX_DIRTY_PIXELS)) // threshold too low, abort
                            break;

                        if (F==1) focus_found_pixel_playback(x, y, e, thr, bvram);
                    }
                }
            }
        }

        //~ bmp_printf(FONT_LARGE, 10, 50, "%d ", thr);
        
        if (1000 * n_over / n_total > (int)focus_peaking_pthr)
        {
            if (thr_delta > 0) thr_increment++; else thr_increment = 1;
            thr += thr_increment;
        }
        else
        {
            if (thr_delta < 0) thr_increment++; else thr_increment = 1;
            thr -= thr_increment;
        }

        thr_increment = COERCE(thr_increment, -5, 5);
        int thr_min = 15;
        thr = COERCE(thr, thr_min, 255);


        thr_delta = thr - prev_thr;
        prev_thr = thr;

        if (n_over > MAX_DIRTY_PIXELS)
            return thr_delta;
    }

    return thr_delta;
    #endif
    
    return 0;
}

static void guess_focus_peaking_threshold()
{
#ifdef FEATURE_FOCUS_PEAK
    if (!focus_peaking) return;
    int prev_thr_delta = 1234;
    for (int i = 0; i < 50; i++)
    {
        int thr_delta = draw_zebra_and_focus(0,2);
        //~ bmp_printf(FONT_LARGE, 0, 0, "%x ", thr_delta); msleep(1000);
        if (!thr_delta) break;
        if (prev_thr_delta != 1234 && SGN(thr_delta) != SGN(prev_thr_delta)) break;
        prev_thr_delta = thr_delta;
    }
#endif
}


// clear only zebra, focus assist and whatever else is in BMP VRAM mirror
static void
clrscr_mirror( void )
{
    if (!lv && !PLAY_OR_QR_MODE) return;
    if (!get_global_draw()) return;

    uint8_t * const bvram = bmp_vram();
    if (!bvram) return;
    if (!bvram_mirror) return;

    int x, y;
    for( y = os.y0; y < os.y_max; y++ )
    {
        for( x = os.x0; x < os.x_max; x += 4 )
        {
            uint32_t* bp = (uint32_t*)bvram        + BM(x,y)/4;
            uint32_t* mp = (uint32_t*)bvram_mirror + BM(x,y)/4;
            #define BP (*bp)
            #define MP (*mp)
            if (BP != 0)
            { 
                if (BP == MP) BP = MP = 0;
                else little_cleanup(bp, mp);
            }           
            #undef MP
            #undef BP
        }
    }
}

#ifdef FEATURE_CROPMARKS

#define MAX_CROP_NAME_LEN 15
#define MAX_CROPMARKS 9
static int num_cropmarks = 0;
static int cropmarks_initialized = 0;
static char cropmark_names[MAX_CROPMARKS][MAX_CROP_NAME_LEN];

// Cropmark sorting code contributed by Nathan Rosenquist
static void sort_cropmarks()
{
    int i = 0;
    int j = 0;
    
    char aux[MAX_CROP_NAME_LEN];
    
    for (i=0; i<num_cropmarks; i++)
    {
        for (j=i+1; j<num_cropmarks; j++)
        {
            if (strcmp(cropmark_names[i], cropmark_names[j]) > 0)
            {
                strcpy(aux, cropmark_names[i]);
                strcpy(cropmark_names[i], cropmark_names[j]);
                strcpy(cropmark_names[j], aux);
            }
        }
    }
}
#endif

int is_valid_cropmark_filename(char* filename)
{
    int n = strlen(filename);
    if ((n > 4) && (streq(filename + n - 4, ".BMP") || streq(filename + n - 4, ".bmp")) && (filename[0] != '.') && (filename[0] != '_'))
        return 1;
    return 0;
}

#ifdef FEATURE_CROPMARKS
static void find_cropmarks()
{
    struct fio_file file;
    struct fio_dirent * dirent = FIO_FindFirstEx( CARD_DRIVE "ML/CROPMKS/", &file );
    if( IS_ERROR(dirent) )
    {
        NotifyBox(2000, "ML/CROPMKS dir missing\n"
                        "Please copy all ML files!" );
        return;
    }
    int k = 0;
    do {
        if (file.mode & ATTR_DIRECTORY) continue; // is a directory
        if (is_valid_cropmark_filename(file.name))
        {
            if (k >= MAX_CROPMARKS)
            {
                NotifyBox(2000, "TOO MANY CROPMARKS (max=%d)", MAX_CROPMARKS);
                break;
            }
            snprintf(cropmark_names[k], MAX_CROP_NAME_LEN, "%s", file.name);
            k++;
        }
    } while( FIO_FindNextEx( dirent, &file ) == 0);
    FIO_CleanupAfterFindNext_maybe(dirent);
    num_cropmarks = k;
    sort_cropmarks();
    cropmarks_initialized = 1;
}
static void reload_cropmark()
{
    int i = crop_index;
    static int old_i = -1;
    if (i == old_i) return; 
    old_i = i;
    //~ bmp_printf(FONT_LARGE, 0, 100, "reload crop: %d", i);

    if (cropmarks)
    {
        void* old_crop = cropmarks;
        cropmarks = 0;
        bmp_free(old_crop);
    }

    cropmark_clear_cache();
    
    if (!num_cropmarks) return;
    i = COERCE(i, 0, num_cropmarks-1);
    char bmpname[100];
    snprintf(bmpname, sizeof(bmpname), CARD_DRIVE "ML/CROPMKS/%s", cropmark_names[i]);
    cropmarks = bmp_load(bmpname,1);
    if (!cropmarks) bmp_printf(FONT_LARGE, 0, 50, "LOAD ERROR %d:%s   ", i, bmpname);
}

static void
crop_toggle( void* priv, int sign )
{
    crop_index = mod(crop_index + sign, num_cropmarks);
    //~ reload_cropmark(crop_index);
    crop_set_dirty(10);
}
#endif

#ifdef FEATURE_ZEBRA
static MENU_UPDATE_FUNC(zebra_draw_display)
{
    unsigned z = CURRENT_VALUE;
    
    int over_disabled = (zebra_level_hi > 100);
    int under_disabled = (zebra_level_lo == 0);
    
    if (z)
    {
        MENU_SET_VALUE(
            "%s, ",
            zebra_colorspace == 0 ? "Luma" :
            zebra_colorspace == 1 ? "RGB" : "LumaFast"
        );
    
        if (over_disabled)
        {
            MENU_APPEND_VALUE(
                "under %d%%",
                zebra_level_lo
            );
        }
        else if (under_disabled)
        {
            MENU_APPEND_VALUE(
                "over %d%%",
                zebra_level_hi
            );
        }
        else
        {
            MENU_APPEND_VALUE(
                "%d..%d%%",
                zebra_level_lo, zebra_level_hi
            );
        }
    }

    #ifdef FEATURE_RAW_ZEBRAS
    if (z && can_use_raw_overlays_menu())
    {
        raw_zebra_update(entry, info);
        if (raw_zebra_enable) MENU_SET_VALUE("RAW RGB");
    }
    #endif
}

static MENU_UPDATE_FUNC(zebra_param_not_used_for_raw)
{
    #ifdef FEATURE_RAW_ZEBRAS
    if (raw_zebra_enable && can_use_raw_overlays_menu())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not used for RAW zebras.");
    #endif
}

static MENU_UPDATE_FUNC(zebra_level_display)
{
    int level = CURRENT_VALUE;
    if (level == 0 || level > 100)
    {
        MENU_SET_VALUE("Disabled");
        MENU_SET_ICON(MNI_PERCENT_OFF, 0);
        MENU_SET_ENABLED(0);
    }
    else
    {
        MENU_SET_VALUE(
            "%d%% (%d)",
            level, 0, 
            (level * 255 + 50) / 100
        );
    }
    
    zebra_param_not_used_for_raw(entry, info);
}
#endif


#ifdef FEATURE_FOCUS_PEAK
static MENU_UPDATE_FUNC(focus_peaking_display)
{
    unsigned f = CURRENT_VALUE;
    if (f)
        MENU_SET_VALUE(
            "ON,%d.%d,%s%s",
            focus_peaking_pthr / 10, focus_peaking_pthr % 10, 
            focus_peaking_color == 0 ? "R" :
            focus_peaking_color == 1 ? "G" :
            focus_peaking_color == 2 ? "B" :
            focus_peaking_color == 3 ? "C" :
            focus_peaking_color == 4 ? "M" :
            focus_peaking_color == 5 ? "Y" :
            focus_peaking_color == 6 ? "global" :
            focus_peaking_color == 7 ? "local" : "err",
            focus_peaking_grayscale ? ",Gray" : ""
        );
}

static void focus_peaking_adjust_thr(void* priv, int delta)
{
    focus_peaking_pthr = (int)focus_peaking_pthr + (focus_peaking_pthr < 10 ? 1 : 5) * delta;
    if ((int)focus_peaking_pthr > 50) focus_peaking_pthr = 1;
    if ((int)focus_peaking_pthr <= 0) focus_peaking_pthr = 50;
}
#endif

#ifdef FEATURE_CROPMARKS

static MENU_UPDATE_FUNC(crop_display)
{
    int index = crop_index;
    index = COERCE(index, 0, num_cropmarks-1);
    if (crop_enabled) MENU_SET_VALUE(
         num_cropmarks ? cropmark_names[index] : "N/A"
    );
    if (cropmark_movieonly && !is_movie_mode())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Cropmarks are configured only for movie mode");
}

static MENU_UPDATE_FUNC(crop_display_submenu)
{
    int index = crop_index;
    index = COERCE(index, 0, num_cropmarks-1);
    MENU_SET_NAME(
        "Bitmap (%d/%d)",
         index+1, num_cropmarks
    );
    MENU_SET_VALUE(
        "%s",
         num_cropmarks ? cropmark_names[index] : "N/A"
    );

    if (info->can_custom_draw)
    {
        int h = 170;
        int w = h * 720 / 480;
        //~ int xc = 360 - w/2;
        int xc = 400;
        int yc = info->y + font_large.height * 3 + 10;
        BMP_LOCK( reload_cropmark(); )
        bmp_fill(0, xc, yc, w, h);
        BMP_LOCK( bmp_draw_scaled_ex(cropmarks, xc, yc, w, h, 0); )
        bmp_draw_rect(COLOR_WHITE, xc, yc, w, h);
    }
    
    MENU_SET_ICON(MNI_DICE, (num_cropmarks<<16) + index);
}
#endif

#ifdef FEATURE_WAVEFORM
static MENU_UPDATE_FUNC(waveform_print)
{
    if (waveform_draw)
        MENU_SET_VALUE(
            waveform_size == 0 ? "Small" : 
            waveform_size == 1 ? "Large" : 
                                 "FullScreen"
        );
}
#endif

#ifdef FEATURE_GLOBAL_DRAW
static MENU_UPDATE_FUNC(global_draw_display)
{
    if (disp_profiles_0)
    {
        MENU_SET_RINFO("DISP %d", get_disp_mode());
        if (entry->selected && info->x) bmp_printf(FONT(FONT_MED, COLOR_CYAN, COLOR_BLACK), 700 - font_med.width * strlen(Q_BTN_NAME), info->y + font_large.height, Q_BTN_NAME);
    }

    #ifdef CONFIG_5D3
    if (hdmi_code==5 && video_mode_resolution>0) // unusual VRAM parameters
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not compatible with HDMI 50p/60p.");
    #endif
    if (lv && lv_disp_mode && ZEBRAS_IN_LIVEVIEW)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Press " INFO_BTN_NAME " (outside ML menu) to turn Canon displays off.");
    if (global_draw && lv && !ZEBRAS_IN_LIVEVIEW)
    {
        MENU_SET_ENABLED(0);
    }
    if (global_draw && !lv && !ZEBRAS_IN_QUICKREVIEW)
    {
        MENU_SET_ENABLED(0);
    }
}
#endif

#ifdef FEATURE_MAGIC_ZOOM
static MENU_UPDATE_FUNC(zoom_overlay_display)
{
    if (zoom_overlay_enabled) MENU_SET_VALUE(
        "%s%s%s%s%s",
        zoom_overlay_trigger_mode == 0 ? "err" :
#ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
        zoom_overlay_trigger_mode == 1 ? "HalfS," :
        zoom_overlay_trigger_mode == 2 ? "Focus," :
        zoom_overlay_trigger_mode == 3 ? "F+HS," : "ALW,",
#else
        zoom_overlay_trigger_mode == 1 ? "Zrec," :
        zoom_overlay_trigger_mode == 2 ? "F+Zr," :
        zoom_overlay_trigger_mode == 3 ? "(+)," : "ALW,",
#endif

        zoom_overlay_trigger_mode == 0 ? "" :
            zoom_overlay_size == 0 ? "Small," :
            zoom_overlay_size == 1 ? "Med," :
            zoom_overlay_size == 2 ? "Large," : "FullScreen",

        zoom_overlay_trigger_mode == 0 || zoom_overlay_size == 3 ? "" :
            zoom_overlay_pos == 0 ? "AFbox," :
            zoom_overlay_pos == 1 ? "TL," :
            zoom_overlay_pos == 2 ? "TR," :
            zoom_overlay_pos == 3 ? "BR," :
            zoom_overlay_pos == 4 ? "BL," : "err",

        zoom_overlay_trigger_mode == 0 || zoom_overlay_size == 3 ? "" :
            zoom_overlay_x == 0 ? "1:1" :
            zoom_overlay_x == 1 ? "2:1" :
            zoom_overlay_x == 2 ? "3:1" :
            zoom_overlay_x == 3 ? "4:1" : "err",

        zoom_overlay_trigger_mode == 0 || zoom_overlay_size == 3 ? "" :
            zoom_overlay_split == 0 ? "" :
            zoom_overlay_split == 1 ? ",Ss" :
            zoom_overlay_split == 2 ? ",Sz" : "err"

    );

    if (EXT_MONITOR_RCA)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Magic Zoom does not work with SD monitors");
    else if (hdmi_code == 5)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Magic Zoom does not work in HDMI 1080i.");
    #ifdef CONFIG_5D2
    if (display_broken_for_mz())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "After using defish/anamorph, go outside LiveView and back.");
    #endif
    #if !defined(CONFIG_6D) && !defined(CONFIG_5D3) && !defined(CONFIG_EOSM)
    else if (is_movie_mode() && video_mode_fps > 30)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Magic Zoom does not work well in current video mode");
    #endif
    else if (is_movie_mode() && video_mode_crop && zoom_overlay_size == 3)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Full-screen Magic Zoom does not work in crop mode");
    else if (zoom_overlay_enabled && zoom_overlay_trigger_mode && !get_zoom_overlay_trigger_mode() && get_global_draw()) // MZ enabled, but for some reason it doesn't work in current mode
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Magic Zoom is not available in this mode");
}
#endif

#ifdef FEATURE_SPOTMETER
static MENU_UPDATE_FUNC(spotmeter_menu_display)
{
    if (spotmeter_draw)
        MENU_SET_VALUE(
            "%s%s",
            
            spotmeter_formula == 0 ? "Percent" :
            spotmeter_formula == 1 ? "0..255" :
            spotmeter_formula == 2 ? "IRE -1..101" :
            spotmeter_formula == 3 ? "IRE 0..108" :
            spotmeter_formula == 4 ? "RGB" : "RAW",
            
            spotmeter_draw && spotmeter_position ? ", AFbox" : ""
        );
}
#endif

void get_spot_yuv_ex(int size_dxb, int dx, int dy, int* Y, int* U, int* V, int draw, int erase)
{
    struct vram_info *  vram = get_yuv422_vram();

    if( !vram->vram )
        return;
    const uint16_t*     vr = (void*) vram->vram;
    const unsigned      width = vram->width;
    //~ const unsigned      pitch = vram->pitch;
    //~ const unsigned      height = vram->height;
    int                 x, y;

    int xcb = os.x0 + os.x_ex/2 + dx;
    int ycb = os.y0 + os.y_ex/2 + dy;
    int xcl = BM2LV_X(xcb);
    int ycl = BM2LV_Y(ycb);
    int dxl = BM2LV_DX(size_dxb);

    if (erase)
    {
        // clean previous marker
        static int spy_pre_xcb = -1;
        static int spy_pre_ycb = -1;
        if ( spy_pre_xcb != -1 && spy_pre_ycb != -1  && (spy_pre_xcb != xcb || spy_pre_ycb != ycb) ) {
            bmp_draw_rect(0, spy_pre_xcb - size_dxb, spy_pre_ycb - size_dxb, 2*size_dxb, 2*size_dxb);
        }
        spy_pre_xcb = xcb;
        spy_pre_ycb = ycb;
    }
    
    if (draw)
    {
        bmp_draw_rect(COLOR_WHITE, xcb - size_dxb, ycb - size_dxb, 2*size_dxb, 2*size_dxb);
    }
    
    unsigned sy = 0;
    int32_t su = 0, sv = 0; // Y is unsigned, U and V are signed
    // Sum the values around the center
    for( y = ycl - dxl ; y <= ycl + dxl ; y++ )
    {
        for( x = xcl - dxl ; x <= xcl + dxl ; x++ )
        {
            uint16_t p = vr[ x + y * width ];
            sy += (p >> 8) & 0xFF;
            if (x % 2) sv += (int8_t)(p & 0x00FF); else su += (int8_t)(p & 0x00FF);
        }
    }

    const int dx1_two_plus1 = (2 * dxl + 1);
    const int val = (dxl + 1) * dx1_two_plus1;
    sy /= dx1_two_plus1 * dx1_two_plus1;
    su /= val;
    sv /= val;

    *Y = sy;
    *U = su;
    *V = sv;
}

void get_spot_yuv(int dxb, int* Y, int* U, int* V)
{
    get_spot_yuv_ex(dxb, 0, 0, Y, U, V, 1, 0);
}

// for surface cleaning
static int spm_pre_xcb = -1;
static int spm_pre_ycb = -1;

int get_spot_motion(int dxb, int xcb, int ycb, int draw)
{
    struct vram_info *  vram = get_yuv422_vram();

    if( !vram->vram )
        return 0;
    const uint16_t*     vr1 = (void*)YUV422_LV_BUFFER_DISPLAY_ADDR;
    const uint16_t*     vr2 = (void*)get_fastrefresh_422_buf();
    uint8_t * const     bm = bmp_vram();
    if (!bm) return 0;
    const unsigned      width = vram->width;
    int                 x, y;


    const int xcl = BM2LV_X(xcb);
    const int ycl = BM2LV_Y(ycb);
    const int dxl = BM2LV_DX(dxb);

	// surface cleaning
	if ( spm_pre_xcb != -1 && spm_pre_ycb != -1 && draw && (spm_pre_xcb != xcb || spm_pre_ycb != ycb) ) {
		int p_xcl = BM2LV_X(spm_pre_xcb);
		int p_ycl = BM2LV_Y(spm_pre_ycb);
		int ymax = p_ycl + dxl+5;
		int xmax = p_xcl + dxl+5;
		int x, y;
		for( y = p_ycl - (dxl+5) ; y <= ymax ; y++ ) {
		    const int yskip = y * BMPPITCH;
		    for( x = p_xcl - (dxl+5) ; x <= xmax ; x++ )
		    {
		        bm[x + yskip] = 0;
		    }
		}
	}

	spm_pre_xcb = xcb;
	spm_pre_ycb = ycb;

    for (int ddxb = dxb; ddxb < dxb+5; ddxb++) {
		draw_line(xcb - ddxb, ycb - ddxb, xcb + ddxb, ycb - ddxb, COLOR_WHITE);
		draw_line(xcb + ddxb, ycb - ddxb, xcb + ddxb, ycb + ddxb, COLOR_WHITE);
		draw_line(xcb + ddxb, ycb + ddxb, xcb - ddxb, ycb + ddxb, COLOR_WHITE);
		draw_line(xcb - ddxb, ycb + ddxb, xcb - ddxb, ycb - ddxb, COLOR_WHITE);
    }

    unsigned D = 0;
    for( y = ycl - dxl ; y <= ycl + dxl ; y++ )
    {
        const int yskip = y * width;
        const int y_skip_pitch = y * BMPPITCH;
        for( x = xcl - dxl ; x <= xcl + dxl ; x++ )
        {
            const int pos = x + yskip;
            int p1 = (vr1[pos] >> 8) & 0xFF;
            int p2 = (vr2[pos] >> 8) & 0xFF;
            int dif = ABS(p1 - p2);
            D += dif;
            if (draw) bm[x + y_skip_pitch] = falsecolor_value_ex(5, dif & 0xFF);
        }
    }

    D <<= 1;
    const int val = (2 * dxl + 1);
    D /= val * val;
    return D;
}

int get_spot_focus(int dxb)
{
    struct vram_info *  vram = get_yuv422_vram();

    if( !vram->vram )
        return 0;
    const uint32_t*     vr = (uint32_t*) vram->vram; // 2px
    const unsigned      width = vram->width;
    //~ const unsigned      pitch = vram->pitch;
    //~ const unsigned      height = vram->height;
    int                 x, y;
    
    unsigned sf = 0;
    unsigned br = 0;

    int xcb = os.x0 + os.x_ex/2;
    int ycb = os.y0 + os.y_ex/2;
    int xcl = BM2LV_X(xcb);
    int ycl = BM2LV_Y(ycb);
    int dxl = BM2LV_DX(dxb);
    
    // Sum the absolute difference of values around the center
    for( y = ycl - dxl ; y <= ycl + dxl ; y++ )
    {
        const int yskip = y * (width >> 1);
        for( x = xcl - dxl ; x <= xcl + dxl ; x++ )
        {
            uint32_t p = vr[ x/2 + yskip ];
            int32_t p0 = (p >> 24) & 0xFF;
            int32_t p1 = (p >>  8) & 0xFF;
            sf += ABS(p1 - p0);
            br += p1 + p0;
        }
    }
    return sf / (br >> 14);
}

#ifdef FEATURE_SPOTMETER

static int spot_prev_xcb = 0;
static int spot_prev_ycb = 0;
static int spotmeter_dirty = 0;

// will be called from prop_handler PROP_LV_AFFRAME
// no BMP_LOCK here, please
void
spotmeter_erase()
{
    if (!spotmeter_dirty) return;
    spotmeter_dirty = 0;

    int xcb = spot_prev_xcb;
    int ycb = spot_prev_ycb;
    int dx = spotmeter_formula <= 3 ? 26 : 52;
    int y0 = -13;
    uint32_t* M = (uint32_t*)get_bvram_mirror();
    uint32_t* B = (uint32_t*)bmp_vram();
    for(int y = (ycb&~1) + y0 ; y <= (ycb&~1) + 36 ; y++ )
    {
        for(int x = xcb - dx ; x <= xcb + dx ; x+=4 )
        {
            uint8_t* m = (uint8_t*)(&(M[BM(x,y)/4])); //32bit to 8bit 
            if (*m == 0x80) *m = 0;
            m++;
            if (*m == 0x80) *m = 0;
            m++;
            if (*m == 0x80) *m = 0;
            m++;
            if (*m == 0x80) *m = 0;
            B[BM(x,y)/4] = 0;
        }
    }
}

/* spotmeter position in QR mode */
/* (in LiveView it's linked to focus box, but in QR we can't always move the LV box) */
static int spotmeter_playback_offset_x = 0;
static int spotmeter_playback_offset_y = 0;

static void spotmeter_step()
{
    if (gui_menu_shown()) return;
    if (!get_global_draw()) return;
    if (digic_zoom_overlay_enabled()) return; // incorrect readings
    //~ if (!lv) return;
    if (!PLAY_OR_QR_MODE)
    {
        if (!lv_luma_is_accurate()) return;
    }
    struct vram_info *  vram = get_yuv422_vram();

    if( !vram->vram )
        return;

    if (!PLAY_OR_QR_MODE)
        spotmeter_erase();
    
    const uint16_t*     vr = (uint16_t*) vram->vram;
    const unsigned      width = vram->width;
    //~ const unsigned      pitch = vram->pitch;
    //~ const unsigned      height = vram->height;
    const unsigned      dxb = spotmeter_size;
    //unsigned        sum = 0;
    int                 x, y;

    int xcb = os.x0 + os.x_ex/2;
    int ycb = os.y0 + os.y_ex/2;
    
    if (spotmeter_position == 1) // AF frame
    {
        int aff_x0 = 360;
        int aff_y0 = 240;
        if (lv)
        {
            if (lv_dispsize == 1)
                get_afframe_pos(720, 480, &aff_x0, &aff_y0);
        }
        else
        {
            spotmeter_playback_offset_x = COERCE(spotmeter_playback_offset_x, -300, 300);
            spotmeter_playback_offset_y = COERCE(spotmeter_playback_offset_y, -200, 200);
            aff_x0 = 360 + spotmeter_playback_offset_x;
            aff_y0 = 240 + spotmeter_playback_offset_y;
        }
        xcb = N2BM_X(aff_x0);
        ycb = N2BM_Y(aff_y0);
        xcb = COERCE(xcb, os.x0 + 50, os.x_max - 50);
        ycb = COERCE(ycb, os.y0 + 50, os.y_max - 50);
    }
    
    // save coords, so we know where to erase the spotmeter from
    spot_prev_xcb = xcb;
    spot_prev_ycb = ycb;
    spotmeter_dirty = 1;
    
    int xcl = BM2LV_X(xcb);
    int ycl = BM2LV_Y(ycb);
    int dxl = BM2LV_DX(dxb);
    
    unsigned sy = 0;
    int32_t su = 0, sv = 0; // Y is unsigned, U and V are signed
    // Sum the values around the center
    for( y = ycl - dxl ; y <= ycl + dxl ; y++ )
    {
        for( x = xcl - dxl ; x <= xcl + dxl ; x++ )
        {
            uint16_t p = vr[ x + y * width ];
            sy += (p >> 8) & 0xFF;
            if (x % 2) sv += (int8_t)(p & 0x00FF); else su += (int8_t)(p & 0x00FF);
        }
    }

    const int two_dx1_plus1 = (2 * dxl + 1);
    const int val = (dxl + 1) * two_dx1_plus1;
    sy /= two_dx1_plus1 * two_dx1_plus1;
    su /= val;
    sv /= val;

    // Scale to 100%
    const unsigned      scaled = (101 * sy) >> 8;

    #ifdef FEATURE_RAW_SPOTMETER
    int raw_luma = 0;
    int raw_ev = 0;
    if (can_use_raw_overlays() && raw_update_params())
    {
        const int xcr = BM2RAW_X(xcb);
        const int ycr = BM2RAW_Y(ycb);
        const int dxr = BM2RAW_DX(dxb);

        raw_luma = 0;
        int raw_count = 0;
        for( y = ycr - dxr ; y <= ycr + dxr ; y++ )
        {
            if (y < raw_info.active_area.y1 || y > raw_info.active_area.y2) continue;
            for( x = xcr - dxr ; x <= xcr + dxr ; x++ )
            {
                if (x < raw_info.active_area.x1 || x > raw_info.active_area.x2) continue;

                raw_luma += raw_get_pixel(x, y);
                raw_count++;
                
                /* define this to check if spotmeter reads from the right place;
                 * you should see some gibberish on raw zebras, right inside the spotmeter box */
                #ifdef RAW_SPOTMETER_TEST
                raw_set_pixel(raw_buf, x, y, rand());
                #endif
            }
        }
        if (!raw_count) return;
        raw_luma /= raw_count;
        raw_ev = (int) roundf(10.0 * raw_to_ev(raw_luma));
    }
    #endif
    
    // spotmeter color: 
    // black on transparent, if brightness > 60%
    // white on transparent, if brightness < 50%
    // previous value otherwise
    
    // if false color is active, draw white on semi-transparent gray

    // protect the surroundings from zebras
    #ifndef RAW_SPOTMETER_TEST
    uint32_t* M = (uint32_t*)get_bvram_mirror();
    uint32_t* B = (uint32_t*)bmp_vram();

    int dx = spotmeter_formula <= 3 ? 26 : 52;
    int y0 = arrow_keys_shortcuts_active() ? (int)(36 - font_med.height) : (int)(-13);
    for( y = (ycb&~1) + y0 ; y <= (ycb&~1) + 36 ; y++ )
    {
        for( x = xcb - dx ; x <= xcb + dx ; x+=4 )
        {
            uint8_t* m = (uint8_t*)(&(M[BM(x,y)/4])); //32bit to 8bit 
            if (!(*m & 0x80)) *m = 0x80;
            m++;
            if (!(*m & 0x80)) *m = 0x80;
            m++;
            if (!(*m & 0x80)) *m = 0x80;
            m++;
            if (!(*m & 0x80)) *m = 0x80;
            B[BM(x,y)/4] = 0;
        }
    }
    #endif
    
    static int fg = 0;
    if (scaled > 60) fg = COLOR_BLACK;
    if (scaled < 50 || falsecolor_draw) fg = COLOR_WHITE;
    int bg = fg == COLOR_BLACK ? COLOR_WHITE : COLOR_BLACK;
    int fnt = FONT(SHADOW_FONT(FONT_MED), fg, bg);
    int fnts = FONT(SHADOW_FONT(FONT_SMALL), fg, bg);

    if (!arrow_keys_shortcuts_active())
    {
        bmp_draw_rect(COLOR_WHITE, xcb - dxb, ycb - dxb, 2*dxb+1, 2*dxb+1);
        bmp_draw_rect(COLOR_BLACK, xcb - dxb + 1, ycb - dxb + 1, 2*dxb+1-2, 2*dxb+1-2);
    }
    ycb += dxb + 20;
    ycb -= font_med.height/2;
    xcb -= 2 * font_med.width;

    #ifdef FEATURE_RAW_SPOTMETER
    if (spotmeter_formula == 5)
    {
        if (can_use_raw_overlays())
        {
            bmp_printf(
                fnt,
                xcb - font_med.width - 5, ycb, 
                "-%d.%d EV",
                -raw_ev/10, 
                -raw_ev%10
            );
        }
        else // will fall back to percent if no raw data is available
        {
            goto fallback_from_raw;
        }
    }
    #endif
    
    if (spotmeter_formula <= 1)
    {
#ifdef FEATURE_RAW_SPOTMETER
fallback_from_raw:
#endif
        bmp_printf(
            fnt,
            xcb, ycb, 
            "%3d%s",
            spotmeter_formula == 1 ? sy : scaled,
            spotmeter_formula == 1 ? "" : "%"
        );
    }
    else if (spotmeter_formula <= 3)
    {
        int ire_aj = (((int)sy) - 2) * 102 / 253 - 1; // formula from AJ: (2...255) -> (-1...101)
        int ire_piers = ((int)sy) * 108/255;           // formula from Piers: (0...255) -> (0...108)
        int ire = (spotmeter_formula == 2) ? ire_aj : ire_piers;
        
        bmp_printf(
            fnt,
            xcb, ycb, 
            "%s%3d", // why does %4d display garbage?!
            ire < 0 ? "-" : " ",
            ire < 0 ? -ire : ire
        );
        bmp_printf(
            fnts,
            xcb + font_med.width*4, ycb,
            "IRE\n%s",
            spotmeter_formula == 2 ? "-1..101" : "0..108"
        );
    }
    else if (spotmeter_formula == 4)
    {
        int uyvy = UYVY_PACK(su,sy,sv,sy);
        int R,G,B,Y;
        COMPUTE_UYVY2YRGB(uyvy, Y, R, G, B);
        xcb -= font_med.width * 3/2;
        bmp_printf(
            fnt,
            xcb, ycb, 
            "#%02x%02x%02x",
            R,G,B
        );
    }
}

#endif

#ifdef FEATURE_GHOST_IMAGE
static MENU_UPDATE_FUNC(transparent_overlay_display)
{
    if (transparent_overlay && (transparent_overlay_offx || transparent_overlay_offy))
        MENU_SET_VALUE(
            "ON, dx=%d, dy=%d", 
            transparent_overlay_offx, 
            transparent_overlay_offy
        );
    transparent_overlay_hidden = 0;
}

static void transparent_overlay_offset(int dx, int dy)
{
    transparent_overlay_offx = COERCE((int)transparent_overlay_offx + dx, -650, 650);
    transparent_overlay_offy = COERCE((int)transparent_overlay_offy + dy, -400, 400);
    transparent_overlay_hidden = 0;
    redraw();
}

static void transparent_overlay_center_or_toggle()
{
    if (transparent_overlay_offx || transparent_overlay_offy) // if off-center, just center it
    {
        transparent_overlay_offset_clear(0, 0);
        transparent_overlay_offset(0, 0);
    }
    else // if centered, hide it or show it back
    {
        transparent_overlay_hidden = !transparent_overlay_hidden;
        redraw();
    }
}

static void transparent_overlay_offset_clear(void* priv, int delta)
{
    transparent_overlay_offx = transparent_overlay_offy = 0;
}

int handle_transparent_overlay(struct event * event)
{
    if (transparent_overlay && event->param == BGMT_LV && PLAY_OR_QR_MODE)
    {
        schedule_transparent_overlay();
        return 0;
    }

    if (!get_global_draw()) return 1;

    if (transparent_overlay && liveview_display_idle() && !gui_menu_shown())
    {
        if (event->param == BGMT_PRESS_UP)
        {
            transparent_overlay_offset(0, -40);
            return 0;
        }
        if (event->param == BGMT_PRESS_DOWN)
        {
            transparent_overlay_offset(0, 40);
            return 0;
        }
        if (event->param == BGMT_PRESS_LEFT)
        {
            transparent_overlay_offset(-40, 0);
            return 0;
        }
        if (event->param == BGMT_PRESS_RIGHT)
        {
            transparent_overlay_offset(40, 0);
            return 0;
        }
        #if defined(BGMT_JOY_CENTER)
        if (event->param == BGMT_JOY_CENTER)
        #else
        if (event->param == BGMT_PRESS_SET)
        #endif
        {
            transparent_overlay_center_or_toggle();
            return 0;
        }
    }
    return 1;
}
#endif

#ifdef FEATURE_POWERSAVE_LIVEVIEW
static char* idle_time_format(int t)
{
    static char msg[50];
    if (t) snprintf(msg, sizeof(msg), "after %d%s", t < 60 ? t : t/60, t < 60 ? "sec" : "min");
    else snprintf(msg, sizeof(msg), "OFF");
    return msg;
}

static PROP_INT(PROP_LCD_BRIGHTNESS_MODE, lcd_brightness_mode);

static MENU_UPDATE_FUNC(idle_display_dim_print)
{
    MENU_SET_VALUE(
        idle_time_format(CURRENT_VALUE)
    );

    #ifdef CONFIG_AUTO_BRIGHTNESS
    int backlight_mode = lcd_brightness_mode;
    if (backlight_mode == 0) // can't restore brightness properly in auto mode
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "LCD brightness is auto in Canon menu. It won't work.");
    }
    #endif
}

static MENU_UPDATE_FUNC(idle_display_feature_print)
{
    MENU_SET_VALUE(
        idle_time_format(CURRENT_VALUE)
    );
}

static int timeout_values[] = {0, 5, 10, 20, 30, 60, 120, 300, 600, 900};

static int current_timeout_index(int t)
{
    int i;
    for (i = 0; i < COUNT(timeout_values); i++)
        if (t == timeout_values[i]) return i;
    return 0;
}

static void idle_timeout_toggle(void* priv, int sign)
{
    int* t = (int*)priv;
    int i = current_timeout_index(*t);
    i = mod(i + sign, COUNT(timeout_values));
    *(int*)priv = timeout_values[i];
}
#endif

static CONFIG_INT("electronic.level", electronic_level, 0);

struct menu_entry zebra_menus[] = {
    #ifdef FEATURE_GLOBAL_DRAW
    {
        .name = "Global Draw",
        .priv       = &global_draw,
        #ifdef FEATURE_OVERLAYS_IN_PLAYBACK_MODE
        .max = 3,
        #else
        .max = 1,
        #endif
        .select_Q   = toggle_disp_mode_menu,
        .update    = global_draw_display,
        .icon_type = IT_DICE_OFF,
        .edit_mode = EM_MANY_VALUES,
        .choices = (const char *[]) {"OFF", "LiveView", "QuickReview", "ON, all modes"},
        .help = "Enable/disable ML overlay graphics (zebra, cropmarks...)",
        //.essential = FOR_LIVEVIEW,
    },
    #endif
    #ifdef FEATURE_ZEBRA
    {
        .name = "Zebras",
        .priv       = &zebra_draw,
        .update     = zebra_draw_display,
        .max = 1,
        .help = "Zebra stripes: show overexposed or underexposed areas.",
        .depends_on = DEP_GLOBAL_DRAW | DEP_EXPSIM,
        .children =  (struct menu_entry[]) {
            {
                .name = "Color Space",
                .priv = &zebra_colorspace, 
                #ifdef FEATURE_ZEBRA_FAST
                .max = 2,
                #else
                .max = 1,
                #endif
                .choices = (const char *[]) {"Luma", "RGB", "Luma Fast"},
                .icon_type = IT_DICE,
                .update = zebra_param_not_used_for_raw,
                .help = "Luma: red/blue. RGB: show color of the clipped channel(s).",
            },
            {
                .name = "Underexposure",
                .priv = &zebra_level_lo, 
                .min = 0,
                .max = 20,
                .icon_type = IT_PERCENT_OFF,
                .update = zebra_level_display,
                .help = "Underexposure threshold.",
            },
            {
                .name = "Overexposure", 
                .priv = &zebra_level_hi,
                .min = 70,
                .max = 101,
                .icon_type = IT_PERCENT_OFF,
                .update = zebra_level_display,
                .help = "Overexposure threshold.",
            },
            #ifdef CONFIG_MOVIE
            {
                .name = "When recording", 
                .priv = &zebra_rec,
                .max = 1,
                .choices = (const char *[]) {"Hide", "Show"},
                .help = "You can hide zebras when recording.",
            },
            #endif
            #ifdef FEATURE_RAW_ZEBRAS
            {
                .name = "Use RAW zebras",
                .priv = &raw_zebra_enable,
                .max = 1,
                .update = raw_zebra_update,
                .help = "Use RAW zebras whenever possible.",
            },
            #endif
            MENU_EOL
        },
    },
    #endif

    #ifdef FEATURE_FOCUS_PEAK_DISP_FILTER
        #ifndef FEATURE_FOCUS_PEAK
        #error This requires FEATURE_FOCUS_PEAK.
        #endif
    #endif

    #ifdef FEATURE_FOCUS_PEAK
    {
        .name = "Focus Peak",
        .priv           = &focus_peaking,
        .update         = focus_peaking_display,
        .max = 1,
        .help = "Show which parts of the image are in focus.",
        .submenu_width = 650,
        .depends_on = DEP_GLOBAL_DRAW,
        .children =  (struct menu_entry[]) {
            {
                .name = "Filter bias", 
                .priv = &focus_peaking_filter_edges,
                .max = 2,
                .choices = (const char *[]) {"Strong edges", "Balanced", "Fine details"},
                .help  = "Fine-tune the focus detection algorithm:",
                .help2 = "Strong edges: looks for edges, works best in low light.\n"
                         "Balanced: tries to cover both strong edges and fine details.\n"
                         "Fine details: looks for microcontrast. Needs lots of light.\n",
                .icon_type = IT_DICE
            },
            {
                .name = "Image buffer",
                .priv = &focus_peaking_lores,
                .max = 1,
                .icon_type = IT_DICE,
                .choices = CHOICES("High-res", "Low-res"),
                .help = "Use a low-res image to get better results in low light.",
            },
            /*
            {
                .name = "Method",
                .priv = &focus_peaking_method, 
                .max = 1,
                .choices = (const char *[]) {"1st deriv.", "2nd deriv.", "Nyquist H"},
                .help = "Contrast detection method. 2: more accurate, 1: less noisy.",
            },*/
            #ifdef FEATURE_FOCUS_PEAK_DISP_FILTER
                #ifndef CONFIG_DISPLAY_FILTERS
                #error This requires CONFIG_DISPLAY_FILTERS.
                #endif
            {
                .name = "Display type",
                .priv = &focus_peaking_disp, 
                .max = 4,
                .choices = (const char *[]) {"Blinking dots", "Fine dots", "Alpha blend", "Sharpness", "Raw"},
                .help = "How to display peaking. Alpha looks nicer, but image lags.",
            },
            #endif
            {
                .name = "Threshold", 
                .priv = &focus_peaking_pthr,
                .select = focus_peaking_adjust_thr,
                .max    = 50,
                .icon_type = IT_PERCENT_LOG,
                .unit = UNIT_PERCENT_x10,
                .help = "How many pixels are considered in focus (percentage).",
            },
            {
                .name = "Color", 
                .priv = &focus_peaking_color,
                .max = 7,
                .choices = (const char *[]) {"Red", "Green", "Blue", "Cyan", "Magenta", "Yellow", "Global Focus", "Local Focus"},
                .help = "Focus peaking color (fixed or color coding).",
                .icon_type = IT_DICE,
            },
            {
                .name = "Grayscale image", 
                .priv = &focus_peaking_grayscale,
                .max = 1,
                .help = "Display LiveView image in grayscale.",
            },
            MENU_EOL
        },
    },
    #endif
    #ifdef FEATURE_MAGIC_ZOOM
    {
        .name = "Magic Zoom",
        .priv = &zoom_overlay_enabled,
        .update = zoom_overlay_display,
        .min = 0,
        .max = 1,
        .help = "Zoom box for checking focus. Can be used while recording.",
        .submenu_width = 650,
        .depends_on = DEP_GLOBAL_DRAW | DEP_LIVEVIEW,
        .children =  (struct menu_entry[]) {
            {
                .name = "Trigger mode",
                .priv = &zoom_overlay_trigger_mode, 
                .min = 1,
                .max = 4,
                #ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
                .choices = (const char *[]) {"HalfShutter", "Focus Ring", "FocusR+HalfS", "Always On"},
                .help = "Trigger Magic Zoom by focus ring or half-shutter.",
                #else
                .choices = (const char *[]) {"Zoom.REC", "Focus+ZREC", "ZoomIn (+)", "Always On"},
                .help = "Zoom when recording / trigger from focus ring / Zoom button",
                #endif
            },
            {
                .name = "Size", 
                .priv = &zoom_overlay_size,
                #ifndef CONFIG_CAN_REDIRECT_DISPLAY_BUFFER_EASILY // old cameras - simple zoom box
                .max = 2,
                .help = "Size of zoom box (small / medium / large).",
                #else // new cameras can do fullscreen too :)
                .max = 3,
                .help = "Size of zoom box (small / medium / large / full screen).",
                #endif
                .choices = (const char *[]) {"Small", "Medium", "Large", "FullScreen"},
                .icon_type = IT_SIZE,
            },
            {
                .name = "Position", 
                .priv = &zoom_overlay_pos,
                .max = 4,
                .choices = (const char *[]) {"Focus box", "Top-Left", "Top-Right", "Bottom-Right", "Bottom-Left"},
                .icon_type = IT_DICE,
                .help = "Position of zoom box (fixed or linked to focus box).",
            },
            {
                .name = "Magnification", 
                .priv = &zoom_overlay_x,
                .max = 2,
                .choices = (const char *[]) {"1:1", "2:1", "3:1", "4:1"},
                .icon_type = IT_SIZE,
                .help = "1:1 displays recorded pixels, 2:1 displays them doubled.",
            },
            #ifdef CONFIG_LV_FOCUS_INFO
            {
                .name = "Focus confirm", 
                .priv = &zoom_overlay_split,
                .max = 2,
                .choices = (const char *[]) {"Green Bars", "SplitScreen", "SS ZeroCross"},
                .icon_type = IT_DICE,
                .help = "How to show focus confirmation (green bars / split screen).",
            },
            #endif
            /*{
                .name = "Look-up Table", 
                .priv = &zoom_overlay_lut,
                .max = 1,
                .choices = (const char *[]) {"OFF", "CineStyle"},
                .help = "LUT for increasing contrast in the zoom box.",
            },*/
            MENU_EOL
        },
    },
    #endif
    #ifdef FEATURE_CROPMARKS
    {
        .name = "Cropmarks",
        .priv = &crop_enabled,
        .update    = crop_display,
        .max = 1,
        .help = "Cropmarks or custom grids for framing.",
        .depends_on = DEP_GLOBAL_DRAW,
        .submenu_width = 710,
        .submenu_height = 250,
        .children =  (struct menu_entry[]) {
            {
                .name = "Bitmap",
                .priv = &crop_index, 
                .select = crop_toggle,
                .update    = crop_display_submenu,
                .icon_type = IT_DICE,
                .help = "You can draw your own cropmarks in Paint.",
            },
            {
                .name = "Show in photo mode",
                .priv = &cropmark_movieonly, 
                .max = 1,
                .choices = (const char *[]) {"ON", "OFF"},
                .help = "Cropmarks are mostly used in movie mode.",
            },
            {
                .name = "Show in PLAY mode",
                .priv = &cropmarks_play, 
                .max = 1,
                .help = "You may also have cropmarks in Playback mode.",
            },
            MENU_EOL
        },
    },
    #endif
    #ifdef FEATURE_GHOST_IMAGE
        #ifndef FEATURE_CROPMARKS
        #error This requires FEATURE_CROPMARKS.
        #endif
    {
        .name = "Ghost image",
        .priv = &transparent_overlay, 
        .update = transparent_overlay_display, 
        .max = 1,
        .help = "Overlay any image in LiveView. In PLAY mode, press LV btn.",
        .depends_on = DEP_GLOBAL_DRAW,
        .works_best_in = DEP_LIVEVIEW, // it will actually go into LV if it's not
        .children =  (struct menu_entry[]) {
            {
                .name = "Auto-update",
                .priv = &transparent_overlay_auto_update, 
                .max = 1,
                .help = "Update the overlay whenever you take a picture.",
            },
            MENU_EOL
        }
    },
    #endif
    #ifdef FEATURE_SPOTMETER
    {
        .name = "Spotmeter",
        .priv           = &spotmeter_draw,
        .max = 1,
        .update        = spotmeter_menu_display,
        .help = "Exposure aid: display brightness from a small spot.",
        .depends_on = DEP_GLOBAL_DRAW | DEP_EXPSIM,
        .children =  (struct menu_entry[]) {
            {
                .name = "Spotmeter Unit",
                .priv = &spotmeter_formula, 
                #ifdef FEATURE_RAW_SPOTMETER
                .max = 5,
                #else
                .max = 4,
                #endif
                .choices = (const char *[]) {"Percent", "0..255", "IRE -1..101", "IRE 0..108", "RGB (HTML)", "RAW (EV)"},
                .icon_type = IT_DICE,
                .help = "Measurement unit for brightness level(s).",
            },
            {
                .name = "Spot Position",
                .priv = &spotmeter_position, 
                .max = 1,
                .choices = (const char *[]) {"Center", "Focus box"},
                .icon_type = IT_DICE,
                .help = "Spotmeter position: center or linked to focus box.",
            },
            MENU_EOL
        }
    },
    #endif
    #ifdef FEATURE_FALSE_COLOR
    {
        .name = "False color",
        .priv       = &falsecolor_draw,
        .update     = falsecolor_display,
        .max = 1,
        .submenu_width = 700,
        .submenu_height = 160,
        .help = "Exposure aid: each brightness level is color-coded.",
        .depends_on = DEP_GLOBAL_DRAW | DEP_EXPSIM,
        .children =  (struct menu_entry[]) {
            {
                .name = "Palette      ",
                .priv = &falsecolor_palette,
                .max = 5,
                .icon_type = IT_DICE,
                .choices = CHOICES("Marshall", "SmallHD", "50-55%", "67-72%", "Banding detection", "GreenScreen"),
                .update = falsecolor_display_palette,
                .help = "False color palettes for exposure, banding, green screen...",
            },
            MENU_EOL
        }
    },
    #endif
    #ifdef FEATURE_HISTOGRAM
    {
        .name = "Histogram",
        .priv       = &hist_draw,
        .max = 1,
        .update = hist_print,
        .help = "Exposure aid: shows the distribution of brightness levels.",
        .depends_on = DEP_GLOBAL_DRAW | DEP_EXPSIM,
        .submenu_width = 700,
        .children =  (struct menu_entry[]) {
            {
                .name = "Color space",
                .priv = &hist_colorspace, 
                .max = 1,
                .choices = (const char *[]) {"Luma", "RGB"},
                .icon_type = IT_DICE,
                .help = "Color space for histogram: Luma channel (YUV) / RGB.",
            },
            {
                .name = "Scaling",
                .priv = &hist_log, 
                .max = 1,
                .choices = (const char *[]) {"Linear", "Log"},
                .help = "Linear or logarithmic histogram.",
                .icon_type = IT_DICE,
            },
            {
                .name = "Clip warning",
                .priv = &hist_warn, 
                .max = 1,
                .help = "Display warning dots when one color channel is clipped.",
            },
            #ifdef FEATURE_RAW_HISTOGRAM
            {
                .name = "Use RAW histogram",
                .priv = &raw_histogram_enable,
                .max = 1,
                .update = raw_histo_update,
                .help = "Use RAW histogram whenever possible.",
            },
            {
                .name = "RAW EV indicator",
                .priv = &hist_meter,
                .max = 2,
                .choices = CHOICES("OFF", "Dynamic Range", "ETTR hint"),
                .help = "Choose an EV image indicator to display on the histogram.",
                .help2 = 
                    " \n"
                    "Display the dynamic range at current ISO, from noise stdev.\n"
                    "Show how many stops you can push the exposure to the right.\n"
            },
            #endif
            MENU_EOL
        },
    },
    #endif
    #ifdef FEATURE_WAVEFORM
    {
        .name = "Waveform",
        .priv       = &waveform_draw,
        .update = waveform_print,
        .max = 1,
        .help = "Exposure aid: useful for checking overall brightness.",
        .depends_on = DEP_GLOBAL_DRAW | DEP_EXPSIM,
        .children =  (struct menu_entry[]) {
            {
                .name = "Waveform Size",
                .priv = &waveform_size, 
                .max = 2,
                .choices = (const char *[]) {"Small", "Large", "FullScreen"},
                .icon_type = IT_SIZE,
                .help = "Waveform size: Small / Large / FullScreen.",
            },
            MENU_EOL
        },
        //.essential = FOR_LIVEVIEW | FOR_PLAYBACK,
    },
    #endif
    #ifdef FEATURE_VECTORSCOPE
    {
        .name = "Vectorscope",
        .priv       = &vectorscope_draw,
        .max = 1,
        .update = vectorscope_update,
        .help = "Shows color distribution as U-V plot. For grading & WB.",
        .depends_on = DEP_GLOBAL_DRAW | DEP_EXPSIM,
        .children =  (struct menu_entry[]) {
            {
                .name = "UV scaling",
                .priv = &vectorscope_gain, 
                .max = 1,
                .choices = (const char *[]) {"OFF", "2x", "4x"},
                .help = "Scaling for input signal (useful with flat picture styles).",
            },
            MENU_EOL
        },
    },
    #endif
};

static struct menu_entry level_indic_menus[] = {
    #ifdef CONFIG_ELECTRONIC_LEVEL
    #ifdef FEATURE_LEVEL_INDICATOR
    {
        .name = "Level Indicator", 
        .priv = &electronic_level, 
        .max  = 1, 
        .help = "Electronic level indicator in 0.5 degree steps.",
        .depends_on = DEP_GLOBAL_DRAW,
    },
    #endif
    #endif
};
static struct menu_entry livev_dbg_menus[] = {
    #ifdef FEATURE_SHOW_OVERLAY_FPS
    {
        .name = "Show Overlay FPS",
        .priv = &show_lv_fps, 
        .max = 1,
        .help = "Show the frame rate of overlay loop (zebras, peaking...)"
    },
    #endif
};

#ifdef CONFIG_BATTERY_INFO
MENU_UPDATE_FUNC(batt_display)
{
    int l = GetBatteryLevel();
    int r = GetBatteryTimeRemaining();
    int d = GetBatteryDrainRate();
    MENU_SET_VALUE(
        "%d%%, %dh%02dm, %d%%/h",
        l, 0, 
        r / 3600, (r % 3600) / 60,
        d, 0
    );
    MENU_SET_ICON(MNI_PERCENT, l);
}
#endif

#ifdef CONFIG_LCD_SENSOR
CONFIG_INT("lcdsensor.wakeup", lcd_sensor_wakeup, 1);
#else
#define lcd_sensor_wakeup 0
#endif

static struct menu_entry powersave_menus[] = {
#ifdef FEATURE_POWERSAVE_LIVEVIEW
{
    .name = "Powersave in LiveView",
    .select = menu_open_submenu,
    .submenu_width = 715,
    .help = "Options for reducing power consumption during idle times.",
    .depends_on = DEP_LIVEVIEW,
    .children =  (struct menu_entry[]) {
        {
            .name = "Enable power saving",
            .priv           = &idle_rec,
            .max = 2,
            .choices = (const char *[]) {"on Standby", "on Recording", "on STBY+REC"},
            .help = "If enabled, powersave (see above) works when recording too."
        },
        #ifdef CONFIG_LCD_SENSOR
        {
            .name = "Use LCD sensor",
            .priv           = &lcd_sensor_wakeup,
            .max = 1,
            .help = "With the LCD sensor you may wakeup or force powersave mode."
        },
        #endif
        {
            .name = "Use shortcut key",
            .priv           = &idle_shortcut_key,
            .max = 1,
            .choices = (const char *[]) {"OFF", INFO_BTN_NAME},
            .help = "Shortcut key for enabling powersave modes right away."
        },
        {
            .name = "Dim display",
            .priv           = &idle_display_dim_after,
            .update         = idle_display_dim_print,
            .select         = idle_timeout_toggle,
            .max            = 900,
            .icon_type      = IT_PERCENT_LOG_OFF,
            .help = "Dim LCD display in LiveView when idle, to save power.",
        },
        {
            .name = "Turn off LCD",
            .priv           = &idle_display_turn_off_after,
            .update         = idle_display_feature_print,
            .select         = idle_timeout_toggle,
            .max            = 900,
            .icon_type      = IT_PERCENT_LOG_OFF,
            .help = "Turn off display and pause LiveView when idle and not REC.",
        },
        {
            .name = "Turn off GlobalDraw",
            .priv           = &idle_display_global_draw_off_after,
            .update         = idle_display_feature_print,
            .select         = idle_timeout_toggle,
            .max            = 900,
            .icon_type      = IT_PERCENT_LOG_OFF,
            .help = "Turn off GlobalDraw when idle, to save some CPU cycles.",
            //~ .edit_mode = EM_MANY_VALUES,
        },
        #ifdef CONFIG_BATTERY_INFO
        {
            .name = "Battery Level",
            .update  = batt_display,
            .icon_type = IT_PERCENT,
            .help = "Battery remaining. Wait for 2% discharge before reading.",
            //~ //.essential = FOR_MOVIE | FOR_PHOTO,
        },
        #endif
        MENU_EOL
    },
}
#endif
};

#ifdef FEATURE_LV_DISPLAY_PRESETS
struct menu_entry livev_cfg_menus[] = {
    {
        .name = "LV Display Presets",
        .priv       = &disp_profiles_0,
        .max        = 3,
        .choices    = (const char *[]) {"OFF (1)", "2", "3", "4"},
        .icon_type  = IT_DICE_OFF,
        .help = "Num. of LV display presets. Switch with " INFO_BTN_NAME " or from LiveV.",
        .depends_on = DEP_LIVEVIEW,
    },
};
#endif

#ifdef FEATURE_CROPMARKS
static void cropmark_draw_from_cache()
{
    uint8_t* B = bmp_vram();
    uint8_t* M = get_bvram_mirror();
    get_yuv422_vram();
    ASSERT(B);
    ASSERT(M);
    
    for (int i = os.y0; i < os.y_max; i++)
    {
        for (int j = os.x0; j < os.x_max; j++)
        {
            uint8_t p = B[BM(j,i)];
            uint8_t m = M[BM(j,i)];
            if (!(m & 0x80)) continue;
            if (p != 0 && p != 0x14 && p != 0x3 && p != m) continue;
            B[BM(j,i)] = m & ~0x80;
            #ifdef CONFIG_500D
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            #endif
        }
    }
}
#endif

/*
void copy_zebras_from_mirror()
{
    uint32_t* B = (uint32_t*)bmp_vram();
    uint32_t* M = (uint32_t*)get_bvram_mirror();
    ASSERT(B);
    ASSERT(M);
    get_yuv422_vram();
    for (int i = os.y0; i < os.y_max; i++)
    {
        for (int j = os.x0; j < os.x_max; j+=4)
        {
            uint32_t p = B[BM(j,i)/4];
            uint32_t m = M[BM(j,i)/4];
            if (p != 0) continue;
            B[BM(j,i)/4] = m & ~0x80808080;
            #ifdef CONFIG_500D
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            #endif
        }
    }
}

void clear_zebras_from_mirror()
{
    uint8_t* M = (uint8_t*)get_bvram_mirror();
    get_yuv422_vram();
    for (int i = os.y0; i < os.y_max; i++)
    {
        for (int j = os.x0; j < os.x_max; j++)
        {
            uint8_t m = M[BM(j,i)];
            if (m & 0x80) continue;
            M[BM(j,i)] = 0;
            #ifdef CONFIG_500D
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            #endif
        }
    }
}
*/

void cropmark_clear_cache()
{
#ifdef FEATURE_CROPMARKS
    BMP_LOCK(
        clrscr_mirror();
        bvram_mirror_clear();
        default_movie_cropmarks();
    )
#endif
}

#ifdef FEATURE_CROPMARKS
static void 
cropmark_draw()
{
    if (!get_global_draw()) return;

    get_yuv422_vram(); // just to refresh VRAM params
    clear_lv_afframe_if_dirty();

    #ifdef FEATURE_GHOST_IMAGE
    if (transparent_overlay && !transparent_overlay_hidden && !PLAY_MODE)
    {
        show_overlay();
        zoom_overlay_dirty = 1;
        cropmark_cache_dirty = 1;
    }
    #endif
    crop_dirty = 0;

    reload_cropmark(); // reloads only when changed

    // this is very fast
    if (cropmark_cache_is_valid())
    {
        clrscr_mirror();
        cropmark_draw_from_cache();
        //~ bmp_printf(FONT_MED, 50, 50, "crop cached");
        //~ info_led_blink(5, 10, 10);
        goto end;
    }

    if (
            (!crop_enabled) ||
            (cropmark_movieonly && !is_movie_mode() && !PLAY_OR_QR_MODE)
       )
    {
        // Cropmarks disabled (or not shown in this mode)
        // Generate and draw default cropmarks
        cropmark_cache_update_signature();
        cropmark_clear_cache();
        cropmark_draw_from_cache();
        //~ info_led_blink(5,50,50);
        goto end;
    }
    
    if (cropmarks) 
    {
        // Cropmarks enabled, but cache is not valid
        if (!lv) msleep(500); // let the bitmap buffer settle, otherwise ML may see black image and not draw anything (or draw half of cropmark)
        clrscr_mirror(); // clean any remaining zebras / peaking
        cropmark_cache_update_signature();
        
        if (hdmi_code == 5 && is_pure_play_movie_mode())
        {   // exception: cropmarks will have some parts of them outside the screen
            bmp_draw_scaled_ex(cropmarks, BMP_W_MINUS+1, BMP_H_MINUS - 50, 960, 640, bvram_mirror);
        }
        else
            bmp_draw_scaled_ex(cropmarks, os.x0, os.y0, os.x_ex, os.y_ex, bvram_mirror);
        //~ info_led_blink(5,50,50);
        //~ bmp_printf(FONT_MED, 50, 50, "crop regen");
        goto end;
    }

end:
    cropmark_cache_dirty = 0;
    zoom_overlay_dirty = 1;
    crop_dirty = 0;
}

static int cropmark_cache_sig = 0;
static int cropmark_cache_get_signature()
{
    get_yuv422_vram(); // update VRAM params if needed
    int sig = 
        crop_index * 13579 + crop_enabled * 14567 +
        os.x0*811 + os.y0*467 + os.x_ex*571 + os.y_ex*487 + (is_movie_mode() ? 113 : 0) + video_mode_resolution * 8765;
    return sig;
}
static void cropmark_cache_update_signature()
{
    cropmark_cache_sig = cropmark_cache_get_signature();
}

static int cropmark_cache_is_valid()
{
    if (cropmark_cache_dirty) return 0; // some other ML task asked for redraw
    if (hdmi_code == 5 && PLAY_MODE) return 0; // unusual geometry - better force full redraw every time
    
    int sig = cropmark_cache_get_signature(); // video mode changed => needs redraw
    if (cropmark_cache_sig != sig) return 0;
    
    return 1; // everything looks alright
}

static void
cropmark_redraw()
{
    if (!cropmarks_initialized) return;
    if (gui_menu_shown()) return; 
    if (!zebra_should_run() && !PLAY_OR_QR_MODE) return;
    if (digic_zoom_overlay_enabled()) return;
    BMP_LOCK(
        if (!cropmark_cache_is_valid())
        {
            cropmark_clear_cache();
        }
        cropmark_draw(); 
    )
}
#endif

#ifdef FEATURE_OVERLAYS_IN_PLAYBACK_MODE
static void trigger_zebras_for_qr()
{
    fake_simple_button(BTN_ZEBRAS_FOR_PLAYBACK);
    #ifdef CONFIG_600D
    if (BTN_ZEBRAS_FOR_PLAYBACK == BGMT_PRESS_DISP) fake_simple_button(BGMT_UNPRESS_DISP);
    #endif
}
#endif

PROP_HANDLER(PROP_GUI_STATE)
{
    bmp_draw_request_stop(); // abort drawing any slow cropmarks

    lv_paused = 0;
    
#ifdef FEATURE_OVERLAYS_IN_PLAYBACK_MODE
    if (ZEBRAS_IN_QUICKREVIEW && buf[0] == GUISTATE_QR)
        trigger_zebras_for_qr();
#endif

#ifdef FEATURE_GHOST_IMAGE
    if (transparent_overlay && transparent_overlay_auto_update && buf[0] == GUISTATE_QR)
    {
        fake_simple_button(BGMT_LV); // update ghost image
    }
#endif
}

extern uint32_t LCD_Palette[];

void palette_disable(uint32_t disabled)
{
    #ifdef CONFIG_VXWORKS
    return; // see set_ml_palette
    #endif

    if(disabled)
    {
        for (int i = 0; i < 0x100; i++)
        {
            EngDrvOut(0xC0F14400 + i*4, 0x00FF0000);
            EngDrvOut(0xC0F14800 + i*4, 0x00FF0000);
        }
    }
    else
    {
        for (int i = 0; i < 0x100; i++)
        {
            EngDrvOut(0xC0F14400 + i*4, LCD_Palette[i*3 + 2]);
            EngDrvOut(0xC0F14800 + i*4, LCD_Palette[i*3 + 2]);
        }
    }
}
//~ #endif

void bmp_on()
{
    if (!_bmp_unmuted) 
    {
        palette_disable(0);
        _bmp_muted = false; _bmp_unmuted = true;
    }
}

void bmp_off()
{
    if (!_bmp_muted)
    {
        _bmp_muted = true; _bmp_unmuted = false;
        palette_disable(1);
    }
}

void bmp_mute_flag_reset()
{
    _bmp_muted = 0;
    _bmp_unmuted = 0;
}

#ifdef FEATURE_MAGIC_ZOOM
static void zoom_overlay_toggle()
{
    zoom_overlay_triggered_by_zoom_btn = !zoom_overlay_triggered_by_zoom_btn;
    if (!zoom_overlay_triggered_by_zoom_btn)
    {
        zoom_overlay_triggered_by_focus_ring_countdown = 0;
    }
}

int handle_zoom_overlay(struct event * event)
{
    if (gui_menu_shown()) return 1;
    if (!lv) return 1;
    if (!get_global_draw()) return 1;
    #ifdef CONFIG_600D
    if (get_disp_pressed()) return 1;
    #endif

#ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
    if (event->param == BGMT_PRESS_HALFSHUTTER && get_zoom_overlay_trigger_by_halfshutter())
        zoom_overlay_toggle();
    if (is_zoom_overlay_triggered_by_zoom_btn() && !get_zoom_overlay_trigger_by_halfshutter())
        zoom_overlay_toggle();
#else

    // zoom in when recording => enable Magic Zoom 
    if (get_zoom_overlay_trigger_mode() && recording == 2 && MVR_FRAME_NUMBER > 10 && event->param == 
        #if defined(CONFIG_5D3) || defined(CONFIG_6D)
        BGMT_PRESS_ZOOMIN_MAYBE
        #else
        BGMT_UNPRESS_ZOOMIN_MAYBE
        #endif
    )
    {
        zoom_overlay_toggle();
        return 0;
    }

    // if magic zoom is enabled, Zoom In should always disable it 
    if (is_zoom_overlay_triggered_by_zoom_btn() && event->param == BGMT_PRESS_ZOOMIN_MAYBE)
    {
        zoom_overlay_toggle();
        return 0;
    }
    
    if (get_zoom_overlay_trigger_mode() && lv_dispsize == 1 && event->param == BGMT_PRESS_ZOOMIN_MAYBE)
    {
        #ifdef FEATURE_LCD_SENSOR_SHORTCUTS
        int lcd_sensor_trigger = (get_lcd_sensor_shortcuts() && display_sensor && DISPLAY_SENSOR_POWERED);
        #else
        int lcd_sensor_trigger = 0;
        #endif
        // magic zoom toggled by sensor+zoom in (modes Zr and Zr+F)
        if (get_zoom_overlay_trigger_mode() < 3 && lcd_sensor_trigger)
        {
            zoom_overlay_toggle();
            return 0;
        }
        // (*): magic zoom toggled by zoom in, normal zoom by sensor+zoom in
        else if (get_zoom_overlay_trigger_mode() == MZ_TAKEOVER_ZOOM_IN_BTN && !get_halfshutter_pressed() && !lcd_sensor_trigger)
        {
            zoom_overlay_toggle();
            return 0;
        }
    }
#endif

    // move AF frame when recording
    if (recording && liveview_display_idle() && is_manual_focus())
    {
        if (event->param == BGMT_PRESS_LEFT)
            { move_lv_afframe(-300, 0); return 0; }
        if (event->param == BGMT_PRESS_RIGHT)
            { move_lv_afframe(300, 0); return 0; }
        if (event->param == BGMT_PRESS_UP)
            { move_lv_afframe(0, -300); return 0; }
        if (event->param == BGMT_PRESS_DOWN)
            { move_lv_afframe(0, 300); return 0; }
    }

    return 1;
}

void zoom_overlay_disable()
{
    zoom_overlay_triggered_by_zoom_btn = 0;
    zoom_overlay_triggered_by_focus_ring_countdown = 0;
}

void zoom_overlay_set_countdown(int x)
{
    zoom_overlay_triggered_by_focus_ring_countdown = x;
}

void digic_zoom_overlay_step(int force_off)
{
#if !defined(CONFIG_VXWORKS)
    static int prev = 0;
    if (digic_zoom_overlay_enabled() && !force_off)
    {
        if (!prev) // first iteration after trigger
        {
            redraw();
        }
        else
        {
            // center of AF frame
            int aff_x0_lv, aff_y0_lv; 
            get_afframe_pos(720, 480, &aff_x0_lv, &aff_y0_lv); // Get the center of the AF frame in normalized coordinates

            // Translate it into LV coord space
            aff_x0_lv = N2LV_X(aff_x0_lv);
            aff_y0_lv = N2LV_Y(aff_y0_lv);

            // Translate it into HD coord space
            int aff_x0_hd = LV2HD_X(aff_x0_lv);
            int aff_y0_hd = LV2HD_Y(aff_y0_lv);
            
            // Find the top-left corner point in HD space
            int corner_x0 = COERCE(aff_x0_hd - vram_lv.width/2, 0, vram_hd.width - vram_lv.width);
            int corner_y0 = COERCE(aff_y0_hd - vram_lv.height/2, 0, vram_hd.height - vram_lv.height);

            // Compute offset for HD buffer
            int offset = corner_x0 * 2 + corner_y0 * vram_hd.pitch;

            // Redirect the display buffer to show the magnified area
            YUV422_LV_BUFFER_DISPLAY_ADDR = prev + offset;
            
            // and make sure the pitch is right
            EngDrvOut(0xc0f140e8, vram_hd.pitch - vram_lv.pitch);
        }
        
        prev = YUV422_HD_BUFFER_DMA_ADDR;
    }
    else
    {
        if (prev) 
        {
            EngDrvOut(0xc0f140e8, 0);
        }
        prev = 0;
    }
#endif
}

/**
 * Draw Magic Zoom overlay
 */
static void draw_zoom_overlay(int dirty)
{   
    if (zoom_overlay_size == 3) return; // fullscreen zoom done via digic
    
    //~ if (vram_width > 720) return;
    if (!lv) return;
    if (!get_global_draw()) return;
    //~ if (gui_menu_shown()) return;
    if (!bmp_is_on()) return;
    if (lv_dispsize != 1) return;
    //~ if (get_halfshutter_pressed() && clearscreen != 2) return;
    if (recording == 1) return;
    
    #ifndef CONFIG_LV_FOCUS_INFO
    zoom_overlay_split = 0; // 50D doesn't report focus
    #endif
    
    struct vram_info *  lv = get_yuv422_vram();
    struct vram_info *  hd = get_yuv422_hd_vram();

    if( !lv->vram ) return;
    if( !hd->vram ) return;
    if( !bmp_vram()) return;

    uint16_t*       lvr = (uint16_t*) lv->vram;
    uint16_t*       hdr = (uint16_t*) hd->vram;

    // select buffer where MZ should be written (camera-specific, guesswork)
    #if defined(CONFIG_5D2) || defined(CONFIG_EOSM)
    /* fixme: ugly hack */
    void busy_vsync(int hd, int timeout_ms)
    {
        int timeout_us = timeout_ms * 1000;
        void* old = (void*)shamem_read(hd ? REG_EDMAC_WRITE_HD_ADDR : REG_EDMAC_WRITE_LV_ADDR);
        int t0 = *(uint32_t*)0xC0242014;
        while(1)
        {
            int t1 = *(uint32_t*)0xC0242014;
            int dt = mod(t1 - t0, 1048576);
            void* new = (void*)shamem_read(hd ? REG_EDMAC_WRITE_HD_ADDR : REG_EDMAC_WRITE_LV_ADDR);
            if (old != new) break;
            if (dt > timeout_us)
                return 0;
            for (int i = 0; i < 100; i++) asm("nop"); // don't stress the digic too much
        }
    }
    lvr = (uint16_t*) shamem_read(REG_EDMAC_WRITE_LV_ADDR);
    busy_vsync(0, 20);
    #endif
    #if defined(CONFIG_DIGIC_V)
    lvr = CACHEABLE(YUV422_LV_BUFFER_DISPLAY_ADDR);
    if (lvr != CACHEABLE(YUV422_LV_BUFFER_1) && lvr != CACHEABLE(YUV422_LV_BUFFER_2) && lvr != CACHEABLE(YUV422_LV_BUFFER_3)) return;
    #else
    #endif
    
    if (!lvr) return;
    
    // center of AF frame
    int aff_x0_lv, aff_y0_lv; 
    get_afframe_pos(720, 480, &aff_x0_lv, &aff_y0_lv); // Get the center of the AF frame in normalized coordinates

    // Translate it into LV coord space
    aff_x0_lv = N2LV_X(aff_x0_lv);
    aff_y0_lv = N2LV_Y(aff_y0_lv);

    // Translate it into HD coord space
    int aff_x0_hd = LV2HD_X(aff_x0_lv);
    int aff_y0_hd = LV2HD_Y(aff_y0_lv);
    
    int W = 0, H = 0;
    
    switch(zoom_overlay_size)
    {
        case 0:
            W = os.x_ex / 5;
            H = os.y_ex / 4;
            break;
        case 1:
            W = os.x_ex / 3;
            H = os.y_ex * 2/5;
            break;
        case 2:
            W = os.x_ex/2;
            H = os.y_ex/2;
            break;
        case 3:
            W = os.x_ex;
            H = os.y_ex;
            break;
    }

    // Magnification factor
    int X = zoom_overlay_x + 1;

    // Center of Magic Zoom box in the LV coordinate space
    int zb_x0_lv, zb_y0_lv; 

    switch(zoom_overlay_pos)
    {
        case 0: // AFF
            zb_x0_lv = aff_x0_lv;
            zb_y0_lv = aff_y0_lv;
            break;
        case 1: // NW
            zb_x0_lv = W/2 + 50;
            zb_y0_lv = H/2 + 50;
            break;
        case 2: // NE
            zb_x0_lv = BM2LV_X(os.x_max) - W/2 - 50;
            zb_y0_lv = H/2 + 50;
            break;
        case 3: // SE
            zb_x0_lv = BM2LV_X(os.x_max) - W/2 - 50;
            zb_y0_lv = BM2LV_Y(os.y_max) - H/2 - 50;
            break;
        case 4: // SV
            zb_x0_lv = W/2 + 50;
            zb_y0_lv = BM2LV_Y(os.y_max) - H/2 - 50;
            break;
        default:
            return;
    }
    //~ bmp_printf(FONT_LARGE, 50, 50, "%d,%d %d,%d", W, H, aff_x0_lv);

    if (zoom_overlay_pos)
    {
        int w = W * lv->width / hd->width;
        int h = H * lv->width / hd->width;

        #ifdef CONFIG_1100D
        h /= 2; // LCD half-height fix
        #endif
        w /= X;
        h /= X;
        const int val_in_coerce_w = aff_x0_lv - (w>>1);
        const int coerce_w = COERCE(val_in_coerce_w, 0, 720-w);
        const int val_in_coerce_h1 = aff_y0_lv - (h>>1);
        const int val_in_coerce_h2 = aff_y0_lv + (h>>1);

        memset64(lvr + coerce_w + COERCE(val_in_coerce_h1 - 2, 0, lv->height) * lv->width, MZ_BLACK, w<<1);
        memset64(lvr + coerce_w + COERCE(val_in_coerce_h1 - 1, 0, lv->height) * lv->width, MZ_WHITE, w<<1);
        memset64(lvr + coerce_w + COERCE(val_in_coerce_h2 + 1, 0, lv->height) * lv->width, MZ_WHITE, w<<1);
        memset64(lvr + coerce_w + COERCE(val_in_coerce_h2 + 2, 0, lv->height) * lv->width, MZ_BLACK, w<<1);
    }

    //~ draw_circle(x0,y0,45,COLOR_WHITE);
    int y;
    int x0c = COERCE(zb_x0_lv - (W>>1), 0, lv->width-W);
    int y0c = COERCE(zb_y0_lv - (H>>1), 0, lv->height-H);

    extern int focus_value;
    extern int focus_min_value;
    //~ bmp_printf(FONT_MED, 300, 100, "%d %d ", focus_value, focus_min_value);
    int rawoff = COERCE(80 - focus_value, 0, 100) >> 2;
    if (focus_min_value > 60) rawoff = 1; // out of focus?
    
    // reverse the sign of split when perfect focus is achieved
    static int rev = 0;
    static int poff = 0;
    if (rawoff != 0 && poff == 0) rev = !rev;
    poff = rawoff;
    if (zoom_overlay_split == 1 /* non zerocross */) rev = 0;

    uint16_t* d = lvr + x0c + (y0c + 2) * lv->width;
    uint16_t* s = hdr + (aff_y0_hd - (H/2/X)) * hd->width + (aff_x0_hd - (W/2/X));
    for (y = 2; y < H-2; y++)
    {
        int off = zoom_overlay_split ? (y < H/2 ? rawoff : -rawoff) : 0;
        if (rev) off = -off;
        #ifdef CONFIG_1100D
        if(y%2 == 0) // The 1100D has half-height LCD res so we line-skip one from the sensor
        #endif
        {
            yuvcpy_main((uint32_t*)d, (uint32_t*)(s + off), W, X, 0 /*zoom_overlay_lut*/);
            d += lv->width;
        }
        if (y%X==0) s += hd->width;
    }

    #ifdef CONFIG_1100D
    H /= 2; //LCD res fix (half height)
    #endif
    memset64(lvr + x0c + COERCE(0   + y0c, 0, 720) * lv->width, rawoff ? MZ_BLACK : MZ_GREEN, W<<1);
    memset64(lvr + x0c + COERCE(1   + y0c, 0, 720) * lv->width, rawoff ? MZ_WHITE : MZ_GREEN, W<<1);
    memset64(lvr + x0c + COERCE(H-1 + y0c, 0, 720) * lv->width, rawoff ? MZ_WHITE : MZ_GREEN, W<<1);
    memset64(lvr + x0c + COERCE(H   + y0c, 0, 720) * lv->width, rawoff ? MZ_BLACK : MZ_GREEN, W<<1);
    #ifdef CONFIG_1100D
    H *= 2; // Undo it
    #endif

    if (dirty) bmp_fill(0, LV2BM_X(x0c), LV2BM_Y(y0c), LV2BM_DX(W), LV2BM_DY(H));
    //~ bmp_fill(rawoff ? COLOR_BLACK : COLOR_GREEN1, x0c, y0c, W, 1);
    //~ bmp_fill(rawoff ? COLOR_WHITE : COLOR_GREEN2, x0c+1, y0c, W, 1);
    //~ bmp_fill(rawoff ? COLOR_WHITE : COLOR_GREEN2, x0c, y0c + H - 1, W, 1);
    //~ bmp_fill(rawoff ? COLOR_BLACK : COLOR_GREEN1, x0c, y0c + H, W, 1);
}
#endif // FEATURE_MAGIC_ZOOM

int liveview_display_idle()
{
    struct gui_task * current = gui_task_list.current;
    struct dialog * dialog = current->priv;
    extern thunk LiveViewApp_handler;
    extern uintptr_t new_LiveViewApp_handler;

    #if defined(CONFIG_5D3)
    extern thunk LiveViewLevelApp_handler;
    #elif defined(CONFIG_DIGIC_V)
    extern thunk LiveViewShutterApp_handler;
    #endif

    #if defined(CONFIG_6D)
    extern thunk LiveViewWifiApp_handler;
    #endif

    return
        LV_NON_PAUSED && 
        DISPLAY_IS_ON &&
        !menu_active_and_not_hidden() && 
        (// gui_menu_shown() || // force LiveView when menu is active, but hidden
            ( gui_state == GUISTATE_IDLE && 
              (dialog->handler == (dialog_handler_t) &LiveViewApp_handler || dialog->handler == (dialog_handler_t) new_LiveViewApp_handler
                  #if defined(CONFIG_5D3)
                  || dialog->handler == (dialog_handler_t) &LiveViewLevelApp_handler
                  #endif
                  #if defined(CONFIG_6D)
                  || dialog->handler == (dialog_handler_t) &LiveViewWifiApp_handler
                  #endif
                  //~ for this, check value of get_current_dialog_handler()
                  #if defined(CONFIG_DIGIC_V) && !defined(CONFIG_5D3)
                  || dialog->handler == (dialog_handler_t) &LiveViewShutterApp_handler
                  #endif
              ) &&
            CURRENT_DIALOG_MAYBE <= 3 && 
            #ifdef CURRENT_DIALOG_MAYBE_2
            CURRENT_DIALOG_MAYBE_2 <= 3 &&
            #endif
            job_state_ready_to_take_pic() &&
            !mirror_down )
        );
}

// when it's safe to draw zebras and other on-screen stuff
int zebra_should_run()
{
    return liveview_display_idle() && get_global_draw() &&
        !is_zoom_mode_so_no_zebras() &&
        !(clearscreen == 1 && (get_halfshutter_pressed() || dofpreview)) &&
        !WAVEFORM_FULLSCREEN;
}

#ifdef FEATURE_OVERLAYS_IN_PLAYBACK_MODE
static int livev_for_playback_running = 0;
static void draw_livev_for_playback()
{
    livev_for_playback_running = 1;

    if (!PLAY_OR_QR_MODE)
    {
        livev_for_playback_running = 0;
        return;
    }

    extern int quick_review_allow_zoom;
    if (quick_review_allow_zoom && image_review_time == 0xff)
    {
        // wait for the camera to switch from QR to PLAY before drawing anything
        while (!PLAY_MODE) msleep(100);
        msleep(500);
    }
    while (!DISPLAY_IS_ON) msleep(100);
    if (!PLAY_OR_QR_MODE)
    {
        livev_for_playback_running = 0;
        return;
    }
    if (QR_MODE) msleep(300);

    get_yuv422_vram(); // just to refresh VRAM params

    info_led_on();

    if (PLAY_OR_QR_MODE) EngDrvOut(DIGIC_ZEBRA_REGISTER, 0); // disable Canon highlight warning, looks ugly with both on the screen :)

BMP_LOCK(

    bvram_mirror_clear(); // may be filled with liveview cropmark / masking info, not needed in play mode
    clrscr();

    #ifdef FEATURE_CROPMARKS
    cropmark_redraw();
    #endif

    #ifdef FEATURE_DEFISHING_PREVIEW
    extern int defish_preview;
    if (defish_preview)
        defish_draw_play();
    #endif

    #ifdef FEATURE_SPOTMETER
    if (spotmeter_draw)
        spotmeter_step();
    #endif

    draw_histogram_and_waveform(1);

    #ifdef FEATURE_FALSE_COLOR
    if (falsecolor_draw) 
    {
        draw_false_downsampled();
    }
    else
    #endif
    {
        guess_focus_peaking_threshold();
        draw_zebra_and_focus(1,1);
    }
    
    #ifdef FEATURE_POST_DEFLICKER
    post_deflicker_show_info();
    #endif

    bvram_mirror_clear(); // may remain filled with playback zebras 
)

    clean_d_cache(); // to avoid display artifacts

    info_led_off();
    livev_for_playback_running = 0;
}
#endif

int should_draw_bottom_graphs()
{
    if (!lv) return 0;
    if (gui_menu_shown()) return 0;
    int screen_layout = get_screen_layout();
    if (screen_layout == SCREENLAYOUT_4_3 && lv_disp_mode == 0) return 1;
    return 0;
}

void draw_histogram_and_waveform(int allow_play)
{

    if (menu_active_and_not_hidden()) return;
    if (!get_global_draw()) return;

    get_yuv422_vram();

#if defined(FEATURE_HISTOGRAM) || defined(FEATURE_WAVEFORM) || defined(FEATURE_VECTORSCOPE)
    if (hist_draw || waveform_draw || vectorscope_draw)
    {
        hist_build(); /* also updates waveform and vectorscope */
        #ifdef FEATURE_RAW_HISTOGRAM
        if (raw_histogram_enable && can_use_raw_overlays())
            hist_build_raw();
        #endif
    }
#endif
    
    if (menu_active_and_not_hidden()) return; // hack: not to draw histo over menu
    if (!get_global_draw()) return;
    if (!liveview_display_idle() && !(PLAY_OR_QR_MODE && allow_play) && !gui_menu_shown()) return;
    if (is_zoom_mode_so_no_zebras()) return;

    int screen_layout = get_screen_layout();

#ifdef FEATURE_HISTOGRAM
    if( hist_draw && !WAVEFORM_FULLSCREEN)
    {
        #ifdef CONFIG_4_3_SCREEN
        if (PLAY_OR_QR_MODE)
            BMP_LOCK( hist_draw_image( os.x0 + 500,  1, -1); )
        else
        #endif
        if (should_draw_bottom_graphs())
            BMP_LOCK( hist_draw_image( os.x0 + 50,  480 - hist_height - 1, -1); )
        else if (screen_layout == SCREENLAYOUT_3_2)
            BMP_LOCK( hist_draw_image( os.x_max - HIST_WIDTH - 2,  os.y_max - (lv ? os.off_169 : 0) - (gui_menu_shown() ? 25 : 0) - hist_height - 1, -1); )
        else
            BMP_LOCK( hist_draw_image( os.x_max - HIST_WIDTH - 5, os.y0 + 100, -1); )
    }
#endif

    if (menu_active_and_not_hidden()) return;
    if (!get_global_draw()) return;
    if (!liveview_display_idle() && !(PLAY_OR_QR_MODE && allow_play) && !gui_menu_shown()) return;
    if (is_zoom_mode_so_no_zebras()) return;
        
#ifdef FEATURE_WAVEFORM
    if( waveform_draw)
    {
        #ifdef CONFIG_4_3_SCREEN
        if (PLAY_OR_QR_MODE && WAVEFORM_FACTOR == 1)
            BMP_LOCK( waveform_draw_image( os.x0 + 100,  1, 54); )
        else
        #endif
        if (should_draw_bottom_graphs() && WAVEFORM_FACTOR == 1)
            BMP_LOCK( waveform_draw_image( os.x0 + 250,  480 - 54, 54); )
        else if (screen_layout == SCREENLAYOUT_3_2 && !WAVEFORM_FULLSCREEN)
        {
            if (WAVEFORM_FACTOR == 1)
                BMP_LOCK( waveform_draw_image( os.x0 + 4, os.y_max - (lv ? os.off_169 : 0) - (gui_menu_shown() ? 25 : 0) - 54, 54); )
            else
                BMP_LOCK( waveform_draw_image( os.x_max - WAVEFORM_WIDTH*WAVEFORM_FACTOR - 4, os.y0 + 100, WAVEFORM_HEIGHT*WAVEFORM_FACTOR ); );
        }
        else
            BMP_LOCK( waveform_draw_image( os.x_max - WAVEFORM_WIDTH*WAVEFORM_FACTOR - (WAVEFORM_FULLSCREEN ? 0 : 4), os.y_max - WAVEFORM_HEIGHT*WAVEFORM_FACTOR - WAVEFORM_OFFSET, WAVEFORM_HEIGHT*WAVEFORM_FACTOR ); )
    }
#endif

#ifdef FEATURE_VECTORSCOPE
    if(vectorscope_draw)
    {
        /* make sure memory address of bvram will be 4 byte aligned */
        BMP_LOCK( vectorscope_draw_image(os.x0 + 32, 64); )
    }
#endif
}

static int idle_countdown_display_dim = 50;
static int idle_countdown_display_off = 50;
static int idle_countdown_globaldraw = 50;
static int idle_countdown_clrscr = 50;
#ifdef FEATURE_POWERSAVE_LIVEVIEW
static int idle_countdown_display_dim_prev = 50;
static int idle_countdown_display_off_prev = 50;
static int idle_countdown_globaldraw_prev = 50;
static int idle_countdown_clrscr_prev = 50;
#endif

#ifdef CONFIG_KILL_FLICKER
static int idle_countdown_killflicker = 5;
static int idle_countdown_killflicker_prev = 5;
#endif

static int idle_is_powersave_enabled()
{
#ifdef FEATURE_POWERSAVE_LIVEVIEW
    return idle_display_dim_after || idle_display_turn_off_after || idle_display_global_draw_off_after;
#else
    return 0;
#endif
}

static int idle_is_powersave_active()
{
#ifdef FEATURE_POWERSAVE_LIVEVIEW
    return (idle_display_dim_after && !idle_countdown_display_dim_prev) || 
           (idle_display_turn_off_after && !idle_countdown_display_off_prev) || 
           (idle_display_global_draw_off_after && !idle_countdown_globaldraw_prev);
#else
    return 0;
#endif
}

void idle_force_powersave_in_1s()
{
    idle_countdown_display_off = MIN(idle_countdown_display_off, 10);
    idle_countdown_display_dim = MIN(idle_countdown_display_dim, 10);
    idle_countdown_globaldraw  = MIN(idle_countdown_globaldraw, 10);
}

void idle_force_powersave_now()
{
    idle_countdown_display_off = MIN(idle_countdown_display_off, 1);
    idle_countdown_display_dim = MIN(idle_countdown_display_dim, 1);
    idle_countdown_globaldraw  = MIN(idle_countdown_globaldraw, 1);
}

int handle_powersave_key(struct event * event)
{
    #ifdef FEATURE_POWERSAVE_LIVEVIEW
    if (event->param == BGMT_INFO)
    {
        if (!idle_shortcut_key) return 1;
        if (!lv) return 1;
        if (!idle_is_powersave_enabled()) return 1;
        if (IS_FAKE(event)) return 1;
        if (gui_menu_shown()) return 1;

        if (!idle_is_powersave_active())
        {
            idle_force_powersave_now();
            info_led_blink(1,50,0);
        }
        return 0;
    }
    #endif
    return 1;
}

void idle_wakeup_reset_counters(int reason) // called from handle_buttons
{
    if (ml_shutdown_requested) return;
    
#if 0
    NotifyBox(2000, "wakeup: %d   ", reason);
#endif

    //~ bmp_printf(FONT_LARGE, 50, 50, "wakeup: %d   ", reason);
    
    // when sensor is covered, timeout changes to 3 seconds
    int sensor_status = lcd_sensor_wakeup && display_sensor && DISPLAY_SENSOR_POWERED;

    // those are for powersaving
    idle_countdown_display_off = sensor_status ? 25 : idle_display_turn_off_after * 10;
    idle_countdown_display_dim = sensor_status ? 25 : idle_display_dim_after * 10;
    idle_countdown_globaldraw  = sensor_status ? 25 : idle_display_global_draw_off_after * 10;

    if (reason == -2345) // disable powersave during recording 
        return;

    // those are not for powersaving
    idle_countdown_clrscr = 30;
    
    if (reason == -10 || reason == -11) // focus event (todo: should define constants for those)
        return;
    
#ifdef CONFIG_KILL_FLICKER
    idle_countdown_killflicker = 10;
#endif
}

// called at 10 Hz
static void update_idle_countdown(int* countdown)
{
    //~ bmp_printf(FONT_MED, 200, 200, "%d  ", *countdown);
    if ((liveview_display_idle() && !get_halfshutter_pressed() && !gui_menu_shown()) || !DISPLAY_IS_ON)
    {
        if (*countdown)
            (*countdown)--;
    }
    else
    {
        idle_wakeup_reset_counters(-100); // will reset all idle countdowns
    }
    
    int sensor_status = lcd_sensor_wakeup && display_sensor && DISPLAY_SENSOR_POWERED;
    static int prev_sensor_status = 0;

    if (sensor_status != prev_sensor_status)
        idle_wakeup_reset_counters(-1);
    
    prev_sensor_status = sensor_status;
}

static void idle_action_do(int* countdown, int* prev_countdown, void(*action_on)(void), void(*action_off)(void))
{
    if (ml_shutdown_requested) return;
    
    update_idle_countdown(countdown);
    int c = *countdown; // *countdown may be changed by "wakeup" => race condition
    //~ bmp_printf(FONT_MED, 100, 200, "%d->%d ", *prev_countdown, c);
    if (*prev_countdown && !c)
    {
        //~ info_led_blink(1, 50, 50);
        //~ bmp_printf(FONT_MED, 100, 200, "action  "); msleep(500);
        action_on();
        //~ msleep(500);
        //~ bmp_printf(FONT_MED, 100, 200, "        ");
    }
    else if (!*prev_countdown && c)
    {
        //~ info_led_blink(1, 50, 50);
        //~ bmp_printf(FONT_MED, 100, 200, "unaction"); msleep(500);
        action_off();
        //~ msleep(500);
        //~ bmp_printf(FONT_MED, 100, 200, "        ");
    }
    *prev_countdown = c;
}

#if defined(CONFIG_LIVEVIEW) && defined(FEATURE_POWERSAVE_LIVEVIEW)
static int lv_zoom_before_pause = 0;
#endif

void PauseLiveView() // this should not include "display off" command
{
#if defined(CONFIG_LIVEVIEW) && defined(FEATURE_POWERSAVE_LIVEVIEW)
    if (ml_shutdown_requested) return;
    if (sensor_cleaning) return;
    if (PLAY_MODE) return;
    if (MENU_MODE) return;
    if (LV_NON_PAUSED)
    {
        //~ ASSERT(DISPLAY_IS_ON);
        int x = 1;
        //~ while (get_halfshutter_pressed()) msleep(MIN_MSLEEP);
        BMP_LOCK(
            lv_zoom_before_pause = lv_dispsize;
            prop_request_change(PROP_LV_ACTION, &x, 4);
            msleep(100);
            clrscr();
            lv_paused = 1;
        )
        ASSERT(LV_PAUSED);
    }
#endif
}

// returns 1 if it did wakeup
int ResumeLiveView()
{
    info_led_on();
    int ans = 0;
#if defined(CONFIG_LIVEVIEW) && defined(FEATURE_POWERSAVE_LIVEVIEW)
    if (ml_shutdown_requested) return 0;
    if (sensor_cleaning) return 0;
    if (PLAY_MODE) return 0;
    if (MENU_MODE) return 0;
    if (LV_PAUSED)
    {
        int x = 0;
        //~ while (get_halfshutter_pressed()) msleep(MIN_MSLEEP);
        BMP_LOCK(
            prop_request_change(PROP_LV_ACTION, &x, 4);
            int iter = 10; while (!lv && iter--) msleep(100);
            iter = 10; while (!DISPLAY_IS_ON && iter--) msleep(100);
        )
        while (sensor_cleaning) msleep(100);
        if (lv) set_lv_zoom(lv_zoom_before_pause);
        msleep(100);
        ans = 1;
    }
    lv_paused = 0;
    idle_wakeup_reset_counters(-1357);
    info_led_off();
#endif
    return ans;
}

#ifdef FEATURE_POWERSAVE_LIVEVIEW
static void idle_display_off_show_warning()
{
    extern int motion_detect;
    if (motion_detect || recording)
    {
        NotifyBox(3000, "DISPLAY OFF...");
    }
    else
    {
        NotifyBox(3000, "DISPLAY AND SENSOR OFF...");
    }
}
static void idle_display_off()
{
    extern int motion_detect;
    if (!(motion_detect || recording)) PauseLiveView();
    display_off();
    msleep(300);
    idle_countdown_display_off = 0;
    ASSERT(!(recording && LV_PAUSED));
    ASSERT(!DISPLAY_IS_ON);
}
static void idle_display_on()
{
    //~ card_led_blink(5, 50, 50);
    ResumeLiveView();
    display_on();
    redraw();
    //~ ASSERT(DISPLAY_IS_ON); // it will take a short time until display will turn on
}

static void idle_bmp_off()
{
    bmp_off();
}
static void idle_bmp_on()
{
    bmp_on();
}

static int old_backlight_level = 0;
static void idle_display_dim()
{
    ASSERT(lv);
    #ifdef CONFIG_AUTO_BRIGHTNESS
    int backlight_mode = lcd_brightness_mode;
    if (backlight_mode == 0) // can't restore brightness properly in auto mode
    {
        NotifyBox(2000, "LCD brightness is automatic.\n"
                        "ML will not dim the display.");
        return;
    }
    #endif

    old_backlight_level = backlight_level;
    set_backlight_level(1);
}
static void idle_display_undim()
{
    if (old_backlight_level)
    {
        set_backlight_level(old_backlight_level);
        old_backlight_level = 0;
    }
}

#endif

void idle_globaldraw_dis()
{
    idle_globaldraw_disable = 1;
}
void idle_globaldraw_en()
{
    idle_globaldraw_disable = 0;
}

#ifdef CONFIG_KILL_FLICKER
static void idle_kill_flicker()
{
    if (!canon_gui_front_buffer_disabled())
    {
        get_yuv422_vram();
        canon_gui_disable_front_buffer();
        clrscr();
        if (is_movie_mode())
        {
            black_bars_16x9();
            if (recording)
                dot(os.x_max - 28, os.y0 + 12, COLOR_RED, 10);
        }
    }
}
static void idle_stop_killing_flicker()
{
    if (canon_gui_front_buffer_disabled())
    {
        canon_gui_enable_front_buffer(0);
    }
}
#endif

static PROP_INT(PROP_LOGICAL_CONNECT, logical_connect); // EOS utility?

static void
clearscreen_task( void* unused )
{
    idle_wakeup_reset_counters(0);

    TASK_LOOP
    {
clearscreen_loop:
        msleep(100);

        //~ bmp_printf(FONT_MED, 100, 100, "%d %d %d", idle_countdown_display_dim, idle_countdown_display_off, idle_countdown_globaldraw);

        // Here we're blinking the info LED approximately once every five
        // seconds to show the user that their camera is still on and has
        // not dropped into standby mode.  But it's distracting to blink
        // it every five seconds, and if the user pushed a button recently
        // then they already _know_ that their camera is still on, so
        // let's only do it if the camera's buttons have been idle for at
        // least 30 seconds.
        if (k % 50 == 0 && !DISPLAY_IS_ON && lens_info.job_state == 0 && !recording && !get_halfshutter_pressed() && !is_intervalometer_running() && idle_blink)
            if ((get_seconds_clock() - get_last_time_active()) > 30)
                info_led_blink(1, 10, 10);

        if (!lv && !lv_paused) continue;

        // especially for 50D
        #ifdef CONFIG_KILL_FLICKER
        if (kill_canon_gui_mode == 1)
        {
            if (ZEBRAS_IN_LIVEVIEW && !gui_menu_shown())
            {
                int idle = liveview_display_idle() && lv_disp_mode == 0;
                if (idle)
                {
                    if (!canon_gui_front_buffer_disabled())
                        idle_kill_flicker();
                }
                else
                {
                    if (canon_gui_front_buffer_disabled())
                        idle_stop_killing_flicker();
                }
                static int prev_idle = 0;
                if (!idle && prev_idle != idle) redraw();
                prev_idle = idle;
            }
        }
        #endif
        
        #ifdef FEATURE_CLEAR_OVERLAYS
        if (clearscreen == 3)
        {
            if (liveview_display_idle() && !gui_menu_shown())
            {
                bmp_off();
            }
            else
            {
                bmp_on();
            }
        }
        
        if (clearscreen == 4)
        {
            if (recording)
            {
                bmp_off();
            }
            else
            {
                bmp_on();
            }
        }

        // clear overlays on shutter halfpress
        if (clearscreen == 1 && (get_halfshutter_pressed() || dofpreview) && !gui_menu_shown())
        {
            BMP_LOCK( clrscr_mirror(); )
            int i;
            for (i = 0; i < (int)clearscreen_delay/20; i++)
            {
                if (i % 10 == 0 && liveview_display_idle()) BMP_LOCK( update_lens_display(); )
                msleep(20);
                if (!(get_halfshutter_pressed() || dofpreview))
                    goto clearscreen_loop;
            }
            bmp_off();
            while ((get_halfshutter_pressed() || dofpreview)) msleep(100);
            bmp_on();
            #ifdef CONFIG_ZOOM_BTN_NOT_WORKING_WHILE_RECORDING
            msleep(100);
            if (get_zoom_overlay_trigger_by_halfshutter()) // this long press should not trigger MZ
                zoom_overlay_toggle();
            #endif
        }
        #endif

        if (recording && idle_rec == 0) // don't go to powersave when recording
            idle_wakeup_reset_counters(-2345);

        if (!recording && idle_rec == 1) // don't go to powersave when not recording
            idle_wakeup_reset_counters(-2345);
        
        if (logical_connect)
            idle_wakeup_reset_counters(-305); // EOS utility
        
        #ifdef FEATURE_POWERSAVE_LIVEVIEW
        if (idle_display_dim_after)
            idle_action_do(&idle_countdown_display_dim, &idle_countdown_display_dim_prev, idle_display_dim, idle_display_undim);

        if (idle_display_turn_off_after)
        {
            idle_action_do(&idle_countdown_display_off, &idle_countdown_display_off_prev, idle_display_off, idle_display_on);

            // show a warning that display is going to be turned off (and clear it if some button is pressed)
            static int warning_dirty = 0;
            if (idle_countdown_display_off == 30)
            {
                idle_display_off_show_warning();
                warning_dirty = 1;
            }
            else if (warning_dirty && idle_countdown_display_off > 30)
            {
                NotifyBoxHide();
                warning_dirty = 0;
            }
        }

        if (idle_display_global_draw_off_after)
            idle_action_do(&idle_countdown_globaldraw, &idle_countdown_globaldraw_prev, idle_globaldraw_dis, idle_globaldraw_en);

        if (clearscreen == 2) // clear overlay when idle
            idle_action_do(&idle_countdown_clrscr, &idle_countdown_clrscr_prev, idle_bmp_off, idle_bmp_on);
        #endif
        
        #ifdef CONFIG_KILL_FLICKER
        if (kill_canon_gui_mode == 2) // LV transparent menus and key presses
        {
            if (ZEBRAS_IN_LIVEVIEW && !gui_menu_shown() && lv_disp_mode == 0)
                idle_action_do(&idle_countdown_killflicker, &idle_countdown_killflicker_prev, idle_kill_flicker, idle_stop_killing_flicker);
        }
        #endif

        #ifdef FEATURE_CROPMARKS
        // since this task runs at 10Hz, I prefer cropmark redrawing here

        if (crop_dirty && lv && zebra_should_run())
        {
            crop_dirty--;
            
            //~ bmp_printf(FONT_MED, 50, 100, "crop: cache=%d dirty=%d ", cropmark_cache_is_valid(), crop_dirty);

            // if cropmarks are disabled, we will still draw default cropmarks (fast)
            if (!(crop_enabled && cropmark_movieonly && !is_movie_mode())) 
                crop_dirty = MIN(crop_dirty, 4);
            
            // if cropmarks are cached, we can redraw them fast
            if (cropmark_cache_is_valid() && !should_draw_zoom_overlay() && !get_halfshutter_pressed())
                crop_dirty = MIN(crop_dirty, 4);
                
            if (crop_dirty == 0)
                cropmark_redraw();
        }
        #endif
    }
}

TASK_CREATE( "cls_task", clearscreen_task, 0, 0x1a, 0x2000 );

//~ CONFIG_INT("disable.redraw", disable_redraw, 0);
CONFIG_INT("display.dont.mirror", display_dont_mirror, 1);

// this should be synchronized with
// * graphics code (like zebra); otherwise zebras will remain frozen on screen
// * gui_main_task (to make sure Canon won't call redraw in parallel => crash)
void redraw_do()
{
    extern int ml_started;
    if (!ml_started) return;
    if (gui_menu_shown()) { menu_redraw(); return; }
    
BMP_LOCK (

#ifdef CONFIG_VARIANGLE_DISPLAY
    if (display_dont_mirror && display_dont_mirror_dirty)
    {
        if (lcd_position == 1) NormalDisplay();
        display_dont_mirror_dirty = 0;
    }
#endif

    //~ if (disable_redraw) 
    //~ {
        //~ clrscr(); // safest possible redraw method :)
    //~ }
    //~ else
    {
        struct gui_task * current = gui_task_list.current;
        struct dialog * dialog = current->priv;

        if (dialog && MEM(dialog->type) == DLG_SIGNATURE) // if dialog seems valid
        {
            #ifdef CONFIG_KILL_FLICKER
            // to redraw, we need access to front buffer
            int d = canon_gui_front_buffer_disabled();
            canon_gui_enable_front_buffer(0);
            #endif
            
            dialog_redraw(dialog); // try to redraw (this has semaphores for winsys)
            
            #ifdef CONFIG_KILL_FLICKER
            // restore things back
            if (d) idle_kill_flicker();
            #endif
        }
        else
        {
            clrscr(); // out of luck, fallback
        }
    }
)

    // ask other stuff to redraw
    afframe_set_dirty();

    #ifdef FEATURE_CROPMARKS
    crop_set_dirty(cropmark_cache_is_valid() ? 2 : 10);
    #endif
    
    menu_set_dirty();
    lens_display_set_dirty();
    zoom_overlay_dirty = 1;
}

void redraw()
{
    fake_simple_button(MLEV_REDRAW);
}

#ifdef FEATURE_GHOST_IMAGE
static int transparent_overlay_flag = 0;
void schedule_transparent_overlay()
{
    transparent_overlay_flag = 1;
}
#endif

static int lens_display_dirty = 0;
void lens_display_set_dirty() 
{ 
    lens_display_dirty = 4; 
    if (menu_active_but_hidden()) // in this case, menu will display bottom bar, force a redraw
        menu_set_dirty(); 
}

#if 0
void draw_cropmark_area()
{
    get_yuv422_vram();
    bmp_draw_rect(COLOR_BLUE, os.x0, os.y0, os.x_ex, os.y_ex);
    draw_line(os.x0, os.y0, os.x_max, os.y_max, COLOR_BLUE);
    draw_line(os.x0, os.y_max, os.x_max, os.y0, COLOR_BLUE);
    
    bmp_draw_rect(COLOR_RED, HD2BM_X(0), HD2BM_Y(0), HD2BM_DX(vram_hd.width), HD2BM_DY(vram_hd.height));
    draw_line(HD2BM_X(0), HD2BM_Y(0), HD2BM_X(vram_hd.width), HD2BM_Y(vram_hd.height), COLOR_RED);
    draw_line(HD2BM_X(0), HD2BM_Y(vram_hd.height), HD2BM_X(vram_hd.width), HD2BM_Y(0), COLOR_RED);
}
#endif

/*
void show_apsc_crop_factor()
{
    int x_ex_crop = os.x_ex * 10/16;
    int y_ex_crop = os.y_ex * 10/16;
    int x_off = (os.x_ex - x_ex_crop)/2;
    int y_off = (os.y_ex - y_ex_crop)/2;
    bmp_draw_rect(COLOR_WHITE, os.x0 + x_off, os.y0 + y_off, x_ex_crop, y_ex_crop);
    bmp_draw_rect(COLOR_BLACK, os.x0 + x_off + 1, os.y0 + y_off + 1, x_ex_crop - 2, y_ex_crop - 2);
}
*/

int is_focus_peaking_enabled()
{
#ifdef FEATURE_FOCUS_PEAK
    return
        focus_peaking &&
        (lv || (QR_MODE && ZEBRAS_IN_QUICKREVIEW))
        && get_global_draw()
        && !should_draw_zoom_overlay()
    ;
#else
    return 0;
#endif
}

#ifdef FEATURE_ZEBRA_FAST
static void digic_zebra_cleanup()
{
    if (!DISPLAY_IS_ON) return;
    EngDrvOut(DIGIC_ZEBRA_REGISTER, 0); 
    clrscr_mirror();
    alter_bitmap_palette_entry(FAST_ZEBRA_GRID_COLOR, FAST_ZEBRA_GRID_COLOR, 256, 256);
    zebra_digic_dirty = 0;
}
#endif

#ifdef FEATURE_SHOW_OVERLAY_FPS
void update_lv_fps() // to be called every 10 seconds
{
    if (show_lv_fps) bmp_printf(FONT_MED, 50, 50, "%d.%d fps ", fps_ticks/10, fps_ticks%10);
    fps_ticks = 0;
}
#endif

// Items which need a high FPS
// Magic Zoom, Focus Peaking, zebra*, spotmeter*, false color*
// * = not really high FPS, but still fluent
 static void
livev_hipriority_task( void* unused )
{
    msleep(1000);
    
    #ifdef FEATURE_CROPMARKS
    find_cropmarks();
    #endif
    
    #ifdef FEATURE_LV_DISPLAY_PRESETS
    update_disp_mode_bits_from_params();
    #endif
    
    TASK_LOOP
    {
        //~ vsync(&YUV422_LV_BUFFER_DISPLAY_ADDR);
        fps_ticks++;

        while (is_mvr_buffer_almost_full())
        {
            msleep(100);
        }
        
        int zd = zebra_draw && (lv_luma_is_accurate() || PLAY_OR_QR_MODE) && (zebra_rec || !recording); // when to draw zebras (should match the one from draw_zebra_and_focus)

        #ifdef FEATURE_ZEBRA_FAST
        if (zebra_digic_dirty && !zd) digic_zebra_cleanup();
        #endif
        
        static int raw_flag = 0;
        
        if (!zebra_should_run())
        {
            while (clearscreen == 1 && (get_halfshutter_pressed() || dofpreview)) msleep(100);
            while (recording == 1) msleep(100);
            if (!zebra_should_run())
            {
#ifdef FEATURE_ZEBRA_FAST
                if (zebra_digic_dirty) digic_zebra_cleanup();
#endif
                if (lv && !gui_menu_shown()) redraw();
                #ifdef CONFIG_ELECTRONIC_LEVEL
                disable_electronic_level();
                #endif
                #ifdef CONFIG_RAW_LIVEVIEW
                if (raw_flag) { raw_lv_release(); raw_flag = 0; }
                #endif
                while (!zebra_should_run()) 
                {
                    msleep(100);
                }
                vram_params_set_dirty();
                zoom_overlay_triggered_by_focus_ring_countdown = 0;
                crop_set_dirty(10);
                msleep(500);
            }
        }
        #if 0
        draw_cropmark_area(); // just for debugging
        struct vram_info * lv = get_yuv422_vram();
        struct vram_info * hd = get_yuv422_hd_vram();
        bmp_printf(FONT_MED, 100, 100, "ext:%d%d%d \nlv:%x %dx%d \nhd:%x %dx%d ", EXT_MONITOR_RCA, ext_monitor_hdmi, hdmi_code, lv->vram, lv->width, lv->height, hd->vram, hd->width, hd->height);
        #endif

        #ifdef CONFIG_RAW_LIVEVIEW
        if (!raw_flag && !is_movie_mode())
        {
            /* if picture quality is raw, switch the LiveView to raw mode */
            int raw = pic_quality & 0x60000;
            /* only histogram and spotmeter are working in LV raw mode */
            if (raw && lv_dispsize == 1
                && (
#ifdef FEATURE_HISTOGRAM
                    hist_draw ||
#endif
                    spotmeter_draw))
            {
                raw_lv_request();
                raw_flag = 1;
            }
        }
        if (raw_flag && lv_dispsize > 1)
        {
            raw_lv_release();
            raw_flag = 0;
        }
        #endif

        int mz = should_draw_zoom_overlay();

        lv_vsync(mz);
        guess_fastrefresh_direction();

        #ifdef FEATURE_MAGIC_ZOOM
        if (mz)
        {
            //~ msleep(k % 50 == 0 ? MIN_MSLEEP : 10);
            if (zoom_overlay_dirty) BMP_LOCK( clrscr_mirror(); )
            draw_zoom_overlay(zoom_overlay_dirty);
            //~ BMP_LOCK( if (lv)  )
            zoom_overlay_dirty = 0;
            //~ crop_set_dirty(10); // don't draw cropmarks while magic zoom is active
            // but redraw them after MZ is turned off
            //~ continue;
        }
        else
        #endif
        {
            if (!zoom_overlay_dirty) { redraw(); msleep(700); } // redraw cropmarks after MZ is turned off
            zoom_overlay_dirty = 1;

            msleep(10);

            #ifdef CONFIG_DISPLAY_FILTERS
            display_filter_step(k);
            #endif
            
            #ifdef FEATURE_FALSE_COLOR
            if (falsecolor_draw)
            {
                if (k % 4 == 0)
                    BMP_LOCK( if (lv) draw_false_downsampled(); )
            }
            else
            #endif
            {
                BMP_LOCK(
                    if (lv)
                        draw_zebra_and_focus(
                            k % ((focus_peaking ? 5 : 3) * (recording ? 5 : 1)) == 0, /* should redraw zebras? */
                            k % 2 == 1  /* should redraw focus peaking? */
                        ); 
                )
            }
        }

        #ifdef FEATURE_SPOTMETER
        // update spotmeter every second, not more often than that
        static int spotmeter_aux = 0;
        if (spotmeter_draw && should_run_polling_action(1000, &spotmeter_aux))
            BMP_LOCK( if (lv) spotmeter_step(); )
        #endif

        #ifdef CONFIG_ELECTRONIC_LEVEL
        if (electronic_level && k % 8 == 5)
            BMP_LOCK( if (lv) show_electronic_level(); )
        #endif

        #ifdef FEATURE_REC_NOTIFY
        if (k % 8 == 7) rec_notify_continuous(0);
        #endif
        
        #ifdef FEATURE_MAGIC_ZOOM
        if (zoom_overlay_triggered_by_focus_ring_countdown)
        {
            zoom_overlay_triggered_by_focus_ring_countdown--;
        }
        #endif
                
        int m = 100;
        if (lens_display_dirty) m = 10;
        if (should_draw_zoom_overlay()) m = 100;
        
        int kmm = k % m;
        if (!gui_menu_shown()) // don't update everything in one step, to reduce magic zoom flicker
        {
            #if defined(CONFIG_550D) || defined(CONFIG_5D2)
            if (kmm == 0)
                BMP_LOCK( if (lv) black_bars(); )
            #endif

            if (kmm == 2)
            {
                BMP_LOCK( if (lv) update_lens_display(1,0); );
                if (lens_display_dirty) lens_display_dirty--;
            }

            if (kmm == 8)
            {
                BMP_LOCK( if (lv) update_lens_display(0,1); );
                if (lens_display_dirty) lens_display_dirty--;
            }

            static int prev_s;
            int s = get_seconds_clock();
            if (s != prev_s)
                if (lv) movie_indicators_show();
            prev_s = s;
        }
    }
}

static void loprio_sleep()
{
    msleep(200);
    while (is_mvr_buffer_almost_full()) msleep(100);
}

static void black_bars()
{
    if (!get_global_draw()) return;
    if (!is_movie_mode()) return;
    if (video_mode_resolution > 1) return; // these are only for 16:9
    int i,j;
    uint8_t * const bvram = bmp_vram();
    get_yuv422_vram();
    ASSERT(bvram);
    for (i = os.y0; i < MIN(os.y_max+1, BMP_H_PLUS); i++)
    {
        if (i < os.y0 + os.off_169 || i > os.y_max - os.off_169)
        {
            int newcolor = (i < os.y0 + os.off_169 - 2 || i > os.y_max - os.off_169 + 2) ? COLOR_BLACK : COLOR_BG;
            for (j = os.x0; j < os.x_max; j++)
            {
                if (bvram[BM(j,i)] == COLOR_BG)
                    bvram[BM(j,i)] = newcolor;
            }
        }
    }
}

static void default_movie_cropmarks()
{
    if (!get_global_draw()) return;
    if (!lv) return;
    if (!is_movie_mode()) return;
    if (video_mode_resolution > 1) return; // these are only for 16:9
    int i,j;
    uint8_t * const bvram_mirror = get_bvram_mirror();
    get_yuv422_vram();
    ASSERT(bvram_mirror);
    for (i = os.y0; i < MIN(os.y_max+1, BMP_H_PLUS); i++)
    {
        if (i < os.y0 + os.off_169 || i > os.y_max - os.off_169)
        {
            int newcolor = (i < os.y0 + os.off_169 - 2 || i > os.y_max - os.off_169 + 2) ? COLOR_BLACK : COLOR_BG;
            for (j = os.x0; j < os.x_max; j++)
            {
                bvram_mirror[BM(j,i)] = newcolor | 0x80;
            }
        }
    }
}

static void black_bars_16x9()
{
#ifdef CONFIG_KILL_FLICKER
    if (!get_global_draw()) return;
    if (!is_movie_mode()) return;
    get_yuv422_vram();
    if (video_mode_resolution > 1)
    {
        int off_43 = (os.x_ex - os.x_ex * 8/9) / 2;
        bmp_fill(COLOR_BLACK, os.x0, os.y0, off_43, os.y_ex);
        bmp_fill(COLOR_BLACK, os.x_max - off_43, os.y0, off_43, os.y_ex);
    }
    else
    {
        bmp_fill(COLOR_BLACK, os.x0, os.y0, os.x_ex, os.off_169);
        bmp_fill(COLOR_BLACK, os.x0, os.y_max - os.off_169, os.x_ex, os.off_169);
    }
#endif
}

// Items which do not need a high FPS, but are CPU intensive
// histogram, waveform...
static void
livev_lopriority_task( void* unused )
{
    msleep(500);
    TASK_LOOP
    {
        #ifdef FEATURE_CROPMARKS
        #ifdef FEATURE_GHOST_IMAGE
        if (transparent_overlay_flag)
        {
            transparent_overlay_from_play();
            transparent_overlay_flag = 0;
        }
        #endif

        // here, redrawing cropmarks does not block fast zoom
        if (cropmarks_play && PLAY_MODE && DISPLAY_IS_ON && MEM(IMGPLAY_ZOOM_LEVEL_ADDR) <= 0)
        {
            msleep(500);
            if (PLAY_MODE && DISPLAY_IS_ON && MEM(IMGPLAY_ZOOM_LEVEL_ADDR) <= 0) // double-check
            {
                cropmark_redraw();
                if (MEM(IMGPLAY_ZOOM_LEVEL_ADDR) >= 0) redraw(); // whoops, CTRL-Z, CTRL-Z :)
            }
        }
        #endif

        loprio_sleep();
        if (!zebra_should_run())
        {
            if (WAVEFORM_FULLSCREEN && liveview_display_idle() && get_global_draw() && !is_zoom_mode_so_no_zebras() && !gui_menu_shown())
            {
                if (get_halfshutter_pressed()) clrscr();
                else draw_histogram_and_waveform(0);
            }
            continue;
        }

        loprio_sleep();

        if (!gui_menu_shown())
        {
            draw_histogram_and_waveform(0);
        }
    }
}

#define HIPRIORITY_TASK_PRIO 0x18

TASK_CREATE( "livev_hiprio_task", livev_hipriority_task, 0, HIPRIORITY_TASK_PRIO, 0x4000 );
TASK_CREATE( "livev_loprio_task", livev_lopriority_task, 0, 0x1f, 0x4000 );

// these may be out of order for config compatibility
void update_disp_mode_bits_from_params()
{
//~ BMP_LOCK(
    uint32_t bits =
        (global_draw & 1      ? 1<<0 : 0) |
        (zebra_draw           ? 1<<1 : 0) |
#ifdef FEATURE_HISTOGRAM
        (hist_draw            ? 1<<2 : 0) |
#endif
        (crop_enabled         ? 1<<3 : 0) |
        (waveform_draw        ? 1<<4 : 0) |
        (falsecolor_draw      ? 1<<5 : 0) |
        (spotmeter_draw       ? 1<<6 : 0) |
        (global_draw & 2      ? 1<<7 : 0) |
        (focus_peaking        ? 1<<8 : 0) |
        (zoom_overlay_enabled ? 1<<9 : 0) |
        (transparent_overlay  ? 1<<10: 0) |
        //~ (electronic_level     ? 1<<11: 0) |
        //~ (defish_preview       ? 1<<12: 0) |
        (vectorscope_draw     ? 1<<13: 0) |
        0;
        
    if (disp_mode == 1) disp_mode_a = bits;
    else if (disp_mode == 2) disp_mode_b = bits;
    else if (disp_mode == 3) disp_mode_c = bits;
    else disp_mode_x = bits;
//~ )
}

void update_disp_mode_params_from_bits()
{
//~ BMP_LOCK(
    uint32_t bits = disp_mode == 1 ? disp_mode_a : 
                    disp_mode == 2 ? disp_mode_b :
                    disp_mode == 3 ? disp_mode_c : disp_mode_x;

    int global_draw_0    = bits & (1<<0) ? 1 : 0;
    zebra_draw           = bits & (1<<1) ? 1 : 0;
#ifdef FEATURE_HISTOGRAM
    hist_draw            = bits & (1<<2) ? 1 : 0;
#endif
    crop_enabled         = bits & (1<<3) ? 1 : 0;
    waveform_draw        = bits & (1<<4) ? 1 : 0;
    falsecolor_draw      = bits & (1<<5) ? 1 : 0;
    spotmeter_draw       = bits & (1<<6) ? 1 : 0;
    int global_draw_1    = bits & (1<<7) ? 1 : 0;
    focus_peaking        = bits & (1<<8) ? 1 : 0;
    zoom_overlay_enabled = bits & (1<<9) ? 1 : 0;
    transparent_overlay  = bits & (1<<10)? 1 : 0;
    //~ electronic_level     = bits & (1<<11)? 1 : 0;
    //~ defish_preview       = bits & (1<<12)? 1 : 0;
    vectorscope_draw     = bits & (1<<13)? 1 : 0;
    global_draw = global_draw_0 + global_draw_1 * 2;
//~ end:
//~ )
}

int get_disp_mode() { return disp_mode; }

static void toggle_disp_mode_menu(void *priv, int delta) {
    if (!disp_profiles_0) menu_toggle_submenu();
    else toggle_disp_mode();
}

int toggle_disp_mode()
{
    update_disp_mode_bits_from_params();
    idle_wakeup_reset_counters(-3);
    disp_mode = mod(disp_mode + 1, disp_profiles_0 + 1);
    BMP_LOCK( do_disp_mode_change(); )
    //~ menu_set_dirty();
    return disp_mode == 0;
}
static void do_disp_mode_change()
{
    if (gui_menu_shown()) 
    { 
        update_disp_mode_params_from_bits(); 
        return; 
    }
    
    display_on();
    bmp_on();
    clrscr();
    idle_globaldraw_dis();
    //~ redraw();
    bmp_printf(SHADOW_FONT(FONT_LARGE), 50, 50, "Display preset: %d", disp_mode);
    msleep(250);
    idle_globaldraw_en();
    update_disp_mode_params_from_bits();
    redraw();
}

int handle_disp_preset_key(struct event * event)
{
    // the INFO key may be also used for enabling powersaving right away
    // if display presets are off: pressing INFO will go to powersave (if any of those modes are enabled)
    // if display presets are on: powersave will act somewhat like an extra display preset
    
    if (event->param == BGMT_INFO)
    {
        if (!disp_profiles_0)
            return handle_powersave_key(event);

        if (!lv && !LV_PAUSED) return 1;
        if (IS_FAKE(event)) return 1;
        if (gui_menu_shown()) return 1;
        
        if (idle_is_powersave_enabled() && idle_shortcut_key)
        {
            if (disp_mode == disp_profiles_0 && !idle_is_powersave_active())
                return handle_powersave_key(event);
            else
                toggle_disp_mode(); // and wake up from powersave
        }
        else
        {
            toggle_disp_mode();
        }
        return 0;
    }
    return 1;
}

#ifdef FEATURE_OVERLAYS_IN_PLAYBACK_MODE
static int livev_playback = 0;

static void livev_playback_toggle()
{
    if (livev_for_playback_running)
        return;
    
    livev_playback = !livev_playback;
    if (livev_playback)
    {
        livev_for_playback_running = 1;
        task_create("lv_playback", 0x1a, 0x4000, draw_livev_for_playback, 0);
    }
    else
    {
        clrscr();
        if (zebra_digic_dirty) digic_zebra_cleanup();
        redraw();
    }
}
static void livev_playback_reset()
{
    if (livev_playback) redraw();
    livev_playback = 0;
}

static void livev_playback_refresh()
{
    while (livev_for_playback_running) msleep(20);
    livev_playback_toggle();
    if (!livev_playback) livev_playback_toggle();
}

int handle_livev_playback(struct event * event, int button)
{
    // move spotmeter in QR or playback mode

    #define CONFIG_MOVE_SPOTMETER_IN_PLAYBACK
    #ifdef CONFIG_MOVE_SPOTMETER_IN_PLAYBACK
    if ((QR_MODE && ZEBRAS_IN_QUICKREVIEW) || (PLAY_MODE && livev_playback))
    {
        switch (event->param)
        {
            case BGMT_PRESS_LEFT:
                spotmeter_playback_offset_x -= 50;
                livev_playback_refresh();
                return 0;

            case BGMT_PRESS_RIGHT:
                spotmeter_playback_offset_x += 50;
                livev_playback_refresh();
                return 0;
            
            case BGMT_PRESS_UP:
                spotmeter_playback_offset_y -= 50;
                livev_playback_refresh();
                return 0;
            
            case BGMT_PRESS_DOWN:
                spotmeter_playback_offset_y += 50;
                livev_playback_refresh();
                return 0;

            #ifdef BGMT_PRESS_UP_LEFT
            case BGMT_PRESS_UP_LEFT:
                spotmeter_playback_offset_x -= 50;
                spotmeter_playback_offset_y -= 50;
                livev_playback_refresh();
                return 0;

            case BGMT_PRESS_DOWN_RIGHT:
                spotmeter_playback_offset_x += 50;
                spotmeter_playback_offset_y += 50;
                livev_playback_refresh();
                return 0;

            case BGMT_PRESS_UP_RIGHT:
                spotmeter_playback_offset_x += 50;
                spotmeter_playback_offset_y -= 50;
                livev_playback_refresh();
                return 0;

            case BGMT_PRESS_DOWN_LEFT:
                spotmeter_playback_offset_x -= 50;
                spotmeter_playback_offset_y += 50;
                livev_playback_refresh();
                return 0;
            #endif

            #ifdef BGMT_JOY_CENTER
            case BGMT_JOY_CENTER:
            #else
            case BGMT_PRESS_SET:
            #endif
                spotmeter_playback_offset_x = spotmeter_playback_offset_y = 0;
                livev_playback_refresh();
                return 0;
            
            #ifdef BGMT_UNPRESS_UDLR
            case BGMT_UNPRESS_UDLR:
            #else
            case BGMT_UNPRESS_LEFT:
            case BGMT_UNPRESS_RIGHT:
            case BGMT_UNPRESS_UP:
            case BGMT_UNPRESS_DOWN:
            #endif
                return 0;
        }
    }
    #endif

    // enable LiveV stuff in Play mode
    if (PLAY_OR_QR_MODE)
    {
        if (event->param == button)
        {
            livev_playback_toggle();
            return 0;
        }
        
        else if (event->param == GMT_OLC_INFO_CHANGED)
            return 1;

        #ifdef GMT_GUICMD_PRESS_BUTTON_SOMETHING
        else if (event->param == GMT_GUICMD_PRESS_BUTTON_SOMETHING)
            return 1;
        #endif

        else
        {
            livev_playback_reset();
        }
    }
    return 1;
}
#endif

static void zebra_init()
{
    precompute_yuv2rgb();
    menu_add( "Overlay", zebra_menus, COUNT(zebra_menus) );
    menu_add( "Debug", livev_dbg_menus, COUNT(livev_dbg_menus) );
    //~ menu_add( "Movie", movie_menus, COUNT(movie_menus) );
    //~ menu_add( "Config", cfg_menus, COUNT(cfg_menus) );
    menu_add( "Prefs", powersave_menus, COUNT(powersave_menus) );
    menu_add( "Display", level_indic_menus, COUNT(level_indic_menus) );
}

INIT_FUNC(__FILE__, zebra_init);




static void make_overlay()
{
    //~ draw_cropmark_area();
    msleep(1000);
    //~ bvram_mirror_init();
    clrscr();

    bmp_printf(FONT_MED, 0, 0, "Saving overlay...");

    struct vram_info * vram = get_yuv422_vram();
    uint8_t * const lvram = vram->vram;
    //~ int lvpitch = YUV422_LV_PITCH;
    uint8_t * const bvram = bmp_vram();
    if (!bvram) return;

    // difficulty: in play mode, image buffer may have different size/position than in LiveView
    // => normalized xn and yn will fix this
    for (int yn = 0; yn < 480; yn++)
    {
        int y = N2BM_Y(yn);
        //~ int k;
        uint16_t * const v_row = (uint16_t*)( lvram        + BM2LV_R(y)); // 1 pixel
        uint8_t  * const b_row = (uint8_t*) ( bvram        + BM_R(y));    // 1 pixel
        uint8_t  * const m_row = (uint8_t*) ( bvram_mirror + BM_R(yn));    // 1 pixel
        uint16_t* lvp; // that's a moving pointer through lv vram
        uint8_t* bp;   // through bmp vram
        uint8_t* mp;   // through bmp vram mirror
        for (int xn = 0; xn < 720; xn++)
        {
            int x = N2BM_X(xn);
            lvp = v_row + BM2LV_X(x);
            bp = b_row + x;
            mp = m_row + xn;
            *bp = *mp = ((*lvp) * 41 >> 16) + 38;
        }
    }
    FILE* f = FIO_CreateFileEx(CARD_DRIVE "ML/DATA/overlay.dat");
    FIO_WriteFile( f, (const void *) UNCACHEABLE(bvram_mirror), BVRAM_MIRROR_SIZE);
    FIO_CloseFile(f);
    bmp_printf(FONT_MED, 0, 0, "Overlay saved.  ");

    msleep(1000);
}

static void show_overlay()
{
    //~ bvram_mirror_init();
    //~ struct vram_info * vram = get_yuv422_vram();
    //~ uint8_t * const lvram = vram->vram;
    //~ int lvpitch = YUV422_LV_PITCH;
    get_yuv422_vram();
    uint8_t * const bvram = bmp_vram_real();
    if (!bvram) return;
    
    clrscr();

    FILE* f = FIO_Open(CARD_DRIVE "ML/DATA/overlay.dat", O_RDONLY | O_SYNC);
    if (f == INVALID_PTR) return;
    FIO_ReadFile(f, bvram_mirror, 960*480 );
    FIO_CloseFile(f);

    for (int y = os.y0; y < os.y_max; y++)
    {
        int yn = BM2N_Y(y);
        int ym = yn - (int)transparent_overlay_offy; // normalized with offset applied
        //~ int k;
        //~ uint16_t * const v_row = (uint16_t*)( lvram + y * lvpitch );        // 1 pixel
        uint8_t * const b_row = (uint8_t*)( bvram + y * BMPPITCH);          // 1 pixel
        uint8_t * const m_row = (uint8_t*)( bvram_mirror + ym * BMPPITCH);   // 1 pixel
        uint8_t* bp;  // through bmp vram
        uint8_t* mp;  //through bmp vram mirror
        if (ym < 0 || ym > 480) continue;
        //~ int offm = 0;
        //~ int offb = 0;
        //~ if (transparent_overlay == 2) offm = 720/2;
        //~ if (transparent_overlay == 3) offb = 720/2;
        for (int x = os.x0; x < os.x_max; x++)
        {
            int xn = BM2N_X(x);
            int xm = xn - (int)transparent_overlay_offx;
            bp = b_row + x;
            mp = m_row + xm;
            if (((x+y) % 2) && xm >= 0 && xm <= 720)
                *bp = *mp;
        }
    }
    
    bvram_mirror_clear();
    afframe_clr_dirty();
}

static void transparent_overlay_from_play()
{
    if (!PLAY_MODE) { fake_simple_button(BGMT_PLAY); msleep(1000); }
    make_overlay();
    get_out_of_play_mode(500);
    msleep(500);
    if (!lv) { force_liveview(); msleep(500); }
    msleep(1000);
    BMP_LOCK( show_overlay(); )
    //~ transparent_overlay = 1;
}

PROP_HANDLER(PROP_LV_ACTION)
{
    zoom_overlay_triggered_by_focus_ring_countdown = 0;
    
    #ifdef FEATURE_POWERSAVE_LIVEVIEW
    idle_display_undim(); // restore LCD brightness, especially for shutdown
    #endif
    
    idle_globaldraw_disable = 0;
    if (buf[0] == 0) lv_paused = 0;
    
    #ifdef FEATURE_EXPO_OVERRIDE
    bv_auto_update();
    #endif
    
    #ifdef FEATURE_LV_ZOOM_SETTINGS
    zoom_sharpen_step();
    zoom_auto_exposure_step();
    #endif
}

void peaking_benchmark()
{
    int lv0 = lv;
    msleep(1000);
    fake_simple_button(BGMT_PLAY);
    msleep(2000);
    int a = get_seconds_clock();
    lv = 1; // lie, to force using the liveview algorithm which is relevant for benchmarking
    for (int i = 0; i < 1000; i++)
    {
        draw_zebra_and_focus(0,1);
    }
    int b = get_seconds_clock();
    NotifyBox(10000, "%d seconds => %d fps", b-a, 1000 / (b-a));
    beep();
    lv = lv0;
}
