#pragma once
#include <ctime>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
  #include <sys/time.h>
  #include <time.h>
#endif

// ------------------------
// Time helpers
// ------------------------

static inline long long now_epoch_ms() {
#if defined(__linux__) || defined(__APPLE__)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
#else
    // fallback (menos preciso)
    return (long long)std::time(NULL) * 1000LL;
#endif
}

static inline long long now_mono_ms() {
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000);
#elif defined(__APPLE__)
    // macOS também suporta CLOCK_MONOTONIC
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000);
#else
    // fallback pobre (não monotónico)
    return now_epoch_ms();
#endif
}

static inline std::string format_epoch_ms_local(long long epoch_ms) {
    std::time_t sec = (std::time_t)(epoch_ms / 1000LL);
    long long ms = epoch_ms % 1000LL;

    std::tm tm_local;
#if defined(_WIN32)
    localtime_s(&tm_local, &sec);
#else
    localtime_r(&sec, &tm_local);
#endif

    char buf[64];
    // YYYY-MM-DD HH:MM:SS
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);

    std::ostringstream oss;
    oss << buf << '.'
        << std::setw(3) << std::setfill('0') << ms;

    return oss.str();
}


// ------------------------
// Logger state
// ------------------------
// Chama init uma vez no arranque (logo no início do run()).
struct LogClock {
    long long start_epoch_ms;
    long long start_mono_ms;
    bool inited;

    LogClock(): start_epoch_ms(0), start_mono_ms(0), inited(false) {}
};

static inline LogClock& logclock() {
    static LogClock lc;
    return lc;
}

static inline void log_init_clock() {
    LogClock& lc = logclock();
    if (!lc.inited) {
        lc.start_epoch_ms = now_epoch_ms();
        lc.start_mono_ms  = now_mono_ms();
        lc.inited = true;
    }
}

// Prefixo padrão: [YYYY-mm-dd HH:MM:SS.mmm +Δms t=NOW] 
static inline void log_prefix(std::ostream& os, long nowMs /* o teu _nowMs */) {
    LogClock& lc = logclock();
    if (!lc.inited) log_init_clock();

    long long mono = now_mono_ms();
    long long delta = mono - lc.start_mono_ms;
    long long epoch = lc.start_epoch_ms + delta;

    os << '[' << format_epoch_ms_local(epoch)
       << " +" << delta << "ms"
       << " t=" << nowMs
       << "] ";
}

// Macro para não repetires boilerplate.
// Uso: LOG_LINE(_nowMs, std::cerr, "[READ] fd="<<fd<<" ...");
#define LOG_LINE(NOWMS, OS, STREAM_EXPR) \
    do { \
        log_prefix((OS), (NOWMS)); \
        (OS) << STREAM_EXPR << "\n"; \
    } while (0)

// Para marcar alinhamento com o início do tcpdump (opcional):
#define LOG_PCAP_MARK(NOWMS, OS, LABEL) \
    do { \
        log_prefix((OS), (NOWMS)); \
        (OS) << "[PCAP-MARK] " << (LABEL) << " epoch_ms=" << now_epoch_ms() << "\n"; \
    } while (0)

	