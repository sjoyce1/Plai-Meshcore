/**
 * @file app_stats.cpp
 * @author d4rkmen
 * @brief Statistics widget - tabbed system info display
 * @version 2.0
 * @date 2025-01-03
 *
 * @copyright Copyright (c) 2025
 *
 */
#include "app_stats.h"
#include "common_define.h"
// #include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include <time.h>
#include "apps/utils/ui/key_repeat.h"
#include "apps/utils/ui/draw_helper.h"
#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "meshtastic/portnums.pb.h"
#include <algorithm>
#include <format>
// assets
#include "assets/stat_system.h"
#include "assets/stat_radio.h"
#include "assets/stat_node.h"
#include "assets/stat_gps.h"
#include "assets/stat_mesh.h"
#include "assets/stat_db.h"
#include "assets/stat_tasks.h"

static const char* TAG __attribute__((unused)) = "APP_STATS";

#define UPDATE_INTERVAL_MS 2000
#define ROW_HEIGHT 13
#define BODY_START_Y 15
#define ICON_SIZE 12

static const char* TAB_NAMES[] = {"NODE", "SYSTEM", "RADIO", "NODE DB", "GPS", "TASKS"};

static const char* HINT_STATS = "[\u2191][\u2193][\u2190][\u2192] [DEL] [ESC]";

static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;

using namespace MOONCAKE::APPS;

void AppStats::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.current_tab = 0;
    _data.scroll_offset = 0;
    _data.scroll_max = 0;
    _data.last_update_ms = 0;
    _data.needs_redraw = true;
    _data.prev_task_count = 0;
    _data.prev_total_runtime = 0;
    _data.prev_valid = false;
    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);
}

void AppStats::onResume()
{
    ANIM_APP_OPEN();
    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    _data.current_tab = 0;
    _data.scroll_offset = 0;
    _data.scroll_max = 0;
    _data.last_update_ms = 0;
    _data.needs_redraw = true;
}

void AppStats::onRunning()
{
    uint32_t now = millis();
    bool updated = false;

    if (_data.needs_redraw || now - _data.last_update_ms > UPDATE_INTERVAL_MS)
    {
        _render_tab();
        _data.last_update_ms = now;
        _data.needs_redraw = false;
        updated = true;
    }

    updated |= _render_hint();

    if (updated)
        _data.hal->canvas_update();

    _handle_input();
}

void AppStats::onDestroy() { hl_text_free(&_data.hint_hl_ctx); }

void AppStats::_render_tab()
{
    auto* canvas = _data.hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);

    _render_tab_header(TAB_NAMES[_data.current_tab]);

    _data.visible_rows = (canvas->height() - BODY_START_Y + 1 - 9) / ROW_HEIGHT;
    _data.row_idx = 0;
    _data.row_y = BODY_START_Y;

    switch (_data.current_tab)
    {
    case TAB_NODE:
        _render_node_info();
        break;
    case TAB_SYSTEM:
        _render_system_info();
        break;
    case TAB_RADIO:
        _render_radio_info();
        break;
    case TAB_NODEDB:
        _render_nodedb_info();
        break;
    case TAB_GPS:
        _render_gps_info();
        break;
    case TAB_TASKS:
        _render_tasks_info();
        break;
    }

    int total_rows = _data.row_idx;
    _data.scroll_max = total_rows > _data.visible_rows ? total_rows - _data.visible_rows : 0;
    if (_data.scroll_offset > _data.scroll_max)
        _data.scroll_offset = _data.scroll_max;

    UTILS::UI::draw_scrollbar(canvas,
                              canvas->width() - 3,
                              BODY_START_Y,
                              2,
                              _data.visible_rows * ROW_HEIGHT,
                              total_rows,
                              _data.visible_rows,
                              _data.scroll_offset);
}

void AppStats::_add_row(const char* label, const char* value, int color)
{
    if (_data.row_idx >= _data.scroll_offset && _data.row_idx < _data.scroll_offset + _data.visible_rows)
    {
        _draw_row(_data.row_y, label, value, color);
        _data.row_y += ROW_HEIGHT;
    }
    _data.row_idx++;
}

