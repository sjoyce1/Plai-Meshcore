#pragma once

#include <vector>
#include <string>
#include <cstdarg>
#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Mesh {

class ConsoleLogger {
private:
    static const size_t MAX_LOG_LINES = 200;
    std::vector<std::string> _log_lines;
    std::string _current_partial_line;
    SemaphoreHandle_t _mutex;
    vprintf_like_t _prev_vprintf;
    bool _auto_scroll;

    ConsoleLogger() : _prev_vprintf(nullptr), _auto_scroll(true) {
        _mutex = xSemaphoreCreateMutex();
    }

public:
    static ConsoleLogger& getInstance() {
        static ConsoleLogger instance;
        return instance;
    }

    void init() {
        if (_prev_vprintf == nullptr) {
            _prev_vprintf = esp_log_set_vprintf(vprintf_hook);
        }
    }

    static int vprintf_hook(const char* fmt, va_list args) {
        char buf[256];
        va_list args_copy;
        va_copy(args_copy, args);
        int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
        va_end(args_copy);

        if (len > 0) {
            getInstance().appendRawText(buf, len);
        }

        if (getInstance()._prev_vprintf) {
            return getInstance()._prev_vprintf(fmt, args);
        }
        return vprintf(fmt, args);
    }

    void appendRawText(const char* text, size_t len) {
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (size_t i = 0; i < len; i++) {
                char c = text[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    if (!_current_partial_line.empty()) {
                        _log_lines.push_back(_current_partial_line);
                        _current_partial_line.clear();
                    }
                } else {
                    _current_partial_line += c;
                }
            }

            while (_log_lines.size() > MAX_LOG_LINES) {
                _log_lines.erase(_log_lines.begin());
            }

            xSemaphoreGive(_mutex);
        }
    }

    std::vector<std::string> getLogLines() {
        std::vector<std::string> lines_copy;
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lines_copy = _log_lines;
            if (!_current_partial_line.empty()) {
                lines_copy.push_back(_current_partial_line);
            }
            xSemaphoreGive(_mutex);
        }
        return lines_copy;
    }

    size_t getLineCount() {
        size_t count = 0;
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            count = _log_lines.size() + (_current_partial_line.empty() ? 0 : 1);
            xSemaphoreGive(_mutex);
        }
        return count;
    }

    void clear() {
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            _log_lines.clear();
            _current_partial_line.clear();
            xSemaphoreGive(_mutex);
        }
    }

    bool isAutoScroll() const { return _auto_scroll; }
    void setAutoScroll(bool enable) { _auto_scroll = enable; }
};

} // namespace Mesh
