/*
 * Apple Magic Trackpad 2 USB-mode emulator (PID 0x0265)
 *
 * Self-contained QEMU USB HID device that exposes the same wire-format
 * face as a real Apple Magic Trackpad 2 in USB-cable mode:
 *   - 4 USB HID interfaces (vendor / Mouse+Digitizer / vendor / vendor)
 *   - Per-interface HID Report Descriptors byte-identical to real device
 *     (verified against a real Magic Trackpad 2 USB pcap — see
 *     paravirt-re/library/apple-magic-hid/captures/usb-pcap-2026-05-10-multitouch.md)
 *   - SET_REPORT(feature, ID=0x02, payload={0x02, 0x01}) on Interface 1 enables
 *     "raw multitouch" mode; thereafter Report 0x02 carries multitouch frames
 *     of 12 + 9N bytes (N active fingers) instead of the 8-byte boot-mouse shape
 *
 * Driver bind: macOS PID match → AppleUSBTopCaseHIDDriver →
 * AppleDeviceManagementHIDEventService → AppleMultitouchTrackpadHIDEventDriver
 * (NOT the generic IOHIDPointing fallback).
 *
 * v1 scope: structural shape + SET_REPORT handler + 1-finger multitouch emit
 * driven from QEMU's input subsystem (single-pointer abs from VNC). Multi-
 * finger gestures (2-finger scroll, pinch, etc.) need a multi-pointer source
 * (SPICE multi-touch path or evdev passthrough) — out of scope for v1.
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/input/hid.h"
#include "hw/usb/hid.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"

/* ---------------------------------------------------------------------------
 * String table
 * ------------------------------------------------------------------------ */

enum {
    STR_AMTP_MFR = 1,
    STR_AMTP_PRODUCT,
    STR_AMTP_SERIAL,
    STR_AMTP_IFACE0,    /* "Device Management" historically */
    STR_AMTP_IFACE1,    /* mouse / digitizer / vendor */
    STR_AMTP_IFACE2,    /* vendor 0xd */
    STR_AMTP_IFACE3,    /* vendor 0x03 */
};

static const USBDescStrings desc_strings_amtp = {
    [STR_AMTP_MFR]     = "Apple Inc.",
    [STR_AMTP_PRODUCT] = "Magic Trackpad",
    [STR_AMTP_SERIAL]  = "CC2916600VBJ2XQA5",
    [STR_AMTP_IFACE0]  = "Device Management",
    [STR_AMTP_IFACE1]  = "Touchpad",
    [STR_AMTP_IFACE2]  = "Vendor",
    [STR_AMTP_IFACE3]  = "Vendor",
};

/* ---------------------------------------------------------------------------
 * HID Report Descriptors (byte-for-byte from real-device pcap)
 *
 * Source: paravirt-re/library/apple-magic-hid/captures/usb-pcap-2026-05-09.md
 * §3 "HID Report descriptors (per interface) — full hex". DO NOT modify
 * without re-capturing — Apple's macOS HID stack matches the literal byte
 * sequence and rejects close-but-not-equal descriptors.
 * ------------------------------------------------------------------------ */

/* Interface 0 — vendor reports (Reports 0xe0 / 0x9a / 0x90). 83 bytes. */
static const uint8_t amtp_iface0_report_desc[] = {
    0x06, 0x00, 0xff, 0x09, 0x0b, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0b,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x96, 0x04, 0x00, 0x85, 0xe0,
    0x81, 0x22, 0x09, 0x0b, 0x96, 0x01, 0x00, 0x85, 0x9a, 0x81, 0x22, 0xc0,
    0x06, 0x00, 0xff, 0x09, 0x14, 0xa1, 0x01, 0x85, 0x90, 0x05, 0x84, 0x75,
    0x01, 0x95, 0x03, 0x15, 0x00, 0x25, 0x01, 0x09, 0x61, 0x05, 0x85, 0x09,
    0x44, 0x09, 0x46, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0x75, 0x08, 0x95,
    0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x65, 0x81, 0x02, 0xc0,
};

