#include "sdkconfig.h"

#if CONFIG_HELLO_BOX_USB_TEST_HELPER
#include "wifi_usb_test.h"

#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define USB_TEST_COMMAND "HELLO_BOX_SETUP_INFO"

static void usb_test_reader(void *arg)
{
    (void)arg;
    char line[sizeof(USB_TEST_COMMAND)];
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\n') {
            if (!overflow) {
                line[length] = 0;
                if (strcmp(line, USB_TEST_COMMAND) == 0)
                    (void)wifi_manager_request_usb_test_info();
            }
            length = 0;
            overflow = false;
        } else {
            if (length + 1 < sizeof(line)) line[length++] = ch;
            else overflow = true;
        }
    }
}

bool wifi_usb_test_start(void)
{
    return xTaskCreate(usb_test_reader, "usb_test", 2048, NULL, 2, NULL) == pdPASS;
}
#endif
