/*
 * machine_ftms.c — BLE central: scan for FTMS treadmills, collect a device
 * list with RSSI, then connect to a chosen device on command.
 *
 * Scan-collect phase (machine_ftms_start_scan):
 *   ble_gap_disc → gap_event_cb(DISC) → adv_has_ftms → ftms_devlist_upsert
 *
 * Connect phase (machine_ftms_connect):
 *   ble_gap_disc_cancel + ble_gap_connect
 *   → BLE_GAP_EVENT_CONNECT → ble_gattc_disc_svc_by_uuid(0x1826)
 *   → svc_disc_cb → ble_gattc_disc_chrs_by_uuid(0x2ACD)
 *   → chr_disc_cb → ble_gattc_disc_all_dscs → dsc_disc_cb → write CCCD 0x0001
 *   → BLE_GAP_EVENT_NOTIFY_RX → parse → invoke stored callback
 *   BLE_GAP_EVENT_DISCONNECT → machine_ftms_try_last (one reconnect attempt,
 *     falling back to scan on failure)
 *
 * The last successfully-connected device is persisted in NVS and retried on
 * boot via machine_ftms_try_last().
 */

#include "machine_ftms.h"
#include "ftms_parse.h"
#include "ftms_devlist.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/ble.h"

#include "esp_log.h"

#include <string.h>
#include <stdint.h>

#define TAG            "machine_ftms"
#define FTMS_SVC_UUID  0x1826
#define TREADMILL_CHR  0x2ACD
#define FTMS_CP_CHR    0x2AD9   /* Fitness Machine Control Point */

/* HCI error code for "remote user terminated connection" — defined only in
 * the NimBLE private header; use the raw value to stay in the public API. */
#ifndef BLE_ERR_REM_USER_CONN_TERM
#define BLE_ERR_REM_USER_CONN_TERM  0x13
#endif

/* Maximum expected notification payload for Treadmill Data. The FTMS spec
 * allows up to ~34 bytes; 64 gives comfortable headroom. */
#define NOTIF_BUF_SIZE 64

/* ---- module-level state ------------------------------------------------- */

static machine_state_cb s_cb;
static uint8_t          s_own_addr_type = BLE_OWN_ADDR_RANDOM;
static uint16_t         s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t         s_data_handle   = 0;   /* Treadmill Data attr handle  */
static uint16_t         s_cp_handle     = 0;   /* Control Point attr handle   */
static uint16_t         s_svc_start     = 0;   /* FTMS service start handle   */
static uint16_t         s_svc_end       = 0;   /* FTMS service end handle     */
static bool             s_connecting    = false; /* true while connect pending */

/* Device list populated during scan */
static ftms_device_t    s_devs[FTMS_MAX_DEVICES];
static int              s_ndev = 0;

/* The device the current connect attempt targets. */
static ftms_device_t    s_target;

/* Connect/disconnect events go to the facade (machine.c), which owns reconnect
 * + persistence so the same policy works for FTMS and iFit. */
static machine_event_cb s_evt_cb;
void machine_ftms_set_event_cb(machine_event_cb cb) { s_evt_cb = cb; }

/* Advert classifier, used by the facade's unified scan. */
bool machine_ftms_is_ftms_adv(const uint8_t *data, uint8_t len);

/* ---- forward declarations ----------------------------------------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---- helpers ------------------------------------------------------------ */

/* Parse the short/complete local name AD structures (type 0x08 / 0x09) from a
 * raw advertisement payload.  Writes a NUL-terminated string into out[0..outlen-1].
 * Writes an empty string if no name AD is found. */
static void adv_name(const uint8_t *data, uint8_t len, char *out, int outlen)
{
    out[0] = '\0';
    uint8_t i = 0;
    while (i < len) {
        uint8_t l = data[i];
        if (l < 1 || (uint16_t)i + 1u + l > len) break;
        uint8_t t = data[i + 1];
        if (t == 0x08 || t == 0x09) {
            int nl = l - 1;
            if (nl > outlen - 1) nl = outlen - 1;
            memcpy(out, &data[i + 2], nl);
            out[nl] = '\0';
            return;
        }
        i += (uint8_t)(l + 1);
    }
}

