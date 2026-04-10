#ifndef XDR_SETTINGS_H_
#define XDR_SETTINGS_H_
#include <gtk/gtk.h>

#define SETTINGS_TAB_DEFAULT     0
#define SETTINGS_TAB_INTERFACE   0
#define SETTINGS_TAB_SIGNAL      1
#define SETTINGS_TAB_RDS         2
#define SETTINGS_TAB_ANTENNA     3
#define SETTINGS_TAB_LOGS        4
#define SETTINGS_TAB_AUDIOBRIDGE 5
#define SETTINGS_TAB_KEYBOARD    6
#define SETTINGS_TAB_PRESETS     7
#define SETTINGS_TAB_SCHEDULER   8
#define SETTINGS_TAB_ABOUT       9

void settings_dialog(gint);

#endif
