/**
 * @file node_db.cpp
 * @author d4rkmen
 * @brief Node database implementation with lazy loading from storage
 * @version 2.0
 * @date 2025-01-03
 *
 * @copyright Copyright (c) 2025
 *
 */

#include "node_db.h"
#include "mesh_data.h"
#include "common_define.h"
#include "esp_log.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <time.h>
#include <format>

static const char* TAG = "NODE_DB";

namespace Mesh
{

    NodeDB::NodeDB()
        : _current_sort_order(SortOrder::LAST_HEARD), _sort_valid(false), _our_node_id(0), _our_lat_i(0), _our_lon_i(0),
          _dirty(false), _last_save_ms(0), _initialized(false), _change_counter(0)
    {
        memset(&_local_config, 0, sizeof(_local_config));
        memset(&_local_module_config, 0, sizeof(_local_module_config));
        memset(_channels, 0, sizeof(_channels));
        _index.reserve(MAX_NODES);
    }

    NodeDB::~NodeDB()
    {
        if (_dirty)
        {
            save();
        }
    }

    bool NodeDB::init(uint32_t our_node_id)
    {
        ESP_LOGI(TAG, "Initializing node database for node 0x%08lX", (unsigned long)our_node_id);

        _our_node_id = our_node_id;

        // Create storage directories
        if (!createDirectories())
        {
            ESP_LOGW(TAG, "Failed to create storage directories, continuing without persistence");
        }

        // Check for legacy format and migrate if needed
        struct stat st;
        if (stat(NODEDB_FILE, &st) == 0)
        {
            ESP_LOGI(TAG, "Found legacy nodedb.pb, migrating to new format...");
            if (migrateFromLegacy())
            {
                ESP_LOGI(TAG, "Migration successful");
                // Remove legacy file after successful migration
                remove(NODEDB_FILE);
            }
            else
            {
                ESP_LOGW(TAG, "Migration failed, starting fresh");
            }
        }

        // Try to load existing index
        if (!loadIndex())
        {
            // No index found, try to rebuild from node files
            rebuildIndex();
        }

        // Load preferences, channels and greetings
        loadPrefs();
        loadChannels();
        loadGreetings();

        // Sort index by default order
        sortIndex(SortOrder::LAST_HEARD);

        // Migrate favorites: if file is missing, populate from existing flags
        {
            struct stat fav_st;
            if (stat(FAVORITES_FILE, &fav_st) != 0)
            {
                int migrated = 0;
                for (const auto& entry : _index)
                {
                    if (entry.flags & NodeIndexEntry::IS_FAVORITE)
                    {
                        favorites_add(entry.node_id);
                        migrated++;
                    }
                }
                if (migrated > 0)
                    ESP_LOGI(TAG, "Migrated %d favorites to %s", migrated, FAVORITES_FILE);
            }
        }

        _initialized = true;
        ESP_LOGI(TAG, "Node database initialized with %d nodes", _index.size());
        return true;
    }

    bool NodeDB::createDirectories()
    {
        struct stat st;

        // Create main mesh directory
        if (stat(MESH_DIR, &st) != 0)
        {
            ESP_LOGI(TAG, "Creating directory: %s", MESH_DIR);
            if (mkdir(MESH_DIR, 0755) != 0)
            {
                ESP_LOGE(TAG, "Failed to create directory: %s", MESH_DIR);
                return false;
            }
        }

        // Create nodes subdirectory
        if (stat(NODES_DIR, &st) != 0)
        {
            ESP_LOGI(TAG, "Creating directory: %s", NODES_DIR);
            if (mkdir(NODES_DIR, 0755) != 0)
            {
                ESP_LOGE(TAG, "Failed to create directory: %s", NODES_DIR);
                return false;
            }
        }

        return true;
    }

    //--------------------------------------------------------------------------
    // Individual Node File Operations
    //--------------------------------------------------------------------------

    std::string NodeDB::getNodeFilePath(uint32_t node_id) const
    {
        // char path[64];
        // snprintf(path, sizeof(path), "%s/%08lx.pb", NODES_DIR, (unsigned long)node_id);
        return std::format("{}/{:08x}.pb", NODES_DIR, node_id);
    }

    bool NodeDB::loadNodeFromFile(uint32_t node_id, NodeInfo& out) const
    {
        std::string path = getNodeFilePath(node_id);
        FILE* file = fopen(path.c_str(), "rb");
        if (!file)
        {
            ESP_LOGD(TAG, "Node file not found: %s", path.c_str());
            return false;
        }

        bool success = false;
        uint8_t buffer[meshtastic_NodeInfo_size + 16];

        // Read length prefix
        uint16_t len = 0;
        if (fread(&len, sizeof(len), 1, file) == 1 && len <= sizeof(buffer))
        {
            if (fread(buffer, 1, len, file) == len)
            {
                out = {};
                pb_istream_t stream = pb_istream_from_buffer(buffer, len);
                if (pb_decode(&stream, meshtastic_NodeInfo_fields, &out.info))
                {
                    // Read RSSI and relay_node (not in protobuf)
                    fread(&out.last_rssi, sizeof(out.last_rssi), 1, file);
                    fread(&out.relay_node, sizeof(out.relay_node), 1, file);
                    success = true;
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to decode node from %s", path.c_str());
                }
            }
        }

        fclose(file);
        return success;
    }

    bool NodeDB::saveNodeToFile(const NodeInfo& node)
    {
        std::string path = getNodeFilePath(node.info.num);
        FILE* file = fopen(path.c_str(), "wb");
        if (!file)
        {
            ESP_LOGE(TAG, "Failed to open %s for writing", path.c_str());
            return false;
        }

        bool success = false;
        uint8_t buffer[meshtastic_NodeInfo_size + 16];

        pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
        if (pb_encode(&stream, meshtastic_NodeInfo_fields, &node.info))
        {
            // Write length prefix
            uint16_t len = stream.bytes_written;
            fwrite(&len, sizeof(len), 1, file);
            fwrite(buffer, 1, len, file);

            // Write RSSI and relay_node (not in protobuf)
            fwrite(&node.last_rssi, sizeof(node.last_rssi), 1, file);
            fwrite(&node.relay_node, sizeof(node.relay_node), 1, file);
            success = true;
        }

        fclose(file);

        if (success)
        {
            ESP_LOGD(TAG, "Saved node 0x%08lX to %s", (unsigned long)node.info.num, path.c_str());
        }

        return success;
    }

