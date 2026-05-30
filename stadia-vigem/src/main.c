/*
 * main.c -- Program entry point and Stadia device mapping.
 *
 * Supports USB HID and BLE (via Win32 GATT + WinRT vibration).
 * BLE vibration requires package identity — install StadiaViGEm.msix once.
 */

#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include <windows.h>
#include <synchapi.h>
#include <shlobj.h>

#include <ViGEm/Client.h>

#include "tray.h"
#include "hid.h"
#include "utils.h"
#include "stadia.h"
#include "gatt.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

#ifndef _DEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#define MAX_ACTIVE_DEVICE_COUNT 4
#define APP_NAME                TEXT("StadiaViGEm")
#define STARTUP_REG_KEY         TEXT("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run")
#define HIDHIDE_CLI             TEXT("C:\\Program Files\\Nefarius Software Solutions\\HidHide\\x64\\HidHideCLI.exe")

/* -----------------------------------------------------------------------
 * Device record
 * ----------------------------------------------------------------------- */
struct active_device
{
    struct hid_device        *src_device;   /* NULL for BLE */
    WCHAR                     bt_address[20];
    struct stadia_controller *controller;
    PVIGEM_TARGET             tgt_device;
    XUSB_REPORT               tgt_report;
    INT                       battery_pct;  /* 0-100 or -1 */
};

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
static int               active_device_count = 0;
static struct active_device *active_devices[MAX_ACTIVE_DEVICE_COUNT];
static SRWLOCK           active_devices_lock = SRWLOCK_INIT;
static PVIGEM_CLIENT     vigem_client;
static BOOL              vigem_connected  = FALSE;
static BOOL              hidhide_enabled  = FALSE;

/* Tray menu items with dynamic labels */
static struct tray_menu  tray_menu_device_count;
static struct tray_menu  tray_menu_startup;
static struct tray_menu  tray_menu_hidhide;

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
static void refresh_devices(void);
static void stadia_controller_update_cb(struct stadia_controller *, struct stadia_state *);
static void stadia_controller_stop_cb(struct stadia_controller *);
static void CALLBACK x360_notification_cb(PVIGEM_CLIENT, PVIGEM_TARGET,
                                           UCHAR, UCHAR, UCHAR, LPVOID);
static void refresh_cb(struct tray_menu *);
static void toggle_startup_cb(struct tray_menu *);
static void toggle_hidhide_cb(struct tray_menu *);
static void quit_cb(struct tray_menu *);
static void rebuild_tray_menu(void);

static const struct tray_menu tray_menu_separator  = {.text = TEXT("-")};
static const struct tray_menu tray_menu_refresh    = {.text = TEXT("Refresh"),  .cb = refresh_cb};
static const struct tray_menu tray_menu_quit_item  = {.text = TEXT("Quit"),     .cb = quit_cb};
static const struct tray_menu tray_menu_terminator = {.text = NULL};

static struct tray tray = {
    .icon = TEXT("APP_ICON"),
    .tip  = TEXT("Stadia Controller"),
    .menu = NULL
};

/* -----------------------------------------------------------------------
 * Axis mapping
 * ----------------------------------------------------------------------- */
static SHORT _map_byte_to_short(BYTE value, BOOL inverted)
{
    CHAR centered = value - 128;
    if (centered < -127) centered = -127;
    if (inverted)        centered = -centered;
    return (SHORT)(32767 * centered / 127);
}

/* -----------------------------------------------------------------------
 * Startup helpers
 * ----------------------------------------------------------------------- */
static void get_exe_path(LPTSTR buf, DWORD size)
{
    GetModuleFileName(NULL, buf, size);
}

static BOOL is_startup_enabled(void)
{
    HKEY hk;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return FALSE;
    TCHAR val[MAX_PATH]; DWORD sz = sizeof(val);
    BOOL found = (RegQueryValueEx(hk, APP_NAME, NULL, NULL, (LPBYTE)val, &sz) == ERROR_SUCCESS);
    RegCloseKey(hk);
    return found;
}

static void set_startup_enabled(BOOL enable)
{
    HKEY hk;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (enable) {
        TCHAR exe[MAX_PATH]; get_exe_path(exe, MAX_PATH);
        RegSetValueEx(hk, APP_NAME, 0, REG_SZ, (LPBYTE)exe,
                      (DWORD)((_tcslen(exe)+1)*sizeof(TCHAR)));
    } else {
        RegDeleteValue(hk, APP_NAME);
    }
    RegCloseKey(hk);
}