/* Interface 1 — Mouse + Digitizer + Vendor 0xc (Reports 0x02 / 0x3f / 0x44).
 * 110 bytes. Note: Report 0x02 declares an 8-byte boot-mouse shape, but after
 * SET_REPORT(feature, 0x02, {0x02, 0x01}) the device re-uses Report 0x02
 * with a variable-length multitouch shape (12 + 9N bytes). The Report 0x44
 * declaration is a pre-allocated max-size slot the device never fills on
 * USB-cable mode. */
static const uint8_t amtp_iface1_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x85, 0x02, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x06, 0x95, 0x04, 0x75, 0x08, 0x81, 0x01, 0xc0, 0xc0, 0x05, 0x0d,
    0x09, 0x05, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0c, 0x15, 0x00, 0x26,
    0xff, 0x00, 0x75, 0x08, 0x95, 0x10, 0x85, 0x3f, 0x81, 0x22, 0xc0, 0x06,
    0x00, 0xff, 0x09, 0x0c, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0c, 0x15,
    0x00, 0x26, 0xff, 0x00, 0x85, 0x44, 0x75, 0x08, 0x96, 0x6b, 0x05, 0x81,
    0x00, 0xc0,
};

/* Interface 2 — vendor 0xd (Report 0x3f input + 0x53 output). 36 bytes. */
static const uint8_t amtp_iface2_report_desc[] = {
    0x06, 0x00, 0xff, 0x09, 0x0d, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0d,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x85, 0x3f, 0x96, 0x0f, 0x00,
    0x81, 0x02, 0x09, 0x0d, 0x85, 0x53, 0x96, 0x3f, 0x00, 0x91, 0x02, 0xc0,
};

/* Interface 3 — vendor 0x03 (Report 0xc0 input). 27 bytes. */
static const uint8_t amtp_iface3_report_desc[] = {
    0x06, 0x00, 0xff, 0x09, 0x03, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x03,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x85, 0xc0, 0x96, 0x6b, 0x00, 0x75, 0x08,
    0x81, 0x02, 0xc0,
};

/* ---------------------------------------------------------------------------
 * USB descriptor structures
 * ------------------------------------------------------------------------ */

#define AMTP_VID                0x05ac
#define AMTP_PID                0x0265
#define AMTP_BCD_DEVICE         0x0871

#define AMTP_EP_IFACE0_IN       1   /* 0x81 — vendor heartbeats */
#define AMTP_EP_IFACE1_IN       3   /* 0x83 — multitouch / boot mouse */
#define AMTP_EP_IFACE2_IN       4   /* 0x84 — vendor 0xd input */
#define AMTP_EP_IFACE2_OUT      4   /* 0x04 — vendor 0xd output */
#define AMTP_EP_IFACE3_IN       5   /* 0x85 — vendor 0x03 input */

