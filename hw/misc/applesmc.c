/*
 *  Apple SMC controller
 *
 *  Copyright (c) 2007 Alexander Graf
 *
 *  Authors: Alexander Graf <agraf@suse.de>
 *           Susanne Graf <suse@csgraf.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************
 *
 * In all Intel-based Apple hardware there is an SMC chip to control the
 * backlight, fans and several other generic device parameters. It also
 * contains the magic keys used to dongle Mac OS X to the device.
 *
 * This driver was mostly created by looking at the Linux AppleSMC driver
 * implementation and does not support IRQ.
 *
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/acpi/acpi_aml_interface.h"

/* #define DEBUG_SMC */

#define APPLESMC_DEFAULT_IOBASE        0x300
#define TYPE_APPLE_SMC "isa-applesmc"
#define APPLESMC_MAX_DATA_LENGTH       32
#define APPLESMC_PROP_IO_BASE "iobase"

enum {
    APPLESMC_DATA_PORT               = 0x00,
    APPLESMC_CMD_PORT                = 0x04,
    APPLESMC_ERR_PORT                = 0x1e,
    APPLESMC_NUM_PORTS               = 0x20,
};

enum {
    APPLESMC_READ_CMD                = 0x10,
    APPLESMC_WRITE_CMD               = 0x11,
    APPLESMC_GET_KEY_BY_INDEX_CMD    = 0x12,
    APPLESMC_GET_KEY_TYPE_CMD        = 0x13,
};

enum {
    APPLESMC_ST_CMD_DONE             = 0x00,
    APPLESMC_ST_DATA_READY           = 0x01,
    APPLESMC_ST_BUSY                 = 0x02,
    APPLESMC_ST_ACK                  = 0x04,
    APPLESMC_ST_NEW_CMD              = 0x08,
};

enum {
    APPLESMC_ST_1E_CMD_INTRUPTED     = 0x80,
    APPLESMC_ST_1E_STILL_BAD_CMD     = 0x81,
    APPLESMC_ST_1E_BAD_CMD           = 0x82,
    APPLESMC_ST_1E_NOEXIST           = 0x84,
    APPLESMC_ST_1E_WRITEONLY         = 0x85,
    APPLESMC_ST_1E_READONLY          = 0x86,
    APPLESMC_ST_1E_BAD_INDEX         = 0xb8,
};

#ifdef DEBUG_SMC
#define smc_debug(...) fprintf(stderr, "AppleSMC: " __VA_ARGS__)
#else
#define smc_debug(...) do { } while (0)
#endif

static char default_osk[64] = "This is a dummy key. Enter the real key "
                              "using the -osk parameter";

struct AppleSMCData {
    uint8_t len;
    const char *key;
    const char *data;
    QLIST_ENTRY(AppleSMCData) node;
};

OBJECT_DECLARE_SIMPLE_TYPE(AppleSMCState, APPLE_SMC)

struct AppleSMCState {
    ISADevice parent_obj;

    MemoryRegion io_data;
    MemoryRegion io_cmd;
    MemoryRegion io_err;
    uint32_t iobase;
    uint8_t cmd;
    uint8_t status;
    uint8_t status_1e;
    uint8_t last_ret;
    char key[4];
    uint8_t read_pos;
    uint8_t data_len;
    uint8_t data_pos;
    uint8_t data[255];
    char *osk;
    QLIST_HEAD(, AppleSMCData) data_def;
};

