/** \file
 * Bitrate
 */
#include "dryos.h"
#include "bmp.h"
#include "tasks.h"
#include "debug.h"
#include "menu.h"
#include "property.h"
#include "config.h"
#include "gui.h"
#include "lens.h"
#include "cache_hacks.h"

static CONFIG_INT("h264.bitrate", bitrate, 0);
static CONFIG_INT("h264.initqp", initqp, 0);
static CONFIG_INT("h264.h2config", h2config, 0);
static CONFIG_INT("h264.autoload", autoload, 0);
static CONFIG_INT("h264.flush", bitrate_flushing_rate, 3);
static CONFIG_INT("h264.gop", bitrate_gop_size, 0);
static CONFIG_INT( "time.indicator", time_indicator, 1); // 0 = off, 1 = current clip length, 2 = time remaining until filling the card, 3 = time remaining until 4GB
static CONFIG_INT( "bitrate.indicator", bitrate_indicator, 1);
static CONFIG_INT( "hibr.wav.record", cfg_hibr_wav_record, 0);

PROP_INT(PROP_REBOOT_MAYBE, rebootnow);
PROP_INT(PROP_MOVIE_SOUND_RECORD, sound);
#ifndef CONFIG_5D3
int video_mode[6];
PROP_HANDLER(PROP_VIDEO_MODE)
{
	memcpy(video_mode, buf, 24);
}
#endif

int ivaparam, patched_errors=0, flush_errors=0;
int time_indic_x =  720 - 160; // 160
int time_indic_y = 0;
int time_indic_width = 160;
int time_indic_height = 20;
int time_indic_warning = 120;

int movie_elapsed_time_01s = 0;   // seconds since starting the current movie * 10
int measured_bitrate = 0; // mbps
int movie_bytes_written_32k = 0;
int set =0;

#ifdef FEATURE_NITRATE_WAV_RECORD
int hibr_should_record_wav() { return cfg_hibr_wav_record; }
static CONFIG_INT("snd.inputvol", input_vol, 36);
static int powered_input = 0;
void SoundDevActiveIn();
void SoundDevShutDownIn();
void SetAudioVolumeIn();
extern int mic_inserted;
//~ uint8_t sound_level[3];
//~ PROP_HANDLER(PROP_MOVIE_REC_VOLUME)
//~ {
	//~ memcpy(sound_level, buf, 4);
//~ }
#else
int hibr_should_record_wav() { return 0; }
#endif


/* Defines */
#ifdef CONFIG_6D
#define l_ivastring 0xAA4F4
#define l_initQP 0x78834
#define l_targetR 0x78830
#define l_EncoMode 0x7882c

#endif
#ifdef CONFIG_5D3
#define l_ivastring 0x4DA10
#define l_initQP 0x27884
#define l_targetR 0x27880
#define l_EncoMode 0X2787C

#endif


