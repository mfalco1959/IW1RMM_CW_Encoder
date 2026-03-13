#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_MEM_SIZE (48 * 1024U)  // 64KB for 5 tabs + buttons + labels

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM 1

/*====================
   FEATURE USAGE
 *====================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/*====================
   FONT USAGE
 *====================*/
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1

/*====================
   WIDGETS USAGE
 *====================*/
#define LV_USE_BTN 1
#define LV_USE_LABEL 1
#define LV_USE_TABVIEW 1
#define LV_USE_TEXTAREA 1
#define LV_USE_DROPDOWN 1
#define LV_USE_SLIDER 1

/*====================
   OTHERS
 *====================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_USE_USER_DATA 1
#define LV_USE_FILESYSTEM 1

#endif /*LV_CONF_H*/
