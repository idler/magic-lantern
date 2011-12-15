/** 
 * FPS control
 * http://magiclantern.wikia.com/wiki/VideoTimer
 * 
 * Found by g3gg0
 **/

#include "dryos.h"
#include "bmp.h"
#include "property.h"
#include "menu.h"
#include "lens.h"

#ifdef CONFIG_600D
#define SENSOR_TIMING_TABLE MEM(0xCB20)
#define VIDEO_PARAMETERS_SRC_3 0x70AE8 // notation from g3gg0
#define LiveViewMgr_struct_ptr 0x1dcc  // aAJ_0x1D78_LiveViewMgr_struct_ptr
#endif
#ifdef CONFIG_60D
#define SENSOR_TIMING_TABLE MEM(0x2a668)
#define VIDEO_PARAMETERS_SRC_3 0x4FDA8
#define LiveViewMgr_struct_ptr 0x1E80
#endif
#ifdef CONFIG_1100D
#define SENSOR_TIMING_TABLE MEM(0xce98)
#endif

#define FRAME_SHUTTER_TIMER (*(uint16_t*)(VIDEO_PARAMETERS_SRC_3+0xC))
#define FRAME_ISO (*(uint8_t*)(VIDEO_PARAMETERS_SRC_3+0x8))

struct lv_path_struct
{
	int SM; // ?! 1 in video mode, 0 in zoom and photo mode
	int fps_sensor_mode; // 24p:4, 25p:3, 30p:2, 50p:1, 60p:0
	int S; // 1920:0, 720:1, vgacrop:4, zoom:8
	int R; // movie size: 1920:0, 720:1, 480:2
	int Z; // (1 / 5 / A) << 16
	int recording;
	int DZ; // bool?
};

extern struct lv_path_struct lv_path_struct;

#define TG_FREQ_PAL  50000000
#define TG_FREQ_NTSC 52747252

#define FPS_x1000_TO_TIMER_PAL(fps_x1000) (TG_FREQ_PAL/(fps_x1000))
#define FPS_x1000_TO_TIMER_NTSC(fps_x1000) (TG_FREQ_NTSC/(fps_x1000))
#define TIMER_TO_FPS_x1000_PAL(t) (TG_FREQ_PAL/(t))
#define TIMER_TO_FPS_x1000_NTSC(t) (TG_FREQ_NTSC/(t))

uint16_t * sensor_timing_table_original = 0;
uint16_t sensor_timing_table_patched[128];

int fps_override = 0;
int shutter_override = 0;
int hdr_mode = 0;
int hdr_ev = 2;

int video_mode[5];
PROP_HANDLER(PROP_VIDEO_MODE)
{
	memcpy(video_mode, buf, 20);
	return prop_cleanup(token, property);
}

const int mode_offset_map[] = { 3, 6, 1, 5, 4, 0, 2 };

int fps_get_current_x1000()
{
	int mode = 
		video_mode_fps == 60 ? 0 : 
		video_mode_fps == 50 ? 1 : 
		video_mode_fps == 30 ? 2 : 
		video_mode_fps == 25 ? 3 : 
		video_mode_fps == 24 ? 4 : 0;
    int fps_timer = ((uint16_t*)SENSOR_TIMING_TABLE)[mode_offset_map[mode]];
    int ntsc = (mode % 2 == 0);
    int fps_x1000 = ntsc ? TIMER_TO_FPS_x1000_NTSC(fps_timer) : TIMER_TO_FPS_x1000_PAL(fps_timer);
    return fps_x1000;
}

int get_shutter_override_degrees_x10()
{
	// 0, 360, 270, 180, 90, 2, 1, 0.5, 0.2
	if (shutter_override == 0) return 0;
	if (shutter_override <= 4) return (5-shutter_override) * 900;
	return 20 >> (shutter_override - 5);
}

int get_shutter_override_reciprocal_x1000()
{
	int mode = 
		video_mode_fps == 60 ? 0 : 
		video_mode_fps == 50 ? 1 : 
		video_mode_fps == 30 ? 2 : 
		video_mode_fps == 25 ? 3 : 
		video_mode_fps == 24 ? 4 : 0;
    int timer = shutter_get_timer(get_shutter_override_degrees_x10());
    int ntsc = (mode % 2 == 0);
    int shutter_x1000 = ntsc ? TIMER_TO_FPS_x1000_NTSC(timer) : TIMER_TO_FPS_x1000_PAL(timer);
    return shutter_x1000;
}