/* Check whether a raw advertisement payload contains the FTMS service UUID
 * (0x1826) in Complete or Incomplete 16-bit UUIDs (AD type 0x02 or 0x03). */
static bool adv_has_ftms(const uint8_t *data, uint8_t data_len)
{
    uint8_t i = 0;
    while (i < data_len) {
        uint8_t len = data[i];
        /* An AD structure is: 1 length byte + len bytes of payload.
         * It is valid when len >= 1 and all bytes fit: i + 1 + len <= data_len. */
        if (len < 1 || (uint16_t)i + 1u + len > data_len) break;
        uint8_t type = data[i + 1];
        if (type == 0x02 || type == 0x03) {
            /* payload is pairs of little-endian 16-bit UUIDs;
             * payload starts at i+2, length is (len - 1) bytes */
            for (uint8_t j = 2; (uint16_t)j + 1u < (uint16_t)1u + len; j += 2) {
                uint16_t uuid = (uint16_t)(data[i + j]) |
                                ((uint16_t)(data[i + j + 1]) << 8);
                if (uuid == FTMS_SVC_UUID) return true;
            }
        }
        i += (uint8_t)(len + 1);
    }
    return false;
}

/* ---- CCCD write --------------------------------------------------------- */

static int cccd_write_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr,
                         void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "CCCD enabled — notifications active on handle %d",
                 s_data_handle);
    } else {
        ESP_LOGE(TAG, "CCCD write failed: %d", error->status);
    }
    return 0;
}

static void enable_notifications(uint16_t conn_handle, uint16_t cccd_handle)
{
    uint8_t val[2] = {0x01, 0x00};  /* GATT_CCC_NOTIFY */
    int rc = ble_gattc_write_flat(conn_handle, cccd_handle,
                                  val, sizeof val, cccd_write_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_flat (CCCD) failed: %d", rc);
    }
}

/* ---- Descriptor discovery (to find the real CCCD handle) --------------- */

static int dsc_disc_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)chr_val_handle; (void)arg;

    if (error->status == BLE_HS_EDONE) {
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "dsc discovery error: %d", error->status);
        return 0;
    }

    /* Match the Client Characteristic Configuration Descriptor (UUID 0x2902). */
    if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        ESP_LOGI(TAG, "CCCD found at handle %d", dsc->handle);
        enable_notifications(conn_handle, dsc->handle);
    }
    return 0;
}

/* ---- Characteristic discovery ------------------------------------------ */

static int chr_disc_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr,
                       void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        if (s_data_handle == 0)
            ESP_LOGW(TAG, "Treadmill Data (0x%04X) not found", TREADMILL_CHR);
        if (s_cp_handle == 0)
            ESP_LOGW(TAG, "FTMS Control Point (0x%04X) not found — writes disabled", FTMS_CP_CHR);
        if (s_data_handle != 0) {
            int rc = ble_gattc_disc_all_dscs(conn_handle,
                                              s_data_handle, s_svc_end,
                                              dsc_disc_cb, NULL);
            if (rc != 0)
                ESP_LOGE(TAG, "ble_gattc_disc_all_dscs failed: %d", rc);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "chr discovery error: %d", error->status);
        return 0;
    }

    if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
        uint16_t uuid16 = chr->uuid.u16.value;
        if (uuid16 == TREADMILL_CHR) {
            s_data_handle = chr->val_handle;
            ESP_LOGI(TAG, "Treadmill Data found: val_handle=%d", s_data_handle);
        } else if (uuid16 == FTMS_CP_CHR) {
            s_cp_handle = chr->val_handle;
            ESP_LOGI(TAG, "FTMS Control Point found: val_handle=%d", s_cp_handle);
        }
    }
    return 0;
}

