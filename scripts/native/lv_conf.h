#ifndef LV_CONF_H
#define LV_CONF_H

/* Host capture configuration matching the device's RGB565 LVGL 8 build. */
#define LV_COLOR_DEPTH 16
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (256U * 1024U)
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_DPI_DEF 160
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16
#define LV_USE_QRCODE 1
#define LV_USE_THEME_DEFAULT 1

#endif