static const USBDescIface desc_iface_amtp[] = {
    /* Interface 0 — vendor reports (Reports 0xe0 / 0x9a / 0x90) */
    {
        .bInterfaceNumber              = 0,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_HID,
        .bInterfaceSubClass            = 0,
        .bInterfaceProtocol            = 0,
        .iInterface                    = STR_AMTP_IFACE0,
        .ndesc                         = 1,
        .descs = (USBDescOther[]) {
            {
                /* HID class descriptor */
                .data = (uint8_t[]) {
                    0x09,                /* bLength */
                    USB_DT_HID,          /* bDescriptorType */
                    0x10, 0x01,          /* bcdHID 0x0110 */
                    0x00,                /* bCountryCode */
                    0x01,                /* bNumDescriptors */
                    USB_DT_REPORT,       /* bDescriptorType */
                    sizeof(amtp_iface0_report_desc), 0x00,
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | AMTP_EP_IFACE0_IN,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 16,
                .bInterval             = 8,
            },
        },
    },
    /* Interface 1 — Mouse + Digitizer + Vendor 0xc (Reports 0x02 / 0x3f / 0x44).
     * bSubClass=1 bProto=2 = Boot Mouse — drives the initial driver bind even
     * before the vendor SET_REPORT switches the device into multitouch mode. */
    {
        .bInterfaceNumber              = 1,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_HID,
        .bInterfaceSubClass            = 1,    /* Boot */
        .bInterfaceProtocol            = 2,    /* Mouse */
        .iInterface                    = STR_AMTP_IFACE1,
        .ndesc                         = 1,
        .descs = (USBDescOther[]) {
            {
                .data = (uint8_t[]) {
                    0x09, USB_DT_HID, 0x10, 0x01, 0x00,
                    0x01, USB_DT_REPORT,
                    sizeof(amtp_iface1_report_desc), 0x00,
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | AMTP_EP_IFACE1_IN,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 64,
                .bInterval             = 1,    /* 125µs poll @ high-speed */
            },
        },
    },
    /* Interface 2 — vendor 0xd (Reports 0x3f input + 0x53 output) */
    {
        .bInterfaceNumber              = 2,
        .bNumEndpoints                 = 2,
        .bInterfaceClass               = USB_CLASS_HID,
        .bInterfaceSubClass            = 0,
        .bInterfaceProtocol            = 0,
        .iInterface                    = STR_AMTP_IFACE2,
        .ndesc                         = 1,
        .descs = (USBDescOther[]) {
            {
                .data = (uint8_t[]) {
                    0x09, USB_DT_HID, 0x10, 0x01, 0x00,
                    0x01, USB_DT_REPORT,
                    sizeof(amtp_iface2_report_desc), 0x00,
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | AMTP_EP_IFACE2_IN,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 16,
                .bInterval             = 8,
            },
            {
                .bEndpointAddress      = USB_DIR_OUT | AMTP_EP_IFACE2_OUT,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 64,
                .bInterval             = 2,
            },
        },
    },
    /* Interface 3 — vendor 0x03 (Report 0xc0 input) */
    {
        .bInterfaceNumber              = 3,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_HID,
        .bInterfaceSubClass            = 0,
        .bInterfaceProtocol            = 0,
        .iInterface                    = STR_AMTP_IFACE3,
        .ndesc                         = 1,
        .descs = (USBDescOther[]) {
            {
                .data = (uint8_t[]) {
                    0x09, USB_DT_HID, 0x10, 0x01, 0x00,
                    0x01, USB_DT_REPORT,
                    sizeof(amtp_iface3_report_desc), 0x00,
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | AMTP_EP_IFACE3_IN,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 64,
                .bInterval             = 2,
            },
        },
    },
};

static const USBDescDevice desc_device_amtp = {
    .bcdUSB                = 0x0200,
    .bMaxPacketSize0       = 64,
    .bNumConfigurations    = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 4,
            .bConfigurationValue   = 1,
            .iConfiguration        = 0,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 250,    /* 500 mA */
            .nif                   = ARRAY_SIZE(desc_iface_amtp),
            .ifs                   = desc_iface_amtp,
        },
    },
};

static const USBDesc desc_amtp = {
    .id = {
        .idVendor          = AMTP_VID,
        .idProduct         = AMTP_PID,
        .bcdDevice         = AMTP_BCD_DEVICE,
        .iManufacturer     = STR_AMTP_MFR,
        .iProduct          = STR_AMTP_PRODUCT,
        .iSerialNumber     = STR_AMTP_SERIAL,
    },
    .full = &desc_device_amtp,
    .high = &desc_device_amtp,
    .str  = desc_strings_amtp,
};

/* ---------------------------------------------------------------------------
 * Device state + report-emit logic
 * ------------------------------------------------------------------------ */

#define TYPE_USB_APPLE_MAGIC_TRACKPAD "apple-magic-trackpad"

#define AMTP_QUEUE_DEPTH       32       /* power of two */
#define AMTP_REPORT_MAX_LEN    64       /* boot mouse 8B, multitouch up to ~64B */

typedef struct USBAppleMagicTrackpadReport {
    uint8_t data[AMTP_REPORT_MAX_LEN];
    uint8_t len;
} USBAppleMagicTrackpadReport;

typedef struct USBAppleMagicTrackpadState {
    USBDevice dev;

    /* Set by SET_REPORT(feature, ID=0x02, payload={0x02, 0x01}) on Interface 1. */
    bool multitouch_enabled;

    /* Single-pointer state from QEMU's input subsystem. VNC delivers ABS;
     * we map (abs_x, abs_y) directly into trackpad-coordinate fingertip.
     * prev_abs_{x,y} feed boot-mouse abs→rel delta computation when the
     * trackpad is still in pre-multitouch-enable mode (Report 0x02 boot
     * mouse shape). */
    int32_t  abs_x;
    int32_t  abs_y;
    int32_t  prev_abs_x;
    int32_t  prev_abs_y;
    bool     button_left;
    bool     finger_down;       /* edge-triggered: was a touch active last frame? */
    bool     pending_event;

    /* Sequence/timestamp counter for the multitouch frame header.
     * Real device uses what looks like a free-running counter; macOS doesn't
     * appear to validate the value, so a monotonic increment is sufficient. */
    uint32_t mt_seq;

    /* Ring queue of pending Interface 1 input reports (boot-mouse OR
     * multitouch — same ring, the report's len field discriminates). */
    USBAppleMagicTrackpadReport queue[AMTP_QUEUE_DEPTH];
    unsigned q_head;
    unsigned q_tail;

    /* QEMU input handler binding. */
    QemuInputHandlerState *input_handler;

    /* Cached EP3 IN endpoint pointer for usb_wakeup() — pattern stolen from
     * dev-hid.c apple-magic-keyboard's `boot_intr`. Without wakeup, after
     * the host's first NAK on EP3 IN it parks the URB and never re-polls
     * (interrupt-IN endpoints are level-triggered in the xhci sense — the
     * device must signal "data ready" or the host stays parked). */
    USBEndpoint *intr_ep3;
} USBAppleMagicTrackpadState;

OBJECT_DECLARE_SIMPLE_TYPE(USBAppleMagicTrackpadState, USB_APPLE_MAGIC_TRACKPAD)

static inline unsigned amtp_q_count(USBAppleMagicTrackpadState *s)
{
    return (s->q_head - s->q_tail) & (AMTP_QUEUE_DEPTH - 1);
}

static inline bool amtp_q_empty(USBAppleMagicTrackpadState *s)
{
    return s->q_head == s->q_tail;
}

static inline bool amtp_q_full(USBAppleMagicTrackpadState *s)
{
    return amtp_q_count(s) == AMTP_QUEUE_DEPTH - 1;
}

static void amtp_enqueue(USBAppleMagicTrackpadState *s,
                         const uint8_t *data, uint8_t len)
{
    if (amtp_q_full(s)) {
        s->q_tail = (s->q_tail + 1) & (AMTP_QUEUE_DEPTH - 1);
    }
    USBAppleMagicTrackpadReport *r = &s->queue[s->q_head];
    memcpy(r->data, data, len);
    r->len = len;
    s->q_head = (s->q_head + 1) & (AMTP_QUEUE_DEPTH - 1);

    /* Notify QEMU's USB stack that EP3 IN now has data. Without this, the
     * host stays parked after its first NAK and never re-polls. */
    if (s->intr_ep3) {
        usb_wakeup(s->intr_ep3, 0);
    }
}

/* Boot-mouse Report 0x02 (8 bytes) — emitted BEFORE multitouch enable.
 *   byte 0: 0x02 (Report ID)
 *   byte 1: button mask (3-bit)
 *   byte 2: int8 dX
 *   byte 3: int8 dY
 *   byte 4..7: reserved 0
 */
static void amtp_emit_boot_mouse(USBAppleMagicTrackpadState *s,
                                 int8_t dx, int8_t dy, uint8_t buttons)
{
    uint8_t buf[8] = { 0x02, buttons, (uint8_t)dx, (uint8_t)dy, 0, 0, 0, 0 };
    amtp_enqueue(s, buf, sizeof(buf));
}

/* Multitouch Report 0x02 — emitted AFTER multitouch enable.
 *   byte 0:    0x02 (Report ID)
 *   byte 1:    button state (0x01 = pressed)
 *   bytes 2..7: 6-byte sequence/timestamp counter (little-endian)
 *   bytes 8 onwards: N × 9-byte per-finger records (Linux hid-magicmouse layout)
 *   last 4 bytes: trailer (purpose unverified — zeros are accepted by macOS
 *                 in observed wire frames)
 *
 * For v1 we emit either 0-finger (12-byte idle frame) or 1-finger (21-byte
 * active frame). 2+ finger paths require multi-pointer input which QEMU's
 * input subsystem doesn't expose via VNC.
 *
 * Per-finger record (9 bytes per Linux hid-magicmouse.c):
 *   byte 0..2: x — signed 13-bit packed: ((b1<<27 | b0<<19) >> 19)
 *   byte 2..4: y — signed 13-bit packed similarly
 *   byte 4: touch_major (low nibble) | touch_minor (high nibble of byte 5)
 *   byte 5: size (high nibble) | touch_minor (low nibble)
 *   byte 6: orientation (signed 4-bit, range -31..32 stretched)
 *   byte 7: pressure (0..253)
 *   byte 8: id (low 4 bits) | state (high 4 bits)
 *
 * v1 fixed values (good enough for cursor + click; gestures need a multi-
 * pointer source):
 *   touch_major = 0x07 (smallish ellipse)
 *   touch_minor = 0x05
 *   size        = 0x40 (mid)
 *   orientation = 0   (no axis tilt)
 *   pressure    = button_left ? 0xC0 : 0x40
 *   id          = 0
 *   state       = 0x4 (active touch) when finger down, 0x0 (lifted) otherwise
 */
static void amtp_pack_finger(uint8_t *out, int32_t x, int32_t y,
                             uint8_t pressure, uint8_t state, uint8_t id)
{
    /* x: signed 13-bit. Pack low 8 in byte 0, high 5 in low 5 of byte 1. */
    uint16_t xu = (uint16_t)(x & 0x1fff);
    uint16_t yu = (uint16_t)(y & 0x1fff);
    out[0] = xu & 0xff;
    out[1] = ((xu >> 8) & 0x1f) | ((yu & 0x07) << 5);
    out[2] = (yu >> 3) & 0xff;
    out[3] = 0x07;                  /* touch_major nibble + low minor */
    out[4] = 0x40 | 0x05;            /* size | touch_minor */
    out[5] = 0;                     /* orientation */
    out[6] = pressure;
    out[7] = (id & 0x0f) | ((state & 0x0f) << 4);
    out[8] = 0;                     /* trailing per-finger byte (observed pattern; verify when multi-finger gestures come online) */
}

/* x range from VNC abs (0..32767) to trackpad x-coord (-3678..3825 per real
 * Magic Trackpad 2 sensor range observed in Linux driver). Symmetric for y. */
static int32_t amtp_map_abs(int32_t v, int32_t out_min, int32_t out_max)
{
    int64_t span = (int64_t)(out_max - out_min);
    return out_min + (int32_t)((int64_t)v * span / 32767);
}

static void amtp_emit_multitouch(USBAppleMagicTrackpadState *s)
{
    uint8_t buf[AMTP_REPORT_MAX_LEN];
    int n_fingers = s->finger_down ? 1 : 0;
    int frame_len = 12 + 9 * n_fingers;

    memset(buf, 0, frame_len);
    buf[0] = 0x02;                                  /* Report ID */
    buf[1] = s->button_left ? 0x01 : 0x00;          /* button state */
    /* 6-byte counter at bytes 2..7 (little-endian) */
    buf[2] = (s->mt_seq >>  0) & 0xff;
    buf[3] = (s->mt_seq >>  8) & 0xff;
    buf[4] = (s->mt_seq >> 16) & 0xff;
    buf[5] = (s->mt_seq >> 24) & 0xff;
    buf[6] = 0;
    buf[7] = 0x01;                                  /* observed: low byte of trailing field is 0x01 */

    if (n_fingers >= 1) {
        int32_t mt_x = amtp_map_abs(s->abs_x, -3678,  3825);
        int32_t mt_y = amtp_map_abs(s->abs_y, -2280,  2280);
        amtp_pack_finger(&buf[8], mt_x, mt_y,
                         s->button_left ? 0xC0 : 0x40,
                         /* state: 4=touching */ 4,
                         /* id */ 0);
    }
    /* trailer (last 4 bytes) — observed all-zero in some frames, varies in
     * others. macOS' AppleMultitouchTrackpadHIDEventDriver appears to ignore
     * non-zero trailing bytes; zero is safe. */

    s->mt_seq++;
    amtp_enqueue(s, buf, frame_len);
}

/* ---------------------------------------------------------------------------
 * QEMU input handler — VNC abs pointer → 1-finger multitouch frames
 * ------------------------------------------------------------------------ */

static void amtp_input_event(DeviceState *dev, QemuConsole *src,
                             InputEvent *evt)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT) {
            s->button_left = btn->down;
        }
        s->pending_event = true;
        break;
    }
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *m = evt->u.abs.data;
        if (m->axis == INPUT_AXIS_X) {
            s->abs_x = m->value;
        } else if (m->axis == INPUT_AXIS_Y) {
            s->abs_y = m->value;
        }
        s->finger_down = true;
        s->pending_event = true;
        break;
    }
    default:
        break;
    }
}

