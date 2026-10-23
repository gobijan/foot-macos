#pragma once

/*
 * There is no epoll on macOS; foot's FDM is implemented on top of
 * kqueue (see macos/fdm.c). The FDM API still describes events with
 * epoll's bit values, so this header only provides those constants.
 */

enum {
    EPOLLIN = 0x001,
    EPOLLPRI = 0x002,
    EPOLLOUT = 0x004,
    EPOLLERR = 0x008,
    EPOLLHUP = 0x010,
};
