/**
 * @file settings.cpp
 * @brief Settings management system implementation
 */

#include "settings.h"
#include "hal/hal.h"
#include "apps/utils/ui/dialog.h"
#include "mesh/mesh_service.h"
#include "mesh/node_db.h"
#include "mbedtls/base64.h"
#include <format>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <cstdlib>
#include <cstring>

static const char* TAG = "SETTINGS";
static const std::string SETTINGS_FILE_NAME = "/sdcard/settings.txt";

namespace SETTINGS
{
    // partition names in order of priority
    const char* const Settings::NVS_PARTITIONS[] = {"nvs", "apps_nvs", nullptr};

    Settings::Settings() : _initialized(false)
    {
        // back item
        SettingItem_t back_item = {"", "[..]", TYPE_NONE, "back", "back", "", "", "Return back to the parent menu"};

        SettingGroup_t sys_group;
        sys_group.name = "System settings";
        sys_group.nvs_namespace = "system";
        sys_group.items = {
            back_item,
            {"brightness", "Brightness", TYPE_NUMBER, "100", "100", "10", "255", "Screen brightness level (10-255)"},
            {"volume",
             "Volume",
             TYPE_NUMBER,
             "64",
             "64",
             "0",
             "255",
             "System sound volume level (0-255)",
             [this](SettingItem_t& item)
             {
                 if (_hal && _hal->speaker())
                     _hal->speaker()->setVolume(std::stoi(item.value));
             }},
            {"dim_time",
             "Dim seconds",
             TYPE_NUMBER,
             "30",
             "30",
             "0",
             "3600",
             "Screen dimming time in seconds (0-3600)",
             [this](SettingItem_t& item)
             {
                 if (_hal && _hal->keyboard())
                     _hal->keyboard()->set_dim_time(std::stoi(item.value) * 1000);
             }},
            {"boot_sound", "Boot sound", TYPE_BOOL, "true", "true", "", "", "Play boot sound on startup"},
            {"show_bat_volt", "Battery voltage", TYPE_BOOL, "true", "true", "", "", "Show battery voltage on the system bar"},
            {"show_time", "Show time", TYPE_BOOL, "true", "true", "", "", "Show time on the system bar"},
            {"timezone",
             "Timezone",
             TYPE_STRING,
             "US-Central",
             "US-Central",
             "US-Central;US-Eastern;US-Mountain;US-Pacific;GMT-12;GMT-11;GMT-10;GMT-9:30;GMT-9;GMT-8;GMT-7;GMT-6;GMT-5;GMT-4;GMT-3:30;GMT-3;GMT-2;GMT-1;"
             "GMT+0;GMT+1;GMT+2;GMT+3;GMT+3:30;GMT+4;GMT+4:30;GMT+5;GMT+5:30;GMT+5:45;"
             "GMT+6;GMT+6:30;GMT+7;GMT+8;GMT+8:45;GMT+9;GMT+9:30;GMT+10;GMT+10:30;GMT+11;GMT+12;GMT+13;GMT+14",
             "",
             "Timezone selection",
             [](SettingItem_t& item) { applyTimezone(item.value); }},
            {"set_time",
             "Set clock (HH:MM)...",
             TYPE_CALLBACK,
             "",
             "",
             "",
             "",
             "Manually set local clock time (24h format)",
             [this](SettingItem_t& item)
             {
                 if (!_hal) return;
                 std::string time_val = "12:00";
                 if (UTILS::UI::show_edit_string_dialog(_hal, "Set Time (HH:MM)", time_val, false, 5))
                 {
                     int hh = 0, mm = 0;
                     if (sscanf(time_val.c_str(), "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60)
                     {
                         time_t now = time(nullptr);
                         struct tm timeinfo;
                         localtime_r(&now, &timeinfo);
                         timeinfo.tm_hour = hh;
                         timeinfo.tm_min = mm;
                         timeinfo.tm_sec = 0;
                         time_t new_time = mktime(&timeinfo);
                         if (new_time != -1)
                         {
                             struct timeval tv = { .tv_sec = new_time, .tv_usec = 0 };
                             settimeofday(&tv, nullptr);
                             UTILS::UI::show_message_dialog(_hal, "Success", "Clock updated!", 1500);
                         }
                     }
                     else
                     {
                         UTILS::UI::show_error_dialog(_hal, "Error", "Use 24h format HH:MM (e.g. 14:30)");
                     }
                 }
             }},
            {"map_style",
             "Map style",
             TYPE_STRING,
             "osm",
             "osm",
             "osm;dark;voyager;topo",
             "",
             "Offline map style (map folder on SD card)"},
        };

        auto mesh_apply_cb = [this](SettingItem_t& item) { applyMeshConfig(item); };
        auto nodeinfo_apply_cb = [this](SettingItem_t& item)
        {
            applyMeshConfig(item);
            if (_hal && _hal->mesh())
                _hal->mesh()->forceNodeInfoBroadcast();
        };
        // Node identity
        SettingGroup_t nodeinfo_group;
        nodeinfo_group.name = "Node identity";
        nodeinfo_group.nvs_namespace = "nodeinfo";
        nodeinfo_group.items = {
            back_item,
            {"long_name", "Owner name", TYPE_STRING, "Cardputer", "Cardputer", "", "40", "Node owner name broadcasted over mesh", nodeinfo_apply_cb},
            {"short_name", "Short handle", TYPE_STRING, "6A6E", "6A6E", "", "4", "4-character node handle", nodeinfo_apply_cb},
        };

        // LoRa radio settings
        SettingGroup_t lora_group;
        lora_group.name = "LoRa radio config";
        lora_group.nvs_namespace = "lora";
        lora_group.items = {
            back_item,
            {"frequency",
             "Frequency (MHz)",
             TYPE_STRING,
             "910.525",
             "910.525",
             "433.0;868.0;902.0;910.525;915.0;923.0",
             "",
             "Operating frequency in MHz",
             mesh_apply_cb},
            {"bandwidth",
             "Bandwidth (kHz)",
             TYPE_STRING,
             "62.5",
             "62.5",
             "7.8;15.6;31.25;62.5;125.0;250.0;500.0",
             "",
             "LoRa signal bandwidth in kHz",
             mesh_apply_cb},
            {"spread_factor",
             "Spreading factor",
             TYPE_NUMBER,
             "7",
             "7",
             "7",
             "12",
             "LoRa spreading factor (SF7 - SF12)",
             mesh_apply_cb},
            {"coding_rate",
             "Coding rate",
             TYPE_NUMBER,
             "5",
             "5",
             "5",
             "8",
             "Coding rate denominator 4/N (5 = 4/5)",
             mesh_apply_cb},
            {"tx_power",
             "TX power (dBm)",
             TYPE_NUMBER,
             "22",
             "22",
             "-9",
             "22",
             "Transmit power in dBm (-9 to 22)",
             mesh_apply_cb},
            {"sync_word",
             "Sync word",
             TYPE_NUMBER,
             "18",
             "18",
             "0",
             "255",
             "Radio sync word (18 = MeshCore 0x12)",
             mesh_apply_cb},
        };

        // MeshCore network settings
        SettingGroup_t meshcore_group;
        meshcore_group.name = "MeshCore network";
        meshcore_group.nvs_namespace = "meshcore";
        meshcore_group.items = {
            back_item,
            {"advert_interval",
             "Advert secs",
             TYPE_NUMBER,
             "30",
             "30",
             "0",
             "3600",
             "Periodic advert broadcast interval in seconds (0 = off)",
             mesh_apply_cb},
            {"hop_limit",
             "Max hops",
             TYPE_NUMBER,
             "3",
             "3",
             "1",
             "7",
             "Maximum mesh routing hops (1-7)",
             mesh_apply_cb},
            {"primary_psk",
             "Public PSK",
             TYPE_STRING,
             "izOH6cXN6mrJ5e26oRXNcg==",
             "izOH6cXN6mrJ5e26oRXNcg==",
             "",
             "",
             "Base64 primary public channel PSK",
             mesh_apply_cb},
        };

        // Security settings
        SettingGroup_t security_group;
        security_group.name = "Security & keys";
        security_group.nvs_namespace = "security";
        security_group.items = {
            back_item,
            {"private_key",
             "Private key",
             TYPE_STRING,
             "",
             "",
             "",
             "",
             "Ed25519 private key (base64). Auto-generated if empty",
             mesh_apply_cb},
            {"derive_key",
             "Derive public key...",
             TYPE_CALLBACK,
             "",
             "",
             "",
             "",
             "Derive public key from private key",
             [this](SettingItem_t& item)
             {
                 if (_hal)
                 {
                     bool confirm = UTILS::UI::show_confirmation_dialog(_hal,
                                                                        "Confirm",
                                                                        "Derive public key from private key?",
                                                                        "Yes",
                                                                        "No");
                     if (!confirm)
                         return;
                 }
                 std::string priv_b64 = getString("security", "private_key");
                 if (priv_b64.empty())
                 {
                     if (_hal)
                         UTILS::UI::show_error_dialog(_hal, "Error", "No private key in settings", "OK");
                     return;
                 }
                 uint8_t priv_key[32];
                 size_t priv_len = 0;
                 if (mbedtls_base64_decode(priv_key, 32, &priv_len, (const unsigned char*)priv_b64.c_str(), priv_b64.size()) !=
                         0 ||
                     priv_len != 32)
                 {
                     if (_hal)
                         UTILS::UI::show_error_dialog(_hal, "Error", "Invalid private key", "OK");
                     return;
                 }
                 uint8_t pub_key[32];
                 bool ok = Mesh::MeshService::derivePublicFromPrivate(priv_key, pub_key);
                 if (ok)
                 {
                     unsigned char b64[48] = {};
                     size_t b64_len = 0;
                     if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, pub_key, 32) == 0)
                     {
                         setString("security", "public_key", std::string((char*)b64, b64_len));
                         applyMeshConfig(item);
                     }
                     else
                     {
                         if (_hal)
                             UTILS::UI::show_error_dialog(_hal, "Error", "Failed to encode public key", "OK");
                     }
                 }
                 else
                 {
                     if (_hal)
                         UTILS::UI::show_error_dialog(_hal, "Error", "Failed to derive public key", "OK");
                 }
             }},
            {"public_key",
             "Public key",
             TYPE_STRING,
             "",
             "",
             "",
             "",
             "Ed25519 public key (base64). Auto-generated if empty",
             mesh_apply_cb},
            {"regen_keys",
             "Regenerate keys...",
             TYPE_CALLBACK,
             "",
             "",
             "",
             "",
             "Generate new Ed25519 key pair",
             [this](SettingItem_t& item)
             {
                 if (_hal)
                 {
                     bool confirm = UTILS::UI::show_confirmation_dialog(_hal, "Confirm", "Regenerate the keys?", "Yes", "No");
                     if (!confirm)
                         return;
                 }
                 uint8_t priv_key[32], pub_key[32];
                 bool ok = Mesh::MeshService::generateKeypair(priv_key, pub_key);
                 if (ok)
                 {
                     unsigned char b64[48] = {};
                     size_t b64_len = 0;
                     if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, priv_key, 32) == 0)
                         setString("security", "private_key", std::string((char*)b64, b64_len));
                     b64_len = 0;
                     if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, pub_key, 32) == 0)
                         setString("security", "public_key", std::string((char*)b64, b64_len));
                     applyMeshConfig(item);
                 }
                 else
                 {
                     if (_hal)
                     {
                         UTILS::UI::show_error_dialog(_hal, "Error", "Key generation failed", "OK");
                     }
                 }
             }},
            {"clear_nodes",
             "Clear all nodes...",
             TYPE_CALLBACK,
             "",
             "",
             "",
             "",
             "Delete all nodes, DMs and conversation logs",
             [this](SettingItem_t& item)
             {
                 if (!_hal || !_hal->nodedb())
                     return;
                 bool confirm =
                     UTILS::UI::show_confirmation_dialog(_hal, "Confirm", "Delete all nodes and messages?", "Yes", "No");
                 if (!confirm)
                     return;
                 _hal->nodedb()->clearNodes();
                 _hal->nodedb()->save();
                 UTILS::UI::show_error_dialog(_hal, "Done", "All nodes cleared", "OK");
             }},
        };

        // GPS settings
        SettingGroup_t gps_group;
        gps_group.name = "GPS config";
        gps_group.nvs_namespace = "gps";
        gps_group.items = {
            back_item,
            {"enabled",
             "GPS Enabled",
             TYPE_BOOL,
             "true",
             "true",
             "",
             "",
             "Enable/disable ATGM336H GPS UART driver",
             [this](SettingItem_t& item)
             {
                 bool en = (item.value == "true");
                 if (_hal && _hal->gps())
                 {
                     if (en)
                         _hal->gps()->init();
                     else
                         _hal->gps()->deinit();
                 }
             }},
            {"sync_gps_time",
             "Sync time from GPS now...",
             TYPE_CALLBACK,
             "",
             "",
             "",
             "",
             "Query GPS fix and sync system clock with satellite time",
             [this](SettingItem_t& item)
             {
                 if (!_hal) return;
                 if (!_hal->gps() || !_hal->gps()->isInitialized())
                 {
                     UTILS::UI::show_error_dialog(_hal, "GPS Disabled", "GPS module is turned off in settings.", "OK");
                     return;
                 }
                 auto data = _hal->gps()->getData();
                 if (!data.has_fix)
                 {
                     std::string msg = "Searching for satellites...\nSats in view: " + std::to_string(data.sats_in_view) + "\nSentences: " + std::to_string(data.sentence_count);
                     UTILS::UI::show_error_dialog(_hal, "No GPS Fix", msg.c_str(), "OK");
                     return;
                 }
                 if (data.time > 1700000000)
                 {
                     struct timeval tv = { .tv_sec = (time_t)data.time, .tv_usec = 0 };
                     settimeofday(&tv, nullptr);
                     _hal->setGPSAdjusted(true);

                     time_t now = time(nullptr);
                     struct tm timeinfo;
                     localtime_r(&now, &timeinfo);
                     char time_buf[32];
                     strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &timeinfo);

                     std::string success_msg = "Clock synced to GPS!\nTime: " + std::string(time_buf) + "\nSats used: " + std::to_string(data.sats_used);
                     UTILS::UI::show_message_dialog(_hal, "GPS Time Synced", success_msg, 3000);
                 }
                 else
                 {
                     UTILS::UI::show_error_dialog(_hal, "Time Not Ready", "GPS fix acquired but satellite date/time is not valid yet.", "OK");
                 }
             }},
        };

        SettingGroup_t export_group;
        export_group.name = "Export (SD card)";
        export_group.items = {};
        export_group.callback = [this](SETTINGS::SettingGroup_t& group)
        {
            bool sdcard_mounted = _hal->sdcard()->is_mounted();
            if (!sdcard_mounted)
                _hal->sdcard()->mount(false);
            if (_hal->sdcard()->is_mounted())
            {
                exportToFile(SETTINGS_FILE_NAME);
                if (!sdcard_mounted)
                    _hal->sdcard()->eject();
                UTILS::UI::show_message_dialog(_hal, "Success", "Settings saved to: " + SETTINGS_FILE_NAME, 0);
            }
            else
            {
                UTILS::UI::show_message_dialog(_hal, "Error", "Failed to mount SD card", 0);
            }
        };

        SettingGroup_t import_group;
        import_group.name = "Import (SD card)";
        import_group.items = {};
        import_group.callback = [this](SETTINGS::SettingGroup_t& group)
        {
            bool sdcard_mounted = _hal->sdcard()->is_mounted();
            if (!sdcard_mounted)
                _hal->sdcard()->mount(false);
            if (_hal->sdcard()->is_mounted())
            {
                importFromFile(SETTINGS_FILE_NAME);
                if (!sdcard_mounted)
                    _hal->sdcard()->eject();
                UTILS::UI::show_message_dialog(_hal, "Success", "Loaded from: " + SETTINGS_FILE_NAME, 0);
            }
            else
            {
                UTILS::UI::show_error_dialog(_hal, "Error", "Failed to mount SD card", "OK");
            }
        };

        _metadata = {sys_group,
                     nodeinfo_group,
                     lora_group,
                     meshcore_group,
                     security_group,
                     gps_group,
                     export_group,
                     import_group};
    }

    void Settings::applyTimezone(const std::string& tz)
    {
        // POSIX sign convention is inverted: UTC+2 = "<GMT+2>-2"
        static const struct
        {
            const char* label;
            const char* posix;
        } tz_table[] = {
            {"US-Central", "CST6CDT,M3.2.0,M11.1.0"},
            {"US-Eastern", "EST5EDT,M3.2.0,M11.1.0"},
            {"US-Mountain", "MST7MDT,M3.2.0,M11.1.0"},
            {"US-Pacific", "PST8PDT,M3.2.0,M11.1.0"},
            {"GMT-12", "<GMT-12>12"},        {"GMT-11", "<GMT-11>11"},        {"GMT-10", "<GMT-10>10"},
            {"GMT-9:30", "<GMT-9:30>9:30"},  {"GMT-9", "<GMT-9>9"},           {"GMT-8", "<GMT-8>8"},
            {"GMT-7", "<GMT-7>7"},           {"GMT-6", "<GMT-6>6"},           {"GMT-5", "<GMT-5>5"},
            {"GMT-4", "<GMT-4>4"},           {"GMT-3:30", "<GMT-3:30>3:30"},  {"GMT-3", "<GMT-3>3"},
            {"GMT-2", "<GMT-2>2"},           {"GMT-1", "<GMT-1>1"},           {"GMT+0", "GMT0"},
            {"GMT+1", "<GMT+1>-1"},          {"GMT+2", "<GMT+2>-2"},          {"GMT+3", "<GMT+3>-3"},
            {"GMT+3:30", "<GMT+3:30>-3:30"}, {"GMT+4", "<GMT+4>-4"},          {"GMT+4:30", "<GMT+4:30>-4:30"},
            {"GMT+5", "<GMT+5>-5"},          {"GMT+5:30", "<GMT+5:30>-5:30"}, {"GMT+5:45", "<GMT+5:45>-5:45"},
            {"GMT+6", "<GMT+6>-6"},          {"GMT+6:30", "<GMT+6:30>-6:30"}, {"GMT+7", "<GMT+7>-7"},
            {"GMT+8", "<GMT+8>-8"},          {"GMT+8:45", "<GMT+8:45>-8:45"}, {"GMT+9", "<GMT+9>-9"},
            {"GMT+9:30", "<GMT+9:30>-9:30"}, {"GMT+10", "<GMT+10>-10"},       {"GMT+10:30", "<GMT+10:30>-10:30"},
            {"GMT+11", "<GMT+11>-11"},       {"GMT+12", "<GMT+12>-12"},       {"GMT+13", "<GMT+13>-13"},
            {"GMT+14", "<GMT+14>-14"},
        };

        const char* posix_tz = "GMT0"; // fallback to UTC
        for (const auto& entry : tz_table)
        {
            if (tz == entry.label)
            {
                posix_tz = entry.posix;
                break;
            }
        }

        setenv("TZ", posix_tz, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone applied: %s -> %s", tz.c_str(), posix_tz);
        // current dattetime is
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        ESP_LOGW(TAG, "Current date and time: %s", asctime(&timeinfo));
    }

    void Settings::setHal(HAL::Hal* hal) { _hal = hal; }

    Settings::~Settings()
    {
        if (_initialized)
        {
            _deinitNvs();
        }
    }

    bool Settings::init()
    {
        if (_initialized)
        {
            return true;
        }
        ESP_LOGW(TAG, "Settings init");

        if (!_initNvs())
        {
            return false;
        }

        _loadSettings();
        _initialized = true;
        return true;
    }

    std::vector<SettingGroup_t> Settings::getMetadata() const { return _metadata; }

    std::vector<SettingGroup_t>& Settings::getMetadataRef() { return _metadata; }

    bool Settings::_initNvs()
    {
        for (const char* const* p = NVS_PARTITIONS; *p; ++p)
        {
            esp_err_t err = nvs_flash_init_partition(*p);
            if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
            {
                ESP_LOGW(TAG, "NVS partition '%s' truncated or new version, erasing...", *p);
                err = nvs_flash_erase_partition(*p);
                if (err == ESP_OK)
                    err = nvs_flash_init_partition(*p);
            }
            if (err == ESP_OK)
            {
                _active_partition = *p;
                ESP_LOGI(TAG, "NVS initialized on partition '%s'", *p);
                return true;
            }
            ESP_LOGW(TAG, "Partition '%s' failed: %s", *p, esp_err_to_name(err));
        }

        ESP_LOGE(TAG, "Failed to initialize NVS on any partition");
        return false;
    }

    void Settings::_deinitNvs()
    {
        if (!_active_partition)
            return;
        esp_err_t err = nvs_flash_deinit_partition(_active_partition);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to deinitialize NVS partition '%s': %s", _active_partition, esp_err_to_name(err));
        }
        _active_partition = nullptr;
    }

    std::string Settings::_makeKey(const std::string& ns, const std::string& key) const
    {
        return std::format("{}-{}", ns, key);
    }

    const SettingItem_t* Settings::_findItem(const std::string& ns, const std::string& key) const
    {
        for (const auto& group : _metadata)
        {
            if (group.nvs_namespace == ns)
            {
                for (const auto& item : group.items)
                {
                    if (item.key == key)
                    {
                        return &item;
                    }
                }
            }
        }
        return nullptr;
    }

    void Settings::_loadSettings()
    {
        for (const auto& group : _metadata)
        {
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open_from_partition(_active_partition, group.nvs_namespace.c_str(), NVS_READONLY, &nvs_handle);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error opening NVS namespace %s", group.nvs_namespace.c_str());
                continue;
            }

            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                CachedValue cached_value;
                cached_value.type = item.type;

                switch (item.type)
                {
                case TYPE_BOOL:
                {
                    uint8_t value;
                    if (nvs_get_u8(nvs_handle, item.key, &value) != ESP_OK)
                    {
                        value = strcmp(item.default_val, "true") == 0 ? 1 : 0;
                    }
                    cached_value.bool_val = value == 1;
                    ESP_LOGI(TAG, "Loaded bool %s = %d", cache_key.c_str(), cached_value.bool_val);
                    break;
                }
                case TYPE_NUMBER:
                {
                    int32_t value;
                    if (nvs_get_i32(nvs_handle, item.key, &value) != ESP_OK)
                    {
                        value = atoi(item.default_val);
                    }
                    cached_value.num_val = value;
                    ESP_LOGI(TAG, "Loaded number %s = %ld", cache_key.c_str(), cached_value.num_val);
                    break;
                }
                case TYPE_STRING:
                {
                    size_t required_size = 0;
                    if (nvs_get_str(nvs_handle, item.key, nullptr, &required_size) == ESP_OK)
                    {
                        std::vector<char> value(required_size);
                        if (nvs_get_str(nvs_handle, item.key, value.data(), &required_size) == ESP_OK)
                        {
                            cached_value.str_val = std::string(value.data());
                        }
                    }
                    if (cached_value.str_val.empty())
                    {
                        cached_value.str_val = item.default_val;
                    }
                    ESP_LOGI(TAG, "Loaded string %s = %s", cache_key.c_str(), cached_value.str_val.c_str());
                    break;
                }
                default:
                    break;
                }

                _cache[cache_key] = cached_value;
            }
            nvs_close(nvs_handle);
        }
    }

    bool Settings::getBool(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_BOOL)
        {
            return it->second.bool_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? (strcmp(item->default_val, "true") == 0) : false;
    }

    int32_t Settings::getNumber(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_NUMBER)
        {
            return it->second.num_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? atoi(item->default_val) : 0;
    }

    std::string Settings::getString(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_STRING)
        {
            return it->second.str_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? item->default_val : "";
    }

    bool Settings::setBool(const std::string& ns, const std::string& key, bool value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_BOOL)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_BOOL;
        cached_value.bool_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(_active_partition, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_u8(nvs_handle, key.c_str(), value ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::setNumber(const std::string& ns, const std::string& key, int32_t value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_NUMBER)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_NUMBER;
        cached_value.num_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(_active_partition, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_i32(nvs_handle, key.c_str(), value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::setString(const std::string& ns, const std::string& key, const std::string& value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_STRING)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_STRING;
        cached_value.str_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(_active_partition, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_str(nvs_handle, key.c_str(), value.c_str());
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::saveAll()
    {
        bool success = true;
        for (const auto& group : _metadata)
        {
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open_from_partition(_active_partition, group.nvs_namespace.c_str(), NVS_READWRITE, &nvs_handle);
            if (err != ESP_OK)
            {
                success = false;
                continue;
            }

            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                auto it = _cache.find(cache_key);
                if (it == _cache.end())
                    continue;

                switch (item.type)
                {
                case TYPE_BOOL:
                    err = nvs_set_u8(nvs_handle, item.key, it->second.bool_val ? 1 : 0);
                    break;
                case TYPE_NUMBER:
                    err = nvs_set_i32(nvs_handle, item.key, it->second.num_val);
                    break;
                case TYPE_STRING:
                    err = nvs_set_str(nvs_handle, item.key, it->second.str_val.c_str());
                    break;
                default:
                    break;
                }

                if (err != ESP_OK)
                {
                    success = false;
                }
            }

            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        return success;
    }

    bool Settings::exportToFile(const std::string& filename) const
    {
        ESP_LOGI(TAG, "Exporting settings to %s", filename.c_str());

        std::map<std::string, std::string> existing_settings;

        std::ifstream infile(filename);
        if (infile.is_open())
        {
            std::string line;
            while (std::getline(infile, line))
            {
                line.erase(0, line.find_first_not_of(" \t\n\r"));
                line.erase(line.find_last_not_of(" \t\n\r") + 1);

                if (line.empty() || line[0] == '#')
                {
                    continue;
                }

                size_t equals_pos = line.find('=');
                if (equals_pos == std::string::npos)
                {
                    ESP_LOGW(TAG, "Skipping invalid line during export pre-read: %s", line.c_str());
                    continue;
                }

                std::string cache_key = line.substr(0, equals_pos);
                std::string value_str = line.substr(equals_pos + 1);

                size_t separator_pos = cache_key.find('-');
                if (separator_pos == std::string::npos)
                {
                    ESP_LOGW(TAG,
                             "Invalid key format (missing namespace separator '-') in existing file: %s",
                             cache_key.c_str());
                    continue;
                }
                existing_settings[cache_key] = value_str;
            }
            infile.close();
            ESP_LOGI(TAG, "Read file %s. Found %d settings", filename.c_str(), existing_settings.size());
        }
        else
        {
            ESP_LOGI(TAG, "File %s does not exist, creating new", filename.c_str());
        }
        // replacing settings in map with current values
        for (const auto& group : _metadata)
        {
            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                auto it = _cache.find(cache_key);
                if (it == _cache.end())
                {
                    ESP_LOGW(TAG, "Setting %s not found in cache during export, skipping", cache_key.c_str());
                    continue;
                }

                // outfile << cache_key << "=";
                std::string str_val;
                switch (item.type)
                {
                case TYPE_BOOL:
                    str_val = (it->second.bool_val ? "true" : "false");
                    break;
                case TYPE_NUMBER:
                    str_val = std::to_string(it->second.num_val);
                    break;
                case TYPE_STRING:
                {
                    std::string escaped_str;
                    for (char c : it->second.str_val)
                    {
                        if (c == '\n')
                        {
                            escaped_str += "\\n";
                        }
                        else
                        {
                            escaped_str += c;
                        }
                    }
                    str_val = escaped_str;
                }
                break;
                default:
                    break;
                }
                // rewriting if exists
                existing_settings[cache_key] = str_val;
            }
        }
        // saving to file
        std::ofstream outfile(filename);
        if (!outfile.is_open())
        {
            ESP_LOGE(TAG, "Failed to open file %s for writing", filename.c_str());
            return false;
        }
        for (const auto& [key, value] : existing_settings)
        {
            outfile << key << "=" << value << std::endl;
        }
        outfile.close();
        ESP_LOGI(TAG, "Settings successfully exported to %s", filename.c_str());
        return true;
    }

    bool Settings::importFromFile(const std::string& filename)
    {
        ESP_LOGI(TAG, "Importing settings from %s", filename.c_str());
        std::ifstream infile(filename);
        if (!infile.is_open())
        {
            ESP_LOGE(TAG, "Failed to open file %s for reading", filename.c_str());
            return false;
        }

        std::string line;
        bool success = false;
        int line_num = 0;

        while (std::getline(infile, line))
        {
            line_num++;
            line.erase(0, line.find_first_not_of(" \t\n\r"));
            line.erase(line.find_last_not_of(" \t\n\r") + 1);

            if (line.empty() || line[0] == '#')
                continue;

            size_t equals_pos = line.find('=');
            if (equals_pos == std::string::npos)
            {
                ESP_LOGW(TAG, "Invalid format on line %d in %s: %s", line_num, filename.c_str(), line.c_str());
                continue;
            }

            std::string cache_key = line.substr(0, equals_pos);
            std::string value_str = line.substr(equals_pos + 1);

            size_t separator_pos = cache_key.find('-');
            if (separator_pos == std::string::npos)
            {
                ESP_LOGW(TAG,
                         "Invalid key format on line %d (missing namespace separator '-'): %s",
                         line_num,
                         cache_key.c_str());
                continue;
            }
            std::string ns = cache_key.substr(0, separator_pos);
            std::string key = cache_key.substr(separator_pos + 1);

            const SettingItem_t* item = _findItem(ns, key);
            if (!item)
            {
                ESP_LOGW(TAG,
                         "Setting %s (ns=%s, key=%s) not found in metadata, skipping line %d",
                         cache_key.c_str(),
                         ns.c_str(),
                         key.c_str(),
                         line_num);
                continue;
            }

            bool import_ok = false;
            switch (item->type)
            {
            case TYPE_BOOL:
            {
                bool val = (value_str == "true");
                if (value_str != "true" && value_str != "false")
                {
                    ESP_LOGW(TAG,
                             "Invalid boolean value '%s' for %s on line %d, using default",
                             value_str.c_str(),
                             cache_key.c_str(),
                             line_num);
                    val = (strcmp(item->default_val, "true") == 0);
                }
                import_ok = setBool(ns, key, val);
                break;
            }
            case TYPE_NUMBER:
            {
                int32_t val = std::stoi(value_str);
                import_ok = setNumber(ns, key, val);
                break;
            }
            case TYPE_STRING:
            {
                std::string unescaped_str;
                for (size_t i = 0; i < value_str.length(); ++i)
                {
                    if (value_str[i] == '\\' && i + 1 < value_str.length() && value_str[i + 1] == 'n')
                    {
                        unescaped_str += '\n';
                        i++; // Skip the 'n'
                    }
                    else
                    {
                        unescaped_str += value_str[i];
                    }
                }
                import_ok = setString(ns, key, unescaped_str);
                break;
            }
            case TYPE_NONE:
            default:
                break;
            }

            if (import_ok)
            {
                ESP_LOGI(TAG, "Imported setting: %s = %s", cache_key.c_str(), value_str.c_str());
                success = true;
            }
            else
            {
                ESP_LOGW(TAG, "Failed to import setting %s on line %d", cache_key.c_str(), line_num);
            }
        }

        infile.close();

        if (success)
        {
            ESP_LOGI(TAG, "Settings successfully imported from %s", filename.c_str());
            // apply timezone
            applyTimezone(getString("system", "timezone"));
            // apply dim_time
            if (_hal && _hal->keyboard())
                _hal->keyboard()->set_dim_time(getNumber("system", "dim_time") * 1000);
            // apply volume
            if (_hal && _hal->speaker())
                _hal->speaker()->setVolume(getNumber("system", "volume"));
            // Re-apply LoRa / mesh config so the radio immediately uses the imported values
            if (_hal && _hal->mesh())
            {
                Mesh::MeshConfig cfg = _hal->mesh()->getConfig();
                _hal->mesh()->loadConfigFromSettings(cfg);
                _hal->mesh()->setConfig(cfg);
                // force node info broadcast
                _hal->mesh()->forceNodeInfoBroadcast();
                ESP_LOGI(TAG, "Mesh/LoRa config re-applied after import");
            }
        }
        else
        {
            ESP_LOGW(TAG, "No settings were imported from %s", filename.c_str());
        }

        return success;
    }

    void Settings::applyMeshConfig(SettingItem_t& item)
    {
        ESP_LOGI(TAG, "Applying mesh config from setting: %s", item.key);
        if (!_hal || !_hal->mesh())
            return;

        // Get current config as base (preserves node_id, channel, etc.)
        Mesh::MeshConfig cfg = _hal->mesh()->getConfig();
        _hal->mesh()->loadConfigFromSettings(cfg);
        _hal->mesh()->setConfig(cfg);

        ESP_LOGI(TAG, "Mesh config applied from settings");
    }

} // namespace SETTINGS