/* ---- Service discovery -------------------------------------------------- */

static int svc_disc_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc,
                       void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        if (s_svc_start == 0) {
            ESP_LOGW(TAG, "FTMS service (0x%04X) not found", FTMS_SVC_UUID);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "svc discovery error: %d", error->status);
        return 0;
    }

    s_svc_start = svc->start_handle;
    s_svc_end   = svc->end_handle;
    ESP_LOGI(TAG, "FTMS service found: handles %d–%d", s_svc_start, s_svc_end);

    int rc = ble_gattc_disc_all_chrs(conn_handle,
                                      s_svc_start, s_svc_end,
                                      chr_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_chrs_by_uuid failed: %d", rc);
    }
    return 0;
}

/* ---- Notification handler ---------------------------------------------- */

static void handle_notify(struct os_mbuf *om)
{
    uint16_t pkt_len = OS_MBUF_PKTLEN(om);
    uint16_t len = (pkt_len > NOTIF_BUF_SIZE) ? NOTIF_BUF_SIZE : pkt_len;
    uint8_t  buf[NOTIF_BUF_SIZE];

    int rc = os_mbuf_copydata(om, 0, len, buf);
    if (rc != 0) {
        ESP_LOGE(TAG, "os_mbuf_copydata failed: %d", rc);
        return;
    }

    treadmill_state_t state;
    if (ftms_parse_treadmill_data(buf, len, &state)) {
        if (s_cb) s_cb(&state);
    } else {
        ESP_LOGW(TAG, "ftms_parse_treadmill_data returned false (len=%d)", len);
    }
}

/* ---- GAP event callback ------------------------------------------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        /* Ignore advert reports while a connect is in flight (connection-setup
         * window). We DO collect while connected: that only happens when the
         * user opens the menu (start_scan) to switch devices — dropping reports
         * then would leave the menu empty. */
        if (s_connecting) {
            break;
        }

        struct ble_gap_disc_desc *desc = &event->disc;
        if (!adv_has_ftms(desc->data, desc->length_data)) {
            break;
        }

        /* Build a device entry and upsert into the list (update RSSI + name if
         * already present, append if new and there is room). */
        ftms_device_t d;
        d.addr_type = desc->addr.type;
        memcpy(d.addr, desc->addr.val, 6);
        d.rssi = desc->rssi;
        adv_name(desc->data, desc->length_data, d.name, FTMS_NAME_LEN);
        s_ndev = ftms_devlist_upsert(s_devs, s_ndev, &d);
        ESP_LOGD(TAG, "FTMS device upserted: rssi=%d name=%s", d.rssi, d.name);
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect failed, status=%d", event->connect.status);
            if (s_evt_cb) s_evt_cb(0);
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected, conn_handle=%d", s_conn_handle);
        if (s_evt_cb) s_evt_cb(1);   /* facade persists for auto-reconnect */

        ble_uuid16_t ftms_uuid = BLE_UUID16_INIT(FTMS_SVC_UUID);
        int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle,
                                             &ftms_uuid.u,
                                             svc_disc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid failed: %d", rc);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d — reconnecting to last",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_data_handle = 0;
        s_svc_start   = 0;
        s_svc_end     = 0;
        s_connecting  = false;
        if (s_evt_cb) s_evt_cb(0);   /* facade decides reconnect vs scan */
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == s_conn_handle &&
            event->notify_rx.attr_handle == s_data_handle) {
            handle_notify(event->notify_rx.om);
        }
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGD(TAG, "disc complete, reason=%d", event->disc_complete.reason);
        /* Only restart scan on a genuine timeout with no connection pending.
         * If s_connecting is true a ble_gap_connect() call is in flight
         * (triggered by disc_cancel), so restarting here would race it. */
        if (!s_connecting && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            machine_ftms_start_scan();
        }
        break;

    default:
        break;
    }

    return 0;
}

