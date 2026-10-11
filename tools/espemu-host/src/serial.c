/* Serial port wrapper for the host viewer's serial input mode.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "serial.h"

#include <stdio.h>

#ifdef _WIN32

int gpsp_host_serial_open(const char *path)
{
    (void)path;
    fprintf(stderr, "Serial mode is not supported on Windows yet\n");
    return -1;
}

void gpsp_host_serial_reset(int port)
{
    (void)port;
}

int gpsp_host_serial_read(int port, void *data, size_t size)
{
    (void)port;
    (void)data;
    (void)size;
    return -1;
}

bool gpsp_host_serial_write(int port, const void *data, size_t size)
{
    (void)port;
    (void)data;
    (void)size;
    return false;
}

void gpsp_host_serial_close(int port)
{
    (void)port;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void gpsp_host_serial_lines(int port, int set_bits, int clear_bits)
{
    if (set_bits)
        ioctl(port, TIOCMBIS, &set_bits);
    if (clear_bits)
        ioctl(port, TIOCMBIC, &clear_bits);
}

int gpsp_host_serial_open(const char *path)
{
    struct termios options;
    int port = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (port < 0)
    {
        perror(path);
        return -1;
    }

    if (tcgetattr(port, &options) != 0)
    {
        perror(path);
        close(port);
        return -1;
    }

    cfmakeraw(&options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_cflag |= CLOCAL | CREAD;
    options.c_cflag &= ~(CSTOPB | CRTSCTS | HUPCL);
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    if (tcsetattr(port, TCSANOW, &options) != 0)
    {
        perror(path);
        close(port);
        return -1;
    }

    gpsp_host_serial_lines(port, 0, TIOCM_DTR | TIOCM_RTS);
    tcflush(port, TCIOFLUSH);
    return port;
}

void gpsp_host_serial_reset(int port)
{
    const struct timespec pulse = {0, 200 * 1000 * 1000};

    /* DTR released keeps IO0 high, so the board boots the app. */
    gpsp_host_serial_lines(port, TIOCM_RTS, TIOCM_DTR);
    nanosleep(&pulse, NULL);
    gpsp_host_serial_lines(port, 0, TIOCM_RTS);
}

int gpsp_host_serial_read(int port, void *data, size_t size)
{
    ssize_t count = read(port, data, size);

    if (count < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    return (int)count;
}

bool gpsp_host_serial_write(int port, const void *data, size_t size)
{
    const unsigned char *bytes = data;

    while (size > 0)
    {
        ssize_t count = write(port, bytes, size);

        if (count < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return false;
            usleep(1000);
            continue;
        }
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

void gpsp_host_serial_close(int port)
{
    if (port >= 0)
        close(port);
}

#endif
