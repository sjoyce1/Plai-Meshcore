/**
 * @file cardputer.cpp
 * @author d4rkmen
 * @brief Meshtastic client application for M5Stack CardPuter
 * @version 2.0
 * @date 2025-01-03
 *
 * @copyright Copyright (c) 2025
 *
 */
#include <stdio.h>
#include "mooncake.h"
#include "hal/hal_cardputer.h"
#include "settings/settings.h"
#include "apps/apps.h"
#include "esp_log.h"

static const char* TAG = "MAIN";

using namespace HAL;
using namespace SETTINGS;
using namespace MOONCAKE;

Settings settings;
HalCardputer hal(&settings);
Mooncake mooncake;

void _data_base_setup_callback(SIMPLEKV::SimpleKV& db)
{
    db.Add<HAL::Hal*>("HAL", &hal);
    db.Add<SETTINGS::Settings*>("SETTINGS", &settings);
}

#include "mesh/console_logger.h"

extern "C" void app_main(void)
{
    // Start capturing console logs for the Live Console Monitor app
    Mesh::ConsoleLogger::getInstance().init();

    // Settings init
    settings.init();
    settings.setHal(&hal);
    Settings::applyTimezone(settings.getString("system", "timezone"));

    // Init hal (includes radio and mesh service initialization)
    hal.init();

    // Init framework
    mooncake.setDatabaseSetupCallback(_data_base_setup_callback);
    mooncake.init();

    // Install launcher
    auto launcher = new APPS::Launcher_Packer;
    mooncake.installApp(launcher);

    // Install apps: Contacts, Channels, Monitor, Stats, Settings
    mooncake.installApp(new APPS::AppNodes_Packer);
    mooncake.installApp(new APPS::AppChannels_Packer);
    mooncake.installApp(new APPS::AppMonitor_Packer);
    mooncake.installApp(new APPS::AppStats_Packer);
    mooncake.installApp(new APPS::AppSettings_Packer);

    // Mount SD card if available
    if (hal.sdcard())
    {
        hal.sdcard()->mount(false);
        if (hal.sdcard()->is_mounted())
        {
            // Start mesh service (LoRa radio)
            if (!hal.startMesh())
            {
                ESP_LOGW(TAG, "Mesh service failed to start, continuing without radio");
            }
            else
            {
                ESP_LOGI(TAG, "Meshtastic client ready");
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "SD card not found, continuing without radio");
    }
    // Create launcher
    mooncake.createApp(launcher);

    // Register main task for interrupt-driven wakeup during sleep
    TaskHandle_t main_task = xTaskGetCurrentTaskHandle();
    hal.keyboard()->setNotifyTask(main_task);
#if HAL_USE_RADIO
    if (hal.radio())
        hal.radio()->setNotifyTask(main_task);
#endif

    // Main loop
    while (1)
    {

        // Update mesh service (process BLE/radio events)
        hal.updateMesh();
        delay(1);
        if (hal.isDisplaySleeping())
        {
            hal.keyboard()->updateKeyList();
            xTaskNotifyWait(0, UINT32_MAX, NULL, pdMS_TO_TICKS(hal.hasPendingTx() ? 1 : 1000));
        }
        else
        {
            // Update UI framework
            mooncake.update();
        }
    }
}
