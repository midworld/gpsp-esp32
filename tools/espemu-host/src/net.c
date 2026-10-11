/* Cross-platform socket wrapper for the ESP-EMU host viewer.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "net.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* intptr_t holds both a POSIX file descriptor and a WinSock SOCKET without
 * exposing either platform's socket headers to the SDL application. */
#ifdef _WIN32
#define GPSP_HOST_NATIVE_SOCKET(handle) ((SOCKET)(handle))
#else
#define GPSP_HOST_NATIVE_SOCKET(handle) ((int)(handle))
#endif

static bool gpsp_host_net_set_nonblocking(gpsp_host_socket_t socket_handle)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket((SOCKET)socket_handle, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl((int)socket_handle, F_GETFL, 0);

    return flags >= 0 &&
            fcntl((int)socket_handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool gpsp_host_net_init(void)
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void gpsp_host_net_shutdown(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

gpsp_host_socket_t gpsp_host_net_listen(
        const char *address, uint16_t port)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    struct addrinfo *current;
    char port_string[6];
    gpsp_host_socket_t listener = GPSP_HOST_INVALID_SOCKET;

    snprintf(port_string, sizeof(port_string), "%u", port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(address, port_string, &hints, &addresses) != 0)
        return GPSP_HOST_INVALID_SOCKET;

    for (current = addresses; current; current = current->ai_next)
    {
        /* Keep the native type until it is known to be valid. WinSock SOCKET
         * is pointer-sized and must not pass through a 32-bit int. */
#ifdef _WIN32
        SOCKET native_socket = socket(current->ai_family,
                current->ai_socktype, current->ai_protocol);

        if (native_socket == INVALID_SOCKET)
            continue;

        listener = (gpsp_host_socket_t)native_socket;
#else
        int native_socket = socket(current->ai_family,
                current->ai_socktype, current->ai_protocol);

        if (native_socket < 0)
            continue;

        listener = native_socket;
#endif

        {
            int reuse_address = 1;
            setsockopt(GPSP_HOST_NATIVE_SOCKET(listener),
                    SOL_SOCKET, SO_REUSEADDR,
                    (const char *)&reuse_address, (int)sizeof(reuse_address));
        }

        if (bind(GPSP_HOST_NATIVE_SOCKET(listener), current->ai_addr,
                (int)current->ai_addrlen) == 0 &&
                listen(GPSP_HOST_NATIVE_SOCKET(listener), 1) == 0 &&
                gpsp_host_net_set_nonblocking(listener))
            break;

        gpsp_host_net_close(listener);
        listener = GPSP_HOST_INVALID_SOCKET;
    }

    freeaddrinfo(addresses);
    return listener;
}

gpsp_host_socket_t gpsp_host_net_accept(gpsp_host_socket_t listener)
{
#ifdef _WIN32
    SOCKET accepted = accept((SOCKET)listener, NULL, NULL);
    gpsp_host_socket_t socket_handle;

    if (accepted == INVALID_SOCKET)
        return GPSP_HOST_INVALID_SOCKET;

    socket_handle = (gpsp_host_socket_t)accepted;
#else
    int accepted = accept((int)listener, NULL, NULL);
    gpsp_host_socket_t socket_handle;

    if (accepted < 0)
        return GPSP_HOST_INVALID_SOCKET;

    socket_handle = accepted;
#endif

    {
        int no_delay = 1;
        setsockopt(GPSP_HOST_NATIVE_SOCKET(socket_handle),
                IPPROTO_TCP, TCP_NODELAY,
                (const char *)&no_delay, (int)sizeof(no_delay));
    }

    if (!gpsp_host_net_set_nonblocking(socket_handle))
    {
        gpsp_host_net_close(socket_handle);
        return GPSP_HOST_INVALID_SOCKET;
    }

    return socket_handle;
}

int gpsp_host_net_send(
        gpsp_host_socket_t socket_handle, const void *data, size_t size)
{
    int length = size > INT_MAX ? INT_MAX : (int)size;

#ifdef _WIN32
    return send((SOCKET)socket_handle, data, length, 0);
#else
#ifdef MSG_NOSIGNAL
    return (int)send((int)socket_handle, data, (size_t)length, MSG_NOSIGNAL);
#else
    return (int)send((int)socket_handle, data, (size_t)length, 0);
#endif
#endif
}

int gpsp_host_net_receive(
        gpsp_host_socket_t socket_handle, void *data, size_t size)
{
    int length = size > INT_MAX ? INT_MAX : (int)size;

#ifdef _WIN32
    return recv((SOCKET)socket_handle, data, length, 0);
#else
    return (int)recv((int)socket_handle, data, (size_t)length, 0);
#endif
}

bool gpsp_host_net_would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void gpsp_host_net_close(gpsp_host_socket_t socket_handle)
{
    if (socket_handle == GPSP_HOST_INVALID_SOCKET)
        return;

#ifdef _WIN32
    closesocket((SOCKET)socket_handle);
#else
    close((int)socket_handle);
#endif
}
