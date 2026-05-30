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

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

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

        HANDLE h = CreateFileW(det->DevicePath, 0,
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

/* Input polling thread â€” reads HID input characteristic at ~125 Hz */
static DWORD WINAPI _poll_thread(LPVOID param)
{
    gatt_device *dev = (gatt_device*)param;

    while (dev->running) {
        USHORT needed = 0;
        BluetoothGATTGetCharacteristicValue(
            dev->dev_handle, &dev->input_char,
            0, NULL, &needed, BLUETOOTH_GATT_FLAG_NONE);

        if (needed > 0) {
            BTH_LE_GATT_CHARACTERISTIC_VALUE *val =
                (BTH_LE_GATT_CHARACTERISTIC_VALUE*)malloc(needed);
            HRESULT hr = BluetoothGATTGetCharacteristicValue(
                dev->dev_handle, &dev->input_char,
                needed, val, NULL,
                BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);

            if (SUCCEEDED(hr) && val->DataSize > 0 && dev->on_input)
                dev->on_input(val->Data, val->DataSize, dev->userdata);

            free(val);
        }
        Sleep(8); /* ~125 Hz */
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

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
            if (rep_type == REPORT_TYPE_INPUT && !has_input) {
                input_char = chars[c]; has_input = true;
            } else if (rep_type == REPORT_TYPE_OUTPUT &&
                       rep_id == STADIA_VIB_REPORT && !has_output) {
                output_char = chars[c]; has_output = true;
            }
        } else {
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
    if (has_output) {
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

    /* WinRT path â€” requires package identity */
    if (dev->winrt_output) {
        try {
            auto writer = DataWriter{};
            /* Skip report ID byte â€” GATT write does not include it */
            size_t payload_len = len > 1 ? len - 1 : len;
            const BYTE *payload = len > 1 ? data + 1 : data;
            writer.WriteBytes(winrt::array_view<const uint8_t>(payload, payload + payload_len));
            auto buf = writer.DetachBuffer();
            auto props = dev->winrt_output.CharacteristicProperties();
            auto opt = ((props & GattCharacteristicProperties::WriteWithoutResponse) ==
                        GattCharacteristicProperties::WriteWithoutResponse)
                ? GattWriteOption::WriteWithoutResponse
                : GattWriteOption::WriteWithResponse;
            auto res = dev->winrt_output.WriteValueAsync(buf, opt).get();
            return res == GattCommunicationStatus::Success ? TRUE : FALSE;
        } catch (...) { return FALSE; }
    }
    return FALSE;
}

extern "C" void gatt_close(struct gatt_device *dev)
{
    if (!dev) return;
    dev->running = false;
    if (dev->poll_thread) {
        WaitForSingleObject(dev->poll_thread, 2000);
        CloseHandle(dev->poll_thread);
    }
    dev->winrt_output = nullptr;
    if (dev->dev_handle != INVALID_HANDLE_VALUE)
        CloseHandle(dev->dev_handle);
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
            wcscpy_s(bt_addresses[found], 20, addr);
            found++;
        }
        free(det);
        if (found >= max_count) break;
    }

    SetupDiDestroyDeviceInfoList(hDev);
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
