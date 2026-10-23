#pragma once

#include "Mesh.h"
#include <stddef.h>
#include <stdio.h>


class LocationProvider {
protected:
    bool _time_sync_needed = true;

public:
    virtual void syncTime() { _time_sync_needed = true; }
    virtual bool waitingTimeSync() { return _time_sync_needed; }
    virtual long getLatitude() = 0;
    virtual long getLongitude() = 0;
    virtual long getAltitude() = 0;
    virtual long satellitesCount() = 0;
    virtual bool isValid() = 0;
    virtual long getTimestamp() = 0;
    virtual void sendSentence(const char * sentence);
    virtual bool waitFor(const char* prefix, uint32_t timeout_ms) { return false; }
    virtual void drain() { }
    virtual void reset() = 0;
    virtual void begin() = 0;
    virtual void stop() = 0;
    virtual void loop() = 0;
    virtual bool isEnabled() = 0;
    // seconds since the last valid fix, or UINT32_MAX if there hasn't been one
    virtual uint32_t getFixAgeSecs() { return isValid() ? 0 : UINT32_MAX; }
    // horizontal dilution of precision in tenths (1.1 -> 11), or -1 if unknown
    virtual int getHDOPx10() { return -1; }

    // Format compact diagnostics in the caller-provided buffer. Providers that
    // have more information (for example UART counters) can override this.
    virtual void formatDiagnostics(char* out, size_t out_size) {
        if (out_size == 0) return;
        snprintf(out, out_size, "en:%u sat:%ld fix:%u",
            isEnabled() ? 1U : 0U,
            satellitesCount(),
            isValid() ? 1U : 0U);
    }
};