/*
static void patch_flush_errors()
{
	 //FF0EB0C8:     eb3c5a0c        bl      assert__ram
     //at ./MovieRecorder/MovWriter.c:980, task MovieRecorder
     cache_fake(0xFF1F1244 , 0xE1A00000, TYPE_ICACHE);
	//Still 112 	
	//FF1F5074 bl	assert__ram
	//at ./MovieRecorder/MovRecResource.c:584, task MovieRecorder
	//Patching this causes the sig error.
	//~ cache_fake(0xFF1F5074 , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1DF250 , 0xE1A00000, TYPE_ICACHE);
	
	//~ cache_fake(0xFF1DF288 , 0xE1A00000, TYPE_ICACHE);


	//~ cache_fake(0xFF1DF1D0 , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1DF21C , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1DF250 , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1DF2D4 , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1DF308 , 0xE1A00000, TYPE_ICACHE); //Freeze STRT



	
	//pHeader->pcsSignature == m_pcsSignature
	//~ cache_fake(0xFF1F4C2C , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1F4D00 , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1F4C1C , 0x000050E1, TYPE_ICACHE);

	//FF1F4C7C - FF1F4C10
	//~ cache_fake(0xFF1F4C7C , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF1F4C7C , 0xE1A00000, TYPE_ICACHE);

	// Calls to Check pheader... Assert 0 Returns
/*	cache_fake(0xFF1E1650 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1E1804 , 0xE1A00000, TYPE_ICACHE);
	
	cache_fake(0xFF1F2F58 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1F2F74 , 0xE1A00000, TYPE_ICACHE);
	
	cache_fake(0xFF1F4E78 , 0xE1A00000, TYPE_ICACHE);
	
	cache_fake(0xFF1F5008 , 0xE1A00000, TYPE_ICACHE);
	
	cache_fake(0xFF1F2F90 , 0xE1A00000, TYPE_ICACHE);
	
	cache_fake(0xFF1E02C4 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1E02D0 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1E02DC , 0xE1A00000, TYPE_ICACHE);
	
	cache_fake(0xFF1D3218 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1D3234 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1D3250 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1D326C , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1D3288 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF1D32A4 , 0xE1A00000, TYPE_ICACHE);
*/
/*

  flush_errors = 1;

	
}
*/
static void patch_errors()
{ 
	#ifdef CONFIG_6D
	//FF1F1230 ./MovieRecorder/EncDrvWrapper.c:1926, task MovieRecorder
	cache_fake(0xFF1F1244 , 0xE1A00000, TYPE_ICACHE);

   //FF3BEFE4 RECORDING: ./Fcreate/FcsMaker.c:2314, task MovieRecorder
   cache_fake(0xFF3BEFF8 , 0xE1A00000, TYPE_ICACHE);

	//FF3B060C:   eb3144bb        bl      assert__ram
	// Player: at Graphics.c:3777, task CtrlSrv
	cache_fake(0xFF3B0620 , 0xE1A00000, TYPE_ICACHE);
  
	//at Stub.c:29, task CtrlSrv
	cache_fake(0xFF0C2894 , 0xE1A00000, TYPE_ICACHE);

	//  Uknown Gop
	//~ cache_fake(0xFF3BEFC8 , 0xE1A00000, TYPE_ICACHE);

	//  Gop Mis Match CMP r1, r1
	cache_fake(0xFF0E5208 , 0xE1510001, TYPE_ICACHE);

   //this->dwTimecodeSize1 May Break timecode Freezes
   //~ cache_fake(0xFF1E2748 , 0xE1510001, TYPE_ICACHE);
	#endif
	
	#ifdef CONFIG_5D3
	cache_fake(0xFF1EA0F4 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF37F35C , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF370444 , 0xE1A00000, TYPE_ICACHE); //Player Not sure
	cache_fake(0xFF0C28AC , 0xE1A00000, TYPE_ICACHE);
	//~ cache_fake(0xFF3BEFB4 , 0xE1A00000, TYPE_ICACHE);
	cache_fake(0xFF0E3B3C , 0xE1510001, TYPE_ICACHE);
	//~ cache_fake(0xFF1E2734 , 0xE1510001, TYPE_ICACHE);
	#endif

   patched_errors = 1;

}

static void load_h264_ini()
{
    gui_stop_menu();
	if (h2config == 1)
		{   call("IVAParamMode", "B:/ML/cbr.ini");
		    //~ call("IVAParamMode", CARD_DRIVE "ML/cbr.ini");
			NotifyBox(2000, "%s", l_ivastring); //0xaa4f4 78838
		}
	else if (h2config == 2)
		 {   call("IVAParamMode", "B:/ML/vbr.ini");
		   //~ call("IVAParamMode", CARD_DRIVE "ML/vbr.ini");
			NotifyBox(2000, "%s", l_ivastring); //0xaa4f4 78838
		}
	else if (h2config == 3)
		{   call("IVAParamMode", "B:/ML/rc.ini");
		    //~ call("IVAParamMode", CARD_DRIVE "ML/rc.ini");
			NotifyBox(2000, "%s", l_ivastring); //0xaa4f4 78838
		}
	else 
	{	
    	//~ call("IVAParamMode", CARD_DRIVE "ML/H264.ini");
    	call("IVAParamMode", "B:/ML/H264.ini");
		NotifyBox(2000, "%s", l_ivastring); //0xaa4f4 78838
	}
}	




