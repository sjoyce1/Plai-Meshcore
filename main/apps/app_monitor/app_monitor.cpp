/**
 * @file app_monitor.cpp
 * @brief Monitor widget - Live serial console log viewer
 *
 */
#include "app_monitor.h"
#include "common_define.h"
#include "apps/utils/theme/theme_define.h"
#include "esp_log.h"
#include "mesh/console_logger.h"
#include "apps/utils/ui/draw_helper.h"
#include "apps/utils/ui/key_repeat.h"
#include <algorithm>

static const char* TAG = "APP_MONITOR";

#define HEADER_HEIGHT 18
#define FOOTER_HEIGHT 14
#define LINE_HEIGHT 11

static bool is_repeat = false;
static uint32_t next_fire_ts = 0;

using namespace MOONCAKE::APPS;

void AppMonitor::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.view_state = ViewState::PACKET_LIST;
    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_log_size = 0;
    _data.update_list = true;
    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);
}

void AppMonitor::onResume()
{
    ANIM_APP_OPEN();
    _data.hal->canvas()->fillScreen(TFT_BLACK);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    _data.scroll_offset = -1; // -1 means auto-scroll to bottom
    _data.update_list = true;
    Mesh::ConsoleLogger::getInstance().setAutoScroll(true);
}

static uint32_t get_log_line_color(const std::string& line)
{
    if (line.find("E (") == 0 || line.find("ERROR") != std::string::npos)
        return TFT_RED;
    if (line.find("W (") == 0 || line.find("WARN") != std::string::npos)
        return TFT_YELLOW;
    if (line.find("I (") == 0 || line.find("BRIDGE:") != std::string::npos)
        return TFT_GREEN;
    if (line.find("DEBUG:") != std::string::npos || line.find("D (") == 0)
        return 0xD89FFF; // Light Magenta / Purple
    if (line.find("MESH") != std::string::npos)
        return TFT_CYAN;
    return TFT_WHITE;
}

void AppMonitor::onRunning()
{
    auto logs = Mesh::ConsoleLogger::getInstance().getLogLines();
    int total_lines = (int)logs.size();

    bool updated = false;

    // Header
    _data.hal->canvas()->fillRect(0, 0, _data.hal->canvas()->width(), HEADER_HEIGHT, THEME_COLOR_BG_DARK);
    _data.hal->canvas()->setTextColor(TFT_WHITE);
    _data.hal->canvas()->drawCenterString("CONSOLE LOG MONITOR", _data.hal->canvas()->width() / 2, 3);
    
    // Status badge (Auto-scroll or Paused)
    bool is_autoscroll = (_data.scroll_offset == -1);
    const char* status_str = is_autoscroll ? "[AUTO]" : "[MANUAL]";
    uint32_t status_col = is_autoscroll ? TFT_GREEN : TFT_YELLOW;
    _data.hal->canvas()->setTextColor(status_col);
    _data.hal->canvas()->drawString(status_str, _data.hal->canvas()->width() - 55, 3);

    // Calculate view parameters
    int view_h = _data.hal->canvas()->height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int max_visible = view_h / LINE_HEIGHT;

    int start_line = 0;
    if (_data.scroll_offset == -1)
    {
        // Auto scroll to bottom
        start_line = (total_lines > max_visible) ? (total_lines - max_visible) : 0;
    }
    else
    {
        int max_scroll = (total_lines > max_visible) ? (total_lines - max_visible) : 0;
        if (_data.scroll_offset > max_scroll) _data.scroll_offset = max_scroll;
        if (_data.scroll_offset < 0) _data.scroll_offset = 0;
        start_line = _data.scroll_offset;
    }

    // Render log area
    _data.hal->canvas()->fillRect(0, HEADER_HEIGHT, _data.hal->canvas()->width(), view_h, TFT_BLACK);

    int y = HEADER_HEIGHT + 1;
    for (int i = start_line; i < total_lines && (y + LINE_HEIGHT) <= (HEADER_HEIGHT + view_h); i++)
    {
        uint32_t col = get_log_line_color(logs[i]);
        _data.hal->canvas()->setTextColor(col);
        _data.hal->canvas()->drawString(logs[i].c_str(), 4, y);
        y += LINE_HEIGHT;
    }

    if (total_lines == 0)
    {
        _data.hal->canvas()->setTextColor(TFT_LIGHTGRAY);
        _data.hal->canvas()->drawCenterString("No log output captured yet...", _data.hal->canvas()->width() / 2, HEADER_HEIGHT + 20);
    }

    // Footer hint bar
    _data.hal->canvas()->fillRect(0, _data.hal->canvas()->height() - FOOTER_HEIGHT, _data.hal->canvas()->width(), FOOTER_HEIGHT, THEME_COLOR_BG_DARK);
    _data.hal->canvas()->setTextColor(TFT_LIGHTGRAY);
    _data.hal->canvas()->drawString("[\u2191][\u2193] Scroll  [ENTER] Auto  [C] Clear  [ESC] Exit", 4, _data.hal->canvas()->height() - FOOTER_HEIGHT + 2);

    _data.hal->canvas_update();

    // Input Handling
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();
        
        // Up arrow / Left arrow -> scroll up
        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                if (_data.scroll_offset == -1)
                {
                    int max_scroll = (total_lines > max_visible) ? (total_lines - max_visible) : 0;
                    _data.scroll_offset = max_scroll > 0 ? max_scroll - 1 : 0;
                }
                else if (_data.scroll_offset > 0)
                {
                    _data.scroll_offset--;
                }
            }
        }
        // Down arrow / Right arrow -> scroll down
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                if (_data.scroll_offset != -1)
                {
                    int max_scroll = (total_lines > max_visible) ? (total_lines - max_visible) : 0;
                    _data.scroll_offset++;
                    if (_data.scroll_offset >= max_scroll)
                    {
                        _data.scroll_offset = -1; // return to auto scroll
                    }
                }
            }
        }
        // Enter / Space -> toggle auto-scroll
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_SPACE))
        {
            _data.hal->playLastSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            _data.hal->keyboard()->waitForRelease(KEY_NUM_SPACE);
            if (_data.scroll_offset == -1)
            {
                int max_scroll = (total_lines > max_visible) ? (total_lines - max_visible) : 0;
                _data.scroll_offset = max_scroll;
            }
            else
            {
                _data.scroll_offset = -1; // resume auto scroll
            }
        }
        // 'C' key -> Clear console
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_C))
        {
            _data.hal->playLastSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_C);
            Mesh::ConsoleLogger::getInstance().clear();
            _data.scroll_offset = -1;
        }
        // ESC / Backspace -> Exit to launcher
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playLastSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
            ANIM_APP_CLOSE();
            destroyApp();
        }
    }
    else
    {
        is_repeat = false;
    }
}

void AppMonitor::onDestroy()
{
}
