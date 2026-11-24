// GTK 后端专用的 gfx 入口：窗口的绘制回调拿到 cairo_t 之后交给 gfx_begin。
#pragma once

#include "gfx.h"

#include <cairo.h>

/// 把窗口这一帧的 cairo_t 交给 surface；之后 gfx_begin 直接画在上面。
void gfx_surface_set_cairo(GfxSurface *surface, cairo_t *cairo);