int get_shutter_override_degrees()
{
	return get_shutter_override_degrees_x10() / 10;
}

int shutter_get_timer(int degrees_x10)
{
	int mode = 
		video_mode_fps == 60 ? 0 : 
		video_mode_fps == 50 ? 1 : 
		video_mode_fps == 30 ? 2 : 
		video_mode_fps == 25 ? 3 : 
		video_mode_fps == 24 ? 4 : 0;
    int ntsc = (mode % 2 == 0);
    int fps_x1000 = fps_get_current_x1000();
    int timer = ntsc ? FPS_x1000_TO_TIMER_NTSC(fps_x1000) 
                     : FPS_x1000_TO_TIMER_PAL (fps_x1000);
    return MAX(1, timer * degrees_x10 / 3600);
}
void shutter_set(int degrees_x10)
{
	int odd_frame = YUV422_HD_BUFFER_DMA_ADDR == YUV422_HD_BUFFER_2;
	int t = shutter_get_timer(degrees_x10);
	if (hdr_mode == 1) // ISO
	{
		int iso_low = COERCE(lens_info.raw_iso - hdr_ev*4, 72, 120);
		int iso_high = COERCE(lens_info.raw_iso + hdr_ev*4, 72, 120);
		FRAME_ISO = odd_frame ? iso_low : iso_high; // ISO 100-1600
	}
	else if (hdr_mode == 2) // shutter
	{
		int ev_x8 = odd_frame ? hdr_ev*4 : -hdr_ev*4;
		t = shutter_get_timer(degrees_x10 * roundf(1000.0*powf(2, ev_x8 / 8.0))/1000);
	}
	FRAME_SHUTTER_TIMER = t;
}


static void
fps_print(
	void *			priv,
	int			x,
	int			y,
	int			selected
)
{
	int current_fps = fps_get_current_x1000();
	
	char msg[30];
	snprintf(msg, sizeof(msg), "%d (%d.%03d)", 
		(current_fps+500)/1000, 
		current_fps/1000, current_fps%1000
		);
	
	bmp_printf(
		selected ? MENU_FONT_SEL : MENU_FONT,
		x, y,
		"FPS override : %s",
		fps_override ? msg : "OFF"
	);
	
	extern int sound_recording_mode;
	if (fps_override && sound_recording_mode != 1)
		menu_draw_icon(x, y, MNI_WARNING, "Turn off sound recording from Canon menu!");
	menu_draw_icon(x, y, MNI_BOOL(SENSOR_TIMING_TABLE != sensor_timing_table_original), 0);
	//~ bmp_hexdump(FONT_SMALL, 0, 400, SENSOR_TIMING_TABLE, 32);
}

static void
shutter_print(
	void *			priv,
	int			x,
	int			y,
	int			selected
)
{
	int current_shutter = get_shutter_override_reciprocal_x1000();
	int d = get_shutter_override_degrees_x10();
	
	char msg[30];
	snprintf(msg, sizeof(msg), "%d.%ddeg 1/%d%s", 
		d/10, d%10,
		current_shutter/1000,
		hdr_ev ? " HDR" : ""
		);
	
	bmp_printf(
		selected ? MENU_FONT_SEL : MENU_FONT,
		x, y,
		"Tv override  : %s",
		shutter_override ? msg : "OFF"
	);
}

