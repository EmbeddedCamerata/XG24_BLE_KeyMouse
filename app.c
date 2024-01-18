/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "event_groups.h"
#include "em_common.h"
#include "app.h"
#include "app_log.h"
#include "app_assert.h"
#include "app_timer.h"
#include "gatt_db.h"
#include "sl_bluetooth.h"
#include "sl_simple_button_instances.h"
#include "sl_simple_led_instances.h"

#define KM_BTN_TASK_NAME        "keymouse_btn"
#define KM_BTN_TASK_STACK_SIZE  1024
#define KM_BTN_TASK_STATIC      0

#define REPORT_ID_INDEX         0
#define KB_REPORT_ID            0x01
#define MOUSE_REPORT_ID         0x02
#define MODIFIER_INDEX          1
#define DATA_INDEX              3
#define WHEEL_INDEX             4
#define LSHIFT_KEY_OFF          0x00
#define LSHIFT_KEY_ON           0x02

#define BTN0_PRESSED            (1 << 0)
#define BTN1_PRESSED            (1 << 1)
#define BTN_NONE_PRESSED        0
#define BTN_BOTH_PRESSED        (BTN0_PRESSED | BTN1_PRESSED)

#if (KM_BTN_TASK_STATIC == 1)
StackType_t km_btn_task_stack[BTN_PRESS_TASK_STACK_SIZE];
StaticTask_t km_btn_task_handle;
#else // configSUPPORT_STATIC_ALLOCATION
TaskHandle_t km_btn_task_handle = NULL;
#endif // configSUPPORT_STATIC_ALLOCATION

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;
static uint8_t notification_enabled = 0;
static uint8_t kb_report_data[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static uint8_t mouse_report_data[] = { 0, 0, 0, 0, 0 };
static km_status_t km_status = KM_IDLE;
static EventGroupHandle_t xbtn_events = NULL;

static void km_btn_task(void *p_arg);
static void btn_press_timer_cb(app_timer_t *timer, void *data);
void send_eetree_string();
void scroll_with_distance(uint8_t distance);

/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
SL_WEAK void app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
  xbtn_events = xEventGroupCreate();
  if (xbtn_events == NULL) {
    app_log_error("BTN events create failed!\r\n");
  }

#if (KM_BTN_TASK_STATIC == 1)
  xTaskCreateStatic(km_btn_task,
                    KM_BTN_TASK_NAME,
                    configMINIMAL_STACK_SIZE,
                    NULL,
                    tskIDLE_PRIORITY,
                    km_btn_task_stack,
                    &km_btn_task_handle);
#else // configSUPPORT_STATIC_ALLOCATION
  xTaskCreate(km_btn_task,
              KM_BTN_TASK_NAME,
              configMINIMAL_STACK_SIZE,
              NULL,
              tskIDLE_PRIORITY,
              &km_btn_task_handle);
#endif // configSUPPORT_STATIC_ALLOCATION
}

/**************************************************************************//**
 * Application Process Action.
 *****************************************************************************/
SL_WEAK void app_process_action(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application code here!                              //
  // This is called infinitely.                                              //
  // Do not call blocking functions from here!                               //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the dummy weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      app_assert_status(sc);

      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Set advertising interval to 100ms.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        160, // min. adv. interval (milliseconds * 1.6)
        160, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration
        0);  // max. num. adv. events
      app_assert_status(sc);

      app_log_info("boot event - starting advertising\r\n");

      sc = sl_bt_sm_configure(0, sl_bt_sm_io_capability_noinputnooutput);
      app_assert_status(sc);
      sc = sl_bt_sm_set_bondable_mode(1);
      app_assert_status(sc);

      // Start advertising and enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      app_assert_status(sc);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      app_log_info("connection opened\r\n");
      sc =
        sl_bt_sm_increase_security(evt->data.evt_connection_opened.connection);
      app_assert_status(sc);
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      app_log_info("connection closed, reason: 0x%2.2x\r\n",
              evt->data.evt_connection_closed.reason);
      notification_enabled = 0;
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Restart advertising after client has disconnected.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      app_assert_status(sc);
      break;

    case sl_bt_evt_sm_bonded_id:
      app_log_info("successful bonding\r\n");
      break;

    case sl_bt_evt_sm_bonding_failed_id:
      app_log_error("bonding failed, reason 0x%2X\r\n",
              evt->data.evt_sm_bonding_failed.reason);

      /* Previous bond is broken, delete it and close connection,
       *  host must retry at least once */
      sc = sl_bt_sm_delete_bondings();
      app_assert_status(sc);
      sc = sl_bt_connection_close(evt->data.evt_sm_bonding_failed.connection);
      app_assert_status(sc);
      break;

    case sl_bt_evt_gatt_server_characteristic_status_id:
      if (evt->data.evt_gatt_server_characteristic_status.characteristic
          == gattdb_report) {
        // client characteristic configuration changed by remote GATT client
        if (evt->data.evt_gatt_server_characteristic_status.status_flags
            == sl_bt_gatt_server_client_config) {
          if (evt->data.evt_gatt_server_characteristic_status.
              client_config_flags == sl_bt_gatt_notification) {
            notification_enabled = 1;
          } else {
            notification_enabled = 0;
          }
        }
      }
      break;

    case  sl_bt_evt_system_external_signal_id:
      if (notification_enabled == 1 && km_status != KM_IDLE) {
        if (km_status == KM_SEND_STRING) {
          send_eetree_string();
        }
        else if (km_status == KM_SCROLL_UP) {
          scroll_with_distance(0x01);
        }
        else { // KM_SCROLL_DOWN
          scroll_with_distance(0xFF);
        }
        app_log_info("Key report %d was sent\r\n", km_status);
        km_status = KM_IDLE;
      }
      break;

    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}