/* -----------------------------------------------------------------------
 * HidHide helpers
 * ----------------------------------------------------------------------- */
static BOOL hidhide_available(void)
{
    return GetFileAttributes(HIDHIDE_CLI) != INVALID_FILE_ATTRIBUTES;
}

static void hidhide_run(LPCTSTR args)
{
    TCHAR cmd[MAX_PATH+64];
    _stprintf_s(cmd, _countof(cmd), TEXT("\"%s\" %s"), HIDHIDE_CLI, args);
    STARTUPINFO si = {0}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

static void hidhide_configure(BOOL enable)
{
    if (!hidhide_available()) return;
    if (enable) {
        TCHAR exe[MAX_PATH]; get_exe_path(exe, MAX_PATH);
        TCHAR arg[MAX_PATH+32];
        _stprintf_s(arg, _countof(arg), TEXT("--app-reg \"%s\""), exe);
        hidhide_run(arg);
        hidhide_run(TEXT("--dev-hide \"HID\\\\VID_18D1&PID_9400\""));
        hidhide_run(TEXT("--dev-hide \"HID\\\\{00001812-0000-1000-8000-00805F9B34FB}\""));
        hidhide_run(TEXT("--cloak-on"));
        hidhide_enabled = TRUE;
    } else {
        hidhide_run(TEXT("--cloak-off"));
        hidhide_enabled = FALSE;
    }
}

/* -----------------------------------------------------------------------
 * Tray menu
 * ----------------------------------------------------------------------- */
static void rebuild_tray_menu(void)
{
    struct tray_menu *prev = tray.menu;
    struct tray_menu *m    = (struct tray_menu*)malloc(10 * sizeof(struct tray_menu));
    int i = 0;

    /* Device count + battery */
    free(tray_menu_device_count.text);
    TCHAR bat_str[32] = TEXT("");
    AcquireSRWLockShared(&active_devices_lock);
    for (int b = 0; b < active_device_count; b++) {
        if (active_devices[b]->bt_address[0] != 0) {
            INT pct = active_devices[b]->battery_pct;
            if (pct >= 0) _stprintf_s(bat_str, 32, TEXT("  [Bat: %d%%]"), pct);
            else          _tcscpy_s(bat_str, 32, TEXT("  [Bat: ?]"));
            break;
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);
    INT len = _sctprintf(TEXT("%d/4 connected%s"), active_device_count, bat_str);
    tray_menu_device_count.text = (LPTSTR)malloc((len+1)*sizeof(TCHAR));
    _stprintf(tray_menu_device_count.text, TEXT("%d/4 connected%s"), active_device_count, bat_str);
    m[i++] = tray_menu_device_count;

    m[i++] = tray_menu_separator;

    /* Startup toggle */
    free(tray_menu_startup.text);
    tray_menu_startup.text = _tcsdup(is_startup_enabled()
        ? TEXT("[ON] Start with Windows")
        : TEXT("[ ] Start with Windows"));
    tray_menu_startup.cb = toggle_startup_cb;
    m[i++] = tray_menu_startup;

    /* HidHide toggle */
    if (hidhide_available()) {
        free(tray_menu_hidhide.text);
        tray_menu_hidhide.text = _tcsdup(hidhide_enabled
            ? TEXT("[ON] Hide controller (HidHide)")
            : TEXT("[ ] Hide controller (HidHide)"));
        tray_menu_hidhide.cb = toggle_hidhide_cb;
        m[i++] = tray_menu_hidhide;
    }

    m[i++] = tray_menu_separator;
    m[i++] = tray_menu_refresh;
    m[i++] = tray_menu_quit_item;
    m[i++] = tray_menu_terminator;

    /* Update tooltip */
    if (active_device_count > 0 && bat_str[0]) {
        static TCHAR tip[128];
        _stprintf_s(tip, 128, TEXT("Stadia Controller%s"), bat_str);
        tray.tip = tip;
    } else {
        tray.tip = TEXT("Stadia Controller");
    }

    tray.menu = m;
    free(prev);
}

/* -----------------------------------------------------------------------
 * Tray callbacks
 * ----------------------------------------------------------------------- */
static void toggle_startup_cb(struct tray_menu *item)
{
    (void)item;
    set_startup_enabled(!is_startup_enabled());
    rebuild_tray_menu();
    tray_update(&tray);
}

static void toggle_hidhide_cb(struct tray_menu *item)
{
    (void)item;
    hidhide_configure(!hidhide_enabled);
    rebuild_tray_menu();
    tray_update(&tray);
}

static void refresh_cb(struct tray_menu *item)
{
    (void)item;
    refresh_devices();
}

static void quit_cb(struct tray_menu *item)
{
    (void)item;
    tray_exit();
}

/* -----------------------------------------------------------------------
 * Device management
 * ----------------------------------------------------------------------- */
static BOOL add_usb_device(LPTSTR path)
{
    if (active_device_count == MAX_ACTIVE_DEVICE_COUNT) return FALSE;

    struct hid_device *device = hid_open_device(path, TRUE, FALSE);
    if (!device) {
        if (hid_reenable_device(path)) {
            device = hid_open_device(path, TRUE, FALSE);
            if (!device) device = hid_open_device(path, TRUE, TRUE);
        } else {
            device = hid_open_device(path, TRUE, TRUE);
        }
    }

    if (!device) {
        if (GetLastError() == ERROR_ACCESS_DENIED)
            tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia ViGEm"),
                TEXT("Controller hidden by HidHide — run installer to whitelist this app"));
        else
            tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia ViGEm"),
                TEXT("Error opening USB device"));
        return FALSE;
    }

    struct stadia_controller *ctrl = stadia_controller_create(device, FALSE, NULL);
    if (!ctrl) {
        tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia ViGEm"),
                               TEXT("Error initializing USB device"));
        hid_close_device(device);
        hid_free_device(device);
        return FALSE;
    }

    struct active_device *ad = (struct active_device*)calloc(1, sizeof(struct active_device));
    ad->src_device  = device;
    ad->battery_pct = -1;
    ad->controller  = ctrl;

    if (vigem_connected) {
        ad->tgt_device = vigem_target_x360_alloc();
        vigem_target_add(vigem_client, ad->tgt_device);
        XUSB_REPORT_INIT(&ad->tgt_report);
        vigem_target_x360_register_notification(vigem_client, ad->tgt_device,
                                                x360_notification_cb, (LPVOID)ad);
    }

    AcquireSRWLockExclusive(&active_devices_lock);
    active_devices[active_device_count++] = ad;
    ReleaseSRWLockExclusive(&active_devices_lock);

    rebuild_tray_menu();
    tray_update(&tray);
    return TRUE;
}