void fps_change_mode(int mode, int fps)
{
    int ntsc = (mode % 2 == 0);
    /** 
     * 60fps = mode 0 - NTSC
     * 50fps = mode 1
     * 30fps = mode 2 - NTSC
     * 25fps = mode 3
     * 24fps = mode 4 - NTSC?
     **/

    int fps_x1000 = fps * 1000;

    // convert fps into timer ticks (for sensor drive speed)
    int fps_timer = ntsc ? FPS_x1000_TO_TIMER_NTSC(fps_x1000*1000/1001) : FPS_x1000_TO_TIMER_PAL(fps_x1000);

    // make sure we set a valid value (don't drive it too fast)
    int fps_timer_absolute_minimum = sensor_timing_table_original[21 + mode_offset_map[mode]];
    fps_timer = MAX(fps_timer_absolute_minimum * 120/100, fps_timer);

    // NTSC is 29.97, not 30
    // also try to round it in order to avoid flicker
    if (ntsc)
    {
        int timer_120hz = FPS_x1000_TO_TIMER_NTSC(120000*1000/1001);
        int fps_timer_rounded = ((fps_timer + timer_120hz/2) / timer_120hz) * timer_120hz;
        if (ABS(TIMER_TO_FPS_x1000_NTSC(fps_timer_rounded) - fps_x1000 + 1) < 500) fps_timer = fps_timer_rounded;
    }
    else
    {
        int timer_100hz = FPS_x1000_TO_TIMER_PAL(100000);
        int fps_timer_rounded = ((fps_timer + timer_100hz/2) / timer_100hz) * timer_100hz;
        if (ABS(TIMER_TO_FPS_x1000_PAL(fps_timer_rounded) - fps_x1000 + 1) < 500) fps_timer = fps_timer_rounded;
    }
    
    // fps = 0 means "don't override, use default"
    int fps_timer_default = sensor_timing_table_original[mode_offset_map[mode]];
    sensor_timing_table_patched[mode_offset_map[mode]] = fps ? fps_timer : fps_timer_default;

    // use the patched sensor table
    SENSOR_TIMING_TABLE = sensor_timing_table_patched;
}

void fps_change_all_modes(int fps)
{
	if (!fps)
	{
		// use the original sensor table (firmware default)
		SENSOR_TIMING_TABLE = sensor_timing_table_original;
	}
	else
	{
		// patch all video modes
		for (int i = 0; i < 2; i++)
			fps_change_mode(i, fps);
		for (int i = 2; i < 5; i++)
			fps_change_mode(i, MIN(fps, 35));
	}

	if (!lv) return;

	// flip video mode back and forth to apply settings instantly
	int f0 = video_mode[2];
	video_mode[2] = 
		f0 == 24 ? 30 : 
		f0 == 25 ? 50 : 
		f0 == 30 ? 24 : 
		f0 == 50 ? 25 :
	  /*f0 == 60*/ 30;
	prop_request_change(PROP_VIDEO_MODE, video_mode, 20);
	msleep(50);
	video_mode[2] = f0;
	prop_request_change(PROP_VIDEO_MODE, video_mode, 20);
	msleep(50);
}

void reset_fps(void* priv, int delta)
{
	if (recording) return;

	fps_override = 0;
	fps_change_all_modes(0);
}

void set_fps(void* priv, int delta)
{
	if (recording) return;

	// first click won't change value
	int fps = (fps_get_current_x1000() + 500) / 1000; // rounded value
	if (fps_override) fps = COERCE(fps + delta, 4, 60);
	fps_override = 1;
	
	fps_change_all_modes(fps);
}

static void
fps_task( void* unused ) // screenshot, draw_prop
{
	while(1)
	{
		msleep(10);
		if (shutter_override)
			shutter_set(get_shutter_override_degrees_x10());
		else
			msleep(500); // zzzZZZzzz
	}
}

TASK_CREATE( "fps", fps_task, 0, 0x17, 0x1000 );


struct menu_entry fps_menu[] = {
	{
		.name = "FPS override", 
		.priv = &fps_override,
		.select = set_fps,
		.select_auto = reset_fps,
		.display = fps_print,
		.show_liveview = 1,
		.help = "Makes French Fries with the camera sensor. Turn off sound!"
	},
	{
		.name = "Shutter Override", 
		.priv = &shutter_override,
		.max = 8,
		.display = shutter_print,
		.show_liveview = 1,
		.help = "Override shutter speed. 1/fps ... 1/50000, HDR option.",
		.children =  (struct menu_entry[]) {
			{
				.name = "HDR mode",
				.priv		= &hdr_mode,
				.min = 0,
				.max = 2,
				.choices = (const char *[]) {"OFF", "ISO", "Shutter",},
				.help = "HDR video: alternates exposure between frames",
				.show_liveview = 1,
			},
			{
				.name = "HDR EV spacing",
				.priv		= &hdr_ev,
				.min = 1,
				.max = 6,
				.help = "HDR video: alternates exposure between frames",
				.show_liveview = 1,
			},
			MENU_EOL
		},
	},
};

void fps_init()
{
	// make a copy of the original sensor timing table (so we can patch it)
	sensor_timing_table_original = (void*)SENSOR_TIMING_TABLE;
	memcpy(sensor_timing_table_patched, sensor_timing_table_original,  sizeof(sensor_timing_table_patched));
	menu_add( "Debug", fps_menu, COUNT(fps_menu) );
}

INIT_FUNC("fps", fps_init);
