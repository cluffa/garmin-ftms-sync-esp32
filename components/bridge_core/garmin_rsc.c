/*
 * garmin_rsc.c — BLE peripheral: GATT server for Running Speed & Cadence
 * service (RSC, 0x1814).  Advertises as a connectable running sensor so a
 * Garmin watch (or any RSC-compatible central) can pair and receive pace/
 * distance measurements.
 *
 * Architecture (bleprph example pattern):
 *   garmin_rsc_start()
 *     → ble_gatts_count_cfg / ble_gatts_add_svcs  (register GATT table)
 *     → ble_svc_gap_device_name_set               (set device name)
 *     → start_advertising()                        (begin connectable ADV)
 *
 *   GAP callback:
 *     BLE_GAP_EVENT_CONNECT    → record conn handle, stop adv
 *     BLE_GAP_EVENT_DISCONNECT → clear conn handle, re-advertise
 *     BLE_GAP_EVENT_SUBSCRIBE  → record subscription state
 *
 *   garmin_rsc_update(s)
 *     → rsc_encode_measurement() → ble_hs_mbuf_from_flat()
 *     → ble_gatts_notify_custom()
 */

#include "garmin_rsc.h"
#include "rsc_encode.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_log.h"

#include <string.h>
#include <stdint.h>

#define TAG              "garmin_rsc"
#define RSC_SVC_UUID     0x1814
#define RSC_MEAS_UUID    0x2A53
#define RSC_FEATURE_UUID 0x2A54
#define BATT_SVC_UUID    0x180F   /* standard Battery Service */
#define BATT_LVL_UUID    0x2A19   /* Battery Level: 1 byte, 0..100 % */

/* RSC Feature bitfield: bit 1 = Total Distance Measurement Supported,
 * bit 2 = Walking or Running Status Supported (we set the running-status flag
 * in every measurement, so it must be declared as supported here). */
#define RSC_FEATURE_BITS 0x0006u

/* Maximum RSC Measurement payload (rsc_encode_measurement always returns 8) */
#define RSC_MEAS_BUF_LEN 16

/* ---- module-level state ------------------------------------------------- */

static uint8_t  s_own_addr_type = BLE_OWN_ADDR_RANDOM; /* set by setter */
static uint16_t s_meas_handle   = 0;
static uint16_t s_batt_handle   = 0;
static uint16_t s_conn          = BLE_HS_CONN_HANDLE_NONE;
static bool     s_subscribed    = false;
static bool     s_batt_subscribed = false;
static uint8_t  s_batt_pct      = 0;
static bool     s_advertising   = false;

/* ---- GATT characteristic access callbacks -------------------------------- */

/* Stub access callback for the notify-only RSC Measurement characteristic.
 * Reads and writes are not supported; the value is pushed via notify. */
static int rsc_meas_access(uint16_t conn_handle,
                            uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt,
                            void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int rsc_feature_access(uint16_t conn_handle,
                               uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t feature = RSC_FEATURE_BITS;
    /* little-endian 2-byte value */
    uint8_t buf[2] = { (uint8_t)(feature & 0xFF),
                       (uint8_t)((feature >> 8) & 0xFF) };
    int rc = os_mbuf_append(ctxt->om, buf, sizeof buf);
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Battery Level — readable + notify; returns the cached SoC %. */
static int batt_lvl_access(uint16_t conn_handle,
                           uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt,
                           void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t v = s_batt_pct;
    int rc = os_mbuf_append(ctxt->om, &v, 1);
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- GATT service table ------------------------------------------------- */

static const struct ble_gatt_svc_def s_rsc_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(RSC_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RSC Measurement — notify only; value handle stored */
                .uuid       = BLE_UUID16_DECLARE(RSC_MEAS_UUID),
                .access_cb  = rsc_meas_access,
                .val_handle = &s_meas_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                /* RSC Feature — readable 2-byte bitfield */
                .uuid      = BLE_UUID16_DECLARE(RSC_FEATURE_UUID),
                .access_cb = rsc_feature_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            { 0 },  /* terminator */
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BATT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Battery Level — read + notify; value handle stored */
                .uuid       = BLE_UUID16_DECLARE(BATT_LVL_UUID),
                .access_cb  = batt_lvl_access,
                .val_handle = &s_batt_handle,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },  /* terminator */
        },
    },
    { 0 },  /* terminator */
};

/* ---- Forward declarations ----------------------------------------------- */

