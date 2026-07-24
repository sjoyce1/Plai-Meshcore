#pragma once

#include "Mesh.h"
#include "meshcore_platform.h"
#include "meshcore_radio_wrapper.h"
#include "StaticPoolPacketManager.h"
#include "SimpleMeshTables.h"
#include "AdvertDataHelpers.h"
#include "node_db.h"
#include <vector>
#include <functional>
#include <string>

namespace mesh {

class MeshCoreBridge : public mesh::Mesh {
    EspRadioWrapper _radio_wrapper;
    EspMillisecondClock _clock;
    EspRNG _rng;
    EspRTCClock _rtc;
    StaticPoolPacketManager _packet_mgr;
    SimpleMeshTables _tables;

    ::Mesh::NodeDB* _nodedb;
    std::vector<uint32_t> _matching_node_ids; // matching node IDs from searchPeersByHash

    std::function<void(uint32_t sender_id, uint32_t dest_id, uint32_t timestamp, const char* text, bool is_group, uint8_t channel)> _on_msg_recv_cb;

public:
    MeshCoreBridge(HAL::RadioInterface* radio, ::Mesh::NodeDB* nodedb)
        : mesh::Mesh(_radio_wrapper, _clock, _rng, _rtc, _packet_mgr, _tables)
        , _radio_wrapper(radio)
        , _packet_mgr(32) // pool size 32
        , _nodedb(nodedb)
    {
    }

    virtual ~MeshCoreBridge() = default;

    void setOnMessageReceived(std::function<void(uint32_t sender_id, uint32_t dest_id, uint32_t timestamp, const char* text, bool is_group, uint8_t channel)> cb) {
        _on_msg_recv_cb = cb;
    }

    // Override searchPeersByHash to search Plai's NodeDB
    int searchPeersByHash(const uint8_t* hash) override {
        _matching_node_ids.clear();
        if (!_nodedb) return 0;

        const auto& index = _nodedb->getIndex();
        for (const auto& entry : index) {
            ::Mesh::NodeInfo node;
            if (_nodedb->getNode(entry.node_id, node)) {
                if (node.info.has_user && node.info.user.public_key.size == 32) {
                    if (node.info.user.public_key.bytes[0] == hash[0]) {
                        _matching_node_ids.push_back(entry.node_id);
                    }
                }
            }
        }
        return _matching_node_ids.size();
    }

    void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override {
        if (peer_idx < 0 || peer_idx >= (int)_matching_node_ids.size()) return;
        uint32_t node_id = _matching_node_ids[peer_idx];
        ::Mesh::NodeInfo node;
        if (_nodedb && _nodedb->getNode(node_id, node)) {
            if (node.info.has_user && node.info.user.public_key.size == 32) {
                self_id.calcSharedSecret(dest_secret, node.info.user.public_key.bytes);
            }
        }
    }

    // Override searchChannelsByHash to search Plai's channels
    int searchChannelsByHash(const uint8_t* hash, GroupChannel channels[], int max_matches) override {
        if (!_nodedb) return 0;
        int matches = 0;
        for (int i = 0; i < 8; i++) { // Plai has 8 channels
            auto* ch = _nodedb->getChannel(i);
            if (ch && ch->role != meshtastic_Channel_Role_DISABLED) {
                if (ch->has_settings && ch->settings.psk.size > 0) {
                    uint8_t secret[32];
                    memset(secret, 0, 32);
                    memcpy(secret, ch->settings.psk.bytes, ch->settings.psk.size);
                    uint8_t ch_hash[32];
                    Utils::sha256(ch_hash, 32, secret, ch->settings.psk.size);
                    ESP_LOGI("BRIDGE", "searchChannelsByHash: target=0x%02X, ch %d name='%s', psk_size=%d, ch_hash=0x%02X", hash[0], i, ch->settings.name, (int)ch->settings.psk.size, ch_hash[0]);
                    if (ch_hash[0] == hash[0] && matches < max_matches) {
                        channels[matches].hash[0] = ch_hash[0];
                        memcpy(channels[matches].secret, secret, 32);
                        matches++;
                    }
                }
            }
        }
        return matches;
    }

