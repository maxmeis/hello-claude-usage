/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from sensecap-sdk/examples/squareline_demo/main/lv_port.h.
 */
#ifndef LV_PORT_H
#define LV_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lv_port_init(void);
void lv_port_set_brightness(uint8_t percent);
void lv_port_sem_take(void);
void lv_port_sem_give(void);

#ifdef __cplusplus
}
#endif

#endif