CONFIG_INT("buffer.warning.level", buffer_warning_level, 70);
static void
buffer_warning_level_display( void * priv, int x, int y, int selected )
{
    bmp_printf(
        selected ? MENU_FONT_SEL : MENU_FONT,
        x, y,
        "BuffWarnLevel : %d%%",
        buffer_warning_level
    );
    menu_draw_icon(x, y, MNI_PERCENT, buffer_warning_level);
}

static void buffer_warning_level_toggle(void* priv, int step)
{
    buffer_warning_level += step;
    if (buffer_warning_level > 100) buffer_warning_level = 30;
    if (buffer_warning_level < 30) buffer_warning_level = 100;
}

int warning = 0;
int is_mvr_buffer_almost_full() 
{
    if (recording == 0) return 0;
    if (recording == 1) return 1;

    int ans = MVR_BUFFER_USAGE > (int)buffer_warning_level;
    if (ans) warning = 1;
    return warning;
}

void show_mvr_buffer_status()
{
    int fnt = warning ? FONT(FONT_SMALL, COLOR_WHITE, COLOR_RED) : FONT(FONT_SMALL, COLOR_WHITE, COLOR_GREEN2);
    if (warning) warning--;
    if (recording && get_global_draw() && !gui_menu_shown()) bmp_printf(fnt, 680, 55, " %3d%%", MVR_BUFFER_USAGE);
}
int8_t* ivaparamstatus = (int8_t*)(l_EncoMode);
uint8_t oldh2config;
void bitrate_set()
{
    if (!lv) return;
    if (!is_movie_mode()) return;
	#ifdef FEATURE_NITRATE_WAV_RECORD
	if (hibr_should_record_wav() && input_vol != 0)
		{   static int mic;
		
			//~ sound_level[0] = 00 | input_vol;
			//~ sound_level[2] = input_vol;
			//~ prop_request_change(PROP_MOVIE_REC_VOLUME, &sound_level, 4);
			//~ SoundDevActiveIn(0); // Apply Maybe + meters on.
						
			/* Only Works On Initialize */
			//~ uint8_t* ivol = (uint8_t*) MEM(0x75340); 
			//~ *(ivol + 64) = 0;
			//~ *(ivol + 65) = input_vol; //L
			//~ *(ivol + 66) = input_vol; //R
			//~ SoundDevActiveIn(0); // Apply Maybe + meters on.
			if (!powered_input || mic != mic_inserted) 
			{   SetAudioVolumeIn(0, input_vol , input_vol);
				SoundDevActiveIn(mic_inserted ? 1 : 0); 
				powered_input = 1; 
			}
			mic = mic_inserted;
		}
	#endif
    if (raw_lv_is_enabled())return; 
    //~ if (gui_menu_shown()) return;
    if (recording) return; 
	if (!(ivaparam == 2)) ivaparam= *ivaparamstatus;	
	if (patched_errors == 0) patch_errors();
	//~ if (flush_errors == 0) patch_flush_errors(); // Otherwise Err70 on Play
	if (ivaparam == 2 ) MEM(l_EncoMode) = 0;
	else {
		if (!gui_menu_shown()) // Don't load config during menu
			{
				if (!ivaparam && h2config !=0 && autoload!=0) load_h264_ini();
				if (autoload!=0 && (oldh2config!=h2config) && h2config !=0)	load_h264_ini();	
			}
		  }

 	   
       if (!ivaparam || (autoload == 2 || ivaparam == 2)) 
			{   /* TODO
				if( !(autoload==2) && bitrate!=0 && MEM(0X78830)==0 )
					{	NotifyBox(2000,"Eko Mode: Must set both parameters!");
						return;
					}*/
				MEM(l_initQP) = initqp;
			    MEM(l_targetR) = bitrate * 10000000;
			}
	   else { 
			if (MEM(l_initQP) != 0)	
				MEM(l_initQP) = 0;
			if (MEM(l_targetR) != 0)
				MEM(l_targetR) = 0;
			 }
		 oldh2config = h2config;
	  
	/* Flush Rate + GOP size from G3gg0 */
	if (bitrate_flushing_rate == 3)
	{
    cache_fake(CACHE_HACK_FLUSH_RATE_SLAVE, MEM(CACHE_HACK_FLUSH_RATE_SLAVE), TYPE_ICACHE);
	}	
	else 
	{
	cache_fake(CACHE_HACK_FLUSH_RATE_SLAVE, 0xE3A00000 | (bitrate_flushing_rate & 0xFF), TYPE_ICACHE);
	// Until Audio Patched
	if (sound != 1)
	 {	static int mode  = 1;
		prop_request_change(PROP_MOVIE_SOUND_RECORD, &mode, 4);
		NotifyBox(1000,"Canon sound disabled");
	 }
	}
	if (!bitrate_gop_size)
	{    
	#ifdef CONFIG_5D3	
	cache_fake(CACHE_HACK_GOP_SIZE_SLAVE, MEM(CACHE_HACK_GOP_SIZE_SLAVE), TYPE_ICACHE);
	#else
	if (video_mode[5] != 1)
		{	if (video_mode[2] > 30)
			video_mode[3] = 15; // 60FPS
			else
			video_mode[3] = video_mode[2]/2; // FPS/2
			prop_request_change(PROP_VIDEO_MODE, video_mode, 24);
		}
	#endif
	}
	else 
	{
		//~ NotifyBox(2000, "1: %x 2:%x 3:%x\n4:%x 5:%x, 6:%x", video_mode[0],video_mode[1],video_mode[2],video_mode[3],video_mode[4],video_mode[5]);
		/*    30I  30IPB   24I 24IPB 60I 60IPB 30IPB
		0 -    0	0		0	0  	  0		0	0	
		1 -    0    0		0	0	  1		1	2   Mode/Size
		2 -    1e   1e		18	18	  3C	3C	1e	FPS
		3 -    1	f		1	c	  1		f	f	GOP
		4 -    0	0		0	0	  0		0	0
		5 -    1	3		1	3	  1		3	3	Ref Frames (M= in Header)
		*/
	#ifdef CONFIG_5D3
    cache_fake(CACHE_HACK_GOP_SIZE_SLAVE, 0xE3A00000 | (bitrate_gop_size & 0xFF), TYPE_ICACHE);
    //~ cache_fake(0xFF226728, 0xE3A00000 | (bitrate_gop_size & 0xFF), TYPE_ICACHE);
    //~ cache_fake(CACHE_HACK_GOP_SIZE_SLAVE, 0xE3A01000 | (bitrate_gop_size & 0xFF), TYPE_ICACHE);
    //~ cache_fake(0x79968, 0x00000000 | (bitrate_gop_size & 0xFF), TYPE_DCACHE);
    //cache_fake(0x79968, 0x00000000, TYPE_DCACHE);
	 #else
	 if (video_mode[5] != 1) // Doesn't Matter for ALL-I
	  {	video_mode[3] = bitrate_gop_size; //Gop Length
		prop_request_change(PROP_VIDEO_MODE, video_mode, 24);
		}
	#endif
    }		

}

