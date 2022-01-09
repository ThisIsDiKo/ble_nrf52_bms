/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <zephyr.h>
#include <drivers/gpio.h>
#include <soc.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <bluetooth/services/bms.h>

#include <settings/settings.h>

#include <dk_buttons_and_leds.h>
#include <logging/log.h>

#include "remote.h"

#define LOG_MODULE_NAME app
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)


#define RUN_STATUS_LED          DK_LED1
#define CON_STATUS_LED          DK_LED2
#define RUN_LED_BLINK_INTERVAL  1000

/* Authorization code for Bond Management Service
 * In hex: {0x41, 0x42, 0x43, 0x44}
 */

static uint8_t numberOfbondedDevices = 0;
static const uint8_t bms_auth_code[] = {'A', 'B', 'C', 'D'};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

// static const struct bt_data sd[] = {
// 	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x1e, 0x18),
// };

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_REMOTE_SERV_VAL),
};

/* Declarations */
static struct bt_conn *current_conn;

void on_notif_changed(enum bt_button_notifications_enabled status);
void on_data_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len);

struct bt_remote_service_cb remote_callbacks = {
	.notif_changed = on_notif_changed,
	.data_received = on_data_received,
};

void on_data_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len){
	uint8_t temp_str[len+1];
	memcpy(temp_str, data, len);
	temp_str[len] = 0x00;

	printk("Received data on conn %p. Len: %d", (void *)conn, len);
	printk("Data: %s", log_strdup(temp_str));
}

void on_notif_changed(enum bt_button_notifications_enabled status)
{
	if (status == BT_BUTTON_NOTIFICATIONS_ENABLED) {
		printk("Notifications enabled");
	}
	else {
		printk("Notificatons disabled");
	}
}

void button_handler(uint32_t button_state, uint32_t has_changed)
{
	int err = 0;
	int button_pressed = 0;
	if (has_changed & button_state)
	{
		switch (has_changed)
		{
			case DK_BTN1_MSK:
				button_pressed = 1;
				break;
			case DK_BTN2_MSK:
				button_pressed = 2;
				break;
			case DK_BTN3_MSK:
				button_pressed = 3;
				break;
			case DK_BTN4_MSK:
				button_pressed = 4;
				break;
			default:
				break;
		}
		printk("Button %d pressed\n", button_pressed);
		set_button_value(button_pressed);
		err = send_button_notification(current_conn, button_pressed);
		if (err) {
			LOG_ERR("couldn't send notification (err: %d)", err);
		}
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err %u)\n", err);
		return;
	}

	printk("Connected\n");
	current_conn = bt_conn_ref(conn);
	dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %u)\n", reason);

	dk_set_led_off(CON_STATUS_LED);
	if (current_conn){
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
	} else {
		printk("Security failed: %s level %u err %d\n", addr, level,
			err);
	}
}

static struct bt_conn_cb conn_callbacks = {
	.connected        = connected,
	.disconnected     = disconnected,
	.security_changed = security_changed,
};


static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}

static void pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	bt_conn_auth_pairing_confirm(conn);

	printk("Pairing confirmed: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d\n", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.cancel = auth_cancel,
	.pairing_confirm = pairing_confirm,
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};

static bool bms_authorize(struct bt_conn *conn,
			  struct bt_bms_authorize_params *params)
{
	if ((params->code_len == sizeof(bms_auth_code)) &&
	    (memcmp(bms_auth_code, params->code, sizeof(bms_auth_code)) == 0)) {
		printk("Authorization of BMS operation is successful\n");
		return true;
	}

	printk("Authorization of BMS operation has failed\n");
	return false;
}

static struct bt_bms_cb bms_callbacks = {
	.authorize = bms_authorize,
};

static int bms_init(void)
{
	struct bt_bms_init_params init_params = {0};

	/* Enable all possible operation codes */
	init_params.features.delete_requesting.supported = true;
	init_params.features.delete_rest.supported = true;
	init_params.features.delete_all.supported = true;

	/* Require authorization code for operations that
	 * also delete bonding information for other devices
	 * than the requesting client.
	 */
	init_params.features.delete_rest.authorize = true;
	init_params.features.delete_all.authorize = true;

	init_params.cbs = &bms_callbacks;

	return bt_bms_init(&init_params);
}

void print_bonded_device_info(const struct bt_bond_info *info, void *user_data){
	numberOfbondedDevices += 1;
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));

	printk("Device in bond list: %s", addr);

	int err = bt_le_whitelist_add(&info->addr);
	if (err){
		printk("whitelist add: %s FAILED!\n", addr);
		//error(99);
	}else{
		printk("whitelist add: %s\n", addr);
	}
}

void main(void)
{
	int blink_status = 0;
	int err;

	printk("Starting Bluetooth Peripheral BMS example\n");

	err = dk_buttons_init(button_handler);
	if (err)
	{
		LOG_ERR("Cannot init buttons (err: %d)", err);
	}

	err = dk_leds_init();
	if (err) {
		printk("LEDs init failed (err %d)\n", err);
		return;
	}

	bt_conn_cb_register(&conn_callbacks);
	bt_conn_auth_cb_register(&conn_auth_callbacks);
	ble_cb_custom_init(&remote_callbacks);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		printk("Settings available\n");
		settings_load();
	}
	else{
		printk("Settings not available\n");
	}

	err = bms_init();
	if (err) {
		printk("Failed to init BMS (err:%d)\n", err);
		return;
	}

	/* show all bonded devices*/
	numberOfbondedDevices = 0;
	bt_foreach_bond(BT_ID_DEFAULT, print_bonded_device_info, NULL);

	//bt_le_whitelist_add()
	if (numberOfbondedDevices == 0){
	//if (true){
		err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
			      							  sd, ARRAY_SIZE(sd));
	}
	else{
		err = bt_le_adv_start(BT_LE_ADV_PARAM(
											BT_LE_ADV_OPT_CONNECTABLE|BT_LE_ADV_OPT_FILTER_CONN|BT_LE_ADV_OPT_FILTER_SCAN_REQ,
											160, 	// units of 0.625ms
											1600,	// units of 0.625ms
											NULL), // no peer address
						ad, ARRAY_SIZE(ad),
						sd, ARRAY_SIZE(sd));
	}
	
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	printk("Advertising successfully started\n");

	for (;;) {
		dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
	}
}