    bool NodeDB::deleteNodeFile(uint32_t node_id)
    {
        std::string path = getNodeFilePath(node_id);
        if (remove(path.c_str()) == 0)
        {
            ESP_LOGI(TAG, "Deleted node file: %s", path.c_str());
            return true;
        }
        ESP_LOGW(TAG, "Failed to delete node file: %s", path.c_str());
        return false;
    }

    //--------------------------------------------------------------------------
    // Index Management
    //--------------------------------------------------------------------------

    bool NodeDB::loadIndex()
    {
        FILE* file = fopen(MANIFEST_FILE, "rb");
        if (!file)
        {
            ESP_LOGW(TAG, "No manifest file found");
            return false;
        }

        bool success = false;

        // Read and verify header
        uint32_t magic = 0, version = 0, count = 0;
        if (fread(&magic, sizeof(magic), 1, file) == 1 && fread(&version, sizeof(version), 1, file) == 1 &&
            fread(&count, sizeof(count), 1, file) == 1)
        {
            if (magic == MANIFEST_MAGIC && version == MANIFEST_VERSION)
            {
                if (count > MAX_NODES)
                {
                    ESP_LOGW(TAG, "Index count %lu exceeds max, truncating", (unsigned long)count);
                    count = MAX_NODES;
                }

                _index.clear();
                _index.reserve(count);

                for (uint32_t i = 0; i < count; i++)
                {
                    NodeIndexEntry entry = {};

                    if (fread(&entry.node_id, sizeof(entry.node_id), 1, file) != 1)
                        break;
                    if (fread(&entry.last_heard, sizeof(entry.last_heard), 1, file) != 1)
                        break;
                    if (fread(&entry.last_rssi, sizeof(entry.last_rssi), 1, file) != 1)
                        break;
                    if (fread(&entry.flags, sizeof(entry.flags), 1, file) != 1)
                        break;
                    if (fread(&entry.short_name, sizeof(entry.short_name), 1, file) != 1)
                        break;
                    if (fread(&entry.long_name, sizeof(entry.long_name), 1, file) != 1)
                        break;
                    if (fread(&entry.role, sizeof(entry.role), 1, file) != 1)
                        break;
                    if (fread(&entry.hops_away, sizeof(entry.hops_away), 1, file) != 1)
                        break;
                    if (fread(&entry.snr, sizeof(entry.snr), 1, file) != 1)
                        break;
                    if (fread(&entry.latitude_i, sizeof(entry.latitude_i), 1, file) != 1)
                        break;
                    if (fread(&entry.longitude_i, sizeof(entry.longitude_i), 1, file) != 1)
                        break;

                    // Verify file exists
                    std::string path = getNodeFilePath(entry.node_id);
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0)
                        entry.flags |= NodeIndexEntry::IS_EXISTS;

                    if (entry.flags & NodeIndexEntry::IS_EXISTS)
                    {
                        _index.push_back(entry);
                    }
                }

                success = true;
                _sort_valid = false;
                ESP_LOGI(TAG, "Loaded index with %d entries", _index.size());
            }
            else
            {
                ESP_LOGW(TAG,
                         "Invalid manifest header (magic=0x%08lX, version=%lu)",
                         (unsigned long)magic,
                         (unsigned long)version);
            }
        }

