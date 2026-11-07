#pragma once

/*
 * Linux: footd only (macos/meson.build). shm.h's buffers have an
 * IOSurface on macOS; footd draws none.
 */

typedef struct __IOSurface *IOSurfaceRef;