    // Handle incoming direct message
    void onPeerDataRecv(Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override {
        ESP_LOGI("BRIDGE", "onPeerDataRecv: type=%d, sender_idx=%d, len=%d", (int)type, sender_idx, (int)len);
        if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
            uint32_t timestamp;
            memcpy(&timestamp, data, 4);
            uint8_t flags = data[4] >> 2;
            data[len] = 0; // null terminate
            
            if (flags == 0) { // Plain text message
                uint32_t sender_id = 0;
                const uint8_t* sender_pubkey = nullptr;
                ::Mesh::NodeInfo node;
                if (sender_idx >= 0 && sender_idx < (int)_matching_node_ids.size()) {
                    sender_id = _matching_node_ids[sender_idx];
                    if (_nodedb && _nodedb->getNode(sender_id, node)) {
                        if (node.info.has_user && node.info.user.public_key.size == 32) {
                            sender_pubkey = node.info.user.public_key.bytes;
                        }
                    }
                }
                uint32_t my_node_id = _nodedb ? _nodedb->getOurNodeId() : 0;
                ESP_LOGI("BRIDGE", "Received DM from node 0x%08X: '%s'", (unsigned int)sender_id, (const char*)&data[5]);
                if (_on_msg_recv_cb) {
                    _on_msg_recv_cb(sender_id, my_node_id, timestamp, (const char*)&data[5], false, 0);
                }

                if (sender_pubkey) {
                    // Calculate & transmit MeshCore ACK back to sender to stop retries: SHA256(data[0..5+text_len] + sender_pubkey[0..32])
                    uint32_t ack_hash = 0;
                    size_t msg_data_len = 5 + strlen((const char*)&data[5]);
                    Utils::sha256((uint8_t*)&ack_hash, 4, data, msg_data_len, sender_pubkey, PUB_KEY_SIZE);

                    Packet* ack = createAck(ack_hash);
                    if (ack) {
                        sendFlood(ack);
                        ESP_LOGI("BRIDGE", "Sent ACK back to node 0x%08X (ack_hash=0x%08X)", (unsigned int)sender_id, (unsigned int)ack_hash);
                    }
                }
            }
        }
    }

    void onAckRecv(Packet* packet, uint32_t ack_crc) override {
        ESP_LOGI("BRIDGE", "onAckRecv: received ACK crc 0x%08X", (unsigned int)ack_crc);
    }

    // Handle incoming group channel message
    void onGroupDataRecv(Packet* packet, uint8_t type, const GroupChannel& channel, uint8_t* data, size_t len) override {
        uint8_t txt_type = data[4];
        ESP_LOGI("BRIDGE", "onGroupDataRecv: type=%d, len=%d, txt_type=%d", (int)type, (int)len, (int)txt_type);
        if (type == PAYLOAD_TYPE_GRP_TXT && len > 5 && (txt_type >> 2) == 0) {
            uint32_t timestamp;
            memcpy(&timestamp, data, 4);
            data[len] = 0; // null terminate

            const char* msg_text = (const char*)&data[5];
            ESP_LOGI("BRIDGE", "Received group message text: '%s'", msg_text);
            const char* colon = strstr(msg_text, ": ");
            std::string sender_name = "";
            std::string clean_text = msg_text;
            if (colon) {
                sender_name = std::string(msg_text, colon - msg_text);
                clean_text = colon + 2;
            }

            uint32_t sender_id = 0;
            if (_nodedb) {
                const auto& index = _nodedb->getIndex();
                for (const auto& entry : index) {
                    if (sender_name == entry.long_name || sender_name == entry.short_name) {
                        sender_id = entry.node_id;
                        break;
                    }
                }

                if (sender_id == 0 && !sender_name.empty()) {
                    uint8_t hash[32];
                    Utils::sha256(hash, 32, (const uint8_t*)sender_name.c_str(), sender_name.length());
                    memcpy(&sender_id, hash, 4);

                    meshtastic_NodeInfo node_info = meshtastic_NodeInfo_init_default;
                    node_info.num = sender_id;
                    node_info.has_user = true;
                    strncpy(node_info.user.long_name, sender_name.c_str(), sizeof(node_info.user.long_name) - 1);
                    strncpy(node_info.user.short_name, sender_name.c_str(), 4);
                    node_info.user.short_name[4] = 0;
                    node_info.snr = packet->getSNR();
                    node_info.last_heard = timestamp;

                    _nodedb->updateNode(node_info, packet->_snr, packet->getSNR(), packet->path_len > 0 ? packet->path[0] : 0);
                    ESP_LOGI("BRIDGE", "Auto-created NodeDB contact for node 0x%08X: '%s'", (unsigned int)sender_id, sender_name.c_str());
                }
            }

            uint8_t channel_idx = 0;
            if (_nodedb) {
                for (int i = 0; i < 8; i++) {
                    auto* ch = _nodedb->getChannel(i);
                    if (ch && ch->role != meshtastic_Channel_Role_DISABLED && ch->has_settings && ch->settings.psk.size > 0) {
                        uint8_t ch_hash[32];
                        Utils::sha256(ch_hash, 32, ch->settings.psk.bytes, ch->settings.psk.size);
                        if (ch_hash[0] == channel.hash[0]) {
                            channel_idx = i;
                            break;
                        }
                    }
                }
            }

            if (_on_msg_recv_cb) {
                _on_msg_recv_cb(sender_id, 0xFFFFFFFF, timestamp, clean_text.c_str(), true, channel_idx);
            }
        }
    }

