#ifndef MAIN_H
#define MAIN_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/uio.h>

#include "ssl.h"
#include "aprintf.h"
#include "stats.h"
#include "units.h"
#include "zmalloc.h"

static void *thread_main(void *);
static int record_rate(aeEventLoop *, long long, void *);

static int response_complete(http_parser *);
static int header_field(http_parser *, const char *, size_t);
static int header_value(http_parser *, const char *, size_t);
static int response_body(http_parser *, const char *, size_t);

static int parse_args(struct config *, char **, struct http_parser_url *, char **, int, char **);
static char *copy_url_part(char *, struct http_parser_url *, enum http_parser_url_fields);

static void print_stats_header();
static void print_stats(char *, stats *, char *(*)(long double));
static void print_stats_latency(stats *);

#endif /* MAIN_H */