static void amtp_input_sync(DeviceState *dev)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);

    if (!s->pending_event) {
        return;
    }
    s->pending_event = false;

    if (s->multitouch_enabled) {
        amtp_emit_multitouch(s);
    } else {
        /* Pre-enable: boot-mouse Report 0x02 (8-byte shape: ID + button
         * + dx + dy + 4B reserved). VNC delivers ABS only; convert to
         * REL by tracking the previous-frame absolute and emitting the
         * delta. Scale: VNC abs is 0..32767 across the screen width
         * (~1080px on a 1920×1080 host), so 1px ≈ 30 abs units. We use
         * a /4 divisor (≈ 7-8 abs units per dx unit) — this gives a
         * snappy cursor while keeping each delta within the int8 range
         * a boot-mouse report can carry. Larger pointer jumps in one
         * frame get clamped to ±127. */
        int32_t dx_abs = s->abs_x - s->prev_abs_x;
        int32_t dy_abs = s->abs_y - s->prev_abs_y;
        int32_t dx = dx_abs / 4;
        int32_t dy = dy_abs / 4;
        if (dx > 127)  dx = 127;
        if (dx < -127) dx = -127;
        if (dy > 127)  dy = 127;
        if (dy < -127) dy = -127;
        s->prev_abs_x = s->abs_x;
        s->prev_abs_y = s->abs_y;
        amtp_emit_boot_mouse(s, (int8_t)dx, (int8_t)dy,
                             s->button_left ? 0x01 : 0x00);
    }
}

