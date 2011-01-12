// 720x480, changes when external monitor is connected
#define YUV422_IMAGE_BUFFER 0x40D07800 
#define YUV422_PITCH 1440

// changes during record
#define YUV422_HD_BUFFER 0x44000080
#define YUV422_HD_PITCH 2112
#define YUV422_HD_HEIGHT 704
#define YUV422_HD_PITCH_REC 3440
#define YUV422_HD_HEIGHT_REC 974

#define FOCUS_CONFIRMATION 0x41d0
#define DISPLAY_SENSOR 0x2dec
#define DISPLAY_SENSOR_MAYBE 0xC0220104

// for gui_main_task
#define GMT_NFUNCS 8
#define GMT_FUNCTABLE 0xFF453E14

// button codes as received by gui_main_task
#define BGMT_PRESS_LEFT 0x1c
#define BGMT_UNPRESS_LEFT 0x1d
#define BGMT_PRESS_UP 0x1e
#define BGMT_UNPRESS_UP 0x1f
#define BGMT_PRESS_RIGHT 0x1a
#define BGMT_UNPRESS_RIGHT 0x1b
#define BGMT_PRESS_DOWN 0x20
#define BGMT_UNPRESS_DOWN 0x21

#define BGMT_PRESS_SET 0x4
#define BGMT_UNPRESS_SET 0x5

#define BGMT_TRASH 0xA
#define BGMT_MENU 6
#define BGMT_DISP 7
#define BGMT_Q 8

#define BGMT_PRESS_HALFSHUTTER 0x3F
#define BGMT_UNPRESS_HALFSHUTTER 0x40



