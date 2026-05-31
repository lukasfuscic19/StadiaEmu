/*
 * tray.h -- Routines for providing the tray control.
 */

#ifndef TRAY_H
#define TRAY_H

#include <wtypes.h>

#define NT_TRAY_INFO 0
#define NT_TRAY_WARNING 1
#define NT_TRAY_ERROR 2

#define DO_TRAY_UNKNOWN 0
#define DO_TRAY_DEV_ATTACHED 1
#define DO_TRAY_DEV_REMOVED 2

struct tray_menu;

struct tray
{
    LPTSTR icon;
    LPTSTR tip;
    struct tray_menu *menu;
};

struct tray_menu
{
    LPTSTR text;
    BOOLEAN disabled;
    BOOLEAN checked;

    void (*cb)(struct tray_menu *);
    void *context;

    struct tray_menu *submenu;
};

int tray_init(struct tray *tray);
int tray_loop(BOOLEAN blocking);
void tray_update(struct tray *tray);
void tray_exit();
void tray_register_device_notification(GUID filter, void (*cb)(UINT, LPTSTR));
void tray_show_notification(UINT type, LPTSTR title, LPTSTR text);
/* Post WM_APP to trigger refresh_devices() on the main thread safely */
void tray_post_refresh(void);
void tray_set_wm_app_callback(void (*cb)(void));

#endif /* TRAY_H */
