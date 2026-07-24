/**
 * @file system_bar.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-18
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "../../launcher.h"
#include "common_define.h"
#include "mesh/mesh_service.h"
#include "mesh/mesh_data.h"
#include "apps/utils/ui/draw_helper.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/app_nodes/assets/pos_external.h"

#include "assets/bat.h"

using namespace MOONCAKE::APPS;

#define PADDING_X 4

void Launcher::_start_system_bar() {}

void Launcher::_update_system_bar()
{
    bool system_bar_force_update = _data.system_bar_force_update_flag && *_data.system_bar_force_update_flag;
    if (((millis() - _data.system_bar_update_count) > _data.system_bar_update_preiod) || system_bar_force_update)
    {
        // Reset force update flag
        if (system_bar_force_update)
        {
            *_data.system_bar_force_update_flag = false;
        }

        // Update state
        _update_system_state();

        // Backgound
        _data.hal->canvas_system_bar()->fillScreen(THEME_COLOR_BG);
        int margin_x = 0;
        int margin_y = 0;
        _data.hal->canvas_system_bar()->fillRect(margin_x,
                                                 margin_y,
                                                 _data.hal->canvas_system_bar()->width() - margin_x * 2,
                                                 _data.hal->canvas_system_bar()->height() - margin_y * 2,
                                                 THEME_COLOR_SYSTEM_BAR);

        int x = 4;
        int y = 3;

        // Draw colored node identifier from mesh config
        if (_data.hal->mesh())
        {
            const auto& config = _data.hal->mesh()->getConfig();
            const char* short_name = config.short_name;

            // Use short_name if available, otherwise use last 4 hex digits of node_id
            std::string display_name;
            if (short_name[0] != '\0')
            {
                display_name = short_name;
            }
            else
            {
                display_name = std::format("{:04x}", config.node_id & 0xFFFF);
            }

            uint32_t nc = UTILS::UI::node_color(config.node_id);
            uint32_t ntc = UTILS::UI::node_text_color(config.node_id);

            int short_width = display_name.length() * 6 + 6;
            _data.hal->canvas_system_bar()->fillRoundRect(x, y + 1, short_width, 14, 4, nc);
            _data.hal->canvas_system_bar()->setFont(FONT_12);
            _data.hal->canvas_system_bar()->setTextColor(ntc, nc);
            x += short_width + PADDING_X + 2;

            // Render GPS indicator immediately to the right of the name pill
#if HAL_USE_GPS
            bool gps_enabled = _data.hal->settings()->getBool("gps", "enabled");
            if (gps_enabled && _data.hal->gps())
            {
                int icon_x = x;
                if (_data.hal->gps()->hasFix())
                {
                    // Fix acquired: Green satellite icon + sat count
                    _data.hal->canvas_system_bar()->pushImage(icon_x, y + 2, 12, 12, image_data_pos_external, TFT_GREEN);
                    auto data = _data.hal->gps()->getData();
                    _data.hal->canvas_system_bar()->setFont(FONT_6);
                    _data.hal->canvas_system_bar()->setTextColor(TFT_GREEN, THEME_COLOR_SYSTEM_BAR);
                    _data.hal->canvas_system_bar()->drawString(std::format("{}", data.sats_used).c_str(), icon_x + 13, y + 4);
                }
                else if (_data.hal->gps()->isInitialized())
                {
                    // Searching for satellites: Yellow satellite icon + ?
                    _data.hal->canvas_system_bar()->pushImage(icon_x, y + 2, 12, 12, image_data_pos_external, TFT_YELLOW);
                    _data.hal->canvas_system_bar()->setFont(FONT_6);
                    _data.hal->canvas_system_bar()->setTextColor(TFT_YELLOW, THEME_COLOR_SYSTEM_BAR);
                    _data.hal->canvas_system_bar()->drawString("?", icon_x + 13, y + 4);
                }
            }
#endif
        }
        _data.hal->canvas_system_bar()->setFont(FONT_16);
        // Time
        bool show_time = _data.hal->settings()->getBool("system", "show_time");
        if (show_time)
        {
            _data.hal->canvas_system_bar()->setFont(FONT_16);
            _data.hal->canvas_system_bar()->setTextColor(THEME_COLOR_SYSTEM_BAR_TEXT);
            _data.hal->canvas_system_bar()->drawCenterString(_data.system_state.time.c_str(),
                                                             _data.hal->canvas_system_bar()->width() / 2 - 8,
                                                             _data.hal->canvas_system_bar()->height() / 2 - FONT_HEIGHT / 2);
        }
        // total new messages badge (DM+CHANNELS)
        uint32_t total_new_messages = Mesh::MeshDataStore::getInstance().getTotalUnreadDMCount() +
                                      Mesh::MeshDataStore::getInstance().getTotalUnreadChannelCount();
        if (total_new_messages > 0)
        {
            std::string total_new_messages_str = total_new_messages < 100 ? std::format("{}", total_new_messages) : "99+";
            int total_new_messages_width = _data.hal->canvas_system_bar()->textWidth(total_new_messages_str.c_str());
            _data.hal->canvas_system_bar()->setFont(FONT_12);
            x = _data.hal->canvas_system_bar()->width() / 2 + 20;
            _data.hal->canvas_system_bar()->fillRoundRect(x, y + 1, total_new_messages_width + 6, 14, 4, TFT_RED);
            // _data.hal->canvas_system_bar()->setFont(FONT_6);
            _data.hal->canvas_system_bar()->setTextColor(TFT_WHITE, TFT_RED);
            _data.hal->canvas_system_bar()->drawCenterString(total_new_messages_str.c_str(),
                                                             x + total_new_messages_width / 2 + 3,
                                                             y + 1);
            _data.hal->canvas_system_bar()->setFont(FONT_16);
        }
        // Battery area
        x = _data.hal->canvas_system_bar()->width() - 36;

        // Voltage / charge % (2 rows, FONT_6)
        bool show_voltage = _data.hal->settings()->getBool("system", "show_bat_volt");
        if (show_voltage)
        {
            _data.hal->canvas_system_bar()->setFont(FONT_6);
            _data.hal->canvas_system_bar()->setTextColor(THEME_COLOR_SYSTEM_BAR_TEXT);
            _data.hal->canvas_system_bar()->drawRightString(std::format("{:.2f}V", _data.system_state.voltage).c_str(),
                                                            x - 2,
                                                            y + 2);
            _data.hal->canvas_system_bar()->drawRightString(std::format("{}%", _data.system_state.bat_level).c_str(),
                                                            x - 2,
                                                            y + 9);
        }

        // Battery icon (image + fill)
        {
            int bat_x = x;
            int bat_y = y;
            _data.hal->canvas_system_bar()->pushImage(bat_x, bat_y, 32, 16, image_data_bat);

            constexpr int fill_x0 = 3, fill_y0 = 3;
            constexpr int fill_x1 = 24, fill_y1 = 12;
            constexpr int max_fill_w = fill_x1 - fill_x0 + 1;
            constexpr int fill_h = fill_y1 - fill_y0 + 1;
            int fill_w = (max_fill_w * _data.system_state.bat_level + 50) / 100;
            uint16_t fill_color = TFT_RED;
            if (_data.system_state.bat_level > 50)
                fill_color = TFT_BLACK;
            else if (_data.system_state.bat_level > 25)
                fill_color = TFT_YELLOW;
            if (fill_w > 0)
                _data.hal->canvas_system_bar()->fillRect(bat_x + fill_x0, bat_y + fill_y0, fill_w, fill_h, fill_color);
        }

        // Push
        _data.hal->canvas_system_bar_update();

        // Reset flag
        _data.system_bar_update_count = millis();
    }
}
