/*
 * stadia.c -- Routines for interacting with a Stadia controller.
 */

#include "stadia.h"
#include "hid.h"
#include "gatt.h"

#include <stdio.h>
#include <stdarg.h>
#include <synchapi.h>
#include <tchar.h>
#include <windows.h>

#pragma comment(lib, "kernel32.lib")

static void stadia_dbg(const char *fmt, ...)
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
        fprintf(f, "[%02d:%02d:%02d.%03d] [STADIA] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fprintf(f, "\n"); fflush(f);
    }
    LeaveCriticalSection(&cs);
}

#define STADIA_READ_TIMEOUT        10
#define STADIA_VIBRATION_REPORT_ID 0x05

static const BYTE init_vibration[5] = {STADIA_VIBRATION_REPORT_ID, 0x00, 0x00, 0x00, 0x00};

static const DWORD dpad_map[8] =
{
    STADIA_BUTTON_UP,
    STADIA_BUTTON_UP    | STADIA_BUTTON_RIGHT,
    STADIA_BUTTON_RIGHT,
    STADIA_BUTTON_RIGHT | STADIA_BUTTON_DOWN,
    STADIA_BUTTON_DOWN,
    STADIA_BUTTON_DOWN  | STADIA_BUTTON_LEFT,
    STADIA_BUTTON_LEFT,
    STADIA_BUTTON_LEFT  | STADIA_BUTTON_UP
};

static void parse_input(struct stadia_controller *c, const BYTE *buf, size_t len)
{
    if (len < 10) return;
    if (buf[0] != 0x03) return;

    AcquireSRWLockExclusive(&c->state_lock);

    c->state.buttons = STADIA_BUTTON_NONE;
    c->state.buttons |= buf[1] < 8 ? dpad_map[buf[1]] : 0;

    c->state.buttons |= (buf[2] & (1<<7)) ? STADIA_BUTTON_RS          : 0;
    c->state.buttons |= (buf[2] & (1<<6)) ? STADIA_BUTTON_OPTIONS     : 0;
    c->state.buttons |= (buf[2] & (1<<5)) ? STADIA_BUTTON_MENU        : 0;
    c->state.buttons |= (buf[2] & (1<<4)) ? STADIA_BUTTON_STADIA_BTN  : 0;
    c->state.buttons |= (buf[3] & (1<<6)) ? STADIA_BUTTON_A           : 0;
    c->state.buttons |= (buf[3] & (1<<5)) ? STADIA_BUTTON_B           : 0;
    c->state.buttons |= (buf[3] & (1<<4)) ? STADIA_BUTTON_X           : 0;
    c->state.buttons |= (buf[3] & (1<<3)) ? STADIA_BUTTON_Y           : 0;
    c->state.buttons |= (buf[3] & (1<<2)) ? STADIA_BUTTON_LB          : 0;
    c->state.buttons |= (buf[3] & (1<<1)) ? STADIA_BUTTON_RB          : 0;
    c->state.buttons |= (buf[3] & (1<<0)) ? STADIA_BUTTON_LS          : 0;

    c->state.left_stick_x  = buf[4];
    c->state.left_stick_y  = buf[5];
    c->state.right_stick_x = buf[6];
    c->state.right_stick_y = buf[7];
    c->state.left_trigger  = buf[8];
    c->state.right_trigger = buf[9];

    ReleaseSRWLockExclusive(&c->state_lock);

    stadia_update_callback(c, &c->state);
}

static DWORD WINAPI _stadia_input_thread(LPVOID lparam)
{
    struct stadia_controller *c = (struct stadia_controller*)lparam;
    INT bytes_read;

    while (c->active) {
        bytes_read = hid_get_input_report(c->device, STADIA_READ_TIMEOUT);

        if (bytes_read == 0) {
            if (WaitForSingleObject(c->stopping_event, 0) == WAIT_OBJECT_0)
                break;
            continue;
        }
        if (bytes_read < 0) break;

        parse_input(c, c->device->input_buffer, (size_t)bytes_read);
    }

    c->active = FALSE;
    SetEvent(c->stopping_event);
    SetEvent(c->output_event);
    if (stadia_disconnect_notify)
        stadia_disconnect_notify();
    return 0;
}