static int garmin_rsc_gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---- Advertising -------------------------------------------------------- */

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode  = BLE_GAP_CONN_MODE_UND,
        .disc_mode  = BLE_GAP_DISC_MODE_GEN,
        .itvl_min   = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max   = BLE_GAP_ADV_ITVL_MS(200),
    };

    /* Build advertisement data with:
     *   - Flags (LE General Discoverable | BR/EDR not supported)
     *   - Complete 16-bit Service UUID list (RSC = 0x1814)
     *   - Complete Local Name                                          */
    struct ble_hs_adv_fields fields = { 0 };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    ble_uuid16_t svc_uuid = BLE_UUID16_INIT(RSC_SVC_UUID);
    fields.uuids16         = &svc_uuid;
    fields.num_uuids16     = 1;
    fields.uuids16_is_complete = 1;

    /* Name goes into the scan-response to keep the adv packet small. */
    struct ble_hs_adv_fields rsp = { 0 };
    const char *name = ble_svc_gap_device_name();
    rsp.name               = (const uint8_t *)name;
    rsp.name_len           = (uint8_t)strlen(name);
    rsp.name_is_complete   = 1;

    int rc;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    s_advertising = false;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, garmin_rsc_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        s_advertising = true;
        ESP_LOGI(TAG, "advertising as RSC running sensor…");
    }
}

/* ---- GAP event callback ------------------------------------------------- */

static int garmin_rsc_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connection failed, status=%d", event->connect.status);
            start_advertising();
            break;
        }
        s_advertising = false;   /* controller stops adv on connect */
        /* Keep the FIRST central as the RSC/battery subscriber; a SECOND central
         * (e.g. the CIQ control client in the dual-connection spike) may connect
         * too but must not steal the notify target. */
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            s_conn       = event->connect.conn_handle;
            s_subscribed = false;
            s_batt_subscribed = false;
            ESP_LOGI(TAG, "central connected, conn_handle=%d", s_conn);
        } else {
            ESP_LOGI(TAG, "second central connected, conn_handle=%d",
                     event->connect.conn_handle);
        }
        start_advertising();
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected, conn=%d reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        if (event->disconnect.conn.conn_handle == s_conn) {
            s_conn       = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed = false;
            s_batt_subscribed = false;
        }
        if (!s_advertising) start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_meas_handle) {
            s_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "RSC Measurement %s",
                     s_subscribed ? "subscribed" : "unsubscribed");
        } else if (event->subscribe.attr_handle == s_batt_handle) {
            s_batt_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "Battery Level %s",
                     s_batt_subscribed ? "subscribed" : "unsubscribed");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGD(TAG, "MTU updated: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ---- Public API --------------------------------------------------------- */

void garmin_rsc_set_addr_type(uint8_t addr_type)
{
    s_own_addr_type = addr_type;
}

/*
 * garmin_rsc_register_gatt() — registers the RSC GATT service and sets the
 * device name.  MUST be called from the host task BEFORE nimble_port_run(),
 * because the host builds and starts the attribute table at startup; services
 * added later (e.g. from the sync callback) are not included in that table and
 * stay invisible to the central.
 */
void garmin_rsc_register_gatt(void)
{
    /* Set device name visible in the Garmin "Add Sensor" screen. */
    int rc = ble_svc_gap_device_name_set("garmin-ftms-sync");
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
    }

    rc = ble_gatts_count_cfg(s_rsc_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(s_rsc_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "RSC GATT service registered");
}

/*
 * garmin_rsc_start() — begins connectable advertising.  Call this from
 * on_host_sync() (after the host has synchronised and the address type is set);
 * the GATT service itself must already have been registered via
 * garmin_rsc_register_gatt() before the host started.
 */
void garmin_rsc_start(void)
{
    ESP_LOGI(TAG, "RSC ready (meas_handle=%d)", s_meas_handle);
    start_advertising();
}

void garmin_rsc_update(const treadmill_state_t *s)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;  /* no subscriber — silent no-op */
    }

    uint8_t buf[RSC_MEAS_BUF_LEN];
    size_t  len = rsc_encode_measurement(s, buf, sizeof buf);
    if (len == 0) {
        ESP_LOGW(TAG, "rsc_encode_measurement returned 0");
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
    if (om == NULL) {
        ESP_LOGE(TAG, "ble_hs_mbuf_from_flat: OOM");
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn, s_meas_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_notify_custom failed: %d", rc);
    }
}

void garmin_rsc_update_battery(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct == s_batt_pct) return;   /* unchanged — skip the notify */
    s_batt_pct = pct;

    if (!s_batt_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;  /* cached for the next READ; no subscriber to notify */
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&pct, 1);
    if (om == NULL) {
        ESP_LOGE(TAG, "battery notify: OOM");
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn, s_batt_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "battery notify failed: %d", rc);
    }
}

bool garmin_rsc_subscribed(void) { return s_subscribed; }
bool garmin_rsc_advertising(void) { return s_advertising; }
