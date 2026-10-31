#ifndef WIFI_USB_TEST_H
#define WIFI_USB_TEST_H

#include <stdbool.h>

/* Only built into temporary firmware with CONFIG_HELLO_BOX_USB_TEST_HELPER. */
bool wifi_usb_test_start(void);
bool wifi_manager_request_usb_test_info(void);

#endif
