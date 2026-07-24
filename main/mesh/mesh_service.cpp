#include "mesh_service.h"
#include "meshcore_bridge.h"
#include "ed_25519.h"
#include "mesh_data.h"
#include "settings/settings.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

static const char* TAG = "MESH_SERVICE";

namespace Mesh
{
    const ModemPresetInfo modem_presets[MODEM_PRESET_COUNT] = {
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, "LongFast", "LongF", 250.0f, 812.5f, 5, 11},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW, "LongSlow", "LongS", 125.0f, 406.25f, 8, 12},
        {meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW, "VeryLongSlow", "VLongS", 62.5f, 203.125f, 8, 12},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW, "MediumSlow", "MedS", 250.0f, 812.5f, 5, 10},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST, "MediumFast", "MedF", 250.0f, 812.5f, 5, 9},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW, "ShortSlow", "ShrtS", 250.0f, 812.5f, 5, 8},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST, "ShortFast", "ShrtF", 250.0f, 812.5f, 5, 7},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE, "LongMod", "LongM", 125.0f, 406.25f, 8, 11},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO, "ShortTurbo", "ShrtT", 500.0f, 1625.0f, 5, 7},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO, "LongTurbo", "LongT", 500.0f, 1625.0f, 8, 11},
    };

    static meshtastic_Config_DeviceConfig_Role roleFromName(const std::string& name)
    {
        if (name == "TAK") return meshtastic_Config_DeviceConfig_Role_TAK;
        if (name == "TAK Tracker") return meshtastic_Config_DeviceConfig_Role_TAK_TRACKER;
        if (name == "Lost&Found") return meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND;
        if (name == "Tracker") return meshtastic_Config_DeviceConfig_Role_TRACKER;
        if (name == "Sensor") return meshtastic_Config_DeviceConfig_Role_SENSOR;
        if (name == "Client Mute") return meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
        if (name == "Client Hidden") return meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN;
        if (name == "Client Base") return meshtastic_Config_DeviceConfig_Role_CLIENT_BASE;
        if (name == "Router") return meshtastic_Config_DeviceConfig_Role_ROUTER;
        if (name == "Router Client") return meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT;
        if (name == "Router Late") return meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
        if (name == "Repeater") return meshtastic_Config_DeviceConfig_Role_REPEATER;
        return meshtastic_Config_DeviceConfig_Role_CLIENT;
    }

    static meshtastic_Config_DeviceConfig_RebroadcastMode rebroadcastModeFromName(const std::string& name)
    {
        if (name == "All skip decode") return meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING;
        if (name == "Local only") return meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
        if (name == "Known only") return meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY;
        if (name == "None") return meshtastic_Config_DeviceConfig_RebroadcastMode_NONE;
        return meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
    }

    static meshtastic_Config_LoRaConfig_ModemPreset modemPresetFromName(const std::string& name)
    {
        if (name == "LongSlow") return meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
        if (name == "VeryLongSlow") return meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW;
        if (name == "MediumSlow") return meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW;
        if (name == "MediumFast") return meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST;
        if (name == "ShortSlow") return meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW;
        if (name == "ShortFast") return meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST;
        if (name == "LongModerate") return meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE;
        if (name == "ShortTurbo") return meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO;
        if (name == "LongTurbo") return meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO;
        return meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    }

    static uint32_t parseIntervalToMs(const std::string& val)
    {
        if (val == "off" || val.empty()) return 0;
        if (val.back() == 'm') return std::stoul(val.substr(0, val.size() - 1)) * 60000;
        if (val.back() == 'h') return std::stoul(val.substr(0, val.size() - 1)) * 3600000;
        return 0;
    }

    MeshService* MeshService::_instance = nullptr;

    MeshService::MeshService(HAL::Hal* hal)
        : _hal(hal)
        , _saved_freq(0.0f)
        , _radio(nullptr)
        , _gps(nullptr)
        , _gps_queue(nullptr)
        , _nodedb(nullptr)
        , _bridge(nullptr)
        , _state(MeshState::UNINITIALIZED)
    {
        _instance = this;
    }

    MeshService::~MeshService()
    {
        if (_bridge)
        {
            delete _bridge;
        }
    }

    meshtastic_Config_LoRaConfig_RegionCode MeshService::regionCodeFromName(const std::string& name)
    {
        return meshtastic_Config_LoRaConfig_RegionCode_UNSET;
    }

    bool MeshService::generateKeypair(uint8_t* out_private, uint8_t* out_public)
    {
        esp_fill_random(out_private, 32);
        uint8_t prv_64[64];
        ed25519_create_keypair(out_public, prv_64, out_private);
        return true;
    }

    bool MeshService::derivePublicFromPrivate(const uint8_t* private_key, uint8_t* out_public)
    {
        uint8_t prv_64[64];
        ed25519_create_keypair(out_public, prv_64, private_key);
        return true;
    }

    bool MeshService::init(HAL::RadioInterface* radio, NodeDB* nodedb, const MeshConfig& config)
    {
        ESP_LOGI(TAG, "Initializing MeshService (MeshCore Bridge)");
        _radio = radio;
        _nodedb = nodedb;
        _config = config;

        _bridge = new mesh::MeshCoreBridge(radio, nodedb);
        _bridge->self_id.readFromSeed(_config.private_key);

        if (_config.long_name[0] == '\0')
        {
            snprintf(_config.long_name, sizeof(_config.long_name), "Cardputer-%04X", (unsigned int)(getNodeId() & 0xFFFF));
        }
        if (_config.short_name[0] == '\0')
        {
            snprintf(_config.short_name, sizeof(_config.short_name), "%04X", (unsigned int)(getNodeId() & 0xFFFF));
        }

        _bridge->setOnMessageReceived([this](uint32_t sender_id, uint32_t dest_id, uint32_t timestamp, const char* text, bool is_group, uint8_t channel) {
            Mesh::TextMessage msg;
            msg.id = esp_random();
            msg.from = sender_id;
            msg.to = dest_id;
            msg.timestamp = timestamp;
            msg.channel = channel;
            msg.is_direct = !is_group;
            msg.read = false;
            msg.text = text;
            msg.status = Mesh::TextMessage::Status::DELIVERED;

            Mesh::MeshDataStore::getInstance().addMessage(msg);

            if (this->_message_callback)
            {
                meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_default;
                packet.to = dest_id;
                packet.channel = channel;
                this->_message_callback(packet);
            }
        });

        _router.setBridge(_bridge);
        _state = MeshState::READY;
        return true;
    }

    bool MeshService::start()
    {
        if (_bridge)
        {
            _bridge->begin();
            _radio->startReceive(0);
            ESP_LOGI(TAG, "Broadcasting self advert on startup");
            sendNodeInfo(0xFFFFFFFF, 0, false);
        }
        return true;
    }

    void MeshService::stop()
    {
    }

    void MeshService::update()
    {
        if (_bridge)
        {
            _bridge->loop();

            static uint32_t last_advert_ts = 0;
            uint32_t now = esp_timer_get_time() / 1000000;
            if (last_advert_ts == 0)
            {
                last_advert_ts = now;
            }
            else if (now - last_advert_ts >= 30) // Broadcast advert every 30 seconds for fast discovery
            {
                last_advert_ts = now;
                ESP_LOGI(TAG, "Broadcasting periodic self advert");
                sendNodeInfo(0xFFFFFFFF, 0, false);
            }
        }
    }

    uint32_t MeshService::sendText(const char* text, uint32_t dest, uint8_t channel)
    {
        if (!_bridge || !text || strlen(text) == 0)
        {
            return 0;
        }

        bool success = false;
        if (dest == 0xFFFFFFFF)
        {
            auto* ch = _nodedb->getChannel(channel);
            if (ch && ch->has_settings && ch->settings.psk.size > 0)
            {
                mesh::GroupChannel group_ch;
                memset(group_ch.secret, 0, 32);
                memcpy(group_ch.secret, ch->settings.psk.bytes, ch->settings.psk.size);
                int key_len = (ch->settings.psk.size == 16) ? 16 : 32;
                mesh::Utils::sha256(group_ch.hash, 1, group_ch.secret, key_len);

                uint32_t now = time(nullptr);
                const char* sender_name = _config.short_name;

                success = _bridge->sendGroupMessage(now, group_ch, sender_name, text, strlen(text));
            }
        }
        else
        {
            Mesh::NodeInfo dest_node;
            if (_nodedb && _nodedb->getNode(dest, dest_node))
            {
                if (dest_node.info.has_user && dest_node.info.user.public_key.size == 32)
                {
                    success = _bridge->sendMessage(dest, dest_node.info.user.public_key.bytes, text);
                }
                else
                {
                    ESP_LOGW(TAG, "Cannot send DM to node 0x%08X: missing 32-byte public key (size=%d). Triggering self-advert broadcast to initiate key exchange.", (unsigned int)dest, dest_node.info.user.public_key.size);
                    sendNodeInfo(0xFFFFFFFF, 0, false);
                }
            }
            else
            {
                ESP_LOGW(TAG, "Cannot send DM: node 0x%08X not found in NodeDB. Triggering self-advert broadcast.", (unsigned int)dest);
                sendNodeInfo(0xFFFFFFFF, 0, false);
            }
        }

        if (success)
        {
            return _router.generatePacketId();
        }
        return 0;
    }

    bool MeshService::sendData(const uint8_t* data, size_t len, meshtastic_PortNum port_num, uint32_t dest)
    {
        return false;
    }

    void MeshService::setMessageCallback(MessageCallback callback)
    {
        _message_callback = callback;
    }

    void MeshService::setGps(HAL::GPS* gps)
    {
        _gps = gps;
    }

    uint32_t MeshService::getNodeId() const
    {
        uint32_t node_id = 0;
        if (_bridge)
        {
            memcpy(&node_id, _bridge->self_id.pub_key, 4);
        }
        return node_id;
    }

    uint8_t MeshService::getChannelHash(const meshtastic_ChannelSettings& settings) const
    {
        uint8_t hash[32];
        mesh::Utils::sha256(hash, 32, settings.psk.bytes, settings.psk.size);
        return hash[0];
    }

    bool MeshService::setConfig(const MeshConfig& config)
    {
        _config = config;
        if (_bridge)
        {
            _bridge->self_id.readFromSeed(_config.private_key);
        }

        // Apply radio physical parameters to SX1262 hardware driver
        if (_hal && _hal->radio())
        {
            HAL::LoRaConfig lora_cfg = _hal->radio()->getConfig();
            lora_cfg.frequency_hz = (uint32_t)(_config.lora_config.override_frequency * 1000000.0f);
            lora_cfg.bandwidth_hz = (uint32_t)(_config.lora_config.bandwidth * 1000.0f);
            lora_cfg.spreading_factor = _config.lora_config.spread_factor;
            lora_cfg.coding_rate = _config.lora_config.coding_rate;
            lora_cfg.tx_power_dbm = (int8_t)_config.lora_config.tx_power;
            lora_cfg.sync_word = (uint8_t)_config.sync_word;
            _hal->radio()->setConfig(lora_cfg);
        }

        // Apply primary public channel PSK if changed
        if (!_config.primary_psk_b64.empty() && _nodedb)
        {
            size_t psk_len = 0;
            uint8_t psk_bytes[32];
            if (mbedtls_base64_decode(psk_bytes, 32, &psk_len, (const unsigned char*)_config.primary_psk_b64.c_str(), _config.primary_psk_b64.size()) == 0 && (psk_len == 16 || psk_len == 32))
            {
                meshtastic_Channel* ch0 = _nodedb->getChannel(0);
                if (ch0)
                {
                    memcpy(ch0->settings.psk.bytes, psk_bytes, psk_len);
                    ch0->settings.psk.size = psk_len;
                    _nodedb->saveChannels();
                }
            }
        }

        return true;
    }

    size_t MeshService::getNodeCount() const
    {
        return _nodedb ? _nodedb->getNodeCount() : 0;
    }

    float MeshService::_getChannelUtilization() const
    {
        return 0.0f;
    }

    float MeshService::_getAirUtilTx() const
    {
        return 0.0f;
    }

    uint32_t MeshService::getNodeInfoBroadcastRemainingMs() const
    {
        return 300000;
    }

    uint32_t MeshService::getPositionBroadcastRemainingMs() const
    {
        return 300000;
    }

    uint32_t MeshService::getTelemetryBroadcastRemainingMs() const
    {
        return 300000;
    }

    uint32_t MeshService::getNeighborInfoBroadcastRemainingMs() const
    {
        return 300000;
    }

    void MeshService::sendNodeInfo(uint32_t dest, uint8_t channel, bool want_response)
    {
        if (_bridge)
        {
            const char* name = _config.long_name[0] != '\0' ? _config.long_name : _config.short_name;
            if (!name || name[0] == '\0')
            {
                name = "Cardputer";
            }
            ESP_LOGI(TAG, "sendNodeInfo: creating advert for '%s'", name);
            auto pkt = _bridge->createSelfAdvert(name);
            if (pkt)
            {
                _bridge->sendFlood(pkt);
                ESP_LOGI(TAG, "Self advert broadcast sent!");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to create self advert packet");
            }
        }
    }

    bool MeshService::sendPosition(uint32_t dest, uint8_t channel, bool want_response)
    {
        return false;
    }

    void MeshService::sendNeighborInfo(uint32_t dest, uint8_t channel, bool want_response)
    {
    }

    bool MeshService::sendTraceRoute(uint32_t dest, uint8_t channel)
    {
        return false;
    }

    void MeshService::forceNodeInfoBroadcast()
    {
        sendNodeInfo(0xFFFFFFFF, 0, false);
    }

    bool MeshService::getNode(uint32_t node_id, NodeInfo& out) const
    {
        return _nodedb ? _nodedb->getNode(node_id, out) : false;
    }

    void MeshService::loadConfigFromSettings(MeshConfig& config)
    {
        SETTINGS::Settings* _settings = _hal->settings();

        // 1. Node Info
        std::string short_name = _settings->getString("nodeinfo", "short_name");
        std::string long_name = _settings->getString("nodeinfo", "long_name");
        if (!short_name.empty())
        {
            strncpy(config.short_name, short_name.c_str(), 4);
            config.short_name[4] = '\0';
        }
        if (!long_name.empty())
        {
            strncpy(config.long_name, long_name.c_str(), sizeof(config.long_name) - 1);
            config.long_name[sizeof(config.long_name) - 1] = '\0';
        }

        // 2. LoRa Radio Config
        std::string freq_str = _settings->getString("lora", "frequency");
        if (!freq_str.empty())
        {
            float f = strtof(freq_str.c_str(), nullptr);
            if (f > 0.0f) config.lora_config.override_frequency = f;
        }
        if (config.lora_config.override_frequency <= 0.0f)
        {
            config.lora_config.override_frequency = 910.525f;
        }

        std::string bw_str = _settings->getString("lora", "bandwidth");
        if (!bw_str.empty())
        {
            float bw = strtof(bw_str.c_str(), nullptr);
            if (bw > 0.0f) config.lora_config.bandwidth = bw;
        }
        if (config.lora_config.bandwidth <= 0.0f)
        {
            config.lora_config.bandwidth = 62.5f;
        }

        int sf = _settings->getNumber("lora", "spread_factor");
        config.lora_config.spread_factor = (sf >= 7 && sf <= 12) ? sf : 7;

        int cr = _settings->getNumber("lora", "coding_rate");
        config.lora_config.coding_rate = (cr >= 5 && cr <= 8) ? cr : 5;

        config.lora_config.tx_power = _settings->getNumber("lora", "tx_power");
        if (config.lora_config.tx_power == 0) config.lora_config.tx_power = 22;

        int sync_w = _settings->getNumber("lora", "sync_word");
        config.sync_word = (sync_w > 0) ? sync_w : 18; // 0x12

        // 3. MeshCore Network Settings
        int adv_int = _settings->getNumber("meshcore", "advert_interval");
        config.advert_interval = (adv_int >= 0) ? adv_int : 30;

        int hops = _settings->getNumber("meshcore", "hop_limit");
        config.lora_config.hop_limit = (hops >= 1 && hops <= 7) ? hops : 3;

        std::string psk = _settings->getString("meshcore", "primary_psk");
        if (!psk.empty()) config.primary_psk_b64 = psk;

        // 4. Security Keys
        std::string priv_b64 = _settings->getString("security", "private_key");
        std::string pub_b64 = _settings->getString("security", "public_key");
        if (!priv_b64.empty() && !pub_b64.empty())
        {
            size_t priv_len = 0, pub_len = 0;
            mbedtls_base64_decode(config.private_key, 32, &priv_len, (const unsigned char*)priv_b64.c_str(), priv_b64.size());
            mbedtls_base64_decode(config.public_key, 32, &pub_len, (const unsigned char*)pub_b64.c_str(), pub_b64.size());
            config.public_key_len = (priv_len == 32 && pub_len == 32) ? 32 : 0;
        }
        if (config.public_key_len != 32)
        {
            if (generateKeypair(config.private_key, config.public_key))
            {
                config.public_key_len = 32;
                unsigned char b64_buf[48] = {};
                size_t b64_len = 0;
                if (mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len, config.private_key, 32) == 0)
                    _settings->setString("security", "private_key", std::string((char*)b64_buf, b64_len));
                b64_len = 0;
                if (mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len, config.public_key, 32) == 0)
                    _settings->setString("security", "public_key", std::string((char*)b64_buf, b64_len));
                ESP_LOGI(TAG, "Generated and saved new Ed25519 keypair");
            }
            else
            {
                config.public_key_len = 0;
                ESP_LOGE(TAG, "Ed25519 keygen failed");
            }
        }
        else
        {
            ESP_LOGI(TAG,
                     "Loaded Ed25519 keypair from settings pub=%02x%02x%02x%02x...",
                     config.public_key[0],
                     config.public_key[1],
                     config.public_key[2],
                     config.public_key[3]);
        }

        const std::string location = _settings->getString("position", "location");
        if (location == "fixed")
            config.position = MeshConfig::POSITION_FIXED;
        else if (location == "gps")
            config.position = MeshConfig::POSITION_GPS;
        else
            config.position = MeshConfig::POSITION_OFF;
        config.fixed_latitude = _settings->getNumber("position", "latitude");
        config.fixed_longitude = _settings->getNumber("position", "longitude");
        config.fixed_altitude = _settings->getNumber("position", "altitude");

        uint32_t pf = 0;
        if (_settings->getBool("position", "pos_alt"))
            pf |= meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE;
        if (_settings->getBool("position", "pos_sats"))
            pf |= meshtastic_Config_PositionConfig_PositionFlags_SATINVIEW;
        if (_settings->getBool("position", "pos_seq"))
            pf |= meshtastic_Config_PositionConfig_PositionFlags_SEQ_NO;
        if (_settings->getBool("position", "pos_time"))
            pf |= meshtastic_Config_PositionConfig_PositionFlags_TIMESTAMP;
        if (_settings->getBool("position", "pos_heading"))
            pf |= meshtastic_Config_PositionConfig_PositionFlags_HEADING;
        if (_settings->getBool("position", "pos_speed"))
            pf |= meshtastic_Config_PositionConfig_PositionFlags_SPEED;
        config.position_flags = pf;

        config.telemetry_bat_level = _settings->getBool("devmetrics", "bat_level");
        config.telemetry_voltage = _settings->getBool("devmetrics", "voltage");
        config.telemetry_ch_util = _settings->getBool("devmetrics", "ch_util");
        config.telemetry_air_util = _settings->getBool("devmetrics", "air_util");
        config.telemetry_uptime = _settings->getBool("devmetrics", "uptime");

        config.neighborinfo_enabled = _settings->getBool("neighborinfo", "enabled");
        config.neighborinfo_broadcast_interval_ms = parseIntervalToMs(_settings->getString("neighborinfo", "bcast_int"));

        config.nodeinfo_broadcast_interval_ms = parseIntervalToMs(_settings->getString("nodeinfo", "bcast_int"));
        config.position_broadcast_interval_ms = parseIntervalToMs(_settings->getString("position", "bcast_int"));
        config.telemetry_broadcast_interval_ms = parseIntervalToMs(_settings->getString("devmetrics", "bcast_int"));
    }
}