static void applesmc_io_cmd_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    AppleSMCState *s = opaque;
    uint8_t status = s->status & 0x0f;

    smc_debug("CMD received: 0x%02x\n", (uint8_t)val);
    switch (val) {
    case APPLESMC_READ_CMD:
    case APPLESMC_WRITE_CMD:
    case APPLESMC_GET_KEY_BY_INDEX_CMD:
    case APPLESMC_GET_KEY_TYPE_CMD:
        /* mos15: Accept all standard SMC commands.
         * Original code only handled READ_CMD — macOS hangs if WRITE/TYPE
         * commands return BAD_CMD during early AppleSMC driver init. */
        if (status == APPLESMC_ST_CMD_DONE || status == APPLESMC_ST_NEW_CMD) {
            s->cmd = val;
            s->status = APPLESMC_ST_NEW_CMD | APPLESMC_ST_ACK;
        } else {
            smc_debug("ERROR: previous command interrupted!\n");
            s->status = APPLESMC_ST_NEW_CMD;
            s->status_1e = APPLESMC_ST_1E_CMD_INTRUPTED;
        }
        break;
    default:
        fprintf(stderr, "mos15-smc: unexpected CMD 0x%02x\n", (uint8_t)val);
        s->status = APPLESMC_ST_NEW_CMD;
        s->status_1e = APPLESMC_ST_1E_BAD_CMD;
    }
    s->read_pos = 0;
    s->data_pos = 0;
}

static const struct AppleSMCData *applesmc_find_key(AppleSMCState *s)
{
    struct AppleSMCData *d;

    QLIST_FOREACH(d, &s->data_def, node) {
        if (!memcmp(d->key, s->key, 4)) {
            return d;
        }
    }
    return NULL;
}