void measure_bitrate() // called once / second
{
    static uint32_t prev_bytes_written = 0;
    uint32_t bytes_written = MVR_BYTES_WRITTEN;
    int bytes_delta = (((int)(bytes_written >> 1)) - ((int)(prev_bytes_written >> 1))) << 1;
    prev_bytes_written = bytes_written;
    movie_bytes_written_32k = bytes_written >> 15;
    measured_bitrate = (ABS(bytes_delta) / 1024) * 8 / 1024;
}


void bitrate_mvr_log(char* mvr_logfile_buffer)
{
    return;
}

static int movie_start_timestamp = 0;
PROP_HANDLER(PROP_MVR_REC_START)
{
    if (buf[0] == 1)
        movie_start_timestamp = get_seconds_clock();
}

static PROP_INT(PROP_CLUSTER_SIZE, cluster_size);
static PROP_INT(PROP_FREE_SPACE, free_space_raw);
#define free_space_32k (free_space_raw * (cluster_size>>10) / (32768>>10))

void free_space_show()
{
    if (!get_global_draw()) return;
    if (gui_menu_shown()) return;
    if (recording && time_indicator) return;
    int fsg = free_space_32k >> 15;
    int fsgr = free_space_32k - (fsg << 15);
    int fsgf = (fsgr * 10) >> 15;

    // trick to erase the old text, if any (problem due to shadow fonts)
    bmp_printf(
        FONT(FONT_MED, COLOR_WHITE, TOPBAR_BGCOLOR),
        time_indic_x + 160 - 6 * font_med.width,
        time_indic_y,
        "      "
    );

    bmp_printf(
        FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_BLACK),
        time_indic_x + 160 - 6 * font_med.width,
        time_indic_y,
        "%d.%dGB",
        fsg,
        fsgf
    );
}