static DWORD WINAPI _stadia_output_thread(LPVOID lparam)
{
    struct stadia_controller *c = (struct stadia_controller*)lparam;
    BYTE vibration[5] = {STADIA_VIBRATION_REPORT_ID, 0x00, 0x00, 0x00, 0x00};
    HANDLE events[2] = {c->output_event, c->stopping_event};

    while (c->active) {
        DWORD wr = WaitForMultipleObjects(2, events, FALSE, 500);
        if (!c->active) break;
        if (wr == WAIT_TIMEOUT) continue;

        AcquireSRWLockShared(&c->vibration_lock);
        /* SDL/Chromium format: [reportId, big_low, big_high, small_low, small_high] */
        vibration[1] = c->big_motor;
        vibration[2] = 0;
        vibration[3] = c->small_motor;
        vibration[4] = 0;
        ReleaseSRWLockShared(&c->vibration_lock);

        stadia_dbg("output_thread: sending vibration big=%d small=%d gatt=%s out_sz=%u",
                   vibration[1], vibration[3], c->gatt ? "yes" : "no",
                   c->device ? c->device->output_report_size : 0);

        if (c->gatt) {
            /* BLE via GATT: try WinRT/Win32 GATT write first */
            BOOL gatt_ok = gatt_send_output_report(c->gatt, vibration, sizeof(vibration));
            if (!gatt_ok) {
                /* GATT write failed — fall back to HID SetOutputReport (ATT Write
                 * Request with response, correct for W=1/WNR=0 characteristics) */
                INT r2 = hid_set_output_report(c->device, vibration, sizeof(vibration));
                stadia_dbg("output_thread: hid_set_output_report returned %d (err=%u)", r2, GetLastError());
            }
        } else {
            INT r = hid_send_output_report(c->device, vibration, sizeof(vibration),
                                   STADIA_READ_TIMEOUT);
            stadia_dbg("output_thread: hid_send_output_report returned %d (err=%u)", r, GetLastError());
        }

        ResetEvent(c->output_event);
    }

    return 0;
}

static void _gatt_input_cb(const BYTE *data, size_t len, void *userdata)
{
    struct stadia_controller *c = (struct stadia_controller*)userdata;
    if (!c->active) return;

    /*
     * BLE GATT characteristic value has no report ID prefix.
     * parse_input expects a USB-style frame: [0x03, dpad, btn1, btn2, axes...].
     * Prepend 0x03 so both USB and BLE share the same parser.
     */
    BYTE buf[32];
    if (len + 1 > sizeof(buf)) return;
    buf[0] = 0x03;
    memcpy(buf + 1, data, len);
    parse_input(c, buf, len + 1);
}

static void _gatt_disconnect_cb(void *userdata)
{
    struct stadia_controller *c = (struct stadia_controller*)userdata;
    if (!c->active) return;
    c->active = FALSE;
    SetEvent(c->stopping_event);
    SetEvent(c->output_event);
    if (stadia_disconnect_notify)
        stadia_disconnect_notify();
}

struct stadia_controller *stadia_controller_create(struct hid_device  *device,
                                                   BOOL                is_bluetooth,
                                                   const WCHAR        *bt_address,
                                                   struct gatt_device *gatt_output)
{
    SECURITY_ATTRIBUTES sa = {
        .nLength              = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = NULL,
        .bInheritHandle       = TRUE
    };

    struct stadia_controller *c =
        (struct stadia_controller*)calloc(1, sizeof(struct stadia_controller));

    c->device       = device;
    c->is_bluetooth = is_bluetooth;
    c->active       = TRUE;

    InitializeSRWLock(&c->state_lock);
    InitializeSRWLock(&c->vibration_lock);
    c->stopping_event = CreateEvent(&sa, TRUE, FALSE, NULL);
    c->output_event   = CreateEvent(&sa, TRUE, FALSE, NULL);

