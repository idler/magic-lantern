/** \file
 * Menu structures and functions.
 *
 * When adding a menu item, the convention in the Magic Lantern menus
 * is that there are 12 characters for the label and up to 7 characters
 * for the value.  The box that is drawn is 540 pixels wide, enough
 * for 19 characters in FONT_LARGE.
 *
 * There is room for 8 entries in the menu.
 *
 * New menus must have a 5 character top level name.
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

#ifndef _menu_h_
#define _menu_h_

#include <stdint.h>
#include <stdbool.h>

#define MENU_FONT       FONT(FONT_LARGE,COLOR_WHITE,COLOR_BLACK)
#define MENU_FONT_SEL   MENU_FONT
#define MENU_FONT_GRAY  FONT(FONT_LARGE,50,COLOR_BLACK)

int get_menu_font_sel();
int gui_menu_shown();
void menu_show_only_selected();
int get_menu_advanced_mode();

extern int submenu_mode;
extern int page_number_active;

struct menu_entry
{
        struct menu_entry *     next;
        struct menu_entry *     prev;
        uint8_t                 selected;
        void *                  priv;
        int min;
        int max;
        int8_t unit;
        const char** choices;
        void                    (*select)(
                void *                  priv,
                int                             delta
        );
        void                    (*select_reverse)(
                void *                  priv,
                int                             delta
        );
        void                    (*select_Q)(
                void *                  priv,
                int                             delta
        );
        void                    (*display)(
                void *                  priv,
                int                     x,
                int                     y,
                int                     selected
        );
        //~ int8_t essential;
        int8_t hidden;
        int8_t icon_type;
        int8_t edit_mode; // not currently used (to be cleaned up)
        const char * help;
        const char * help2;
        const char * name; // used for context help and sometimes for display
        struct menu_entry * children;
        //~ uint32_t id; // unique ID (not supported; menus are identified by strings)
    
        // not required for entry item, but makes it easier to declare in existing menu structures
        int16_t submenu_width; 
        int16_t submenu_height;
        
        // for vscroll
        int8_t pos;
        int8_t childnum;
        int8_t childnummax; 

        int16_t depends_on;     // hard requirement, won't work otherwise
        int16_t works_best_in;  // soft requirement, it will work, but not as well
};

#define IS_VISIBLE(entry) ((entry)->hidden != MENU_ENTRY_HIDDEN)

// whether the feature is enabled or disabled
#define TRUTH_VALUE(entry) ((entry)->priv ? *(int*)(entry)->priv : 0)

// index into choices[] array
#define SELECTED_INDEX(entry) (TRUTH_VALUE(entry) - (entry)->min)

// how many choices we have (index runs from 0 to N-1)
#define NUM_CHOICES(entry) ((entry)->max - (entry)->min + 1)

#define MENU_ENTRY_NOT_HIDDEN 0
#define MENU_ENTRY_HIDDEN 1
#define MENU_ENTRY_NEVER_HIDE -1

#define EM_FEW_VALUES 0
#define EM_MANY_VALUES 1
#define EM_MANY_VALUES_LV 2


#define IT_AUTO 0
#define IT_BOOL 1
#define IT_SIZE 2
#define IT_DICE 3
#define IT_PERCENT 4
#define IT_ALWAYS_ON 5
#define IT_ACTION 6
#define IT_NAMED_COLOR 7
#define IT_BOOL_NEG 8
#define IT_DISABLE_SOME_FEATURE 9
#define IT_DISABLE_SOME_FEATURE_NEG 10
#define IT_REPLACE_SOME_FEATURE 11
#define IT_SUBMENU 12
#define IT_DICE_OFF 13

#define UNIT_1_8_EV 1
#define UNIT_x10 2
#define UNIT_PERCENT 3
#define UNIT_PERCENT_x10 4
#define UNIT_ISO 5
#define UNIT_HEX 6

#define DEP_GLOBAL_DRAW (1<<0)
#define DEP_LIVEVIEW (1<<1)
#define DEP_NOT_LIVEVIEW (1<<2)
#define DEP_MOVIE_MODE (1<<3)
#define DEP_PHOTO_MODE (1<<4)
#define DEP_AUTOFOCUS (1<<5)
#define DEP_MANUAL_FOCUS (1<<6)
#define DEP_CFN_AF_HALFSHUTTER (1<<7)
#define DEP_CFN_AF_BACK_BUTTON (1<<8)
#define DEP_EXPSIM (1<<9)
#define DEP_NOT_EXPSIM (1<<10)
#define DEP_CHIPPED_LENS (1<<11)
#define DEP_M_MODE (1<<12)
#define DEP_MANUAL_ISO (1<<13)

struct menu
{
        struct menu *           next;
        struct menu *           prev;
        const char *            name;
        struct menu_entry *     children;
        int                     selected;
        int icon;
        uint32_t id; // unique ID
        int16_t submenu_width;
        int16_t submenu_height;
        int16_t pos;
        int16_t childnum;
        int16_t childnummax;
        int16_t delnum;
};

#define IS_SUBMENU(menu) (menu->icon == ICON_ML_SUBMENU)

extern void
menu_print(
        void *                  priv,
        int                     x,
        int                     y,
        int                     selected
);

extern void menu_binary_toggle( void * priv, int unused );
extern void menu_ternary_toggle(void* priv, int delta);
extern void menu_quaternary_toggle(void* priv, int delta);
extern void menu_quinternary_toggle(void* priv, int delta);
extern struct menu * menu_get_root(void);
extern struct menu_entry * menu_find_by_id(uint32_t id);

// temporary definitions for compatibility
#define menu_ternary_toggle_reverse menu_ternary_toggle
#define menu_quaternary_toggle_reverse menu_quaternary_toggle
#define menu_quinternary_toggle_reverse menu_quinternary_toggle


extern void
menu_select(
        struct menu_entry *     entry
);

extern void run_in_separate_task(void (*priv)(void), int delta);


OS_FUNCTION( 0x0700001,	void,	menu_add, const char * name, struct menu_entry * new_entry, int count );
OS_FUNCTION( 0x0700002, void,	menu_draw_icon, int x, int y, int type, intptr_t arg);


extern void
menu_init( void );

extern void menu_stop(void);

#define MNI_NONE -1
#define MNI_OFF -2
#define MNI_ON 1
#define MNI_AUTO 2
#define MNI_WARNING 3
#define MNI_PERCENT 4
#define MNI_ACTION 5
#define MNI_DICE 6
#define MNI_SIZE 7
#define MNI_NAMED_COLOR 8
#define MNI_NEUTRAL 9
#define MNI_DISABLE 10
#define MNI_SUBMENU 11
#define MNI_DICE_OFF 12
#define MNI_BOOL(x) ((x) ? MNI_ON : MNI_OFF)
#define MNI_BOOL_AUTO(x) ((x) == 1 ? MNI_ON : (x) == 0 ? MNI_OFF : MNI_AUTO)

#define MNI_STOP_DRAWING -100

#define _ZEBRAS_IN_LIVEVIEW (get_global_draw_setting() & 1)
#define GDR_WARNING_MSG ((lv && lv_disp_mode && _ZEBRAS_IN_LIVEVIEW) ? "Press " INFO_BTN_NAME " (outside ML menu) to turn Canon displays off." : get_global_draw_setting() ? "GlobalDraw is disabled, check your settings." : "GlobalDraw is OFF.")
#define EXPSIM_WARNING_MSG (expsim == 0 ? "ExpSim is OFF." : "Display Gain is active.") // no other causes.. yet

#define MNI_BOOL_GDR(x) ((x) ? ( get_global_draw() ? MNI_ON : MNI_WARNING ) : MNI_OFF), (intptr_t) GDR_WARNING_MSG
#define MNI_BOOL_GDR_EXPSIM(x) ((x) ? ( get_global_draw() && (lv_luma_is_accurate() || !lv) ? MNI_ON : MNI_WARNING ) : MNI_OFF), (intptr_t)( !get_global_draw() ? GDR_WARNING_MSG : EXPSIM_WARNING_MSG )
#define MNI_BOOL_LV(x) ((x) ? ( lv ? MNI_ON : MNI_WARNING ) : MNI_OFF), (intptr_t) "This option only works in LiveView." 

#define MENU_EOL_PRIV (void*)-1
#define MENU_EOL { .priv = MENU_EOL_PRIV }
#define MENU_IS_EOL(entry) ((intptr_t)(entry)->priv == -1)

#ifdef CONFIG_VXWORKS
#define MENU_WARNING_COLOR COLOR_RED
#else
#define MENU_WARNING_COLOR 254
#endif


#endif