void time_indicator_show()
{
    if (!get_global_draw()) return;

    if (!recording) 
    {
        //~ free_space_show();
        return;
    }
    
    // time until filling the card
    // in "movie_elapsed_time_01s" seconds, the camera saved "movie_bytes_written_32k"x32kbytes, and there are left "free_space_32k"x32kbytes
    int time_cardfill = movie_elapsed_time_01s * free_space_32k / movie_bytes_written_32k / 10;
    
    // time until 4 GB
    int time_4gb = movie_elapsed_time_01s * (4 * 1024 * 1024 / 32 - movie_bytes_written_32k) / movie_bytes_written_32k / 10;

    //~ bmp_printf(FONT_MED, 0, 300, "%d %d %d %d ", movie_elapsed_time_01s, movie_elapsed_ticks, rec_time_card, rec_time_4gb);

    // what to display
    int dispvalue = time_indicator == 1 ? movie_elapsed_time_01s / 10:
                    time_indicator == 2 ? time_cardfill :
                    time_indicator == 3 ? MIN(time_4gb, time_cardfill)
                    : 0;
    
    if (time_indicator)
    { 
	    bmp_printf(
            FONT(FONT_SMALL, COLOR_WHITE, COLOR_BLACK),
            680 - font_small.width,
            55 + font_small.height,
            "%3d:%02d",
            dispvalue / 60,
            dispvalue % 60
        );
    }
    if (bitrate_indicator)
    {
        bmp_printf( FONT_SMALL,
            680 - font_small.width * 5,
            55,
            "A%3d ",
            movie_bytes_written_32k * 32 * 80 / 1024 / movie_elapsed_time_01s);

        bmp_printf(FONT_SMALL,
            680 - font_small.width * 5,
            55 + font_small.height,
            "B%3d ",
            measured_bitrate
        );
    }
    
}

void fps_show()
{
    if (!get_global_draw()) return;
    if (gui_menu_shown()) return;
    if (!is_movie_mode() || recording) return;
    //~ if (hdmi_code == 5) return; // workaround
    int screen_layout = get_screen_layout();
    if (screen_layout > SCREENLAYOUT_3_2_or_4_3) return;
    
    bmp_printf(
        FONT(FONT_MED, COLOR_WHITE, TOPBAR_BGCOLOR),
        time_indic_x + 160 - 6 * font_med.width,
        time_indic_y + font_med.height - 3,
        "      "
    );

    int f = fps_get_current_x1000();
    bmp_printf(
        SHADOW_FONT(FONT_MED),
        time_indic_x + 160 - 6 * font_med.width,
        time_indic_y + font_med.height - 3,
        "%2d.%03d", 
        f / 1000, f % 1000
    );
}

void free_space_show_photomode()
{
    int fsg = free_space_32k >> 15;
    int fsgr = free_space_32k - (fsg << 15);
    int fsgf = (fsgr * 10) >> 15;
    
#if defined DISPLAY_CLOCK_POS_X
    int x = DISPLAY_CLOCK_POS_X - 135;
    int y = DISPLAY_CLOCK_POS_Y;
#else
    int x = time_indic_x + 2 * font_med.width;
    int y =  452;
#endif
    bmp_printf(
               FONT(SHADOW_FONT(FONT_LARGE), COLOR_FG_NONLV, bmp_getpixel(x-10,y+10)),
               x, y,
               "%d.%dGB",
               fsg,
               fsgf
               );
}