    if (is_bluetooth) {
        if (!bt_address) { stadia_controller_destroy(c); return NULL; }

        c->gatt = gatt_open_stadia(bt_address, _gatt_input_cb, _gatt_disconnect_cb, c);
        if (!c->gatt) { stadia_controller_destroy(c); return NULL; }

        gatt_send_output_report(c->gatt, init_vibration, sizeof(init_vibration));

        c->input_thread  = NULL;
        c->output_thread = CreateThread(&sa, 0, _stadia_output_thread, c, 0, NULL);
    } else {
        /* BLE-via-HID: use gatt_output handle for vibration if provided */
        c->gatt = gatt_output;

        stadia_dbg("create USB/BLE-HID: output_report_size=%u gatt_output=%s",
                   device->output_report_size, gatt_output ? "yes" : "no");
        if (!gatt_output) {
            INT r = hid_send_output_report(device, init_vibration, sizeof(init_vibration),
                                   STADIA_READ_TIMEOUT);
            stadia_dbg("create USB: init_vibration hid_send returned %d (err=%u)", r, GetLastError());
        } else {
            BOOL gatt_ok = gatt_send_output_report(gatt_output, init_vibration, sizeof(init_vibration));
            if (!gatt_ok) {
                INT r2 = hid_set_output_report(device, init_vibration, sizeof(init_vibration));
                stadia_dbg("create BLE: init hid_set_output_report returned %d (err=%u)", r2, GetLastError());
            }
        }

        c->input_thread  = CreateThread(&sa, 0, _stadia_input_thread, c,
                                        CREATE_SUSPENDED, NULL);
        c->output_thread = CreateThread(&sa, 0, _stadia_output_thread, c,
                                        CREATE_SUSPENDED, NULL);

        if (!c->input_thread || !c->output_thread) {
            stadia_controller_destroy(c);
            return NULL;
        }

        ResumeThread(c->input_thread);
        ResumeThread(c->output_thread);
    }

    return c;
}

void stadia_controller_set_vibration(struct stadia_controller *c,
                                     BYTE small_motor, BYTE big_motor)
{
    if (!c->active) return;
    AcquireSRWLockExclusive(&c->vibration_lock);
    c->small_motor = small_motor;
    c->big_motor   = big_motor;
    ReleaseSRWLockExclusive(&c->vibration_lock);
    SetEvent(c->output_event);
}

void stadia_controller_destroy(struct stadia_controller *c)
{
    if (!c) return;

    /* Ensure destroy runs only once */
    if (InterlockedExchange(&c->destroying, 1) != 0) return;

    /* Signal all threads to stop */
    c->active = FALSE;
    SetEvent(c->stopping_event);
    SetEvent(c->output_event);

    if (c->device && c->device->handle != INVALID_HANDLE_VALUE)
        CancelIoEx(c->device->handle, &c->device->input_ol);

    /*
     * Wait for threads individually with short timeout.
     * Do NOT use WaitForMultipleObjects — it can deadlock if one handle
     * is invalid. Do NOT use long timeouts — output thread may be stuck
     * in WinRT COM cleanup which pumps the main thread message loop,
     * causing deadlock if main thread waits here.
     */
    DWORD calling_tid = GetCurrentThreadId();
    HANDLE threads[2] = { c->input_thread, c->output_thread };
    for (int i = 0; i < 2; i++) {
        HANDLE th = threads[i];
        if (!th) continue;
        if (GetThreadId(th) == calling_tid) continue;
        /* 200ms — enough for clean exit, short enough to avoid UI freeze */
        WaitForSingleObject(th, 200);
    }

    /* Release WinRT object before gatt_close to prevent COM deadlock */
    if (c->gatt) {
        gatt_close(c->gatt);
        c->gatt = NULL;
    }

    CloseHandle(c->stopping_event);
    CloseHandle(c->output_event);
    if (c->input_thread)  CloseHandle(c->input_thread);
    if (c->output_thread) CloseHandle(c->output_thread);

    free(c);
}
