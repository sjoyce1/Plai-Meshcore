/**
 * @file app_map.h
 * @brief Dedicated Interactive MAP application for MeshCore UI
 */
#pragma once
#include "../apps.h"
#include "apps/utils/theme/theme_define.h"
#include <string>
#include <vector>

#include "apps/app_graphs/assets/app_graphs.h"

namespace MOONCAKE::APPS
{
    class AppMap : public APP_BASE
    {
    private:
        struct Data_t
        {
            HAL::Hal* hal = nullptr;
            double map_center_lat = 0.0;
            double map_center_lon = 0.0;
            int map_zoom = 14;
            int map_style_idx = 0;
            char map_tile_dir[128] = {};
            uint32_t selected_node_id = 0;
            int focused_node_idx = -1;
            uint32_t last_update = 0;
        };

        Data_t _data;

        void _init_map_center();
        bool _render_map_view();
        void _handle_input();

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppMap_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "MAP"; }
        std::string getAppDesc() override { return "Interactive map & nodes"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_graphs, nullptr)); }
        void* newApp() override { return new AppMap; }
        void deleteApp(void* app) override { delete (AppMap*)app; }
    };

} // namespace MOONCAKE::APPS