static void applesmc_io_data_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    AppleSMCState *s = opaque;
    const struct AppleSMCData *d;

    smc_debug("DATA received: 0x%02x\n", (uint8_t)val);
    switch (s->cmd) {
    case APPLESMC_READ_CMD:
        if ((s->status & 0x0f) == APPLESMC_ST_CMD_DONE) {
            break;
        }
        if (s->read_pos < 4) {
            s->key[s->read_pos] = val;
            s->status = APPLESMC_ST_ACK;
        } else if (s->read_pos == 4) {
            d = applesmc_find_key(s);
            if (d != NULL) {
                memcpy(s->data, d->data, d->len);
                s->data_len = d->len;
                s->data_pos = 0;
                s->status = APPLESMC_ST_ACK | APPLESMC_ST_DATA_READY;
                s->status_1e = APPLESMC_ST_CMD_DONE;
            } else {
                /* mos15: Return zeros for unknown keys instead of NOEXIST. */
                fprintf(stderr, "mos15-smc: READ unknown key '%c%c%c%c' len=%d\n",
                        s->key[0], s->key[1], s->key[2], s->key[3], (uint8_t)val);
                memset(s->data, 0, APPLESMC_MAX_DATA_LENGTH);
                s->data_len = (uint8_t)val;
                s->data_pos = 0;
                s->status = APPLESMC_ST_ACK | APPLESMC_ST_DATA_READY;
                s->status_1e = APPLESMC_ST_CMD_DONE;
            }
        }
        s->read_pos++;
        break;
    case APPLESMC_WRITE_CMD:
        /* mos15: Accept writes silently. macOS writes SMC keys during
         * power management, fan control, etc. We accept and log them. */
        if ((s->status & 0x0f) == APPLESMC_ST_CMD_DONE) {
            break;
        }
        if (s->read_pos < 4) {
            s->key[s->read_pos] = val;
            s->status = APPLESMC_ST_ACK;
        } else if (s->read_pos == 4) {
            s->data_len = (uint8_t)val;
            s->data_pos = 0;
            s->status = APPLESMC_ST_ACK;
        } else {
            if (s->data_pos < s->data_len) {
                s->data[s->data_pos] = (uint8_t)val;
                s->data_pos++;
                if (s->data_pos == s->data_len) {
                    fprintf(stderr, "mos15-smc: WRITE key '%c%c%c%c' len=%d\n",
                            s->key[0], s->key[1], s->key[2], s->key[3], s->data_len);
                    s->status = APPLESMC_ST_CMD_DONE;
                    s->status_1e = APPLESMC_ST_CMD_DONE;
                } else {
                    s->status = APPLESMC_ST_ACK;
                }
            }
        }
        s->read_pos++;
        break;
    case APPLESMC_GET_KEY_TYPE_CMD:
        /* mos15: Return key type info. Protocol (from VirtualSMC):
         * - Receive 4 bytes of key name
         * - After 4th byte, immediately set DATA_READY with response
         * - Response is 6 bytes: type[4] + size[1] + attr[1]
         * NO length byte between key name and response (unlike READ_CMD). */
        if ((s->status & 0x0f) == APPLESMC_ST_CMD_DONE) {
            break;
        }
        if (s->read_pos < 3) {
            s->key[s->read_pos] = val;
            s->status = APPLESMC_ST_ACK;
        } else if (s->read_pos == 3) {
            /* 4th and final key byte. Unlike READ_CMD which has a 5th byte
             * for data length, GET_KEY_TYPE responds immediately after the
             * 4-byte key name (matching VirtualSMC kern_pmio.cpp behavior). */
            s->key[3] = val;
            d = applesmc_find_key(s);
            if (d != NULL) {
                switch (d->len) {
                case 1:
                    s->data[0] = 'u'; s->data[1] = 'i';
                    s->data[2] = '8'; s->data[3] = ' ';
                    break;
                case 2:
                    s->data[0] = 'u'; s->data[1] = 'i';
                    s->data[2] = '1'; s->data[3] = '6';
                    break;
                case 4:
                    s->data[0] = 'u'; s->data[1] = 'i';
                    s->data[2] = '3'; s->data[3] = '2';
                    break;
                default:
                    s->data[0] = 'c'; s->data[1] = 'h';
                    s->data[2] = '8'; s->data[3] = '*';
                    break;
                }
                s->data[4] = d->len;
                s->data[5] = 0xD0;
            } else {
                fprintf(stderr, "mos15-smc: GET_KEY_TYPE unknown '%c%c%c%c'\n",
                        s->key[0], s->key[1], s->key[2], s->key[3]);
                s->data[0] = 'u'; s->data[1] = 'i';
                s->data[2] = '8'; s->data[3] = ' ';
                s->data[4] = 1;
                s->data[5] = 0xD0;
            }
            s->data_len = 6;
            s->data_pos = 0;
            s->status = APPLESMC_ST_ACK | APPLESMC_ST_DATA_READY;
            s->status_1e = APPLESMC_ST_CMD_DONE;
        }
        s->read_pos++;
        break;
    case APPLESMC_GET_KEY_BY_INDEX_CMD:
        /* mos15: Return key name by index. VirtualSMC receives a 4-byte
         * big-endian index, responds with 4-byte key name. */
        if ((s->status & 0x0f) == APPLESMC_ST_CMD_DONE) {
            break;
        }
        if (s->read_pos < 3) {
            s->key[s->read_pos] = val;
            s->status = APPLESMC_ST_ACK;
        } else if (s->read_pos == 3) {
            s->key[3] = val;
            memset(s->data, 0, 4);
            s->data_len = 4;
            s->data_pos = 0;
            s->status = APPLESMC_ST_ACK | APPLESMC_ST_DATA_READY;
            s->status_1e = APPLESMC_ST_CMD_DONE;
        }
        s->read_pos++;
        break;
    default:
        fprintf(stderr, "mos15-smc: unhandled data for cmd 0x%02x\n", s->cmd);
        s->status = APPLESMC_ST_CMD_DONE;
        s->status_1e = APPLESMC_ST_1E_STILL_BAD_CMD;
    }
}

static void applesmc_io_err_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    smc_debug("ERR_CODE received: 0x%02x, ignoring!\n", (uint8_t)val);
    /* NOTE: writing to the error port not supported! */
}

static uint64_t applesmc_io_data_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSMCState *s = opaque;

    switch (s->cmd) {
    case APPLESMC_READ_CMD:
        if (!(s->status & APPLESMC_ST_DATA_READY)) {
            break;
        }
        if (s->data_pos < s->data_len) {
            s->last_ret = s->data[s->data_pos];
            smc_debug("READ '%c%c%c%c'[%d] = %02x\n",
                      s->key[0], s->key[1], s->key[2], s->key[3],
                      s->data_pos, s->last_ret);
            s->data_pos++;
            if (s->data_pos == s->data_len) {
                s->status = APPLESMC_ST_CMD_DONE;
                smc_debug("READ '%c%c%c%c' Len=%d complete!\n",
                          s->key[0], s->key[1], s->key[2], s->key[3],
                          s->data_len);
            } else {
                s->status = APPLESMC_ST_ACK | APPLESMC_ST_DATA_READY;
            }
        }
        break;
    default:
        s->status = APPLESMC_ST_CMD_DONE;
        s->status_1e = APPLESMC_ST_1E_STILL_BAD_CMD;
    }
    smc_debug("DATA sent: 0x%02x\n", s->last_ret);

    return s->last_ret;
}