bool AppStats::_render_hint()
{
    return hl_text_render(&_data.hint_hl_ctx,
                          HINT_STATS,
                          0,
                          _data.hal->canvas()->height() - 9,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

void AppStats::_render_tab_header(const char* title)
{
    auto* canvas = _data.hal->canvas();
    const uint16_t* icons[] = {image_data_stat_node,
                               image_data_stat_system,
                               image_data_stat_radio,
                               image_data_stat_db,
                               image_data_stat_gps,
                               image_data_stat_tasks};
    if (_data.current_tab < TAB_COUNT)
    {
        canvas->pushImage(4, 1, ICON_SIZE, ICON_SIZE, icons[_data.current_tab]);
    }
    else
    {
        canvas->drawRect(4, 1, ICON_SIZE, ICON_SIZE, TFT_DARKGREY);
    }
    constexpr int dot_spacing = 10;
    constexpr int dot_r = 3;
    int dots_start_x = canvas->width() - TAB_COUNT * dot_spacing - 4;
    canvas->setFont(FONT_12);
    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString(title, ICON_SIZE + 8, 1);
    for (int i = 0; i < TAB_COUNT; i++)
    {
        int cx = dots_start_x + i * dot_spacing + dot_r;
        int cy = 4 + dot_r;
        if (i == _data.current_tab)
            canvas->fillCircle(cx, cy, dot_r, TFT_ORANGE);
        else
            canvas->drawCircle(cx, cy, dot_r, TFT_DARKGREY);
    }

    canvas->drawFastHLine(0, BODY_START_Y - 1, canvas->width(), THEME_COLOR_HEADER_LINE);
}

void AppStats::_draw_row(int y, const char* label, const char* value, int value_color)
{
    auto* canvas = _data.hal->canvas();
    canvas->setFont(FONT_12);
    canvas->setTextColor(TFT_WHITE);
    canvas->drawString(label, 5, y);
    canvas->setTextColor(value_color);
    canvas->drawRightString(value, canvas->width() - 5, y);
}

// ========== Tab: Node Info ==========

void AppStats::_render_node_info()
{
    if (!_data.hal->mesh())
    {
        _add_row("Mesh", "Not initialized", TFT_RED);
        return;
    }

    const auto& config = _data.hal->mesh()->getConfig();
    char buf[48];

    _add_row("Name", config.long_name, TFT_GREEN);
    _add_row("Short Name", config.short_name, TFT_GREEN);
    _add_row("Role", Mesh::NodeDB::getRoleName(config.role), TFT_YELLOW);
    _add_row("PKI", config.public_key_len == 32 ? "Enabled" : "None", config.public_key_len == 32 ? TFT_GREEN : TFT_DARKGREY);
    // show only if enabled in settings
    uint32_t remain_ms;
    uint32_t sec;
    if (config.nodeinfo_broadcast_interval_ms > 0)
    {
        remain_ms = _data.hal->mesh()->getNodeInfoBroadcastRemainingMs();
        sec = remain_ms / 1000;
        snprintf(buf, sizeof(buf), "%02lum %02lus", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        _add_row("Next NodeInfo", buf, TFT_CYAN);
    }

    if (config.neighborinfo_enabled && config.neighborinfo_broadcast_interval_ms > 0)
    {
        remain_ms = _data.hal->mesh()->getNeighborInfoBroadcastRemainingMs();
        sec = remain_ms / 1000;
        snprintf(buf, sizeof(buf), "%02lum %02lus", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        _add_row("Next NeighborInfo", buf, TFT_CYAN);
    }

    if (config.position_broadcast_interval_ms > 0)
    {
        remain_ms = _data.hal->mesh()->getPositionBroadcastRemainingMs();
        sec = remain_ms / 1000;
        snprintf(buf, sizeof(buf), "%02lum %02lus", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        _add_row("Next Position", buf, TFT_CYAN);
    }
    if (config.telemetry_broadcast_interval_ms > 0)
    {
        remain_ms = _data.hal->mesh()->getTelemetryBroadcastRemainingMs();
        sec = remain_ms / 1000;
        snprintf(buf, sizeof(buf), "%02lum %02lus", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        _add_row("Next Telemetry", buf, TFT_CYAN);
    }
}

// ========== Tab: System Info ==========

void AppStats::_render_system_info()
{
    char buf[64];

    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t free_heap = esp_get_free_heap_size();
    snprintf(buf, sizeof(buf), "%u / %u KB", (unsigned)(free_heap / 1024), (unsigned)(total_heap / 1024));
    _add_row("Heap (free/total)", buf, TFT_CYAN);

    uint32_t min_heap = esp_get_minimum_free_heap_size();
    snprintf(buf, sizeof(buf), "%lu KB", min_heap / 1024);
    _add_row("Min Heap Ever", buf, min_heap < 20480 ? TFT_RED : TFT_CYAN);

#if BOARD_HAS_PSRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0)
    {
        snprintf(buf, sizeof(buf), "%u / %u KB", (unsigned)(psram_free / 1024), (unsigned)(psram_total / 1024));
        _add_row("PSRAM", buf, TFT_CYAN);
    }
#endif

    if (_data.hal->sdcard() && _data.hal->sdcard()->is_mounted())
    {
        uint64_t total_bytes = 0, free_bytes = 0;
        if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) == ESP_OK && total_bytes > 0)
        {
            uint32_t total_mb = (uint32_t)(total_bytes / (1024 * 1024));
            uint32_t used_mb = (uint32_t)((total_bytes - free_bytes) / (1024 * 1024));
            snprintf(buf, sizeof(buf), "%lu / %lu MB", (unsigned long)used_mb, (unsigned long)total_mb);
            _add_row("Storage (used/total)", buf, TFT_CYAN);
        }
    }

    uint32_t uptime_ms = (uint32_t)millis();
    _add_row("Uptime", _format_uptime(uptime_ms).c_str(), TFT_CYAN);

    time_t now_t = time(nullptr);
    struct tm ti;
    localtime_r(&now_t, &ti);
    if (ti.tm_year > 100)
    {
        snprintf(buf,
                 sizeof(buf),
                 "%02d.%02d.%04d %02d:%02d:%02d",
                 ti.tm_mday,
                 ti.tm_mon + 1,
                 ti.tm_year + 1900,
                 ti.tm_hour,
                 ti.tm_min,
                 ti.tm_sec);
        _add_row("DateTime", buf, TFT_CYAN);
    }
    else
    {
        _add_row("DateTime", "Not set", TFT_DARKGREY);
    }

    float voltage = _data.hal->getBatVoltage();
    uint8_t level = _data.hal->getBatLevel(voltage);
    snprintf(buf, sizeof(buf), "%.2fV %u%%", voltage, level);
    _add_row("Battery", buf, level < 25 ? TFT_RED : level < 50 ? TFT_YELLOW : level < 75 ? TFT_GREEN : TFT_CYAN);

    _add_row("Version", BUILD_NUMBER, TFT_CYAN);
    _add_row("Built", __DATE__ " " __TIME__, TFT_DARKGREY);
}

// ========== Tab: Radio Info ==========

const char* AppStats::_preset_name(int preset)
{
    return Mesh::getPresetName(static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(preset));
}

void AppStats::_render_radio_info()
{
    char buf[32];

    if (!_data.hal->mesh())
    {
        _add_row("Radio", "Not initialized", TFT_RED);
        return;
    }

    const auto& mesh_config = _data.hal->mesh()->getConfig();

    float freq = 0.0f;
    if (_data.hal->radio())
    {
        freq = (float)_data.hal->radio()->getConfig().frequency_hz / 1000000.0f;
    }
    if (freq <= 0.0f)
    {
        int32_t freq_khz = _data.hal->settings()->getNumber("lora", "frequency");
        freq = (freq_khz > 0) ? (float)freq_khz / 1000.0f : 910.525f;
    }
    snprintf(buf, sizeof(buf), "%.3f MHz", freq);
    _add_row("Freq", buf, TFT_CYAN);

    if (mesh_config.lora_config.use_preset)
        _add_row(
            "Preset / slot",
            std::string(
                std::format("{} / {}", _preset_name(mesh_config.lora_config.modem_preset), mesh_config.lora_config.channel_num))
                .c_str(),
            TFT_GREEN);

    if (_data.hal->radio())
    {
        auto radio_cfg = _data.hal->radio()->getConfig();
        snprintf(buf,
                 sizeof(buf),
                 "SF%u BW%.0f CR4/%u",
                 radio_cfg.spreading_factor,
                 radio_cfg.bandwidth_hz / 1000.0f,
                 radio_cfg.coding_rate);
        _add_row("Waveform", buf, TFT_CYAN);

        snprintf(buf, sizeof(buf), "%d dBm", radio_cfg.tx_power_dbm);
        _add_row("TX Power", buf, TFT_CYAN);
    }

    const auto& stats = Mesh::MeshDataStore::getInstance().getStats();
    snprintf(buf, sizeof(buf), "%lu", stats.rx_packets);
    _add_row("RX Packets", buf, TFT_CYAN);

    snprintf(buf, sizeof(buf), "%lu", stats.tx_packets);
    _add_row("TX Packets", buf, TFT_GREEN);
}

// ========== Tab: Node DB Info ==========

void AppStats::_render_nodedb_info()
{
    char buf[16];

    if (_data.hal->nodedb())
    {
        size_t online = _data.hal->nodedb()->getOnlineNodeCount();
        size_t total = _data.hal->nodedb()->getNodeCount();
        snprintf(buf, sizeof(buf), "%u / %u", (unsigned)online, (unsigned)total);
        _add_row("Nodes (online/total)", buf, TFT_CYAN);
    }

    snprintf(buf, sizeof(buf), "%u", (unsigned)Mesh::favorites_get_count());
    _add_row("Favorites", buf, TFT_YELLOW);

    snprintf(buf, sizeof(buf), "%u", (unsigned)Mesh::ignorelist_get_count());
    _add_row("Ignored", buf, Mesh::ignorelist_get_count() > 0 ? TFT_RED : TFT_DARKGREY);

    const auto& stats = Mesh::MeshDataStore::getInstance().getStats();
    snprintf(buf, sizeof(buf), "%lu", stats.messages_sent);
    _add_row("Msgs Sent", buf, TFT_GREEN);

    snprintf(buf, sizeof(buf), "%lu", stats.messages_received);
    _add_row("Msgs Recv", buf, TFT_CYAN);
}

// ========== Tab: GPS Info ==========

void AppStats::_render_gps_info()
{
    char buf[32];

#if HAL_USE_GPS
    auto* gps = _data.hal->gps();
    if (!gps || !gps->isInitialized())
    {
        _add_row("GPS", "Not available", TFT_DARKGREY);
        return;
    }

    auto data = gps->getData();

    static const char* fix_names[] = {"No Fix", "GPS", "DGPS", "PPS", "RTK", "FloatRTK", "Est", "Manual", "Sim"};
    int fix_idx = (int)data.fix_quality;
    if (fix_idx > 8)
        fix_idx = 0;
    _add_row("Fix", fix_names[fix_idx], data.has_fix ? TFT_GREEN : TFT_RED);

    snprintf(buf, sizeof(buf), "%lu / %lu", (unsigned long)data.sats_used, (unsigned long)data.sats_in_view);
    _add_row("Satellites (used/in view)", buf, data.sats_used > 0 ? TFT_CYAN : TFT_DARKGREY);

    if (data.has_fix)
    {
        snprintf(buf, sizeof(buf), "%.7f", data.latitude);
        _add_row("Latitude", buf, TFT_CYAN);

        snprintf(buf, sizeof(buf), "%.7f", data.longitude);
        _add_row("Longitude", buf, TFT_CYAN);

        snprintf(buf, sizeof(buf), "%d / %d m", (int)data.altitude_msl, (int)data.altitude_hae);
        _add_row("Altitude (MSL / HAE)", buf, TFT_CYAN);

        snprintf(buf, sizeof(buf), "%.1f", data.hdop / 100.0f);
        _add_row("HDOP (precision)", buf, data.hdop < 200 ? TFT_GREEN : TFT_YELLOW);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.1f", data.hdop / 100.0f);
        _add_row("HDOP", buf, TFT_DARKGREY);

        snprintf(buf, sizeof(buf), "%lu", data.sentence_count);
        _add_row("NMEA Msgs", buf, TFT_DARKGREY);
    }

    // Date/time + computed Unix time. A red date (year < 2020) or a zero Unix
    // time indicates the module is not delivering a usable timestamp (e.g. GPS
    // week-number rollover), which suppresses time sync and stamps broadcast
    // positions with time=0.
    snprintf(buf,
             sizeof(buf),
             "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)data.year,
             (unsigned)data.month,
             (unsigned)data.day,
             (unsigned)data.hour,
             (unsigned)data.minute,
             (unsigned)data.second);
    _add_row("UTC Date/Time", buf, data.year >= 2020 ? TFT_CYAN : TFT_RED);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.time);
    _add_row("Unix time", buf, data.time > 0 ? TFT_GREEN : TFT_RED);
#else
    _add_row("GPS", "Not supported", TFT_DARKGREY);
#endif
}

// ========== Tab: FreeRTOS Tasks ==========

void AppStats::_render_tasks_info()
{
    TaskStatus_t tasks[MAX_TASKS];
    configRUN_TIME_COUNTER_TYPE total_runtime;
    UBaseType_t filled = uxTaskGetSystemState(tasks, MAX_TASKS, &total_runtime);

    if (filled == 0)
    {
        _add_row("Tasks", "None", TFT_DARKGREY);
        return;
    }

    configRUN_TIME_COUNTER_TYPE delta_total = total_runtime - _data.prev_total_runtime;
    configRUN_TIME_COUNTER_TYPE total_cpu_time = delta_total * portNUM_PROCESSORS;

    uint32_t cpu_pct[MAX_TASKS] = {};
    if (_data.prev_valid && total_cpu_time > 0)
    {
        for (UBaseType_t i = 0; i < filled; i++)
        {
            configRUN_TIME_COUNTER_TYPE prev_rt = 0;
            for (UBaseType_t p = 0; p < _data.prev_task_count; p++)
            {
                if (_data.prev_tasks[p].handle == tasks[i].xHandle)
                {
                    prev_rt = _data.prev_tasks[p].runtime;
                    break;
                }
            }
            configRUN_TIME_COUNTER_TYPE delta_task = tasks[i].ulRunTimeCounter - prev_rt;
            cpu_pct[i] = (uint32_t)((delta_task * 100UL) / total_cpu_time);
        }
    }

    uint8_t idx[MAX_TASKS];
    for (UBaseType_t i = 0; i < filled; i++)
        idx[i] = i;
    std::sort(idx, idx + filled, [&cpu_pct](uint8_t a, uint8_t b) { return cpu_pct[a] > cpu_pct[b]; });

    std::string tasks_count = std::format("Tasks: {}", (unsigned)filled);
    _add_row(tasks_count.c_str(), "CPU Core Pri  Stk", TFT_ORANGE);

    for (UBaseType_t j = 0; j < filled; j++)
    {
        UBaseType_t i = idx[j];

#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        int core = tasks[i].xCoreID;
        const char* core_str = core == 0 ? "#0" : core == 1 ? "#1" : "#*";
#else
        const char* core_str = "#?";
#endif
        std::string task_descr = std::format("{:3d}%   {:2s}  {:2d} {:4d}",
                                             cpu_pct[i],
                                             core_str,
                                             (unsigned)tasks[i].uxCurrentPriority,
                                             (unsigned long)tasks[i].usStackHighWaterMark);
        int color;
        if (cpu_pct[i] >= 50)
            color = TFT_RED;
        else if (cpu_pct[i] >= 10)
            color = TFT_YELLOW;
        else if (tasks[i].usStackHighWaterMark < 512)
            color = TFT_RED;
        else
            color = TFT_CYAN;

        _add_row(tasks[i].pcTaskName, task_descr.c_str(), color);
    }

    for (UBaseType_t i = 0; i < filled && i < MAX_TASKS; i++)
    {
        _data.prev_tasks[i].handle = tasks[i].xHandle;
        _data.prev_tasks[i].runtime = tasks[i].ulRunTimeCounter;
    }
    _data.prev_task_count = filled < MAX_TASKS ? filled : MAX_TASKS;
    _data.prev_total_runtime = total_runtime;
    _data.prev_valid = true;
}

// ========== Input Handling ==========

void AppStats::_handle_input()
{
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) ||
            _data.hal->home_button()->is_pressed())
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            destroyApp();
            return;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                _data.current_tab = (_data.current_tab + 1) % TAB_COUNT;
                _data.scroll_offset = 0;
                _data.needs_redraw = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                _data.current_tab = (_data.current_tab + TAB_COUNT - 1) % TAB_COUNT;
                _data.scroll_offset = 0;
                _data.needs_redraw = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.scroll_offset < _data.scroll_max)
                {
                    _data.hal->playNextSound();
                    _data.scroll_offset++;
                    _data.needs_redraw = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.scroll_offset > 0)
                {
                    _data.hal->playNextSound();
                    _data.scroll_offset--;
                    _data.needs_redraw = true;
                }
            }
        }
    }
    else
    {
        is_repeat = false;
    }
}

// ========== Helpers ==========

std::string AppStats::_format_uptime(uint32_t ms)
{
    uint32_t secs = ms / 1000;
    uint32_t mins = secs / 60;
    uint32_t hours = mins / 60;
    uint32_t days = hours / 24;

    char buf[32];
    if (days > 0)
        snprintf(buf, sizeof(buf), "%dd %dh %dm", (int)days, (int)(hours % 24), (int)(mins % 60));
    else if (hours > 0)
        snprintf(buf, sizeof(buf), "%dh %dm %ds", (int)hours, (int)(mins % 60), (int)(secs % 60));
    else if (mins > 0)
        snprintf(buf, sizeof(buf), "%dm %ds", (int)mins, (int)(secs % 60));
    else
        snprintf(buf, sizeof(buf), "%ds", (int)secs);
    return buf;
}