static BOOL add_ble_device(const WCHAR *bt_address)
{
    if (active_device_count == MAX_ACTIVE_DEVICE_COUNT) return FALSE;

    struct stadia_controller *ctrl =
        stadia_controller_create(NULL, TRUE, bt_address);
    if (!ctrl) return FALSE;

    struct active_device *ad = (struct active_device*)calloc(1, sizeof(struct active_device));
    ad->src_device  = NULL;
    ad->battery_pct = gatt_get_battery_level(bt_address);
    ad->controller  = ctrl;
    wcscpy_s(ad->bt_address, 20, bt_address);

    if (vigem_connected) {
        ad->tgt_device = vigem_target_x360_alloc();
        vigem_target_add(vigem_client, ad->tgt_device);
        XUSB_REPORT_INIT(&ad->tgt_report);
        vigem_target_x360_register_notification(vigem_client, ad->tgt_device,
                                                x360_notification_cb, (LPVOID)ad);
    }

    AcquireSRWLockExclusive(&active_devices_lock);
    active_devices[active_device_count++] = ad;
    ReleaseSRWLockExclusive(&active_devices_lock);

    rebuild_tray_menu();
    tray_update(&tray);
    return TRUE;
}

static BOOL remove_device(struct stadia_controller *ctrl)
{
    BOOL removed = FALSE;

    AcquireSRWLockExclusive(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++) {
        if (active_devices[i]->controller != ctrl) continue;

        if (active_devices[i]->src_device) {
            hid_close_device(active_devices[i]->src_device);
            hid_free_device(active_devices[i]->src_device);
        }
        if (vigem_connected) {
            vigem_target_x360_unregister_notification(active_devices[i]->tgt_device);
            vigem_target_remove(vigem_client, active_devices[i]->tgt_device);
            vigem_target_free(active_devices[i]->tgt_device);
        }
        free(active_devices[i]);

        if (i < active_device_count-1)
            memmove(&active_devices[i], &active_devices[i+1],
                    sizeof(struct active_device*) * (active_device_count-i-1));
        active_device_count--;
        removed = TRUE;
        break;
    }
    ReleaseSRWLockExclusive(&active_devices_lock);
    return removed;
}

/* -----------------------------------------------------------------------
 * Device scan
 * ----------------------------------------------------------------------- */
