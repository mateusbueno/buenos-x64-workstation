#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_ARGB8888
#define LV_USE_OS LV_OS_NONE

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 16  /* ~60 FPS */
#define LV_DPI_DEF 170         /* 7" 1024x600 panel has ~170 DPI */

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Widget choices */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_LINE 1
#define LV_USE_BAR 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1

/* Drivers */
#if defined(__APPLE__) || defined(VELORA_USE_SDL)
  #define LV_USE_SDL 1
  #define LV_SDL_INCLUDE_PATH <SDL.h>
#else
  #define LV_USE_SDL 0
#endif

#if defined(__linux__) && !defined(VELORA_USE_SDL)
  #define LV_USE_LINUX_DRM 1
  #define LV_USE_EVDEV 1
#else
  #define LV_USE_LINUX_DRM 0
  #define LV_USE_EVDEV 0
#endif

#endif /* LV_CONF_H */
