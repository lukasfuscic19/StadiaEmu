/*
 * main.c -- Program entry point and Stadia device mapping.
 *
 * USB + BLE input via HID. USB vibration works; BT vibration is blocked on Win11.
 */

#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include <windows.h>
#include <synchapi.h>
#include <shlobj.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <ViGEm/Client.h>

#include "tray.h"
#include "hid.h"
#include "utils.h"
#include "stadia.h"
#include "gatt.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#ifndef _DEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#define MAX_ACTIVE_DEVICE_COUNT 4
#define APP_NAME                TEXT("StadiaViGEm")
#define STARTUP_REG_KEY         TEXT("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run")
#define HIDHIDE_CLI             TEXT("C:\\Program Files\\Nefarius Software Solutions\\HidHide\\x64\\HidHideCLI.exe")

/* -----------------------------------------------------------------------
 * Debug logging (writes to debug.log next to exe)
 * ----------------------------------------------------------------------- */
#include <stdarg.h>
static void dbg_log(const char *fmt, ...)
{
    static FILE *f = NULL;
    static CRITICAL_SECTION cs;
    static BOOL cs_ok = FALSE;
    if (!cs_ok) { InitializeCriticalSection(&cs); cs_ok = TRUE; }
    EnterCriticalSection(&cs);
    if (!f) {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        WCHAR *sl = wcsrchr(path, L'\\');
        if (sl) { sl[1] = 0; wcscat_s(path, MAX_PATH, L"debug.log"); }
        f = _wfopen(path, L"a");
    }
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [MAIN] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fprintf(f, "\n"); fflush(f);
    }
    LeaveCriticalSection(&cs);
}

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
    TCHAR cmd[1024];
    _stprintf_s(cmd, _countof(cmd), TEXT("\"%s\" %s"), HIDHIDE_CLI, args);
    dbg_log("hidhide_run: %ls", cmd);
    STARTUPINFO si = {0}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        dbg_log("hidhide_run: CreateProcess FAILED err=%u", GetLastError());
        return;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    dbg_log("hidhide_run: exit_code=%u", exit_code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

/*
 * Enumerate all HID devices whose Hardware ID starts with VID_18D1&PID_9400
 * (Stadia) and write their Device Instance IDs into the caller's buffer.
 * Returns the number of IDs found (capped at max_count).
 *
 * HidHide's --dev-hide expects a Device Instance ID, e.g.:
 *   HID\VID_18D1&PID_9400\6&1234ABCD&0&0001
 *   HID\{UUID}_DEV_VID&...\...   (BLE HID variant)
 */
static int find_stadia_hid_instance_ids(WCHAR ids[][256], int max_count)
{
    int found = 0;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO hDev = SetupDiGetClassDevs(&hidGuid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDev == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA devInfo;
    ZeroMemory(&devInfo, sizeof(devInfo));
    devInfo.cbSize = sizeof(devInfo);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInfo(hDev, idx, &devInfo) && found < max_count;
         idx++)
    {
        /* Get Hardware IDs — log ALL of them for diagnostics */
        WCHAR hwIds[2048];
        ZeroMemory(hwIds, sizeof(hwIds));
        if (!SetupDiGetDeviceRegistryPropertyW(hDev, &devInfo,
                SPDRP_HARDWAREID, NULL, (PBYTE)hwIds, sizeof(hwIds)-2, NULL))
            continue;

        /* Log each hardware ID so we can see the format in debug.log */
        BOOL isStadia = FALSE;
        for (WCHAR *p = hwIds; *p; p += wcslen(p) + 1) {
            /* Uppercase for case-insensitive comparison */
            WCHAR hwUp[2048];
            wcsncpy_s(hwUp, _countof(hwUp), p, _TRUNCATE);
            _wcsupr_s(hwUp, _countof(hwUp));

            dbg_log("hidhide scan hwid: %ls", p);
            /*
             * USB HID:  HID\VID_18D1&PID_9400...          (standard VID/PID format)
             * BLE HID:  HID\{UUID}_Dev_VID&0218D1_PID&9400 (BT SIG: type 02 + VID 18D1)
             */
            BOOL vidMatch = (wcsstr(hwUp, L"VID_18D1")  != NULL) ||
                            (wcsstr(hwUp, L"VID&0218D1") != NULL);
            BOOL pidMatch = (wcsstr(hwUp, L"PID_9400")  != NULL) ||
                            (wcsstr(hwUp, L"PID&9400")   != NULL);
            if (vidMatch && pidMatch) {
                isStadia = TRUE;
                dbg_log("hidhide scan: STADIA MATCH: %ls", p);
            }
        }
        if (!isStadia) continue;

        /* Get Device Instance ID */
        WCHAR instId[256];
        ZeroMemory(instId, sizeof(instId));
        if (SetupDiGetDeviceInstanceIdW(hDev, &devInfo, instId, 256, NULL)) {
            dbg_log("hidhide: found Stadia HID instance: %ls", instId);
            wcscpy_s(ids[found++], 256, instId);
        }
    }
    SetupDiDestroyDeviceInfoList(hDev);
    return found;
}

static void hidhide_configure(BOOL enable)
{
    if (!hidhide_available()) return;
    if (enable) {
        /* Register this exe in HidHide whitelist */
        TCHAR exe[MAX_PATH]; get_exe_path(exe, MAX_PATH);
        TCHAR arg[MAX_PATH+32];
        _stprintf_s(arg, _countof(arg), TEXT("--app-reg \"%s\""), exe);
        hidhide_run(arg);

        /* Enumerate actual Stadia HID device instance IDs and hide each one.
         * Using real instance IDs (not just hardware ID prefix) is more reliable
         * and correctly handles BLE HID devices which have a different path format. */
        WCHAR ids[8][256];
        int n = find_stadia_hid_instance_ids(ids, 8);
        if (n == 0) {
            /* Fallback: try USB hardware ID prefix */
            dbg_log("hidhide: no Stadia HID devices enumerated, using USB fallback");
            hidhide_run(TEXT("--dev-hide \"HID\\VID_18D1&PID_9400\""));
        }
        for (int i = 0; i < n; i++) {
            TCHAR hide_arg[320];
            _stprintf_s(hide_arg, _countof(hide_arg), TEXT("--dev-hide \"%s\""), ids[i]);
            hidhide_run(hide_arg);
        }
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
/* Extract 12-char Bluetooth MAC from a BLE HID device path.
 * Path format: ...{00001812-...}_dev_vid&0218d1_pid&9400_rev&XXXX_AABBCCDDEEFF#...
 * Returns TRUE and writes uppercase MAC to mac_out[20] on success. */
static BOOL extract_ble_mac(const WCHAR *path, WCHAR mac_out[20])
{
    /* Look for _rev& marker; MAC follows the next _ after the 4-digit rev */
    const WCHAR *p = _tcsistr(path, TEXT("_rev&"));
    if (!p) return FALSE;
    p += 5; /* skip "_rev&" */
    /* Skip the 4-char revision digits */
    for (int i = 0; i < 4 && *p && *p != L'_'; i++, p++);
    if (*p != L'_') return FALSE;
    p++; /* skip underscore */
    /* Read 12 hex chars */
    int k;
    for (k = 0; k < 12; k++) {
        if (!iswxdigit(p[k])) break;
        mac_out[k] = towupper(p[k]);
    }
    if (k != 12) return FALSE;
    mac_out[12] = L'\0';
    return TRUE;
}

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

    /* Log usage info for diagnostics — used to identify duplicate HID collections. */
    dbg_log("add_usb_device: UP=%04X U=%04X in_sz=%u path=%ls",
            device->usage_page, device->usage, device->input_report_size, path);

    BOOL is_ble_hid = (_tcsistr(path, TEXT("00001812-0000-1000-8000-00805f9b34fb")) != NULL);
    struct stadia_controller *ctrl =
        stadia_controller_create(device, !is_ble_hid);
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

    if (is_ble_hid) {
        WCHAR mac[20] = {0};
        if (extract_ble_mac(path, mac)) {
            wcscpy_s(ad->bt_address, 20, mac);
            ad->battery_pct = gatt_get_battery_level(mac);
            dbg_log("add_usb_device: BLE HID mac=%ls battery=%d", mac, ad->battery_pct);
        }
    }

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
    /* Re-entrancy guard: drop nested refresh calls from WM_DEVICECHANGE / WM_APP. */
    static volatile LONG in_refresh = 0;
    if (InterlockedCompareExchange(&in_refresh, 1, 0) != 0) {
        dbg_log("refresh_devices: SKIPPED (re-entrant call)");
        return;
    }

    dbg_log("refresh_devices: begin (active=%d)", active_device_count);

    /* --- Cleanup disconnected devices (active=FALSE set by disconnect callbacks) --- */
    {
        BOOL found_dead = FALSE;
        AcquireSRWLockExclusive(&active_devices_lock);
        for (int i = 0; i < active_device_count; i++) {
            if (active_devices[i]->controller->active) continue;

            dbg_log("refresh_devices: destroying dead device[%d] bt=%ls", i, active_devices[i]->bt_address);

            /* Detach ViGEm */
            if (vigem_connected) {
                vigem_target_x360_unregister_notification(active_devices[i]->tgt_device);
                vigem_target_remove(vigem_client, active_devices[i]->tgt_device);
                vigem_target_free(active_devices[i]->tgt_device);
            }
            /* Close USB handle if any */
            if (active_devices[i]->src_device) {
                hid_close_device(active_devices[i]->src_device);
                hid_free_device(active_devices[i]->src_device);
            }
            /* Destroy controller (threads already stopped, gatt already closed) */
            struct stadia_controller *dead = active_devices[i]->controller;
            free(active_devices[i]);
            if (i < active_device_count - 1)
                memmove(&active_devices[i], &active_devices[i+1],
                        sizeof(struct active_device*) * (active_device_count-i-1));
            active_device_count--;
            found_dead = TRUE;
            /* stadia_controller_destroy frees the struct and closes handles */
            ReleaseSRWLockExclusive(&active_devices_lock);
            stadia_controller_destroy(dead);
            AcquireSRWLockExclusive(&active_devices_lock);
            i--; /* recheck same index */
        }
        ReleaseSRWLockExclusive(&active_devices_lock);
        if (found_dead) {
            rebuild_tray_menu();
            tray_update(&tray);
        }
    }

    /* --- HID scan (USB + BLE via HID class) ---
     * BLE Stadia appears as a HID gamepad (VID&0218d1_PID&9400) managed by the
     * Windows Bluetooth HID driver. We read input via ReadFile, same as USB.
     * Direct GATT notifications are blocked by the OS HID driver's exclusive
     * CCCD subscription, so we do NOT use BluetoothGATTRegisterEvent for input. */
    LPTSTR usb_filters[3] = {STADIA_USB_HW_FILTER, STADIA_BLE_HID_FILTER, NULL};
    struct hid_device_info *dev_info = hid_enumerate(usb_filters);
    struct hid_device_info *cur;

    /* Remove HID devices no longer present. */
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++) {
        BOOL found = FALSE;
        for (cur = dev_info; cur; cur = cur->next)
            if (active_devices[i]->src_device &&
                _tcscmp(active_devices[i]->src_device->path, cur->path) == 0)
            { found = TRUE; break; }
        if (!found && active_devices[i]->controller->active) {
            dbg_log("refresh_devices: HID device[%d] gone, flagging inactive", i);
            active_devices[i]->controller->active = FALSE;
            SetEvent(active_devices[i]->controller->stopping_event);
            if (active_devices[i]->src_device)
                CancelIoEx(active_devices[i]->src_device->handle,
                            &active_devices[i]->src_device->input_ol);
            tray_post_refresh();
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);

    /* Add new HID devices (USB and BLE) */
    for (cur = dev_info; cur; cur = cur->next) {
        BOOL found = FALSE;
        AcquireSRWLockShared(&active_devices_lock);
        for (int i = 0; i < active_device_count; i++)
            if (active_devices[i]->src_device &&
                _tcscmp(cur->path, active_devices[i]->src_device->path) == 0)
            { found = TRUE; break; }
        ReleaseSRWLockShared(&active_devices_lock);
        if (!found) {
            dbg_log("refresh_devices: adding HID device %ls", cur->path);
            add_usb_device(cur->path);
        }
    }
    while (dev_info) { cur = dev_info->next; hid_free_device_info(dev_info); dev_info = cur; }

    /* Refresh battery for connected BLE HID devices */
    AcquireSRWLockExclusive(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
        if (active_devices[i]->bt_address[0] != 0)
            active_devices[i]->battery_pct =
                gatt_get_battery_level(active_devices[i]->bt_address);
    ReleaseSRWLockExclusive(&active_devices_lock);

    rebuild_tray_menu();
    tray_update(&tray);

    dbg_log("refresh_devices: done (active=%d)", active_device_count);
    InterlockedExchange(&in_refresh, 0);
}

static void device_change_cb(UINT op, LPTSTR path)
{
    (void)op; (void)path;
    dbg_log("device_change_cb: WM_DEVICECHANGE op=%u", op);
    refresh_devices();
}

/* Called when WM_APP is posted (e.g. HID disconnect) */
static void on_wm_app(void) {
    dbg_log("on_wm_app: WM_APP received, calling refresh_devices");
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

    tray_set_wm_app_callback(on_wm_app);
    stadia_update_callback    = stadia_controller_update_cb;
    stadia_disconnect_notify  = tray_post_refresh;
    stadia_destroy_callback = stadia_controller_stop_cb;

    /* First pass: scan devices so HidHide knows which instance IDs to blacklist. */
    refresh_devices();
    tray_register_device_notification(hid_get_class(), device_change_cb);

    /* HidHide auto-management: enable after device scan so the blacklist is populated.
     * Disable on quit (see cleanup below).  Tray toggle lets user temporarily disable. */
    if (hidhide_available()) {
        dbg_log("startup: HidHide available — auto-enabling after device scan");
        hidhide_configure(TRUE);
    }

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

    /* Disable HidHide cloaking on exit so the physical controller is visible
     * again without the app running. */
    if (hidhide_available())
        hidhide_run(TEXT("--cloak-off"));

    free(tray_menu_device_count.text);
    free(tray_menu_startup.text);
    free(tray.menu);
    return 0;
}
