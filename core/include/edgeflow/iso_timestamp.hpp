#pragma once
#include <cstdio>
#include <ctime>
#include <string>

namespace edgeflow {

// Formats a time point as ISO-8601 UTC ("2026-08-28T12:34:56Z"), caching the
// result and regenerating only when the second changes.
//
// The simulator formats a timestamp for every event it sends. Doing that with
// std::ostringstream and std::put_time cost roughly a microsecond per event and
// allocated a fresh string each time, for a value that only changes once a
// second. This caches it.
//
// NOT thread-safe, and deliberately so: each DeviceClient owns one, and the
// simulator's io_context threads never share a client. Sharing one across
// threads would need external synchronisation.
//
// The returned reference points at an internal buffer and is invalidated by the
// next call. Copy it if you need to keep it.
class CachedIsoTimestamp {
public:
    // Formats `seconds` (Unix epoch, UTC). Regenerates only when `seconds`
    // differs from the cached second -- in either direction, since system
    // clocks can step backwards.
    const std::string& format(std::time_t seconds) {
        if (!has_value_ || seconds != cached_second_) {
            regenerate(seconds);
        }
        return cached_;
    }

    const std::string& now() { return format(std::time(nullptr)); }

private:
    void regenerate(std::time_t seconds) {
        std::tm utc{};
        gmtime_r(&seconds, &utc);
        char buffer[32];
        // snprintf rather than put_time: no stream, no allocation, and the
        // format is fixed-width so the size is known.
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                      utc.tm_hour, utc.tm_min, utc.tm_sec);
        cached_.assign(buffer);
        cached_second_ = seconds;
        has_value_ = true;
    }

    std::string cached_;
    std::time_t cached_second_ = 0;
    bool has_value_ = false;
};

} // namespace edgeflow