/* ---- Public API --------------------------------------------------------- */

void machine_ftms_set_addr_type(uint8_t addr_type)
{
    s_own_addr_type = addr_type;
}

void machine_ftms_set_data_cb(machine_state_cb cb)
{
    s_cb = cb;
}

void machine_ftms_start_scan(void)
{
    /* Cancel any in-flight reconnect so scanning cleanly overrides it. */
    if (s_connecting) {
        ble_gap_conn_cancel();
        s_connecting = false;
    }
    s_ndev = 0;   /* reset device list */

    struct ble_gap_disc_params disc_params = {
        .filter_duplicates = 0,   /* allow repeated reports to refresh RSSI */
        .passive           = 0,
        .itvl              = 0,
        .window            = 0,
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                          &disc_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "scanning for FTMS treadmills…");
    }
}

int machine_ftms_get_devices(ftms_device_t *out, int max)
{
    int count = s_ndev < max ? s_ndev : max;
    memcpy(out, s_devs, (size_t)count * sizeof(ftms_device_t));
    ftms_devlist_sort(out, count);
    return count;
}

void machine_ftms_connect(const ftms_device_t *dev)
{
    /* Set s_connecting BEFORE cancelling discovery: ble_gap_disc_cancel()
     * fires a synchronous DISC_COMPLETE whose handler would otherwise restart
     * scanning and race the ble_gap_connect() below. */
    s_target = *dev;
    s_connecting = true;
    ble_gap_disc_cancel();

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* Reset per-connection state */
    s_data_handle = 0;
    s_cp_handle   = 0;
    s_svc_start   = 0;
    s_svc_end     = 0;

    ble_addr_t a;
    a.type = dev->addr_type;
    memcpy(a.val, dev->addr, 6);

    int rc = ble_gap_connect(s_own_addr_type, &a, 30000 /* ms */, NULL,
                             gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        s_connecting = false;
        if (s_evt_cb) s_evt_cb(0);
    }
}

bool machine_ftms_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool machine_ftms_connecting(void)
{
    return s_connecting;
}

bool machine_ftms_is_ftms_adv(const uint8_t *data, uint8_t len)
{
    return adv_has_ftms(data, len);
}

int8_t machine_ftms_conn_rssi(void)
{
    int8_t r = 0;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_conn_rssi(s_conn_handle, &r);
    }
    return r;
}

/* ---- FTMS Control Point writes ------------------------------------------ */

/* FTMS CP opcodes */
#define FTMS_OP_SET_SPEED    0x02   /* param: uint16 in 0.01 km/h */
#define FTMS_OP_SET_INCLINE  0x03   /* param: int16  in 0.1 %     */
#define FTMS_OP_STOP         0x08   /* no param                    */

static int cp_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status != 0)
        ESP_LOGW(TAG, "FTMS CP write failed: %d", error->status);
    return 0;
}

static bool cp_write(uint8_t opcode, uint8_t *param, uint8_t param_len)
{
    if (s_cp_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    uint8_t buf[3];
    buf[0] = opcode;
    if (param_len > 0) memcpy(buf + 1, param, param_len);
    int rc = ble_gattc_write_flat(s_conn_handle, s_cp_handle,
                                  buf, 1 + param_len, cp_write_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "ble_gattc_write_flat (CP) failed: %d", rc);
    return rc == 0;
}

bool machine_ftms_set_speed(float kmh)
{
    uint16_t val = (uint16_t)(kmh * 100.0f + 0.5f);
    return cp_write(FTMS_OP_SET_SPEED, (uint8_t *)&val, 2);
}

bool machine_ftms_set_incline(float pct)
{
    int16_t val = (int16_t)(pct * 10.0f);
    return cp_write(FTMS_OP_SET_INCLINE, (uint8_t *)&val, 2);
}

bool machine_ftms_stop(void)
{
    return cp_write(FTMS_OP_STOP, NULL, 0);
}