    // Handle new node advertisement
    void onAdvertRecv(Packet* packet, const Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len) override {
        AdvertDataParser parser(app_data, app_data_len);
        ESP_LOGI("BRIDGE", "onAdvertRecv: valid=%d, hasName=%d, name='%s'", parser.isValid(), parser.hasName(), parser.getName());
        if (parser.isValid() && parser.hasName()) {
            uint32_t node_id = 0;
            memcpy(&node_id, id.pub_key, 4);

            if (_nodedb) {
                // If there's an auto-created synthetic node with matching name, remove it so real node entry replaces it
                const auto& index = _nodedb->getIndex();
                for (const auto& entry : index) {
                    if (entry.node_id != node_id && (strcmp(entry.long_name, parser.getName()) == 0 || strcmp(entry.short_name, parser.getName()) == 0)) {
                        _nodedb->removeNode(entry.node_id);
                        ESP_LOGI("BRIDGE", "Removed synthetic node entry 0x%08X in favor of real advert 0x%08X", (unsigned int)entry.node_id, (unsigned int)node_id);
                        break;
                    }
                }

                meshtastic_NodeInfo node_info = meshtastic_NodeInfo_init_default;
                node_info.num = node_id;
                node_info.has_user = true;
                strncpy(node_info.user.long_name, parser.getName(), sizeof(node_info.user.long_name) - 1);
                strncpy(node_info.user.short_name, parser.getName(), 4);
                node_info.user.short_name[4] = 0;
                
                memcpy(node_info.user.public_key.bytes, id.pub_key, 32);
                node_info.user.public_key.size = 32;

                node_info.has_position = parser.hasLatLon();
                if (parser.hasLatLon()) {
                    node_info.position.latitude_i = parser.getIntLat();
                    node_info.position.longitude_i = parser.getIntLon();
                }
                node_info.snr = packet->getSNR();
                node_info.last_heard = timestamp;

                _nodedb->updateNode(node_info, packet->_snr, packet->getSNR(), packet->path_len > 0 ? packet->path[0] : 0);
                ESP_LOGI("BRIDGE", "Updated NodeDB entry for node 0x%08X: '%s' (32-byte public key saved)", (unsigned int)node_id, parser.getName());
            }
        }
    }

    bool sendMessage(uint32_t dest_id, const uint8_t* dest_pubkey, const char* text) {
        int text_len = strlen(text);
        if (text_len > MAX_PACKET_PAYLOAD - 6) return false;

        uint8_t temp[5 + MAX_PACKET_PAYLOAD];
        uint32_t now = time(nullptr);
        memcpy(temp, &now, 4);
        temp[4] = 0; // attempt 0, type plain text
        memcpy(&temp[5], text, text_len + 1);

        uint8_t secret[PUB_KEY_SIZE];
        self_id.calcSharedSecret(secret, dest_pubkey);

        Identity dest_identity(dest_pubkey);
        auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, dest_identity, secret, temp, 5 + text_len);
        if (pkt) {
            ESP_LOGI("BRIDGE", "sendMessage: sending DM to 0x%08X (dest_hash=0x%02X): '%s'", (unsigned int)dest_id, dest_pubkey[0], text);
            sendFlood(pkt);
            return true;
        }
        ESP_LOGE("BRIDGE", "sendMessage: failed to create datagram packet for 0x%08X", (unsigned int)dest_id);
        return false;
    }

    Packet* createSelfAdvert(const char* name) {
        uint8_t app_data[MAX_ADVERT_DATA_SIZE];
        uint8_t app_data_len;
        {
            AdvertDataBuilder builder(ADV_TYPE_CHAT, name);
            app_data_len = builder.encodeTo(app_data);
        }
        return createAdvert(self_id, app_data, app_data_len);
    }

    bool sendGroupMessage(uint32_t timestamp, const GroupChannel& channel, const char* sender_name, const char* text, size_t text_len) {
        std::string full_text = std::string(sender_name) + ": " + std::string(text);
        int payload_len = 5 + full_text.length();
        if (payload_len > MAX_PACKET_PAYLOAD) return false;

        uint8_t temp[5 + MAX_PACKET_PAYLOAD];
        memcpy(temp, &timestamp, 4);
        temp[4] = 0; // Text type
        memcpy(&temp[5], full_text.c_str(), full_text.length());

        auto pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, temp, payload_len);
        if (pkt) {
            sendFlood(pkt);
            return true;
        }
        return false;
    }

    int getOutboundCount(uint32_t now) const {
        return _mgr ? _mgr->getOutboundCount(now) : 0;
    }
};

}