static QemuInputHandler amtp_input_handler = {
    .name  = "Apple Magic Trackpad 2",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = amtp_input_event,
    .sync  = amtp_input_sync,
};

/* ---------------------------------------------------------------------------
 * USB device callbacks
 * ------------------------------------------------------------------------ */

static void usb_apple_magic_trackpad_handle_reset(USBDevice *dev)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);
    s->multitouch_enabled = false;
    s->pending_event      = false;
    s->finger_down        = false;
    s->button_left        = false;
    s->mt_seq             = 0;
    s->prev_abs_x         = 0;
    s->prev_abs_y         = 0;
    s->q_head = s->q_tail = 0;
}

static void usb_apple_magic_trackpad_handle_control(USBDevice *dev, USBPacket *p,
                                                    int request, int value,
                                                    int index, int length,
                                                    uint8_t *data)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    /* GET_DESCRIPTOR(REPORT) — per-interface HID Report Descriptor lookup.
     * usb_desc_handle_control doesn't deliver these (HID-class extension);
     * we have to dispatch by wIndex (interface number) ourselves. macOS'
     * AppleUserUSBHostHIDDevice dext crashes with "::start fail" + IOUserServer
     * "server exit before start()" if this STALLs — the dext can't parse the
     * report layout and bails. Pattern mirrors dev-hid.c:1500 for
     * apple-magic-keyboard. */
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        if ((value >> 8) == 0x22) {
            const uint8_t *rd = NULL;
            uint16_t rd_len = 0;
            uint16_t copy;
            switch (index) {
            case 0: rd = amtp_iface0_report_desc; rd_len = sizeof(amtp_iface0_report_desc); break;
            case 1: rd = amtp_iface1_report_desc; rd_len = sizeof(amtp_iface1_report_desc); break;
            case 2: rd = amtp_iface2_report_desc; rd_len = sizeof(amtp_iface2_report_desc); break;
            case 3: rd = amtp_iface3_report_desc; rd_len = sizeof(amtp_iface3_report_desc); break;
            default: break;
            }
            if (rd) {
                copy = length < rd_len ? length : rd_len;
                memcpy(data, rd, copy);
                p->actual_length = copy;
                return;
            }
        }
        break;
    /* HID class SET_REPORT — macOS issues feature/Report-0x02/{0x02, 0x01} on
     * Interface 1 to enable multitouch raw mode. ANY Interface-1 SET_REPORT
     * with a {0x02, *} payload trips multitouch; Linux's hid-magicmouse uses
     * the same magic. */
    case HID_SET_REPORT: {
        uint8_t report_type = (value >> 8) & 0xff;
        uint8_t report_id   = value & 0xff;

        if (report_type == 0x03 && report_id == 0x02 &&
            index == 1 && length >= 1 && data[0] == 0x02) {
            s->multitouch_enabled = true;
        }
        /* ACK with success — even unknown SET_REPORTs (other report IDs,
         * other interfaces) get silently absorbed so a probe loop never
         * stalls waiting for a STALL. */
        p->actual_length = length;
        break;
    }

    /* HID class SET_IDLE / GET_IDLE — boilerplate. Accept any SET_IDLE; report
     * idle=0 (infinite) on GET_IDLE. macOS probes these on every HID iface
     * during AppleUSBTopCaseHIDDriver match-probe; STALLing trips a retry
     * loop that pushes the driver-chain attach into the BT-fallback path. */
    case HID_SET_IDLE:
        break;
    case HID_GET_IDLE:
        data[0] = 0;
        p->actual_length = 1;
        break;

    /* HID class GET_PROTOCOL / SET_PROTOCOL — accept SET; report protocol=1
     * (report mode, not boot) on GET. Same defensive reasoning as SET_IDLE. */
    case HID_GET_PROTOCOL:
        data[0] = 1;
        p->actual_length = 1;
        break;
    case HID_SET_PROTOCOL:
        break;

    /* HID class GET_REPORT — per-interface, per-Report-ID. Sizes mirror the
     * Report Descriptor declarations exactly (decoded from the byte arrays
     * above with tools/hid-decode.py); a size mismatch trips the macOS HID
     * parser into a (a,4020001) busy timeout and the driver-chain probe
     * (AppleMultitouchTrackpadHIDEventDriver) falls through to the Bluetooth
     * Setup Assistant. STALL for IDs not declared on the queried interface
     * (real-device behaviour). Pattern mirrors dev-hid.c's apple-magic-keyboard.
     *
     * Interface 0 — Apple TopCase vendor (shared across kbd and trackpad):
     *   0xe0  4B Input — vendor TopCase event
     *   0x9a  1B Input — modifier signal
     *   0x90  2B Input — battery (AC/charge bits + level)
     *
     * Interface 1 — Mouse + Digitizer + Vendor 0xc:
     *   0x02  7B Input  — boot-mouse (button + dx + dy + 4B reserved). After
     *                     SET_REPORT(feat,0x02,{0x02,0x01}) the same Report ID
     *                     carries variable-length multitouch frames over EP3
     *                     IN, but the HID-class GET_REPORT reply still uses
     *                     the descriptor-declared 7B shape.
     *   0x3f 16B Input  — vendor 0xc reply (descriptor REPORT_COUNT=16, NOT 15
     *                     — Iface 2's 0x3f is 15B but Iface 1's is 16B).
     *   0x44 1387B      — pre-allocated descriptor slot the device never fills
     *                     on USB-cable mode (per 2026-05-10 multitouch pcap
     *                     §1). Real device STALLs GET_REPORT on this ID; we
     *                     match — returning 1387 zeros breaks the parser.
     *
     * Interface 2 — vendor 0xd:
     *   0x3f 15B Input  — vendor IN
     *   0x53 63B Output — vendor OUT (host may still issue GET on Output)
     *
     * Interface 3 — vendor 0x03:
     *   0xc0 107B Input — vendor IN (NOT 1387B; 2026-05-09 doc misread)
     *
     * Body is zero-filled — probe checks descriptor+size agreement, not
     * content. Live trackpad data flows over Interface 1 EP3 IN.
     */
    case HID_GET_REPORT: {
        uint8_t report_id = value & 0xff;
        uint16_t reply_len = 0;

        if (index == 0) {
            switch (report_id) {
            case 0xe0: reply_len = 4; break;
            case 0x9a: reply_len = 1; break;
            case 0x90: reply_len = 2; break;
            default: break;
            }
        } else if (index == 1) {
            switch (report_id) {
            case 0x02: reply_len = 7; break;
            case 0x3f: reply_len = 16; break;
            /* 0x44 — match real-device STALL (unfilled descriptor slot). */
            default: break;
            }
        } else if (index == 2) {
            switch (report_id) {
            case 0x3f: reply_len = 15; break;
            case 0x53: reply_len = 63; break;
            default: break;
            }
        } else if (index == 3) {
            switch (report_id) {
            case 0xc0: reply_len = 107; break;
            default: break;
            }
        }

        if (reply_len == 0) {
            p->status = USB_RET_STALL;
            break;
        }
        if (reply_len > length) {
            reply_len = length;
        }
        memset(data, 0, reply_len);
        p->actual_length = reply_len;
        break;
    }

    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_apple_magic_trackpad_handle_data(USBDevice *dev, USBPacket *p)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);

    if (p->pid != USB_TOKEN_IN) {
        p->status = USB_RET_STALL;
        return;
    }

    /* Only Interface 1 EP3 IN carries cursor/multitouch data in v1. Other
     * IN endpoints return NAK so the driver doesn't queue garbage. */
    if (p->ep->nr != AMTP_EP_IFACE1_IN) {
        p->status = USB_RET_NAK;
        return;
    }

    if (amtp_q_empty(s)) {
        p->status = USB_RET_NAK;
        return;
    }

    USBAppleMagicTrackpadReport *r = &s->queue[s->q_tail];
    s->q_tail = (s->q_tail + 1) & (AMTP_QUEUE_DEPTH - 1);
    usb_packet_copy(p, r->data, r->len);
}

