/**
 * @file app_map.cpp
 * @brief Map app implementation delegating to AppNodes map mode
 */
#include "app_map.h"
#include "apps/app_nodes/app_nodes.h"

namespace MOONCAKE::APPS
{
    void* AppMap_Packer::newApp()
    {
        AppNodes::setLaunchInMapMode(true);
        return new AppNodes;
    }

    void AppMap_Packer::deleteApp(void* app)
    {
        delete (AppNodes*)app;
    }
} // namespace MOONCAKE::APPS