        fclose(file);
        return success;
    }

    bool NodeDB::saveIndex()
    {
        FILE* file = fopen(MANIFEST_FILE, "wb");
        if (!file)
        {
            ESP_LOGE(TAG, "Failed to open %s for writing", MANIFEST_FILE);
            return false;
        }

        // Write header
        uint32_t magic = MANIFEST_MAGIC;
        uint32_t version = MANIFEST_VERSION;
        uint32_t count = _index.size();

        fwrite(&magic, sizeof(magic), 1, file);
        fwrite(&version, sizeof(version), 1, file);
        fwrite(&count, sizeof(count), 1, file);

        // Write entries
        for (const auto& entry : _index)
        {
            fwrite(&entry.node_id, sizeof(entry.node_id), 1, file);
            fwrite(&entry.last_heard, sizeof(entry.last_heard), 1, file);
            fwrite(&entry.last_rssi, sizeof(entry.last_rssi), 1, file);
            fwrite(&entry.flags, sizeof(entry.flags), 1, file);
            fwrite(&entry.short_name, sizeof(entry.short_name), 1, file);
            fwrite(&entry.long_name, sizeof(entry.long_name), 1, file);
            fwrite(&entry.role, sizeof(entry.role), 1, file);
            fwrite(&entry.hops_away, sizeof(entry.hops_away), 1, file);
            fwrite(&entry.snr, sizeof(entry.snr), 1, file);
            fwrite(&entry.latitude_i, sizeof(entry.latitude_i), 1, file);
            fwrite(&entry.longitude_i, sizeof(entry.longitude_i), 1, file);
        }

        fclose(file);
        ESP_LOGD(TAG, "Saved index with %d entries", _index.size());
        return true;
    }

    bool NodeDB::rebuildIndex()
    {
        ESP_LOGI(TAG, "Rebuilding index from node files...");
        _index.clear();

        DIR* dir = opendir(NODES_DIR);
        if (!dir)
        {
            ESP_LOGW(TAG, "Cannot open nodes directory");
            return false;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            // Check if file matches pattern xxxxxxxx.pb
            size_t len = strlen(entry->d_name);
            if (len == 11 && strcmp(entry->d_name + 8, ".pb") == 0)
            {
                // Parse node ID from filename
                uint32_t node_id = 0;
                if (sscanf(entry->d_name, "%08lx", (unsigned long*)&node_id) == 1)
                {
                    // Load node to get index data
                    NodeInfo node;
                    if (loadNodeFromFile(node_id, node))
                    {
                        updateIndexEntry(node);
                    }
                }
            }
        }

        closedir(dir);
        _sort_valid = false;

        ESP_LOGI(TAG, "Rebuilt index with %d entries", _index.size());
        return true;
    }

    NodeIndexEntry* NodeDB::findIndexEntry(uint32_t node_id)
    {
        for (auto& entry : _index)
        {
            if (entry.node_id == node_id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    const NodeIndexEntry* NodeDB::findIndexEntry(uint32_t node_id) const
    {
        for (const auto& entry : _index)
        {
            if (entry.node_id == node_id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    std::string NodeDB::getLongLabel(const NodeInfo& node)
    {
        if (!node.info.has_user)
        {
            return std::format("!{:08x}", node.info.num);
        }
        if (node.info.user.long_name[0])
        {
            return node.info.user.long_name;
        }
        return getLabel(node);
    }

    std::string NodeDB::getLabel(const NodeInfo& node)
    {
        if (node.info.has_user && node.info.user.short_name[0])
        {
            return node.info.user.short_name;
        }
        return std::format("{:04x}", node.info.num & 0xFFFF);
    }

    std::string NodeDB::getIndexLabel(const NodeIndexEntry& entry)
    {
        if (entry.short_name[0])
        {
            return entry.short_name;
        }
        return std::format("{:04x}", entry.node_id & 0xFFFF);
    }

    const char* NodeDB::getRoleName(meshtastic_Config_DeviceConfig_Role role)
    {
        switch (role)
        {
        case meshtastic_Config_DeviceConfig_Role_CLIENT:
            return "Client";
        case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
            return "Client Mute";
        case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
            return "Client Hidden";
        case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:
            return "Client Base";
        case meshtastic_Config_DeviceConfig_Role_ROUTER:
            return "Router";
        case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
            return "Router Client";
        case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
            return "Router Late";
        case meshtastic_Config_DeviceConfig_Role_REPEATER:
            return "Repeater";
        case meshtastic_Config_DeviceConfig_Role_TRACKER:
            return "Tracker";
        case meshtastic_Config_DeviceConfig_Role_SENSOR:
            return "Sensor";
        case meshtastic_Config_DeviceConfig_Role_TAK:
            return "TAK";
        case meshtastic_Config_DeviceConfig_Role_TAK_TRACKER:
            return "TAK Tracker";
        case meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND:
            return "Lost&Found";
        default:
            return "Unknown";
        }
    }

    void NodeDB::fillIndexEntryFromNode(NodeIndexEntry& entry, const NodeInfo& node)
    {
        entry.node_id = node.info.num;
        entry.last_heard = node.info.last_heard;
        entry.last_rssi = node.last_rssi;
        entry.flags = 0;
        if (node.info.is_favorite)
            entry.flags |= NodeIndexEntry::IS_FAVORITE;
        if (node.info.is_ignored)
            entry.flags |= NodeIndexEntry::IS_IGNORED;

        entry.flags |= NodeIndexEntry::IS_EXISTS;
        // Sort-relevant fields
        memset(entry.short_name, 0, sizeof(entry.short_name));
        memset(entry.long_name, 0, sizeof(entry.long_name));
        if (node.info.has_user)
        {
            strncpy(entry.short_name, node.info.user.short_name, sizeof(entry.short_name) - 1);
            strncpy(entry.long_name, node.info.user.long_name, sizeof(entry.long_name) - 1);
            entry.role = (uint8_t)node.info.user.role;
        }
        else
        {
            entry.role = 0;
        }
        entry.hops_away = node.info.has_hops_away ? (uint8_t)node.info.hops_away : 0;
        entry.snr = node.info.snr;
        if (node.info.has_position && (node.info.position.latitude_i != 0 || node.info.position.longitude_i != 0))
        {
            entry.latitude_i = node.info.position.latitude_i;
            entry.longitude_i = node.info.position.longitude_i;
        }
        else
        {
            entry.latitude_i = 0;
            entry.longitude_i = 0;
        }
    }

    void NodeDB::updateIndexEntry(const NodeInfo& node)
    {
        NodeIndexEntry* entry = findIndexEntry(node.info.num);
        if (entry)
        {
            fillIndexEntryFromNode(*entry, node);
        }
        else
        {
            // Add new entry
            if (_index.size() >= MAX_NODES)
            {
                // Remove oldest non-favorite entry
                auto oldest = std::min_element(_index.begin(),
                                               _index.end(),
                                               [](const NodeIndexEntry& a, const NodeIndexEntry& b)
                                               {
                                                   bool a_fav = a.flags & NodeIndexEntry::IS_FAVORITE;
                                                   bool b_fav = b.flags & NodeIndexEntry::IS_FAVORITE;
                                                   if (a_fav != b_fav)
                                                       return !a_fav;
                                                   return a.last_heard < b.last_heard;
                                               });
                if (oldest != _index.end())
                {
                    neighbors_delete(oldest->node_id);
                    deleteNodeFile(oldest->node_id);
                    _index.erase(oldest);
                }
            }

            NodeIndexEntry new_entry = {};
            fillIndexEntryFromNode(new_entry, node);
            _index.push_back(new_entry);
        }
        _sort_valid = false;
    }

    void NodeDB::removeIndexEntry(uint32_t node_id)
    {
        auto it =
            std::find_if(_index.begin(), _index.end(), [node_id](const NodeIndexEntry& e) { return e.node_id == node_id; });
        if (it != _index.end())
        {
            _index.erase(it);
            _sort_valid = false;
        }
    }

    //--------------------------------------------------------------------------
    // Legacy Migration
    //--------------------------------------------------------------------------

    bool NodeDB::loadLegacyNodeDb(std::vector<NodeInfo>& nodes)
    {
        FILE* file = fopen(NODEDB_FILE, "rb");
        if (!file)
        {
            return false;
        }

        // Read number of nodes
        uint32_t count = 0;
        if (fread(&count, sizeof(count), 1, file) != 1)
        {
            fclose(file);
            return false;
        }

        if (count > MAX_NODES)
        {
            count = MAX_NODES;
        }

        nodes.clear();
        uint8_t buffer[meshtastic_NodeInfo_size + 16];

        for (uint32_t i = 0; i < count; i++)
        {
            uint16_t len = 0;
            if (fread(&len, sizeof(len), 1, file) != 1)
                break;

            if (len > sizeof(buffer))
                break;

            if (fread(buffer, 1, len, file) != len)
                break;

            NodeInfo node = {};
            pb_istream_t stream = pb_istream_from_buffer(buffer, len);
            if (!pb_decode(&stream, meshtastic_NodeInfo_fields, &node.info))
            {
                continue;
            }

            // Read legacy metadata and migrate to protobuf fields
            uint32_t legacy_last_heard;
            int16_t legacy_rssi;
            float legacy_snr;
            bool legacy_favorite;
            fread(&legacy_last_heard, sizeof(legacy_last_heard), 1, file);
            fread(&legacy_rssi, sizeof(legacy_rssi), 1, file);
            fread(&legacy_snr, sizeof(legacy_snr), 1, file);
            fread(&legacy_favorite, sizeof(legacy_favorite), 1, file);

            // Migrate to new format
            node.info.last_heard = legacy_last_heard;
            node.info.snr = legacy_snr;
            node.info.is_favorite = legacy_favorite;
            node.last_rssi = legacy_rssi;

            nodes.push_back(node);
        }

        fclose(file);
        return !nodes.empty();
    }

    bool NodeDB::migrateFromLegacy()
    {
        std::vector<NodeInfo> legacy_nodes;
        if (!loadLegacyNodeDb(legacy_nodes))
        {
            return false;
        }

        ESP_LOGI(TAG, "Migrating %d nodes from legacy format", legacy_nodes.size());

        _index.clear();
        for (const auto& node : legacy_nodes)
        {
            if (saveNodeToFile(node))
            {
                updateIndexEntry(node);
            }
        }

        saveIndex();
        return true;
    }

    //--------------------------------------------------------------------------
    // Sorting
    //--------------------------------------------------------------------------

    void NodeDB::sortIndex(SortOrder order)
    {
        if (_sort_valid && _current_sort_order == order)
        {
            return;
        }

        // Refresh IS_UNREAD flags from message store before sorting
        auto& msg_store = MeshDataStore::getInstance();
        for (auto& entry : _index)
        {
            if (msg_store.getUnreadDMCount(entry.node_id) > 0)
                entry.flags |= NodeIndexEntry::IS_UNREAD;
            else
                entry.flags &= ~NodeIndexEntry::IS_UNREAD;
        }

        _sorted_indices.clear();
        _sorted_indices.reserve(_index.size());
        for (size_t i = 0; i < _index.size(); i++)
        {
            _sorted_indices.push_back(i);
        }

        // Unread-first check shared by all comparators
        auto unread_cmp = [this](size_t a, size_t b) -> int
        {
            bool a_unread = _index[a].flags & NodeIndexEntry::IS_UNREAD;
            bool b_unread = _index[b].flags & NodeIndexEntry::IS_UNREAD;
            if (a_unread != b_unread)
                return a_unread ? -1 : 1;
            return 0; // both same — fall through to mode-specific order
        };

        switch (order)
        {
        case SortOrder::NONE:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          return a < b;
                      });
            break;

        case SortOrder::SHORT_NAME:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          return strncasecmp(_index[a].short_name, _index[b].short_name, sizeof(_index[0].short_name)) < 0;
                      });
            break;

        case SortOrder::LONG_NAME:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          return strncasecmp(_index[a].long_name, _index[b].long_name, sizeof(_index[0].long_name)) < 0;
                      });
            break;

        case SortOrder::ROLE:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          if (_index[a].role != _index[b].role)
                              return _index[a].role < _index[b].role;
                          return _index[a].last_heard > _index[b].last_heard;
                      });
            break;

        case SortOrder::SIGNAL:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          return _index[a].last_rssi > _index[b].last_rssi;
                      });
            break;

        case SortOrder::HOPS_AWAY:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          if (_index[a].hops_away != _index[b].hops_away)
                              return _index[a].hops_away < _index[b].hops_away;
                          return _index[a].last_heard > _index[b].last_heard;
                      });
            break;

        case SortOrder::LAST_HEARD:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          return _index[a].last_heard > _index[b].last_heard;
                      });
            break;

        case SortOrder::FAVORITES_FIRST:
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          bool a_fav = _index[a].flags & NodeIndexEntry::IS_FAVORITE;
                          bool b_fav = _index[b].flags & NodeIndexEntry::IS_FAVORITE;
                          if (a_fav != b_fav)
                              return a_fav;
                          return _index[a].last_heard > _index[b].last_heard;
                      });
            break;

        case SortOrder::DISTANCE:
        {
            int32_t our_lat = _our_lat_i;
            int32_t our_lon = _our_lon_i;
            std::sort(_sorted_indices.begin(),
                      _sorted_indices.end(),
                      [&, our_lat, our_lon](size_t a, size_t b)
                      {
                          int u = unread_cmp(a, b);
                          if (u)
                              return u < 0;
                          bool a_has = (_index[a].latitude_i != 0 || _index[a].longitude_i != 0);
                          bool b_has = (_index[b].latitude_i != 0 || _index[b].longitude_i != 0);
                          if (a_has != b_has)
                              return a_has;
                          if (!a_has)
                              return _index[a].last_heard > _index[b].last_heard;
                          auto dist_sq = [our_lat, our_lon](int32_t lat_i, int32_t lon_i) -> double
                          {
                              double dlat = (double)(lat_i - our_lat);
                              double dlon = (double)(lon_i - our_lon);
                              double avg_lat_rad = ((double)(lat_i + our_lat) * 0.5e-7) * (M_PI / 180.0);
                              double x = dlon * cos(avg_lat_rad);
                              return x * x + dlat * dlat;
                          };
                          double da = dist_sq(_index[a].latitude_i, _index[a].longitude_i);
                          double db = dist_sq(_index[b].latitude_i, _index[b].longitude_i);
                          if (da != db)
                              return da < db;
                          return _index[a].last_heard > _index[b].last_heard;
                      });
            break;
        }
        }

        _current_sort_order = order;
        _sort_valid = true;
    }

    //--------------------------------------------------------------------------
    // Public API
    //--------------------------------------------------------------------------

    bool NodeDB::load()
    {
        ESP_LOGI(TAG, "Loading node database from storage");

        bool success = loadIndex();
        // success &= loadPrefs();
        success &= loadChannels();

        return success;
    }

    bool NodeDB::save()
    {
        ESP_LOGD(TAG, "Saving node database to storage");

        bool success = saveIndex();
        // success &= savePrefs();
        success &= saveChannels();

        if (success)
        {
            _dirty = false;
            _last_save_ms = millis();
        }

        return success;
    }

    size_t NodeDB::getNodeCount() const { return _index.size(); }

    size_t NodeDB::getOnlineNodeCount(uint32_t max_age_sec) const
    {
        uint32_t now = (uint32_t)time(nullptr);
        size_t count = 0;
        for (const auto& e : _index)
        {
            if (e.last_heard > 0 && (now - e.last_heard) <= max_age_sec)
                count++;
        }
        return count;
    }

    bool NodeDB::getNodeByIndex(size_t index, NodeInfo& out) const
    {
        // Ensure sorted
        if (!_sort_valid)
        {
            const_cast<NodeDB*>(this)->sortIndex(_current_sort_order);
        }

        if (index >= _sorted_indices.size())
        {
            return false;
        }

        size_t actual_index = _sorted_indices[index];
        if (actual_index >= _index.size())
        {
            return false;
        }

        return loadNodeFromFile(_index[actual_index].node_id, out);
    }

    size_t NodeDB::getNodesInRange(size_t offset, size_t count, std::vector<NodeInfo>& out, SortOrder order) const
    {
        out.clear();

        // Ensure sorted with requested order
        if (!_sort_valid || _current_sort_order != order)
        {
            const_cast<NodeDB*>(this)->sortIndex(order);
        }

        if (offset >= _sorted_indices.size())
        {
            return 0;
        }

        size_t end = std::min(offset + count, _sorted_indices.size());
        out.reserve(end - offset);

        for (size_t i = offset; i < end; i++)
        {
            NodeInfo node;
            size_t actual_index = _sorted_indices[i];
            if (actual_index < _index.size() && loadNodeFromFile(_index[actual_index].node_id, node))
            {
                out.push_back(node);
            }
        }

        return out.size();
    }

    bool NodeDB::getNode(uint32_t node_id, NodeInfo& out) const
    {
        const NodeIndexEntry* entry = findIndexEntry(node_id);
        if (!entry || !(entry->flags & NodeIndexEntry::IS_EXISTS))
        {
            return false;
        }
        return loadNodeFromFile(node_id, out);
    }

    const NodeIndexEntry* NodeDB::getNodeIndex(uint32_t node_id) const { return findIndexEntry(node_id); }

    uint32_t NodeDB::findNodeByRelayByte(uint8_t relay_byte) const
    {
        uint32_t best_id = 0;
        uint8_t best_hops = 0xFF;
        for (const auto& entry : _index)
        {
            if ((entry.node_id & 0xFF) == relay_byte && entry.hops_away < best_hops)
            {
                best_id = entry.node_id;
                best_hops = entry.hops_away;
            }
        }
        return best_id;
    }

    int NodeDB::getSortedIndexForNode(uint32_t node_id, SortOrder order) const
    {
        if (!_sort_valid || _current_sort_order != order)
        {
            const_cast<NodeDB*>(this)->sortIndex(order);
        }
        for (size_t i = 0; i < _sorted_indices.size(); i++)
        {
            size_t idx = _sorted_indices[i];
            if (idx < _index.size() && _index[idx].node_id == node_id)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool NodeDB::updateNode(const meshtastic_NodeInfo& node, int16_t rssi, float snr, uint8_t relay_node)
    {
        NodeInfo full_node = {};
        bool exists = loadNodeFromFile(node.num, full_node);

        if (exists)
        {
            // Update existing node - preserve is_favorite
            bool was_favorite = full_node.info.is_favorite;
            bool was_ignored = full_node.info.is_ignored;
            full_node.info = node;
            full_node.info.is_favorite = was_favorite;
            full_node.info.is_ignored = was_ignored;
            full_node.info.last_heard = (uint32_t)time(nullptr);
            if (rssi != -1)
            {
                full_node.last_rssi = rssi;
                full_node.info.snr = snr;
                full_node.relay_node = relay_node;
            }
        }
        else
        {
            // New node
            full_node.info = node;
            full_node.info.last_heard = (uint32_t)time(nullptr);
            full_node.last_rssi = (rssi != -1) ? rssi : 0;
            full_node.info.snr = snr;
            full_node.info.is_favorite = favorites_contains(node.num);
            full_node.info.is_ignored = ignorelist_contains(node.num);
            full_node.relay_node = relay_node;
        }

        bool saved = saveNodeToFile(full_node);
        updateIndexEntry(full_node);
        markDirty();
        ESP_LOGI(TAG, "%s node 0x%08lX (saved=%d)", exists ? "Updated" : "Added", (unsigned long)node.num, (int)saved);
        return true;
    }

    bool NodeDB::removeNode(uint32_t node_id)
    {
        if (deleteNodeFile(node_id))
        {
            Mesh::MeshDataStore::getInstance().removeNodeData(node_id);
            neighbors_delete(node_id);
            removeIndexEntry(node_id);
            markDirty();
            ESP_LOGI(TAG, "Removed node 0x%08lX", (unsigned long)node_id);
            return true;
        }

        return false;
    }

    void NodeDB::clearNodes()
    {
        for (const auto& entry : _index)
        {
            Mesh::MeshDataStore::getInstance().removeNodeData(entry.node_id);
            neighbors_delete(entry.node_id);
            deleteNodeFile(entry.node_id);
        }

        _index.clear();
        _sort_valid = false;
        markDirty();
        ESP_LOGI(TAG, "Cleared all nodes");
    }

    bool NodeDB::updatePosition(uint32_t node_id, const meshtastic_Position& position)
    {
        NodeInfo node = {};
        bool exists = loadNodeFromFile(node_id, node);

        if (!exists)
        {
            // Create new node with just position
            node.info = meshtastic_NodeInfo_init_default;
            node.info.num = node_id;
            node.info.last_heard = (uint32_t)time(nullptr);
        }

        node.info.has_position = true;
        node.info.position = position;
        node.info.last_heard = (uint32_t)time(nullptr);

        if (saveNodeToFile(node))
        {
            updateIndexEntry(node);
            markDirty();
            return true;
        }
        return false;
    }

    bool NodeDB::updateUser(uint32_t node_id, const meshtastic_User& user)
    {
        NodeInfo node = {};
        bool exists = loadNodeFromFile(node_id, node);

        if (!exists)
        {
            // Create new node with just user info
            node.info = meshtastic_NodeInfo_init_default;
            node.info.num = node_id;
            node.info.last_heard = (uint32_t)time(nullptr);
        }

        node.info.has_user = true;
        node.info.user = user;
        node.info.last_heard = (uint32_t)time(nullptr);

        if (saveNodeToFile(node))
        {
            updateIndexEntry(node);
            markDirty();
            return true;
        }
        return false;
    }

    bool NodeDB::setFavorite(uint32_t node_id, bool favorite)
    {
        NodeInfo node = {};
        if (!loadNodeFromFile(node_id, node))
        {
            return false;
        }

        node.info.is_favorite = favorite;

        if (saveNodeToFile(node))
        {
            NodeIndexEntry* entry = findIndexEntry(node_id);
            if (entry)
            {
                if (favorite)
                    entry->flags |= NodeIndexEntry::IS_FAVORITE;
                else
                    entry->flags &= ~NodeIndexEntry::IS_FAVORITE;
                _sort_valid = false;
            }
            markDirty();
            return true;
        }
        return false;
    }

    bool NodeDB::setIgnored(uint32_t node_id, bool ignored)
    {
        NodeInfo node = {};
        if (!loadNodeFromFile(node_id, node))
        {
            return false;
        }

        node.info.is_ignored = ignored;

        if (saveNodeToFile(node))
        {
            NodeIndexEntry* entry = findIndexEntry(node_id);
            if (entry)
            {
                if (ignored)
                    entry->flags |= NodeIndexEntry::IS_IGNORED;
                else
                    entry->flags &= ~NodeIndexEntry::IS_IGNORED;
                _sort_valid = false;
            }
            markDirty();
            return true;
        }
        return false;
    }

    //--------------------------------------------------------------------------
    // Preferences and Channels (unchanged)
    //--------------------------------------------------------------------------

    bool NodeDB::savePrefs()
    {
        FILE* file = fopen(PREFS_FILE, "wb");
        if (!file)
        {
            ESP_LOGE(TAG, "Failed to open %s for writing", PREFS_FILE);
            return false;
        }

        // Save LocalConfig
        uint8_t buffer[meshtastic_LocalConfig_size];
        pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
        if (pb_encode(&stream, meshtastic_LocalConfig_fields, &_local_config))
        {
            uint16_t len = stream.bytes_written;
            fwrite(&len, sizeof(len), 1, file);
            fwrite(buffer, 1, len, file);
        }

        // Save LocalModuleConfig
        stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
        if (pb_encode(&stream, meshtastic_LocalModuleConfig_fields, &_local_module_config))
        {
            uint16_t len = stream.bytes_written;
            fwrite(&len, sizeof(len), 1, file);
            fwrite(buffer, 1, len, file);
        }

        fclose(file);
        return true;
    }

    bool NodeDB::loadPrefs()
    {
        FILE* file = fopen(PREFS_FILE, "rb");
        if (!file)
        {
            ESP_LOGW(TAG, "No existing prefs file");
            return false;
        }

        uint8_t buffer[meshtastic_LocalConfig_size];

        // Load LocalConfig
        uint16_t len = 0;
        if (fread(&len, sizeof(len), 1, file) == 1 && len <= sizeof(buffer))
        {
            if (fread(buffer, 1, len, file) == len)
            {
                pb_istream_t stream = pb_istream_from_buffer(buffer, len);
                pb_decode(&stream, meshtastic_LocalConfig_fields, &_local_config);
            }
        }

        // Load LocalModuleConfig
        if (fread(&len, sizeof(len), 1, file) == 1 && len <= sizeof(buffer))
        {
            if (fread(buffer, 1, len, file) == len)
            {
                pb_istream_t stream = pb_istream_from_buffer(buffer, len);
                pb_decode(&stream, meshtastic_LocalModuleConfig_fields, &_local_module_config);
            }
        }

        fclose(file);
        return true;
    }

    bool NodeDB::saveChannels()
    {
        FILE* file = fopen(CHANNELS_FILE, "wb");
        if (!file)
        {
            ESP_LOGE(TAG, "Failed to open %s for writing", CHANNELS_FILE);
            return false;
        }

        uint8_t buffer[meshtastic_Channel_size];

        for (int i = 0; i < 8; i++)
        {
            pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
            if (pb_encode(&stream, meshtastic_Channel_fields, &_channels[i]))
            {
                uint16_t len = stream.bytes_written;
                fwrite(&len, sizeof(len), 1, file);
                fwrite(buffer, 1, len, file);
            }
        }

        fclose(file);
        return true;
    }

    bool NodeDB::loadChannels()
    {
        FILE* file = fopen(CHANNELS_FILE, "rb");
        if (!file)
        {
            ESP_LOGW(TAG, "No existing channels file");
            return false;
        }

        uint8_t buffer[meshtastic_Channel_size];

        for (int i = 0; i < 8; i++)
        {
            uint16_t len = 0;
            if (fread(&len, sizeof(len), 1, file) != 1)
                break;

            if (len > sizeof(buffer))
                break;

            if (fread(buffer, 1, len, file) != len)
                break;

            pb_istream_t stream = pb_istream_from_buffer(buffer, len);
            pb_decode(&stream, meshtastic_Channel_fields, &_channels[i]);
        }

        fclose(file);
        return true;
    }

    meshtastic_Channel* NodeDB::getChannel(uint8_t index)
    {
        if (index >= 8)
        {
            return nullptr;
        }
        return &_channels[index];
    }

    bool NodeDB::setChannel(uint8_t index, const meshtastic_Channel& channel)
    {
        if (index >= 8)
        {
            return false;
        }
        _channels[index] = channel;
        _channels[index].index = index;
        markDirty();
        return true;
    }

    bool NodeDB::deleteChannel(uint8_t index)
    {
        if (index >= 8)
        {
            return false;
        }
        _channels[index] = meshtastic_Channel_init_default;
        _greetings[index] = {};
        markDirty();
        return true;
    }

    const ChannelGreeting& NodeDB::getGreeting(uint8_t index) const
    {
        static const ChannelGreeting empty = {};
        if (index >= 8)
            return empty;
        return _greetings[index];
    }

    void NodeDB::setGreeting(uint8_t index, const ChannelGreeting& greeting)
    {
        if (index >= 8)
            return;
        _greetings[index] = greeting;
    }

    bool NodeDB::saveGreetings()
    {
        FILE* file = fopen(GREETINGS_FILE, "wb");
        if (!file)
        {
            ESP_LOGE(TAG, "Failed to open %s for writing", GREETINGS_FILE);
            return false;
        }
        fwrite(_greetings, sizeof(_greetings), 1, file);
        fclose(file);
        return true;
    }

    bool NodeDB::loadGreetings()
    {
        FILE* file = fopen(GREETINGS_FILE, "rb");
        if (!file)
        {
            ESP_LOGD(TAG, "No existing greetings file");
            return false;
        }
        size_t read = fread(_greetings, 1, sizeof(_greetings), file);
        fclose(file);
        if (read != sizeof(_greetings))
        {
            ESP_LOGW(TAG, "Greetings file size mismatch, resetting");
            memset(_greetings, 0, sizeof(_greetings));
            return false;
        }
        return true;
    }

    void NodeDB::checkSave()
    {
        if (!_dirty)
        {
            return;
        }

        uint32_t now = millis();
        if ((now - _last_save_ms) >= SAVE_INTERVAL_MS)
        {
            save();
        }
    }

    // ========== Favorites file helpers ==========

    size_t favorites_get_count()
    {
        FILE* f = fopen(FAVORITES_FILE, "rb");
        if (!f)
            return 0;
        fseek(f, 0, SEEK_END);
        size_t sz = ftell(f);
        fclose(f);
        return sz / sizeof(uint32_t);
    }

    bool favorites_load_range(size_t offset, size_t count, std::vector<uint32_t>& out)
    {
        out.clear();
        FILE* f = fopen(FAVORITES_FILE, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t total = ftell(f) / sizeof(uint32_t);
        if (offset >= total)
        {
            fclose(f);
            return true;
        }
        size_t avail = total - offset;
        if (count > avail)
            count = avail;
        out.resize(count);
        fseek(f, (long)(offset * sizeof(uint32_t)), SEEK_SET);
        size_t rd = fread(out.data(), sizeof(uint32_t), count, f);
        fclose(f);
        out.resize(rd);
        return true;
    }

    bool favorites_contains(uint32_t node_id)
    {
        FILE* f = fopen(FAVORITES_FILE, "rb");
        if (!f)
            return false;
        uint32_t id;
        while (fread(&id, sizeof(id), 1, f) == 1)
        {
            if (id == node_id)
            {
                fclose(f);
                return true;
            }
        }
        fclose(f);
        return false;
    }

    bool favorites_add(uint32_t node_id)
    {
        if (favorites_contains(node_id))
            return true;
        FILE* f = fopen(FAVORITES_FILE, "ab");
        if (!f)
            return false;
        bool ok = fwrite(&node_id, sizeof(node_id), 1, f) == 1;
        fclose(f);
        return ok;
    }

    bool favorites_remove(uint32_t node_id)
    {
        FILE* f = fopen(FAVORITES_FILE, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t total = ftell(f) / sizeof(uint32_t);
        if (total == 0)
        {
            fclose(f);
            return false;
        }
        fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> ids(total);
        fread(ids.data(), sizeof(uint32_t), total, f);
        fclose(f);

        auto it = std::find(ids.begin(), ids.end(), node_id);
        if (it == ids.end())
            return false;
        ids.erase(it);

        if (ids.empty())
        {
            ::remove(FAVORITES_FILE);
            return true;
        }
        f = fopen(FAVORITES_FILE, "wb");
        if (!f)
            return false;
        fwrite(ids.data(), sizeof(uint32_t), ids.size(), f);
        fclose(f);
        return true;
    }

    bool favorites_remove_at(size_t index)
    {
        FILE* f = fopen(FAVORITES_FILE, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t total = ftell(f) / sizeof(uint32_t);
        if (index >= total)
        {
            fclose(f);
            return false;
        }
        fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> ids(total);
        fread(ids.data(), sizeof(uint32_t), total, f);
        fclose(f);

        ids.erase(ids.begin() + (int)index);

        if (ids.empty())
        {
            ::remove(FAVORITES_FILE);
            return true;
        }
        f = fopen(FAVORITES_FILE, "wb");
        if (!f)
            return false;
        fwrite(ids.data(), sizeof(uint32_t), ids.size(), f);
        fclose(f);
        return true;
    }

    void favorites_clear() { ::remove(FAVORITES_FILE); }

    // ========== Ignore list file helpers ==========

    size_t ignorelist_get_count()
    {
        FILE* f = fopen(IGNORELIST_FILE, "rb");
        if (!f)
            return 0;
        fseek(f, 0, SEEK_END);
        size_t sz = ftell(f);
        fclose(f);
        return sz / sizeof(uint32_t);
    }

    bool ignorelist_load_range(size_t offset, size_t count, std::vector<uint32_t>& out)
    {
        out.clear();
        FILE* f = fopen(IGNORELIST_FILE, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t total = ftell(f) / sizeof(uint32_t);
        if (offset >= total)
        {
            fclose(f);
            return true;
        }
        size_t avail = total - offset;
        if (count > avail)
            count = avail;
        out.resize(count);
        fseek(f, (long)(offset * sizeof(uint32_t)), SEEK_SET);
        size_t rd = fread(out.data(), sizeof(uint32_t), count, f);
        fclose(f);
        out.resize(rd);
        return true;
    }

    bool ignorelist_contains(uint32_t node_id)
    {
        FILE* f = fopen(IGNORELIST_FILE, "rb");
        if (!f)
            return false;
        uint32_t id;
        while (fread(&id, sizeof(id), 1, f) == 1)
        {
            if (id == node_id)
            {
                fclose(f);
                return true;
            }
        }
        fclose(f);
        return false;
    }

    bool ignorelist_add(uint32_t node_id)
    {
        if (ignorelist_contains(node_id))
            return true;
        FILE* f = fopen(IGNORELIST_FILE, "ab");
        if (!f)
            return false;
        bool ok = fwrite(&node_id, sizeof(node_id), 1, f) == 1;
        fclose(f);
        return ok;
    }

    bool ignorelist_remove(uint32_t node_id)
    {
        FILE* f = fopen(IGNORELIST_FILE, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t total = ftell(f) / sizeof(uint32_t);
        if (total == 0)
        {
            fclose(f);
            return false;
        }
        fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> ids(total);
        fread(ids.data(), sizeof(uint32_t), total, f);
        fclose(f);

        auto it = std::find(ids.begin(), ids.end(), node_id);
        if (it == ids.end())
            return false;
        ids.erase(it);

        if (ids.empty())
        {
            ::remove(IGNORELIST_FILE);
            return true;
        }
        f = fopen(IGNORELIST_FILE, "wb");
        if (!f)
            return false;
        fwrite(ids.data(), sizeof(uint32_t), ids.size(), f);
        fclose(f);
        return true;
    }

    bool ignorelist_remove_at(size_t index)
    {
        FILE* f = fopen(IGNORELIST_FILE, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t total = ftell(f) / sizeof(uint32_t);
        if (index >= total)
        {
            fclose(f);
            return false;
        }
        fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> ids(total);
        fread(ids.data(), sizeof(uint32_t), total, f);
        fclose(f);

        ids.erase(ids.begin() + (int)index);

        if (ids.empty())
        {
            ::remove(IGNORELIST_FILE);
            return true;
        }
        f = fopen(IGNORELIST_FILE, "wb");
        if (!f)
            return false;
        fwrite(ids.data(), sizeof(uint32_t), ids.size(), f);
        fclose(f);
        return true;
    }

    void ignorelist_clear() { ::remove(IGNORELIST_FILE); }

    // ========== Neighbor list file helpers ==========

    static std::string _neighbors_file_path(uint32_t source_node_id)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s/%08lx.dat", NEIGHBORS_DIR, (unsigned long)source_node_id);
        return buf;
    }

    bool neighbors_save(uint32_t source_node_id, const std::vector<NeighborEntry>& entries)
    {
        struct stat st;
        if (stat(NEIGHBORS_DIR, &st) != 0)
            mkdir(NEIGHBORS_DIR, 0755);

        std::string path = _neighbors_file_path(source_node_id);
        if (entries.empty())
        {
            ::remove(path.c_str());
            return true;
        }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
            return false;
        bool ok = fwrite(entries.data(), sizeof(NeighborEntry), entries.size(), f) == entries.size();
        fclose(f);
        return ok;
    }

    bool neighbors_load(uint32_t source_node_id, std::vector<NeighborEntry>& out)
    {
        out.clear();
        std::string path = _neighbors_file_path(source_node_id);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        size_t sz = ftell(f);
        size_t count = sz / sizeof(NeighborEntry);
        if (count == 0)
        {
            fclose(f);
            return true;
        }
        out.resize(count);
        fseek(f, 0, SEEK_SET);
        size_t rd = fread(out.data(), sizeof(NeighborEntry), count, f);
        fclose(f);
        out.resize(rd);
        return true;
    }

    size_t neighbors_get_count(uint32_t source_node_id)
    {
        std::string path = _neighbors_file_path(source_node_id);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f)
            return 0;
        fseek(f, 0, SEEK_END);
        size_t sz = ftell(f);
        fclose(f);
        return sz / sizeof(NeighborEntry);
    }

    void neighbors_delete(uint32_t source_node_id)
    {
        std::string path = _neighbors_file_path(source_node_id);
        ::remove(path.c_str());
    }

} // namespace Mesh