static void refresh_devices(void)
{
    /* --- USB scan --- */
    LPTSTR usb_filters[2] = {STADIA_USB_HW_FILTER, NULL};
    struct hid_device_info *dev_info = hid_enumerate(usb_filters);
    struct hid_device_info *cur;

    /* Remove USB devices no longer present */
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++) {
        if (active_devices[i]->bt_address[0] != 0) continue; /* skip BLE */
        BOOL found = FALSE;
        for (cur = dev_info; cur; cur = cur->next)
            if (active_devices[i]->src_device &&
                _tcscmp(active_devices[i]->src_device->path, cur->path) == 0)
            { found = TRUE; break; }
        if (!found)
            stadia_controller_destroy(active_devices[i]->controller);
    }
    ReleaseSRWLockShared(&active_devices_lock);

    /* Add new USB devices */
    for (cur = dev_info; cur; cur = cur->next) {
        BOOL found = FALSE;
        AcquireSRWLockShared(&active_devices_lock);
        for (int i = 0; i < active_device_count; i++)
            if (active_devices[i]->src_device &&
                _tcscmp(cur->path, active_devices[i]->src_device->path) == 0)
            { found = TRUE; break; }
        ReleaseSRWLockShared(&active_devices_lock);
        if (!found) add_usb_device(cur->path);
    }
    while (dev_info) { cur = dev_info->next; hid_free_device_info(dev_info); dev_info = cur; }

    /* --- BLE scan --- */
    WCHAR ble_addrs[4][20];
    int ble_count = gatt_find_stadia_devices(ble_addrs, 4);

    /* Remove BLE devices no longer present */
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++) {
        if (active_devices[i]->bt_address[0] == 0) continue; /* skip USB */
        BOOL found = FALSE;
        for (int b = 0; b < ble_count; b++)
            if (_wcsicmp(active_devices[i]->bt_address, ble_addrs[b]) == 0)
            { found = TRUE; break; }
        if (!found)
            stadia_controller_destroy(active_devices[i]->controller);
    }
    ReleaseSRWLockShared(&active_devices_lock);

    /* Add new BLE devices */
    for (int b = 0; b < ble_count; b++) {
        BOOL found = FALSE;
        AcquireSRWLockShared(&active_devices_lock);
        for (int i = 0; i < active_device_count; i++)
            if (_wcsicmp(active_devices[i]->bt_address, ble_addrs[b]) == 0)
            { found = TRUE; break; }
        ReleaseSRWLockShared(&active_devices_lock);
        if (!found) add_ble_device(ble_addrs[b]);
    }

    /* Refresh battery for connected BLE devices */
    AcquireSRWLockExclusive(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
        if (active_devices[i]->bt_address[0] != 0)
            active_devices[i]->battery_pct =
                gatt_get_battery_level(active_devices[i]->bt_address);
    ReleaseSRWLockExclusive(&active_devices_lock);

    rebuild_tray_menu();
    tray_update(&tray);
}

static void device_change_cb(UINT op, LPTSTR path)
{
    (void)op; (void)path;
    refresh_devices();
}

/* -----------------------------------------------------------------------
 * Controller callbacks
 * ----------------------------------------------------------------------- */
static void stadia_controller_update_cb(struct stadia_controller *ctrl,
                                        struct stadia_state *state)
{
    struct active_device *ad = NULL;
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
        if (active_devices[i]->controller == ctrl) { ad = active_devices[i]; break; }
    ReleaseSRWLockShared(&active_devices_lock);
    if (!ad || !vigem_connected) return;