static void usb_apple_magic_trackpad_realize(USBDevice *dev, Error **errp)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);

    usb_desc_create_serial(dev);
    usb_desc_init(dev);

    /* Cache EP3 IN for usb_wakeup() in amtp_enqueue(). */
    s->intr_ep3 = usb_ep_get(dev, USB_TOKEN_IN, AMTP_EP_IFACE1_IN);

    s->input_handler = qemu_input_handler_register(DEVICE(dev),
                                                   &amtp_input_handler);
    qemu_input_handler_activate(s->input_handler);
}

static void usb_apple_magic_trackpad_unrealize(USBDevice *dev)
{
    USBAppleMagicTrackpadState *s = USB_APPLE_MAGIC_TRACKPAD(dev);

    if (s->input_handler) {
        qemu_input_handler_unregister(s->input_handler);
        s->input_handler = NULL;
    }
}

static const VMStateDescription vmstate_apple_magic_trackpad = {
    .name = "apple-magic-trackpad",
    .unmigratable = 1,
};

static void usb_apple_magic_trackpad_class_initfn(ObjectClass *klass,
                                                  const void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_apple_magic_trackpad_realize;
    uc->product_desc   = "Apple Magic Trackpad 2 (USB-mode emulator, multitouch)";
    uc->usb_desc       = &desc_amtp;
    uc->handle_reset   = usb_apple_magic_trackpad_handle_reset;
    uc->handle_control = usb_apple_magic_trackpad_handle_control;
    uc->handle_data    = usb_apple_magic_trackpad_handle_data;
    uc->unrealize      = usb_apple_magic_trackpad_unrealize;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc           = "Apple Magic Trackpad 2 (USB HID multitouch, "
                         "VID 0x05ac PID 0x0265)";
    dc->vmsd           = &vmstate_apple_magic_trackpad;
}

static const TypeInfo usb_apple_magic_trackpad_info = {
    .name          = TYPE_USB_APPLE_MAGIC_TRACKPAD,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBAppleMagicTrackpadState),
    .class_init    = usb_apple_magic_trackpad_class_initfn,
};

static void usb_apple_magic_trackpad_register_types(void)
{
    type_register_static(&usb_apple_magic_trackpad_info);
}

type_init(usb_apple_magic_trackpad_register_types)
