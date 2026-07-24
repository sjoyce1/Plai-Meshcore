/**
 * @file app_stats.h
 * @author d4rkmen
 * @brief Statistics widget - tabbed system info display
 * @version 2.0
 * @date 2025-01-03
 *
 * @copyright Copyright (c) 2025
 *
 */
#pragma once

#include "../apps.h"
#include <vector>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "apps/utils/anim/hl_text.h"
#include "mesh/mesh_data.h"

#include "assets/app_stats.h"

namespace MOONCAKE::APPS
{

    class AppStats : public APP_BASE
    {
    private:
        static constexpr int TAB_COUNT = 6;
        static constexpr int MAX_TASKS = 16;

        enum Tab
        {
            TAB_NODE = 0,
            TAB_SYSTEM,
            TAB_RADIO,
            TAB_NODEDB,
            TAB_GPS,
            TAB_TASKS
        };

        struct TaskSnapshot
        {
            TaskHandle_t handle;
            configRUN_TIME_COUNTER_TYPE runtime;
        };

        struct
        {
            HAL::Hal* hal;
            int current_tab;
            int scroll_offset;
            int scroll_max;
            int row_idx;
            int visible_rows;
            int row_y;
            uint32_t last_update_ms;
            bool needs_redraw;
            UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;

            TaskSnapshot prev_tasks[MAX_TASKS];
            UBaseType_t prev_task_count;
            configRUN_TIME_COUNTER_TYPE prev_total_runtime;
            bool prev_valid;
        } _data;

        void _render_tab();
        bool _render_hint();
        void _render_tab_header(const char* title);
        void _render_node_info();
        void _render_system_info();
        void _render_radio_info();
        void _render_nodedb_info();
        void _render_gps_info();
        void _render_tasks_info();
        void _handle_input();
        void _add_row(const char* label, const char* value, int color = TFT_CYAN);
        void _draw_row(int y, const char* label, const char* value, int value_color = TFT_CYAN);
        std::string _format_uptime(uint32_t ms);
        static const char* _preset_name(int preset);

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppStats_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "STATS"; }
        std::string getAppDesc() override { return "Network statistics"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_stats, nullptr)); }
        void* newApp() override { return new AppStats; }
        void deleteApp(void* app) override { delete (AppStats*)app; }
    };

} // namespace MOONCAKE::APPS