#ifdef FEATURE_NITRATE_WAV_RECORD
static void hibr_wav_record_select( void * priv, int x, int y, int selected ){
    menu_numeric_toggle(priv, 1, 0, 1);
    if (recording) return;
    int *onoff = (int *)priv;
    if(*onoff == 1){
        if (sound_recording_mode != 1){
            int mode  = 1; //disabled
            prop_request_change(PROP_MOVIE_SOUND_RECORD, &mode, 4);
            NotifyBox(2000,"Canon sound disabled");
            audio_configure(1);
        }
    }
}
static void in_vol_toggle(void * priv, int delta)
{   if (!hibr_should_record_wav()) return; //Yes it will work but cannon may overwrite.
	int *input_volume = (int *)priv;
	*input_volume = mod(*input_volume + delta, 64);
	SetAudioVolumeIn(0, *input_volume , *input_volume);
}
MENU_UPDATE_FUNC(input_vol_up)
{
	if (input_vol == 0 || !hibr_should_record_wav())	
	{
		MENU_SET_ENABLED(0);
		MENU_SET_VALUE("OFF");
		if (sound == 0 && !recording) { SoundDevShutDownIn(); powered_input = 0; }
	}
	else if(hibr_should_record_wav())
		{
			MENU_SET_ENABLED(1);
			MENU_SET_VALUE("%d dB", input_vol);
		}
}

#endif

MENU_UPDATE_FUNC(bit_rated)
{
if ( (!ivaparam || (autoload == 2 || ivaparam == 2)) && CURRENT_VALUE != 0)
MENU_SET_ENABLED(1);
else
{ MENU_SET_ENABLED(0);
	MENU_SET_VALUE("OFF");
}

}

MENU_UPDATE_FUNC(init_qp_d)
{
if ( (!ivaparam || (autoload == 2 || ivaparam == 2)) && CURRENT_VALUE != 0)
MENU_SET_ENABLED(1);
else
{ MENU_SET_ENABLED(0);
	MENU_SET_VALUE("OFF");
}

}

MENU_UPDATE_FUNC(ivaparam_d)
{ 
if (!ivaparam) { MENU_SET_ENABLED(0); MENU_SET_VALUE("No"); }
if (ivaparam == 1) { MENU_SET_ENABLED(1);	MENU_SET_VALUE("Yes"); }
if (ivaparam == 2) { MENU_SET_ENABLED(0);	MENU_SET_VALUE("Disable"); }
if (ivaparam == 3) { MENU_SET_ENABLED(0);	MENU_SET_VALUE("Check"); }
					
}

static MENU_UPDATE_FUNC(bitrate_flushing_rate_update)
{
    MENU_SET_VALUE("%d frames", bitrate_flushing_rate );
    if(bitrate_flushing_rate == 3)
    {
        MENU_SET_VALUE("Auto");
    }
}

static MENU_UPDATE_FUNC(bitrate_gop_size_update)
{
    MENU_SET_VALUE("%d frames", bitrate_gop_size);
    if(!bitrate_gop_size)
    {
        MENU_SET_VALUE("Default");
    }
}


static struct menu_entry mov_menus[] = {
    /*
   {     .name = "Encoder",
		.select = menu_open_submenu,     
		.help = "Change H.264 bitrate. Pick configs. Be careful, recording may stop!",
		.submenu_width = 715,        
//		.edit_mode = EM_MANY_VALUES,
        .depends_on = DEP_MOVIE_MODE,
        .children =  (struct menu_entry[]) {
    {
        .name = "Bit Rate     ",
        .priv = &bitrate,
		.update = bit_rated,        
		.min = 0,
        .max = 50,
		.icon_type = IT_BOOL,		
		.help = "H.264 bitrate. One unit = 10 mb/s. 0 = Off",	
		.help2 = "If not loading a config MUST set InitQP"
    },
    {
        .name = "InitQP     ",
        .priv = &initqp,
		.update = init_qp_d,        
        .min = 0,
        .max = 50,
		.icon_type = IT_BOOL,
        .help = "Init QP - Lower is better. 0 = Off",
		.help2 = "Overriding dynamic configs will result in fixed QP"
    },
			{
                .name = "Flush rate",
                .priv = &bitrate_flushing_rate,
                .update = bitrate_flushing_rate_update,
                .min  = 3, // It can do 1 but can't figure patch.
                .max  = 50,
                .help = "Flush movie buffer every n frames."
            },
            {
                .name = "GOP size",
                .priv = &bitrate_gop_size,
                .update = bitrate_gop_size_update,
                .min  = 0,
                .max  = 100,
                .help = "Set GOP size to n frames."
            },
	    {
        .name = "Autoload Conf  ",
        .priv = &autoload,
        .min = 0,
        .max = 2,
		.choices = (const char *[]) {"Off", "On", "Override"},        
		.help = "Auto Load Config at Start and/or Override BR/InitQP"
    },

    {
        .name = "Config Select",
        .priv = &h2config,
        .min = 0,
        .max = 3,
        .choices = (const char *[]) {"Off/H264.ini", "CBR Fixed QP", "VBR", "Rate Control"},
        .help = "Select an Encoder Config. If off H264.ini will load. Exit menu to apply."
    },
		MENU_EOL
        },
    },
    {
        .name = "Load Config",
        //~ .priv = &bitrate,
        //~ .min = 1,
        //~ .max = 20,
        .select = load_h264_ini,
        .help = "Manual Load Config from selection"
    },

    {
        .name = "Config Loaded ",
         .priv = &ivaparam,
         .min = 2,
         .max = 3,
		.update = ivaparam_d,        
		.icon_type = IT_BOOL,		
//		.choices = (const char *[]) {"No", "Yes", "Disable"},    
		.help = "Config Loaded. Disabling may keep some of the parameters"
    },
     */