    ad->tgt_report.wButtons = 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_UP)         ? XUSB_GAMEPAD_DPAD_UP        : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_DOWN)       ? XUSB_GAMEPAD_DPAD_DOWN      : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_LEFT)       ? XUSB_GAMEPAD_DPAD_LEFT      : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_RIGHT)      ? XUSB_GAMEPAD_DPAD_RIGHT     : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_MENU)       ? XUSB_GAMEPAD_START          : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_OPTIONS)    ? XUSB_GAMEPAD_BACK           : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_LS)         ? XUSB_GAMEPAD_LEFT_THUMB     : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_RS)         ? XUSB_GAMEPAD_RIGHT_THUMB    : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_LB)         ? XUSB_GAMEPAD_LEFT_SHOULDER  : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_RB)         ? XUSB_GAMEPAD_RIGHT_SHOULDER : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_A)          ? XUSB_GAMEPAD_A              : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_B)          ? XUSB_GAMEPAD_B              : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_X)          ? XUSB_GAMEPAD_X              : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_Y)          ? XUSB_GAMEPAD_Y              : 0;
    ad->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_STADIA_BTN) ? XUSB_GAMEPAD_GUIDE          : 0;

    ad->tgt_report.bLeftTrigger  = state->left_trigger;
    ad->tgt_report.bRightTrigger = state->right_trigger;
    ad->tgt_report.sThumbLX = _map_byte_to_short(state->left_stick_x,  FALSE);
    ad->tgt_report.sThumbLY = _map_byte_to_short(state->left_stick_y,  TRUE);
    ad->tgt_report.sThumbRX = _map_byte_to_short(state->right_stick_x, FALSE);
    ad->tgt_report.sThumbRY = _map_byte_to_short(state->right_stick_y, TRUE);
    vigem_target_x360_update(vigem_client, ad->tgt_device, ad->tgt_report);
}

static void stadia_controller_stop_cb(struct stadia_controller *ctrl)
{
    if (remove_device(ctrl)) {
        rebuild_tray_menu();
        tray_update(&tray);
    }
}

static void CALLBACK x360_notification_cb(PVIGEM_CLIENT client, PVIGEM_TARGET target,
                                           UCHAR large_motor, UCHAR small_motor,
                                           UCHAR led_number, LPVOID user_data)
{
    (void)client; (void)target; (void)led_number;
    struct active_device *ad = (struct active_device*)user_data;
    stadia_controller_set_vibration(ad->controller, small_motor, large_motor);
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
INT main(void)
{
    tray_menu_device_count.text = NULL;
    tray_menu_startup.text      = NULL;
    tray_menu_hidhide.text      = NULL;

    rebuild_tray_menu();
    if (tray_init(&tray) < 0) {
        MessageBox(NULL, TEXT("Failed to create tray icon"), APP_NAME, MB_ICONERROR);
        return 1;
    }

    vigem_client = vigem_alloc();
    VIGEM_ERROR vr = vigem_connect(vigem_client);
    if (vr == VIGEM_ERROR_BUS_NOT_FOUND)
        tray_show_notification(NT_TRAY_ERROR, TEXT("Stadia ViGEm"),
                               TEXT("ViGEmBus not installed — run the installer"));
    else if (vr == VIGEM_ERROR_BUS_VERSION_MISMATCH)
        tray_show_notification(NT_TRAY_ERROR, TEXT("Stadia ViGEm"),
                               TEXT("ViGEmBus version mismatch — reinstall"));
    else if (vr != VIGEM_ERROR_NONE)
        tray_show_notification(NT_TRAY_ERROR, TEXT("Stadia ViGEm"),
                               TEXT("Could not connect to ViGEmBus"));
    else
        vigem_connected = TRUE;

    /* Read HidHide state from registry */
    if (hidhide_available()) {
        HKEY hk;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            TEXT("SYSTEM\\CurrentControlSet\\Services\\HidHide\\Parameters"),
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
            DWORD active = 0, sz = sizeof(active);
            RegQueryValueEx(hk, TEXT("Active"), NULL, NULL, (LPBYTE)&active, &sz);
            RegCloseKey(hk);
            hidhide_enabled = (active != 0);
        }
    }

    stadia_update_callback  = stadia_controller_update_cb;
    stadia_destroy_callback = stadia_controller_stop_cb;

    refresh_devices();
    tray_register_device_notification(hid_get_class(), device_change_cb);

    while (tray_loop(TRUE) == 0)
        ;

    /* Cleanup */
    AcquireSRWLockExclusive(&active_devices_lock);
    for (INT i = 0; i < active_device_count; i++) {
        if (active_devices[i]->src_device) {
            hid_close_device(active_devices[i]->src_device);
            hid_free_device(active_devices[i]->src_device);
        }
        if (vigem_connected) {
            vigem_target_x360_unregister_notification(active_devices[i]->tgt_device);
            vigem_target_remove(vigem_client, active_devices[i]->tgt_device);
            vigem_target_free(active_devices[i]->tgt_device);
        }
        free(active_devices[i]);
    }
    active_device_count = 0;
    ReleaseSRWLockExclusive(&active_devices_lock);

    if (vigem_connected) vigem_disconnect(vigem_client);
    vigem_free(vigem_client);

    free(tray_menu_device_count.text);
    free(tray_menu_startup.text);
    free(tray_menu_hidhide.text);
    free(tray.menu);
    return 0;
}
