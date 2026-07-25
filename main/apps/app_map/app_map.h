/**
 * @file app_map.h
 * @brief Dedicated Interactive MAP launcher packer for MeshCore UI
 */
#pragma once
#include <mooncake.h>
#include "assets/app_map.h"
#include "apps/utils/icon/icon_define.h"

namespace MOONCAKE::APPS
{
    class AppMap_Packer : public APP_PACKER_BASE
    {
    public:
        std::string getAppName() override { return "MAP"; }
        std::string getAppDesc() override { return "Interactive map & nodes"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_map, nullptr)); }
        void* newApp() override;
        void deleteApp(void* app) override;
    };
} // namespace MOONCAKE::APPS
