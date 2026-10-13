// log.h — printf-style logging for iw8-zonetool. Header-only.
// info()/warn()/err() go to stderr with a tag prefix; fatal() prints + exits(1).
// Verbosity gate: set g_logLevel (0=err only, 1=+warn, 2=+info[default], 3=+debug).
#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace zt
{

inline int g_logLevel = 2; // 0 err, 1 +warn, 2 +info, 3 +debug

namespace detail
{
inline void vlog(FILE *out, const char *tag, const char *fmt, va_list ap)
{
    std::fputs(tag, out);
    std::vfprintf(out, fmt, ap);
    std::fputc('\n', out);
    std::fflush(out);
}
} // namespace detail

inline void info(const char *fmt, ...)
{
    if (g_logLevel < 2)
        return;
    va_list ap;
    va_start(ap, fmt);
    detail::vlog(stderr, "[info] ", fmt, ap);
    va_end(ap);
}
inline void warn(const char *fmt, ...)
{
    if (g_logLevel < 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    detail::vlog(stderr, "[warn] ", fmt, ap);
    va_end(ap);
}
inline void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    detail::vlog(stderr, "[err ] ", fmt, ap);
    va_end(ap);
}
inline void debug(const char *fmt, ...)
{
    if (g_logLevel < 3)
        return;
    va_list ap;
    va_start(ap, fmt);
    detail::vlog(stderr, "[dbg ] ", fmt, ap);
    va_end(ap);
}

// Logs how long a build stage took when it goes out of scope.
class StageTimer
{
  public:
    explicit StageTimer(const char *name) : name_(name), start_(std::chrono::steady_clock::now())
    {
    }
    ~StageTimer()
    {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_);
        info("timing: %s %.1fs", name_, elapsed.count());
    }
    StageTimer(const StageTimer &) = delete;
    StageTimer &operator=(const StageTimer &) = delete;

  private:
    const char *name_;
    std::chrono::steady_clock::time_point start_;
};

// Logs the time since the previous lap at each named build checkpoint.
class LapTimer
{
  public:
    explicit LapTimer(const char *scope) : scope_(scope), last_(std::chrono::steady_clock::now()) {}
    void lap(const char *name)
    {
        const auto now = std::chrono::steady_clock::now();
        info("timing: %s/%s %.1fs", scope_, name,
             std::chrono::duration<double>(now - last_).count());
        last_ = now;
    }

  private:
    const char *scope_;
    std::chrono::steady_clock::time_point last_;
};

// fatal: print to stderr and terminate the process with code 1.
[[noreturn]] inline void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    detail::vlog(stderr, "[FATAL] ", fmt, ap);
    va_end(ap);
    std::exit(1);
}

} // namespace zt
