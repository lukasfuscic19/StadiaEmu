/*
 * gatt.cpp -- Win32 GATT interface for Stadia BLE controller.
 *
 * Device enumeration and input polling use SetupAPI + BluetoothGATT
 * Win32 APIs (no Package manifest needed).
 *
 * Vibration write uses WinRT GattCharacteristic::WriteValueAsync,
 * which requires the process to have package identity with the
 * bluetooth.genericAttributeProfile device capability.
 * Install the Sparse Package (StadiaViGEm.msix) once and always
 * launch via COM ActivationManager or the Start Menu shortcut.
 */

#include "gatt.h"

#include <windows.h>
#include <appmodel.h>
#include <setupapi.h>
#include <bluetoothleapis.h>
#include <bthledef.h>
#include <devpkey.h>

/* WinRT â€” only used for vibration write */
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "bluetoothapis.lib")
#pragma comment(lib, "windowsapp")
#pragma comment(lib, "kernel32.lib")

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
/* -----------------------------------------------------------------------
 * Debug logging (writes to debug.log next to exe)
 * ----------------------------------------------------------------------- */
#include <stdio.h>
#include <stdarg.h>
static void gatt_dbg(const char *fmt, ...)
{
    static FILE *f = nullptr;
    static CRITICAL_SECTION cs;
    static bool cs_ok = false;
    if (!cs_ok) { InitializeCriticalSection(&cs); cs_ok = true; }
    EnterCriticalSection(&cs);
    if (!f) {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        WCHAR *sl = wcsrchr(path, L'\\');
        if (sl) { sl[1] = 0; wcscat_s(path, L"debug.log"); }
        f = _wfopen(path, L"a");
    }
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [GATT] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fprintf(f, "\n"); fflush(f);
    }
    LeaveCriticalSection(&cs);
}

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */
static const GUID BTH_LE_ATT_GUID =
    {0x781aee18, 0x7733, 0x4ce4, {0xad,0xd0,0x91,0xf4,0x1c,0x67,0xb5,0x92}};

#define HID_SERVICE_UUID    0x1812
#define HID_REPORT_UUID     0x2A4D
#define REPORT_REF_UUID     0x2908
#define REPORT_TYPE_INPUT   1
#define REPORT_TYPE_OUTPUT  2
#define STADIA_VIB_REPORT   0x05

/* -----------------------------------------------------------------------
 * Internal struct
 * ----------------------------------------------------------------------- */
struct gatt_device {
    HANDLE                  dev_handle;

    BTH_LE_GATT_CHARACTERISTIC input_char;
    BTH_LE_GATT_CHARACTERISTIC output_char;
    bool                    has_output;

    /* WinRT output char (vibration) â€” valid only with package identity */
    GattCharacteristic      winrt_output{ nullptr };

    HANDLE                  poll_thread;
    volatile bool           running;