    {
        .name = "REC indicator",
        .priv = &time_indicator,
        .min = 0,
        .max = 3,
        .choices = (const char *[]) {"Off", "Elapsed Time", "Remain.time (card)", "Time Until 4GB"},
        .help = "What to display in top-right corner while recording."
    },
};

#ifdef FEATURE_NITRATE_WAV_RECORD
static struct menu_entry wav_menus[] = {
            {
                .name = "Sound Record",
                .priv = &cfg_hibr_wav_record,
                .select = hibr_wav_record_select,
                .max = 1,
                .choices = (const char *[]) {"Normal", "Separate WAV"},
                .help = "Record audio with WAV separately. Source will auto select.",
            },
			{
                .name = "Input Volume",
                .priv = &input_vol,
				.update = input_vol_up,
				.select = in_vol_toggle,
                .min = 0,
                .max = 63,
                .help = "Record audio with WAV separately.",
            },
};
#endif


void bitrate_init()
{
    menu_add( "Movie", mov_menus, COUNT(mov_menus) );
	#ifdef FEATURE_NITRATE_WAV_RECORD
	menu_add( "Audio", wav_menus, COUNT(wav_menus) );
	#endif
}

INIT_FUNC(__FILE__, bitrate_init);

void movie_indicators_show()
{
    if (recording && !raw_lv_is_enabled())
    {
        BMP_LOCK( time_indicator_show(); )
    }
    else
    {
        BMP_LOCK(
            free_space_show(); 
            fps_show();
        )
    }
}


static void
bitrate_task( void* unused )
{    
    TASK_LOOP
    {

        if (recording && !raw_lv_is_enabled())
        {
            /* uses a bit of CPU, but it's precise */
            wait_till_next_second();
            movie_elapsed_time_01s += 10;
            measure_bitrate();
            
            BMP_LOCK( show_mvr_buffer_status(); )
			if ( (movie_elapsed_time_01s<200) && (movie_elapsed_time_01s>100) && 		
					(measured_bitrate == 0) && (movie_bytes_written_32k == 0) )
					//~ && (!fps_override) && (bitrate_flushing_rate == 3) )
						{// ASSERT (0);
							//call ("SHUTDOWN");
							//call ("dumpf");
					NotifyBox(2000,"Not writing! Check settings, rebooting!");
					msleep(1000);					
					prop_request_change(PROP_REBOOT_MAYBE, 0, 4);
					//msleep(100000);
					}

        }
        else
        {
			movie_elapsed_time_01s = 0;
           	msleep(1000);
			bitrate_set();
//bmp_printf(FONT_MED, 10,120, "IvaparamStatus: %d, InIq: %d", *ivaparamstatus, ivaparam);

		}
    }
}

//~ TASK_CREATE("bitrate_task", bitrate_task, 0, 0x1d, 0x1000 );

