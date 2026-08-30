
#ifndef _LOG_MYLOG_H_
#define _LOG_MYLOG_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <set>

using namespace std;

#define RED "\x1B[31m"
#define GRN "\x1B[32m"
#define YEL "\x1B[33m"
#define BLU "\x1B[34m"
#define MAG "\x1B[35m"
#define CYN "\x1B[36m"
#define WHT "\x1B[37m"
#define RESET "\x1B[0m"

const int log_never = 0;
const int log_fatal = 1;
const int log_error = 2;
const int log_warn = 3;
const int log_info = 4;
const int log_debug = 5;
const int log_trace = 6;
const int log_end = 7;

const char log_text[][20] = {"NEVER", "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE", ""};
const char log_color[][20] = {RED, RED, RED, YEL, GRN, MAG, ""};

extern int log_level;
extern int enable_log_position;
extern int enable_log_color;

#ifdef MY_DEBUG
#define mylog(__first_argu__dummy_abcde__, ...) printf(__VA_ARGS__)

#else
// A function-level filter still evaluates every formatting argument before
// log0() can reject the message.  Trace calls sit on the UDP packet path and
// may format addresses or calculate diagnostics, so filter in the macro to
// make disabled logging genuinely cold.
#define mylog(level, ...)                           \
    do {                                            \
        if ((level) <= log_level)                   \
            log0(__FILE__, __FUNCTION__, __LINE__, (level), __VA_ARGS__); \
    } while (0)
#endif

//#define mylog(__first_argu__dummy_abcde__,...) {;}

void log0(const char* file, const char* function, int line, int level, const char* str, ...);

void log_bare(int level, const char* str, ...);

#endif