    gatt_input_cb           on_input;
    gatt_disconnect_cb      on_disconnect;
    void                   *userdata;
};

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Open the BTH_LE_ATT device handle for a given MAC address. */
static HANDLE open_btle_handle(const WCHAR *bt_address)
{
    HDEVINFO hDev = SetupDiGetClassDevs(
        (GUID*)&BTH_LE_ATT_GUID, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDev == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA iface = {};
    iface.cbSize = sizeof(iface);
    HANDLE result = INVALID_HANDLE_VALUE;

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(hDev, NULL, (GUID*)&BTH_LE_ATT_GUID, i, &iface);
         i++)
    {
        DWORD n = 0;
        SetupDiGetDeviceInterfaceDetail(hDev, &iface, NULL, 0, &n, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA det =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(n);
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(hDev, &iface, det, n, NULL, NULL)) {
            free(det); continue;
        }

        /* Match MAC address in path (case-insensitive) */
        WCHAR up_path[512], up_mac[20];
        wcsncpy_s(up_path, 512, det->DevicePath, _TRUNCATE);
        _wcsupr_s(up_path, 512);
        wcsncpy_s(up_mac, 20, bt_address, _TRUNCATE);
        _wcsupr_s(up_mac, 20);

        if (!wcsstr(up_path, up_mac)) { free(det); continue; }

        /* GENERIC_READ required for BluetoothGATTRegisterEvent (notifications).
         * GENERIC_WRITE required for BluetoothGATTSetDescriptorValue (CCCD enable).
         * Zero access is enough for attribute enumeration but not for event reg. */
        HANDLE h = CreateFileW(det->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            /* Verify HID service present */
            USHORT sc = 0;
            BluetoothGATTGetServices(h, 0, NULL, &sc, BLUETOOTH_GATT_FLAG_NONE);
            if (sc > 0) {
                BTH_LE_GATT_SERVICE *svcs =
                    (BTH_LE_GATT_SERVICE*)malloc(sizeof(BTH_LE_GATT_SERVICE)*sc);
                BluetoothGATTGetServices(h, sc, svcs, &sc, BLUETOOTH_GATT_FLAG_NONE);
                for (USHORT s = 0; s < sc; s++) {
                    if (svcs[s].ServiceUuid.IsShortUuid &&
                        svcs[s].ServiceUuid.Value.ShortUuid == HID_SERVICE_UUID) {
                        result = h; break;
                    }
                }
                free(svcs);
            }
            if (result == INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        free(det);
        if (result != INVALID_HANDLE_VALUE) break;
    }

    SetupDiDestroyDeviceInfoList(hDev);
    return result;
}

/* Notification callback -- called by BT stack thread pool when a new input
 * report arrives from the Stadia controller.
 * The HID Input Report characteristic is Notify-only (no Read property),
 * so BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE returns 0x80070001. */
static void CALLBACK _gatt_notify_cb(
    BTH_LE_GATT_EVENT_TYPE evType,
    PVOID evParam,
    PVOID ctx)
{
    if (evType != CharacteristicValueChangedEvent) return;
    gatt_device *dev = (gatt_device*)ctx;
    if (!dev->running) return;
    PBLUETOOTH_GATT_VALUE_CHANGED_EVENT ev =
        (PBLUETOOTH_GATT_VALUE_CHANGED_EVENT)evParam;
    if (ev->CharacteristicValue->DataSize > 0 && dev->on_input)
        dev->on_input(ev->CharacteristicValue->Data,
                      ev->CharacteristicValue->DataSize,
                      dev->userdata);
}

/* Poll thread -- registers for GATT notifications, then sleeps until
 * gatt_close() sets running=false.  Disconnect detection is handled by
 * the main thread (WM_DEVICECHANGE -> refresh_devices -> BLE scan). */
static DWORD WINAPI _poll_thread(LPVOID param)
{
    gatt_device *dev = (gatt_device*)param;
    gatt_dbg("poll_thread: started, char IsNotifiable=%d", dev->input_char.IsNotifiable);

    /*
     * Enable notifications by writing 0x0001 to the CCCD (0x2902).
     * BluetoothGATTRegisterEvent on some Windows versions/configs does NOT
     * do this automatically and returns ERROR_INVALID_FUNCTION without it.
     */
    USHORT dc = 0;
    BluetoothGATTGetDescriptors(dev->dev_handle, &dev->input_char,
        0, NULL, &dc, BLUETOOTH_GATT_FLAG_NONE);
    gatt_dbg("poll_thread: input_char has %d descriptors", dc);
    if (dc > 0) {
        BTH_LE_GATT_DESCRIPTOR *descs =
            (BTH_LE_GATT_DESCRIPTOR*)malloc(sizeof(BTH_LE_GATT_DESCRIPTOR)*dc);
        BluetoothGATTGetDescriptors(dev->dev_handle, &dev->input_char,
            dc, descs, &dc, BLUETOOTH_GATT_FLAG_NONE);
        for (USHORT d = 0; d < dc; d++) {
            gatt_dbg("poll_thread: desc[%d] uuid=0x%04X isShort=%d",
                d, descs[d].DescriptorUuid.Value.ShortUuid,
                descs[d].DescriptorUuid.IsShortUuid);
            if (!descs[d].DescriptorUuid.IsShortUuid) continue;
            if (descs[d].DescriptorUuid.Value.ShortUuid != 0x2902) continue; /* CCCD */
            /* Write Notification Enable (0x0001) */
            BTH_LE_GATT_DESCRIPTOR_VALUE cccd;
            ZeroMemory(&cccd, sizeof(cccd));
            cccd.DescriptorType = ClientCharacteristicConfiguration;
            cccd.ClientCharacteristicConfiguration.IsSubscribeToNotification = 1;
            HRESULT hrc = BluetoothGATTSetDescriptorValue(
                dev->dev_handle, &descs[d], &cccd,
                BLUETOOTH_GATT_FLAG_NONE);
            gatt_dbg("poll_thread: CCCD write hr=0x%08X", (unsigned)hrc);
            break;
        }
        free(descs);
    }

    BLUETOOTH_GATT_VALUE_CHANGED_EVENT_REGISTRATION reg;
    ZeroMemory(&reg, sizeof(reg));
    reg.NumCharacteristics = 1;
    reg.Characteristics[0] = dev->input_char;
    BLUETOOTH_GATT_EVENT_HANDLE evHandle = NULL;

    HRESULT hr = BluetoothGATTRegisterEvent(
        dev->dev_handle,
        CharacteristicValueChangedEvent,
        &reg,
        _gatt_notify_cb,
        dev,
        &evHandle,
        BLUETOOTH_GATT_FLAG_NONE);
    gatt_dbg("poll_thread: BluetoothGATTRegisterEvent hr=0x%08X", (unsigned)hr);

    if (SUCCEEDED(hr)) {
        gatt_dbg("poll_thread: notifications registered OK, waiting for input");
        while (dev->running) Sleep(50);
        gatt_dbg("poll_thread: shutdown, unregistering notifications");
        BluetoothGATTUnregisterEvent(evHandle, BLUETOOTH_GATT_FLAG_NONE);
    } else {
        gatt_dbg("poll_thread: BluetoothGATTRegisterEvent FAILED hr=0x%08X", (unsigned)hr);
    }

    /* Do NOT call on_disconnect here -- disconnect is handled by the main
     * thread via WM_DEVICECHANGE to avoid a race with gatt_close. */
    gatt_dbg("poll_thread: done");
    return 0;
}
/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

extern "C" BOOL gatt_has_package_identity(void)
{
    WCHAR name[256];
    UINT32 len = ARRAYSIZE(name);
    LONG err = GetCurrentPackageFullName(&len, name);
    gatt_dbg("gatt_has_package_identity: err=%ld name=%ls", err, err == ERROR_SUCCESS ? name : L"(none)");
    return err == ERROR_SUCCESS ? TRUE : FALSE;
}
extern "C" struct gatt_device *gatt_open_stadia(const WCHAR       *bt_address,
                                                 gatt_input_cb      on_input,
                                                 gatt_disconnect_cb on_disconnect,
                                                 void              *userdata)
{
    HANDLE h = open_btle_handle(bt_address);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    /* Get HID service */
    USHORT sc = 0;
    BluetoothGATTGetServices(h, 0, NULL, &sc, BLUETOOTH_GATT_FLAG_NONE);
    if (sc == 0) { CloseHandle(h); return nullptr; }

    BTH_LE_GATT_SERVICE *svcs =
        (BTH_LE_GATT_SERVICE*)malloc(sizeof(BTH_LE_GATT_SERVICE)*sc);
    BluetoothGATTGetServices(h, sc, svcs, &sc, BLUETOOTH_GATT_FLAG_NONE);

    BTH_LE_GATT_SERVICE hid_svc = {};
    bool found_svc = false;
    for (USHORT s = 0; s < sc; s++) {
        if (svcs[s].ServiceUuid.IsShortUuid &&
            svcs[s].ServiceUuid.Value.ShortUuid == HID_SERVICE_UUID) {
            hid_svc = svcs[s]; found_svc = true; break;
        }
    }
    free(svcs);
    if (!found_svc) { CloseHandle(h); return nullptr; }

    /* Get characteristics */
    USHORT cc = 0;
    BluetoothGATTGetCharacteristics(h, &hid_svc, 0, NULL, &cc,
                                     BLUETOOTH_GATT_FLAG_NONE);
    if (cc == 0) { CloseHandle(h); return nullptr; }

    BTH_LE_GATT_CHARACTERISTIC *chars =
        (BTH_LE_GATT_CHARACTERISTIC*)malloc(sizeof(BTH_LE_GATT_CHARACTERISTIC)*cc);
    BluetoothGATTGetCharacteristics(h, &hid_svc, cc, chars, &cc,
                                     BLUETOOTH_GATT_FLAG_NONE);

    BTH_LE_GATT_CHARACTERISTIC input_char = {}, output_char = {};
    bool has_input = false, has_output = false;

    for (USHORT c = 0; c < cc; c++) {
        if (!chars[c].CharacteristicUuid.IsShortUuid) continue;
        if (chars[c].CharacteristicUuid.Value.ShortUuid != HID_REPORT_UUID) continue;

        /* Try to read Report Reference descriptor */
        UCHAR rep_id = 0, rep_type = 0;
        bool got_ref = false;

        USHORT dc = 0;
        BluetoothGATTGetDescriptors(h, &chars[c], 0, NULL, &dc,
                                     BLUETOOTH_GATT_FLAG_NONE);
        if (dc > 0) {
            BTH_LE_GATT_DESCRIPTOR *descs =
                (BTH_LE_GATT_DESCRIPTOR*)malloc(sizeof(BTH_LE_GATT_DESCRIPTOR)*dc);
            BluetoothGATTGetDescriptors(h, &chars[c], dc, descs, &dc,
                                         BLUETOOTH_GATT_FLAG_NONE);
            for (USHORT d = 0; d < dc; d++) {
                if (!descs[d].DescriptorUuid.IsShortUuid) continue;
                if (descs[d].DescriptorUuid.Value.ShortUuid != REPORT_REF_UUID) continue;
                USHORT needed = 0;
                BluetoothGATTGetDescriptorValue(h, &descs[d], 0, NULL, &needed,
                                                 BLUETOOTH_GATT_FLAG_NONE);
                if (needed > 0) {
                    BTH_LE_GATT_DESCRIPTOR_VALUE *val =
                        (BTH_LE_GATT_DESCRIPTOR_VALUE*)malloc(needed);
                    if (SUCCEEDED(BluetoothGATTGetDescriptorValue(h, &descs[d], needed,
                                   val, NULL, BLUETOOTH_GATT_FLAG_NONE)) &&
                        val->DataSize >= 2) {
                        rep_id   = val->Data[0];
                        rep_type = val->Data[1];
                        got_ref  = true;
                    }
                    free(val);
                }
                break;
            }
            free(descs);
        }

        if (got_ref) {
            gatt_dbg("gatt_open_stadia: char[%d] uuid=0x%04X rep_id=%d rep_type=%d notifiable=%d readable=%d writable=%d",
                c, chars[c].CharacteristicUuid.Value.ShortUuid, rep_id, rep_type,
                chars[c].IsNotifiable, chars[c].IsReadable, chars[c].IsWritable);
            if (rep_type == REPORT_TYPE_INPUT && !has_input) {
                input_char = chars[c]; has_input = true;
            } else if (rep_type == REPORT_TYPE_OUTPUT &&
                       rep_id == STADIA_VIB_REPORT && !has_output) {
                output_char = chars[c]; has_output = true;
            }
        } else {
            gatt_dbg("gatt_open_stadia: char[%d] uuid=0x%04X (no ref) notifiable=%d readable=%d writable=%d",
                c, chars[c].CharacteristicUuid.Value.ShortUuid,
                chars[c].IsNotifiable, chars[c].IsReadable, chars[c].IsWritable);
            /* Fallback: notifiable = input, writable = output */
            if (chars[c].IsNotifiable && !has_input) {
                input_char = chars[c]; has_input = true;
            } else if ((chars[c].IsWritable || chars[c].IsWritableWithoutResponse)
                       && !has_output) {
                output_char = chars[c]; has_output = true;
            }
        }
    }
    free(chars);

    if (!has_input) { CloseHandle(h); return nullptr; }

    gatt_device *dev = new gatt_device{};
    dev->dev_handle  = h;
    dev->input_char  = input_char;
    dev->output_char = output_char;
    dev->has_output  = has_output;
    dev->on_input    = on_input;
    dev->on_disconnect = on_disconnect;
    dev->userdata    = userdata;
    dev->running     = true;

    /*
     * Try to acquire WinRT GattCharacteristic for vibration write.
     * This requires package identity â€” silently skip if unavailable.
     */
    gatt_dbg("gatt_open_stadia: opened ok, has_input=%d has_output=%d", has_input, has_output);

    /*
     * WinRT vibration init requires package identity (Sparse Package).
     * Skipping without it prevents blocking the main thread with .get() calls
     * which would pump the message loop and cause re-entrant refresh_devices.
     */
    if (has_output) {
        gatt_dbg("gatt_open_stadia: trying WinRT vibration init (pkg_id=%d)", gatt_has_package_identity());
        try {
            winrt::init_apartment();
            ULONGLONG addr = 0;
            for (int k = 0; k < 6; k++) {
                WCHAR byte_str[3] = { bt_address[k*2], bt_address[k*2+1], L'\0' };
                addr = (addr << 8) | wcstoul(byte_str, nullptr, 16);
            }
            auto ble = BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
            if (ble) {
                winrt::guid hid_uuid{0x00001812,0x0000,0x1000,
                    {0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
                auto r = ble.GetGattServicesForUuidAsync(
                             hid_uuid, BluetoothCacheMode::Uncached).get();
                if (r.Status() == GattCommunicationStatus::Success &&
                    r.Services().Size() > 0) {
                    GattDeviceService svc = r.Services().GetAt(0);
                    svc.OpenAsync(GattSharingMode::SharedReadAndWrite).get();
                    GattCharacteristicsResult cr =
                        svc.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
                    for (uint32_t c = 0; c < cr.Characteristics().Size(); c++) {
                        GattCharacteristic ch = cr.Characteristics().GetAt(c);
                        GattCharacteristicProperties props = ch.CharacteristicProperties();
                        bool w  = (props & GattCharacteristicProperties::Write) ==
                                  GattCharacteristicProperties::Write;
                        bool wn = (props & GattCharacteristicProperties::WriteWithoutResponse) ==
                                  GattCharacteristicProperties::WriteWithoutResponse;
                        if (w || wn) {
                            dev->winrt_output = ch;
                            break;
                        }
                    }
            }
                }
        } catch (...) {
            /* No package identity or WinRT unavailable â€” vibration disabled */
        }
    }

    dev->poll_thread = CreateThread(NULL, 0, _poll_thread, dev, 0, NULL);
    return dev;
}

extern "C" BOOL gatt_send_output_report(struct gatt_device *dev,
                                         const BYTE *data, size_t len)
{
    if (!dev || !dev->has_output) return FALSE;

    /* Skip report ID -- GATT writes do not include it */
    const BYTE *payload     = len > 1 ? data + 1 : data;
    size_t      payload_len = len > 1 ? len - 1  : len;

    /* WinRT path -- requires package identity (Sparse Package installed) */
    if (dev->winrt_output) {
        try {
            auto writer = DataWriter{};
            writer.WriteBytes(winrt::array_view<const uint8_t>(payload, payload + payload_len));
            auto buf   = writer.DetachBuffer();
            auto props = dev->winrt_output.CharacteristicProperties();
            auto opt   = ((props & GattCharacteristicProperties::WriteWithoutResponse) ==
                          GattCharacteristicProperties::WriteWithoutResponse)
                       ? GattWriteOption::WriteWithoutResponse
                       : GattWriteOption::WriteWithResponse;
            dev->winrt_output.WriteValueAsync(buf, opt);
            gatt_dbg("gatt_send_output_report: WinRT write fired");
            return TRUE;
        } catch (...) {
            gatt_dbg("gatt_send_output_report: WinRT write threw exception");
        }
    }

    /* Win32 GATT fallback -- direct characteristic write bypassing HID interface. */
    ULONG val_size = (ULONG)(sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE) + payload_len);
    BTH_LE_GATT_CHARACTERISTIC_VALUE *val =
        (BTH_LE_GATT_CHARACTERISTIC_VALUE *)malloc(val_size);
    if (!val) return FALSE;
    ZeroMemory(val, val_size);
    val->DataSize = (ULONG)payload_len;
    memcpy(val->Data, payload, payload_len);

    /* Use WriteWithoutResponse only if the characteristic supports it.
     * Using that flag on a Write-only characteristic causes ERROR_INVALID_FUNCTION. */
    ULONG flags = dev->output_char.IsWritableWithoutResponse
        ? BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE
        : BLUETOOTH_GATT_FLAG_NONE;
    HRESULT hr = BluetoothGATTSetCharacteristicValue(
        dev->dev_handle, &dev->output_char, val, 0, flags);
    free(val);
    gatt_dbg("gatt_send_output_report: Win32 hr=0x%08X flags=0x%lX (W=%d WNR=%d)",
        (unsigned)hr, flags,
        dev->output_char.IsWritable, dev->output_char.IsWritableWithoutResponse);
    return SUCCEEDED(hr);
}

/* Open GATT for vibration output only -- no input poll thread.
 * Used when BLE Stadia input comes via HID (ReadFile) but vibration
 * needs a direct GATT write to the output characteristic.          */
extern "C" struct gatt_device *gatt_open_output_only(const WCHAR *bt_address)
{
    if (!bt_address || bt_address[0] == L'\0') return nullptr;
    gatt_dbg("gatt_open_output_only: addr=%ls", bt_address);

    HANDLE h = open_btle_handle(bt_address);
    if (h == INVALID_HANDLE_VALUE) {
        gatt_dbg("gatt_open_output_only: open_btle_handle failed");
        return nullptr;
    }

    USHORT sc = 0;
    BluetoothGATTGetServices(h, 0, NULL, &sc, BLUETOOTH_GATT_FLAG_NONE);
    if (sc == 0) { CloseHandle(h); return nullptr; }

    BTH_LE_GATT_SERVICE *svcs =
        (BTH_LE_GATT_SERVICE *)malloc(sizeof(BTH_LE_GATT_SERVICE) * sc);
    BluetoothGATTGetServices(h, sc, svcs, &sc, BLUETOOTH_GATT_FLAG_NONE);

    BTH_LE_GATT_CHARACTERISTIC output_char{};
    bool has_output = false;

    for (USHORT s = 0; s < sc && !has_output; s++) {
        if (!svcs[s].ServiceUuid.IsShortUuid ||
            svcs[s].ServiceUuid.Value.ShortUuid != HID_SERVICE_UUID) continue;
        USHORT cc = 0;
        BluetoothGATTGetCharacteristics(h, &svcs[s], 0, NULL, &cc, BLUETOOTH_GATT_FLAG_NONE);
        if (cc == 0) continue;
        BTH_LE_GATT_CHARACTERISTIC *chars =
            (BTH_LE_GATT_CHARACTERISTIC *)malloc(sizeof(BTH_LE_GATT_CHARACTERISTIC) * cc);
        BluetoothGATTGetCharacteristics(h, &svcs[s], cc, chars, &cc, BLUETOOTH_GATT_FLAG_NONE);
        for (USHORT c = 0; c < cc; c++) {
            if (!chars[c].CharacteristicUuid.IsShortUuid ||
                chars[c].CharacteristicUuid.Value.ShortUuid != HID_REPORT_UUID) continue;
            if (chars[c].IsWritable || chars[c].IsWritableWithoutResponse) {
                output_char = chars[c]; has_output = true;
                gatt_dbg("gatt_open_output_only: output char handle=%u W=%d WNR=%d",
                    output_char.AttributeHandle, output_char.IsWritable,
                    output_char.IsWritableWithoutResponse);
                break;
            }
        }
        free(chars);
    }
    free(svcs);

    if (!has_output) {
        gatt_dbg("gatt_open_output_only: no writable char found");
        CloseHandle(h); return nullptr;
    }

    gatt_device *dev = new gatt_device{};
    dev->dev_handle  = h;
    dev->output_char = output_char;
    dev->has_output  = true;
    dev->running     = false;
    dev->poll_thread = NULL;

    /* Try WinRT unconditionally -- on Win10 1803+ most WinRT BT APIs work without
     * package identity. The catch block handles the case where it is unavailable. */
    {
        gatt_dbg("gatt_open_output_only: trying WinRT (pkg_id=%d)", gatt_has_package_identity());
        try {
            winrt::init_apartment();
            ULONGLONG addr = 0;
            for (int k = 0; k < 6; k++) {
                WCHAR b[3] = { bt_address[k*2], bt_address[k*2+1], L'\0' };
                addr = (addr << 8) | wcstoul(b, nullptr, 16);
            }
            gatt_dbg("gatt_open_output_only: WinRT FromBluetoothAddressAsync...");
            auto ble = BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
            if (!ble) {
                gatt_dbg("gatt_open_output_only: WinRT ble=null");
            } else {
                gatt_dbg("gatt_open_output_only: WinRT ble OK, RequestAccessAsync...");
                auto acc = ble.RequestAccessAsync().get();
                gatt_dbg("gatt_open_output_only: WinRT RequestAccessAsync status=%d", (int)acc);

                /* Enumerate ALL services to detect custom non-HID service */
                auto allSvc = ble.GetGattServicesAsync(BluetoothCacheMode::Cached).get();
                gatt_dbg("gatt_open_output_only: WinRT ALL services status=%d count=%u",
                    (int)allSvc.Status(), allSvc.Services().Size());
                for (uint32_t si = 0; si < allSvc.Services().Size(); si++) {
                    auto s = allSvc.Services().GetAt(si);
                    gatt_dbg("gatt_open_output_only: WinRT svc[%u] uuid=%08X-%04X",
                        si, s.Uuid().Data1, s.Uuid().Data2);
                }

                gatt_dbg("gatt_open_output_only: WinRT querying HID service");
                winrt::guid hid_uuid{0x00001812,0x0000,0x1000,
                    {0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
                auto r = ble.GetGattServicesForUuidAsync(hid_uuid, BluetoothCacheMode::Uncached).get();
                gatt_dbg("gatt_open_output_only: WinRT services status=%d count=%u",
                    (int)r.Status(), r.Services().Size());
                if (r.Status() == GattCommunicationStatus::Success && r.Services().Size() > 0) {
                    GattDeviceService svc = r.Services().GetAt(0);

                    /* hidbthle.sys holds the HID service exclusively so OpenAsync
                     * returns AccessDenied(5). Try without opening first (Cached
                     * reads from OS cache, bypassing the driver lock). */
                    auto cr = svc.GetCharacteristicsAsync(BluetoothCacheMode::Cached).get();
                    gatt_dbg("gatt_open_output_only: WinRT chars(Cached) status=%d count=%u",
                        (int)cr.Status(), cr.Characteristics().Size());

                    if (cr.Status() != GattCommunicationStatus::Success || cr.Characteristics().Size() == 0) {
                        /* Cached failed — try opening service first */
                        auto openRes = svc.OpenAsync(GattSharingMode::SharedReadOnly).get();
                        gatt_dbg("gatt_open_output_only: WinRT OpenAsync(SharedReadOnly)=%d", (int)openRes);
                        if (openRes != GattOpenStatus::Success && openRes != GattOpenStatus::AlreadyOpened) {
                            openRes = svc.OpenAsync(GattSharingMode::SharedReadAndWrite).get();
                            gatt_dbg("gatt_open_output_only: WinRT OpenAsync(SharedReadAndWrite)=%d", (int)openRes);
                        }
                        cr = svc.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
                        gatt_dbg("gatt_open_output_only: WinRT chars(Uncached) status=%d count=%u",
                            (int)cr.Status(), cr.Characteristics().Size());
                    }

                    for (uint32_t c = 0; c < cr.Characteristics().Size(); c++) {
                        GattCharacteristic ch = cr.Characteristics().GetAt(c);
                        GattCharacteristicProperties p = ch.CharacteristicProperties();
                        bool w  = (p & GattCharacteristicProperties::Write) ==
                                  GattCharacteristicProperties::Write;
                        bool wn = (p & GattCharacteristicProperties::WriteWithoutResponse) ==
                                  GattCharacteristicProperties::WriteWithoutResponse;
                        gatt_dbg("gatt_open_output_only: WinRT char[%u] uuid=%08X W=%d WNR=%d",
                            c, ch.Uuid().Data1, (int)w, (int)wn);
                        if (w || wn) { dev->winrt_output = ch; break; }
                    }
                }
            }
        } catch (...) { gatt_dbg("gatt_open_output_only: WinRT threw exception"); }
    }

    gatt_dbg("gatt_open_output_only: done, winrt=%s", dev->winrt_output ? "yes" : "no");
    return dev;
}


extern "C" void gatt_close(struct gatt_device *dev)
{
    if (!dev) return;
    gatt_dbg("gatt_close: begin, running=%d", dev->running);
    dev->running = false;
    if (dev->poll_thread) {
        DWORD wait = WaitForSingleObject(dev->poll_thread, 1000);
        if (wait == WAIT_TIMEOUT) {
            gatt_dbg("gatt_close: poll_thread timed out, terminating");
            TerminateThread(dev->poll_thread, 0);
        } else {
            gatt_dbg("gatt_close: poll_thread exited cleanly");
        }
        CloseHandle(dev->poll_thread);
        dev->poll_thread = NULL;
    }
    dev->winrt_output = nullptr;
    if (dev->dev_handle != INVALID_HANDLE_VALUE)
        CloseHandle(dev->dev_handle);
    gatt_dbg("gatt_close: done");
    delete dev;
}

extern "C" int gatt_find_stadia_devices(WCHAR bt_addresses[][20], int max_count)
{
    int found = 0;

    HDEVINFO hDev = SetupDiGetClassDevs(
        (GUID*)&BTH_LE_ATT_GUID, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDev == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA iface = {};
    iface.cbSize = sizeof(iface);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(hDev, NULL, (GUID*)&BTH_LE_ATT_GUID, idx, &iface);
         idx++)
    {
        DWORD n = 0;
        SetupDiGetDeviceInterfaceDetail(hDev, &iface, NULL, 0, &n, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA det =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(n);
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (!SetupDiGetDeviceInterfaceDetail(hDev, &iface, det, n, NULL, NULL)) {
            free(det); continue;
        }

        HANDLE h = CreateFileW(det->DevicePath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { free(det); continue; }

        /* Check HID service 0x1812 */
        bool isStadia = false;
        USHORT sc = 0;
        BluetoothGATTGetServices(h, 0, NULL, &sc, BLUETOOTH_GATT_FLAG_NONE);
        if (sc > 0) {
            BTH_LE_GATT_SERVICE *svcs =
                (BTH_LE_GATT_SERVICE*)malloc(sizeof(BTH_LE_GATT_SERVICE)*sc);
            BluetoothGATTGetServices(h, sc, svcs, &sc, BLUETOOTH_GATT_FLAG_NONE);
            for (USHORT s = 0; s < sc; s++) {
                if (svcs[s].ServiceUuid.IsShortUuid &&
                    svcs[s].ServiceUuid.Value.ShortUuid == HID_SERVICE_UUID) {
                    isStadia = true; break;
                }
            }
            free(svcs);
        }
        CloseHandle(h);

        if (!isStadia) { free(det); continue; }

        /* Extract MAC: 12 hex chars after "dev_" in path */
        WCHAR addr[20] = {};
        WCHAR *devPtr = wcsstr(det->DevicePath, L"dev_");
        if (devPtr) {
            devPtr += 4;
            bool ok = true;
            for (int k = 0; k < 12; k++) {
                if (!iswxdigit(devPtr[k])) { ok = false; break; }
                addr[k] = towupper(devPtr[k]);
            }
            if (!ok) addr[0] = 0;
        }

        if (addr[0] == 0) { free(det); continue; }

        /* Deduplicate */
        bool dup = false;
        for (int i = 0; i < found; i++)
            if (_wcsicmp(bt_addresses[i], addr) == 0) { dup = true; break; }

        if (!dup) {
            gatt_dbg("gatt_find_stadia_devices: found Stadia BLE addr=%ls", addr);
            wcscpy_s(bt_addresses[found], 20, addr);
            found++;
        }
        free(det);
        if (found >= max_count) break;
    }

    SetupDiDestroyDeviceInfoList(hDev);
    gatt_dbg("gatt_find_stadia_devices: returning %d device(s)", found);
    return found;
}

extern "C" INT gatt_get_battery_level(const WCHAR *bt_address)
{
    if (!bt_address || bt_address[0] == 0) return -1;

    /* DEVPKEY {104EA319-6EE2-4701-BD47-8DDBF425BBE5} pid=2 = battery % */
    const DEVPROPKEY battKey = {
        {0x104EA319,0x6EE2,0x4701,{0xBD,0x47,0x8D,0xDB,0xF4,0x25,0xBB,0xE5}}, 2
    };

    WCHAR prefix[64];
    swprintf_s(prefix, 64, L"BTHLE\\DEV_%s", bt_address);

    HDEVINFO hDev = SetupDiGetClassDevs(NULL, NULL, NULL,
                                         DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDev == INVALID_HANDLE_VALUE) return -1;

    INT result = -1;
    SP_DEVINFO_DATA dd = {};
    dd.cbSize = sizeof(dd);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDev, i, &dd); i++) {
        WCHAR id[256] = {};
        SetupDiGetDeviceInstanceIdW(hDev, &dd, id, 256, NULL);
        if (_wcsnicmp(id, prefix, wcslen(prefix)) != 0) continue;

        DEVPROPTYPE propType;
        DWORD val = 0, propSize = sizeof(val);
        if (SetupDiGetDevicePropertyW(hDev, &dd, &battKey,
                &propType, (PBYTE)&val, propSize, &propSize, 0))
            result = (INT)(val > 100 ? 100 : val);
        break;
    }

    SetupDiDestroyDeviceInfoList(hDev);
    return result;
}
