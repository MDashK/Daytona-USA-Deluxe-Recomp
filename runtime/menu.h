/* menu.h — the port's options menu (Select on the main menu), runtime/menu.c */
#ifndef MENU_H
#define MENU_H
#include "plat.h"
extern volatile unsigned int g_mainmenu_stamp;  /* g_dbg_presents when the main menu last ran */
extern int g_opt_widescreen;                    /* 16:9 3D in races */
void menu_load_settings(void);
void menu_input(PlatPad *pad);                  /* hides the pad from the game while open */
void menu_draw(void);                           /* over the finished frame */
int  menu_is_open(void);
#endif