void sl_button_on_change(const sl_button_t *handle)
{
  BaseType_t xHigherPriorityTaskWoken, xResult;

  if (&sl_button_btn0 == handle) {
    if (sl_button_get_state(handle) == SL_SIMPLE_BUTTON_PRESSED) {
      xResult = xEventGroupSetBitsFromISR(xbtn_events, BTN0_PRESSED, &xHigherPriorityTaskWoken);
      if (xResult == pdPASS) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
      }
      else {
        app_log_error("Set BTN0_PRESSED event failed\r\n");
      }
    }
    else {
      xResult = xEventGroupClearBitsFromISR(xbtn_events, BTN0_PRESSED);
      if (xResult == pdFAIL) {
        app_log_error("Clear BTN0_PRESSED event failed\r\n");
      }
    }
  }

  if (&sl_button_btn1 == handle) {
    if (sl_button_get_state(handle) == SL_SIMPLE_BUTTON_PRESSED) {
      xResult = xEventGroupSetBitsFromISR(xbtn_events, BTN1_PRESSED, &xHigherPriorityTaskWoken);
      if (xResult == pdPASS) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
      }
      else {
        app_log_error("Set BTN1_PRESSED event failed\r\n");
      }
    }
    else {
      xResult = xEventGroupClearBitsFromISR(xbtn_events, BTN1_PRESSED);
      if (xResult == pdFAIL) {
        app_log_error("Clear BTN1_PRESSED event failed\r\n");
      }
    }
  }
}

static void km_btn_task(void *p_arg)
{
  (void)p_arg;
  app_timer_t btn_press_timer;
  sl_status_t sc;
  bool is_running = false;
  EventBits_t btn_events;

  while (1) {
    btn_events = xEventGroupGetBits(xbtn_events);

    switch (btn_events) {
      case (BTN_BOTH_PRESSED):
        app_log_debug("BTN0 & BTN1 pressed\r\n");

        if (!is_running) {
          app_log_debug("Timer started\r\n");

          sc = app_timer_start(&btn_press_timer, 2000, btn_press_timer_cb, NULL, false);
          app_assert_status(sc);
          is_running = true;
        }
        break;

      case (BTN0_PRESSED):
        app_log_debug("BTN0 pressed\r\n");

        sc = app_timer_stop(&btn_press_timer);
        app_assert_status(sc);

        km_status = KM_SCROLL_UP; // scroll up
        sl_bt_external_signal(1);
        break;

      case (BTN1_PRESSED):
        app_log_debug("BTN1 pressed\r\n");

        sc = app_timer_stop(&btn_press_timer);
        app_assert_status(sc);

        km_status = KM_SCROLL_DOWN; // scroll down
        sl_bt_external_signal(1);
        break;

      default: // BTN_NONE_PRESSED
        sc = app_timer_stop(&btn_press_timer);
        app_assert_status(sc);
        is_running = false;
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  vTaskDelete(NULL);
}

void scroll_with_distance(uint8_t distance)
{
  sl_status_t sc;

  memset(mouse_report_data, 0, sizeof(mouse_report_data));

  mouse_report_data[REPORT_ID_INDEX] = MOUSE_REPORT_ID;
  mouse_report_data[WHEEL_INDEX] = distance;

  sc = sl_bt_gatt_server_notify_all(gattdb_report,
                                    sizeof(mouse_report_data),
                                    mouse_report_data);
  app_assert_status(sc);
}

void send_keyboard(uint8_t caps_key, uint8_t c)
{
  sl_status_t sc;

  memset(kb_report_data, 0, sizeof(kb_report_data));
  kb_report_data[REPORT_ID_INDEX] = KB_REPORT_ID;
  kb_report_data[MODIFIER_INDEX] = caps_key;
  kb_report_data[DATA_INDEX] = c;

  sc = sl_bt_gatt_server_notify_all(gattdb_report,
                                    sizeof(kb_report_data),
                                    kb_report_data);
  app_assert_status(sc);

  memset(kb_report_data, 0, sizeof(kb_report_data));
  kb_report_data[REPORT_ID_INDEX] = KB_REPORT_ID;

  sc = sl_bt_gatt_server_notify_all(gattdb_report,
                                    sizeof(kb_report_data),
                                    kb_report_data);
  app_assert_status(sc);
  sl_sleeptimer_delay_millisecond(20);
}

void send_eetree_string()
{
  send_keyboard(LSHIFT_KEY_ON, 0x08); // E
  send_keyboard(LSHIFT_KEY_ON, 0x08); // E
  send_keyboard(LSHIFT_KEY_ON, 0x17); // T
  send_keyboard(LSHIFT_KEY_ON, 0x15); // R
  send_keyboard(LSHIFT_KEY_ON, 0x08); // E
  send_keyboard(LSHIFT_KEY_ON, 0x08); // E
  send_keyboard(LSHIFT_KEY_OFF,0x37); // .
  send_keyboard(LSHIFT_KEY_ON, 0x06); // C
  send_keyboard(LSHIFT_KEY_ON, 0x11); // N
}

static void btn_press_timer_cb(app_timer_t *timer, void *data)
{
  (void)data;
  (void)timer;
  BaseType_t xResult;

  xResult = xEventGroupClearBitsFromISR(xbtn_events, BTN_BOTH_PRESSED);
  if (xResult == pdFAIL) {
    app_log_error("Clear BTN_BOTH_PRESSED event failed\r\n");
  }

  km_status = KM_SEND_STRING;

  sl_led_toggle(&sl_led_led0);
  sl_led_toggle(&sl_led_led1);

  sl_bt_external_signal(1);

  app_log_debug("Timer stopped\r\n");
}