static uint64_t applesmc_io_cmd_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSMCState *s = opaque;

    smc_debug("CMD sent: 0x%02x\n", s->status);
    return s->status;
}

static uint64_t applesmc_io_err_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSMCState *s = opaque;

    /* NOTE: read does not clear the 1e status */
    smc_debug("ERR_CODE sent: 0x%02x\n", s->status_1e);
    return s->status_1e;
}

static void applesmc_add_key(AppleSMCState *s, const char *key,
                             int len, const char *data)
{
    struct AppleSMCData *def;

    def = g_new0(struct AppleSMCData, 1);
    def->key = key;
    def->len = len;
    def->data = data;

    QLIST_INSERT_HEAD(&s->data_def, def, node);
}

static void qdev_applesmc_isa_reset(DeviceState *dev)
{
    AppleSMCState *s = APPLE_SMC(dev);

    s->status = 0x00;
    s->status_1e = 0x00;
    s->last_ret = 0x00;
}

static const MemoryRegionOps applesmc_data_io_ops = {
    .write = applesmc_io_data_write,
    .read = applesmc_io_data_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps applesmc_cmd_io_ops = {
    .write = applesmc_io_cmd_write,
    .read = applesmc_io_cmd_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps applesmc_err_io_ops = {
    .write = applesmc_io_err_write,
    .read = applesmc_io_err_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void applesmc_isa_realize(DeviceState *dev, Error **errp)
{
    AppleSMCState *s = APPLE_SMC(dev);

    memory_region_init_io(&s->io_data, OBJECT(s), &applesmc_data_io_ops, s,
                          "applesmc-data", 1);
    isa_register_ioport(&s->parent_obj, &s->io_data,
                        s->iobase + APPLESMC_DATA_PORT);

    memory_region_init_io(&s->io_cmd, OBJECT(s), &applesmc_cmd_io_ops, s,
                          "applesmc-cmd", 1);
    isa_register_ioport(&s->parent_obj, &s->io_cmd,
                        s->iobase + APPLESMC_CMD_PORT);

    memory_region_init_io(&s->io_err, OBJECT(s), &applesmc_err_io_ops, s,
                          "applesmc-err", 1);
    isa_register_ioport(&s->parent_obj, &s->io_err,
                        s->iobase + APPLESMC_ERR_PORT);

    if (!s->osk || (strlen(s->osk) != 64)) {
        warn_report("Using AppleSMC with invalid key");
        s->osk = default_osk;
    }

    QLIST_INIT(&s->data_def);

    /* System identification */
    applesmc_add_key(s, "REV ", 6, "\x01\x13\x0f\x00\x00\x03");
    applesmc_add_key(s, "OSK0", 32, s->osk);
    applesmc_add_key(s, "OSK1", 32, s->osk + 32);
    applesmc_add_key(s, "NATJ", 1, "\0");
    applesmc_add_key(s, "MSSP", 1, "\0");
    applesmc_add_key(s, "MSSD", 1, "\x03");

    /* mos15: GPU power management — prevents AGPM crash cascade.
     * Without HE2N, AGPM reads NOEXIST → error 0x82 → system hang
     * on dynamic wallpaper changes. Value 0x00 = no GPU power mgmt. */
    applesmc_add_key(s, "HE2N", 1, "\x00");

    /* mos15: Watchdog timer control — fixes 16 SMCWDT errors per boot.
     * QEMU's SMC doesn't implement the watchdog key, VirtualSMC doesn't
     * either. macOS queries it during early boot. */
    applesmc_add_key(s, "WDTC", 1, "\x00");

    /* mos15: GPU temperature sensors — iMac20,1 has dGPU temp reporting.
     * Returns 0 (no real GPU to measure). Prevents NOEXIST errors. */
    applesmc_add_key(s, "TGDD", 2, "\x00\x00");
    applesmc_add_key(s, "TG0P", 2, "\x00\x00");

    /* mos15: Fan control — iMac has 1 fan.
     * macOS reads fan count and speed during power management init. */
    applesmc_add_key(s, "FNum", 1, "\x01");
    applesmc_add_key(s, "F0Ac", 2, "\x03\x00");  /* ~768 RPM */
    applesmc_add_key(s, "F0Mn", 2, "\x02\x00");  /* min RPM */
    applesmc_add_key(s, "F0Mx", 2, "\x19\x00");  /* max RPM */

    /* mos15: Platform info keys that VirtualSMC looks for */
    applesmc_add_key(s, "MPRO", 1, "\x01");  /* model property */
    applesmc_add_key(s, "MPRD", 1, "\x00");  /* model product */
    applesmc_add_key(s, "LGPB", 1, "\x00");  /* lid/GPU power */

    /* mos15: CPU/power keys for X86PlatformPlugin */
    applesmc_add_key(s, "BSLN", 1, "\x00");  /* baseline */
    applesmc_add_key(s, "EPCI", 4, "\x00\x00\x00\x00"); /* EPC info */
    applesmc_add_key(s, "BEMB", 1, "\x01");  /* board embedded */

    /* mos15: Boot-discovered keys */
    applesmc_add_key(s, "OSWD", 2, "\x00\x00");  /* OS watchdog timer */
    applesmc_add_key(s, "MSSW", 1, "\x00");  /* macOS software state */
    applesmc_add_key(s, "RGEN", 1, "\x02");  /* SMC generation/revision */
    applesmc_add_key(s, "DPLM", 4, "\x00\x00\x00\x00"); /* display power limit */
    applesmc_add_key(s, "$Adr", 4, "\x00\x00\x03\x00"); /* SMC base address */

    /* mos15: Temperature sensors (27 keys) — all return 0 (no real sensors) */
    applesmc_add_key(s, "TC0F", 2, "\x00\x00");  /* CPU 0 PECI filtered */
    applesmc_add_key(s, "TC0P", 2, "\x00\x00");  /* CPU 0 proximity */
    applesmc_add_key(s, "TCXc", 2, "\x00\x00");  /* CPU PECI core max */
    applesmc_add_key(s, "TG0F", 2, "\x00\x00");  /* GPU 0 filtered */
    applesmc_add_key(s, "TG1F", 2, "\x00\x00");  /* GPU 1 filtered */
    applesmc_add_key(s, "TH0P", 2, "\x00\x00");  /* HDD proximity */
    applesmc_add_key(s, "TH1A", 2, "\x00\x00");  /* HDD 1 ambient */
    applesmc_add_key(s, "TH1C", 2, "\x00\x00");  /* HDD 1 core */
    applesmc_add_key(s, "TH1F", 2, "\x00\x00");  /* HDD 1 filtered */
    applesmc_add_key(s, "TL0V", 2, "\x00\x00");  /* LCD 0 */
    applesmc_add_key(s, "TL1V", 2, "\x00\x00");  /* LCD 1 */
    applesmc_add_key(s, "TM0P", 2, "\x00\x00");  /* memory proximity */
    applesmc_add_key(s, "TM0V", 2, "\x00\x00");  /* memory VRM */
    applesmc_add_key(s, "Tp00", 2, "\x00\x00");  /* power supply */
    applesmc_add_key(s, "Tp2F", 2, "\x00\x00");  /* power supply 2 */
    applesmc_add_key(s, "Ts0S", 2, "\x00\x00");  /* sensor 0 */
    applesmc_add_key(s, "TS0V", 2, "\x00\x00");  /* sensor 0 voltage */
    applesmc_add_key(s, "Ts1S", 2, "\x00\x00");  /* sensor 1 */
    applesmc_add_key(s, "Ts2S", 2, "\x00\x00");  /* sensor 2 */
    applesmc_add_key(s, "TB0T", 2, "\x00\x00");  /* battery 0 */
    applesmc_add_key(s, "TB1T", 2, "\x00\x00");  /* battery 1 */
    applesmc_add_key(s, "TB2T", 2, "\x00\x00");  /* battery 2 */
    applesmc_add_key(s, "TA0V", 2, "\x00\x00");  /* ambient 0 */
    applesmc_add_key(s, "TVMD", 2, "\x00\x00");  /* VRM diode */
    applesmc_add_key(s, "TVmS", 2, "\x00\x00");  /* VRM sense */
    applesmc_add_key(s, "TVSL", 2, "\x00\x00");  /* VRM sense left */
    applesmc_add_key(s, "TVSR", 2, "\x00\x00");  /* VRM sense right */

    /* mos15: Power/platform (12 keys) */
    applesmc_add_key(s, "PC0R", 2, "\x00\x00");  /* CPU 0 rail */
    applesmc_add_key(s, "PCPC", 2, "\x00\x00");  /* CPU package core */
    applesmc_add_key(s, "PCPG", 2, "\x00\x00");  /* CPU package GPU */
    applesmc_add_key(s, "PCPT", 2, "\x00\x00");  /* CPU package total */
    applesmc_add_key(s, "PfCP", 2, "\x00\x00");  /* platform CPU power */
    applesmc_add_key(s, "PfCT", 2, "\x00\x00");  /* platform CPU temp */
    applesmc_add_key(s, "PfGT", 2, "\x00\x00");  /* platform GPU temp */
    applesmc_add_key(s, "PfHT", 2, "\x00\x00");  /* platform HDD temp */
    applesmc_add_key(s, "PfM0", 2, "\x00\x00");  /* platform memory 0 */
    applesmc_add_key(s, "PfST", 2, "\x00\x00");  /* platform system temp */
    applesmc_add_key(s, "PSTR", 2, "\x00\x00");  /* power supply temp rail */
    applesmc_add_key(s, "PHDC", 2, "\x00\x00");  /* platform HDC */

    /* mos15: Memory/DIMM (6 keys) */
    applesmc_add_key(s, "DM0P", 2, "\x00\x00");  /* DIMM 0 proximity */
    applesmc_add_key(s, "DM0S", 2, "\x00\x00");  /* DIMM 0 sensor */
    applesmc_add_key(s, "DM1P", 2, "\x00\x00");  /* DIMM 1 proximity */
    applesmc_add_key(s, "DM1S", 2, "\x00\x00");  /* DIMM 1 sensor */
    applesmc_add_key(s, "MD1R", 2, "\x00\x00");  /* memory DIMM 1 read */
    applesmc_add_key(s, "MD1W", 2, "\x00\x00");  /* memory DIMM 1 write */

    /* mos15: SMC internal (11 keys) */
    applesmc_add_key(s, "CLKH", 1, "\x00");  /* clock halt */
    applesmc_add_key(s, "DICT", 1, "\x00");  /* dictionary */
    applesmc_add_key(s, "RPlt", 2, "\x00\x00");  /* platform revision */
    applesmc_add_key(s, "SBFL", 1, "\x00");  /* secure boot flags */
    applesmc_add_key(s, "VRTC", 2, "\x00\x00");  /* virtual RTC */
    applesmc_add_key(s, "WKTP", 2, "\x00\x00");  /* wake type */
    applesmc_add_key(s, "zEPD", 1, "\x00");  /* endpoint descriptor */
    applesmc_add_key(s, "cePn", 1, "\x00");  /* CE panel */
    applesmc_add_key(s, "cmDU", 1, "\x00");  /* cm display unit */
    applesmc_add_key(s, "maNN", 1, "\x00");  /* manufacturer NN */
    applesmc_add_key(s, "mxT1", 2, "\x00\x00");  /* max temp 1 */

    /* mos15: Sensors/misc (13 keys) */
    applesmc_add_key(s, "MSAc", 1, "\x00");  /* motion sensor active */
    applesmc_add_key(s, "MSAf", 1, "\x00");  /* motion sensor filter */
    applesmc_add_key(s, "MSAg", 1, "\x00");  /* motion sensor gain */
    applesmc_add_key(s, "MSAi", 1, "\x00");  /* motion sensor interval */
    applesmc_add_key(s, "MSGA", 1, "\x00");  /* MSG A */
    applesmc_add_key(s, "MSHP", 1, "\x00");  /* MS HP */
    applesmc_add_key(s, "MSPA", 1, "\x00");  /* MS PA */
    applesmc_add_key(s, "MTLV", 1, "\x00");  /* MT level */
    applesmc_add_key(s, "QCLV", 1, "\x00");  /* Q clevel */
    applesmc_add_key(s, "QENA", 1, "\x00");  /* Q enable */
    applesmc_add_key(s, "WIr0", 2, "\x00\x00");  /* WiFi rate 0 */
    applesmc_add_key(s, "WIw0", 2, "\x00\x00");  /* WiFi write 0 */
    applesmc_add_key(s, "WIz0", 2, "\x00\x00");  /* WiFi zone 0 */

    /* mos15: Write targets (also need to be readable) */
    applesmc_add_key(s, "HE0N", 1, "\x00");  /* iGPU power envelope */
    applesmc_add_key(s, "MSDW", 1, "\x00");  /* MSD write */
    applesmc_add_key(s, "NTOK", 1, "\x00");  /* notification token */

    /* mos15: Key count — must match actual number of keys above */
    applesmc_add_key(s, "$Num", 4, "\x00\x00\x00\x5e"); /* 94 keys */
}

static void applesmc_unrealize(DeviceState *dev)
{
    AppleSMCState *s = APPLE_SMC(dev);
    struct AppleSMCData *d, *next;

    /* Remove existing entries */
    QLIST_FOREACH_SAFE(d, &s->data_def, node, next) {
        QLIST_REMOVE(d, node);
        g_free(d);
    }
}

static const Property applesmc_isa_properties[] = {
    DEFINE_PROP_UINT32(APPLESMC_PROP_IO_BASE, AppleSMCState, iobase,
                       APPLESMC_DEFAULT_IOBASE),
    DEFINE_PROP_STRING("osk", AppleSMCState, osk),
};

static void build_applesmc_aml(AcpiDevAmlIf *adev, Aml *scope)
{
    Aml *crs;
    AppleSMCState *s = APPLE_SMC(adev);
    uint32_t iobase = s->iobase;
    Aml *dev = aml_device("SMC");

    aml_append(dev, aml_name_decl("_HID", aml_eisaid("APP0001")));
    /* device present, functioning, decoding, not shown in UI */
    aml_append(dev, aml_name_decl("_STA", aml_int(0xB)));
    crs = aml_resource_template();
    aml_append(crs,
        aml_io(AML_DECODE16, iobase, iobase, 0x01, APPLESMC_MAX_DATA_LENGTH)
    );
    aml_append(crs, aml_irq_no_flags(6));
    aml_append(dev, aml_name_decl("_CRS", crs));
    aml_append(scope, dev);
}

static void qdev_applesmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AcpiDevAmlIfClass *adevc = ACPI_DEV_AML_IF_CLASS(klass);

    dc->realize = applesmc_isa_realize;
    dc->unrealize = applesmc_unrealize;
    device_class_set_legacy_reset(dc, qdev_applesmc_isa_reset);
    device_class_set_props(dc, applesmc_isa_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    adevc->build_dev_aml = build_applesmc_aml;
}

static const TypeInfo applesmc_isa_info = {
    .name          = TYPE_APPLE_SMC,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(AppleSMCState),
    .class_init    = qdev_applesmc_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_ACPI_DEV_AML_IF },
        { },
    },
};

static void applesmc_register_types(void)
{
    type_register_static(&applesmc_isa_info);
}

type_init(applesmc_register_types)
