#ifndef IPERF_CONFIG_H
#define IPERF_CONFIG_H

#define HAVE_CLOCK_GETTIME 1
#define HAVE_GETLINE 1
#define HAVE_INTTYPES_H 1
#define HAVE_NANOSLEEP 1
#define HAVE_SOCKET_SHUTDOWN_SHUT_WR 1
#define HAVE_STDATOMIC_H 1
#define PACKAGE_STRING "iperf 3.15"

/* LwIP does not implement TCP_MAXSEG; the default path ignores its failure. */
#define TCP_MAXSEG 2

#endif
