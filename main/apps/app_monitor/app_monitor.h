/**
 * @file app_monitor.h
 * @brief Monitor widget - Live serial console log viewer
 *
 */
#pragma once

#include "../apps.h"
#include <string>

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "apps/utils/anim/hl_text.h"

#include "assets/app_monitor.h"

namespace MOONCAKE::APPS
{

    class AppMonitor : public APP_BASE
    {
    public:
        enum class ViewState
        {
            PACKET_LIST,
            PACKET_DETAIL
        };

    private:
        struct
        {
            HAL::Hal* hal;
            ViewState view_state;

            int selected_index;
            int scroll_offset;
            uint32_t last_log_size;
            bool update_list;

            UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;
        } _data;

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppMonitor_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "MONITOR"; }
        std::string getAppDesc() override { return "Live console & debug log viewer"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_monitor, nullptr)); }
        void* newApp() override { return new AppMonitor; }
        void deleteApp(void* app) override { delete (AppMonitor*)app; }
    };

} // namespace MOONCAKE::APPS
