/**
 * @file app_nodes.h
 * @author d4rkmen
 * @brief Nodes widget - node list with P2P messaging
 * @version 1.0
 * @date 2025-01-03
 *
 * @copyright Copyright (c) 2025
 *
 */
#pragma once

#include "../apps.h"
#include <vector>
#include <string>

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "apps/utils/anim/scroll_text.h"
#include "apps/utils/anim/hl_text.h"
#include "mesh/node_db.h"
#include "mesh/mesh_data.h"

#include "assets/app_nodes.h"
#include "assets/key_shared.h"
#include "assets/key_pke.h"
#include "assets/key_mismatch.h"
#include "assets/pos_manual.h"
#include "assets/pos_internal.h"
#include "assets/pos_external.h"
#include "assets/pwr.h"
#include "assets/chat.h"

namespace MOONCAKE::APPS
{

    class AppNodes : public APP_BASE
    {
    public:
        // View states
        enum class ViewState
        {
            NODE_LIST,
            NODE_DETAIL,
            NODE_MAP,
            DIRECT_MESSAGE,
            TRACEROUTE_LOG,
            TRACEROUTE_DETAIL,
            FAVORITE_LIST,
            IGNORE_LIST,
            NEIGHBOR_LIST,
            QUICK_MESSAGES
        };

        // OSM raster tile map constants
        static constexpr int MAP_TILE_PX = 256;
        static constexpr int MAP_MIN_ZOOM = 1;
        static constexpr int MAP_MAX_ZOOM = 15;
        static constexpr int MAP_DEFAULT_ZOOM = 10;

    private:
        struct
        {
            HAL::Hal* hal;
            ViewState view_state;
            ViewState prev_view_state;

            // Node list state (no longer stores all nodes in memory)
            int selected_index;
            int scroll_offset;
            size_t total_node_count;        // Just the count, nodes loaded on-demand
            uint32_t list_selected_node_id; // Track selected node by ID across refreshes
            bool update_list;

            // Detail view state
            uint32_t selected_node_id;
            int detail_scroll;
            Mesh::NodeInfo selected_node; // Currently selected node (loaded on demand)
            bool selected_node_valid;

            // DM state (lazy-loaded: only visible messages kept in RAM)
            uint32_t dm_msg_count;                // Total messages in conversation
            std::vector<uint16_t> dm_line_counts; // Wrapped line count per message (lightweight)
            int dm_cur_line;                      // First visible line (scroll position)
            int dm_total_lines;                   // Total wrapped lines across all messages
            int dm_text_width_px;                 // Cached max text pixel width for wrap calculation
            bool ctrl;
            // Traceroute state (file-backed, only visible items loaded)
            uint32_t tr_total_count;                 // Total records on disk
            int tr_selected_index;                   // Selected item in log list
            int tr_scroll_offset;                    // Scroll offset in log list
            int tr_detail_scroll;                    // Scroll offset in detail view
            Mesh::TraceRouteResult tr_detail_result; // Loaded detail for selected item

            // Favorite list state (file-backed, only visible items loaded)
            size_t fav_total_count;
            int fav_selected_index;
            int fav_scroll_offset;

            // Ignore list state (file-backed, only visible items loaded)
            size_t ign_total_count;
            int ign_selected_index;
            int ign_scroll_offset;

            // Neighbor list state (in-memory, from NEIGHBORINFO_APP packets)
            std::vector<Mesh::NeighborEntry> nbr_list;
            int nbr_selected_index;
            int nbr_scroll_offset;
            uint32_t nbr_source_node_id;

            // Quick messages state
            std::vector<std::string> qm_templates;
            int qm_selected_index;
            int qm_scroll_offset;

            // Map view state
            double map_center_lat;
            double map_center_lon;
            int map_zoom;
            char map_tile_dir[64];
            int map_style_idx;

            // Sorting
            Mesh::SortOrder sort_order;

            // Animation contexts
            UTILS::SCROLL_TEXT::ScrollTextContext_t name_scroll_ctx;
            UTILS::SCROLL_TEXT::ScrollTextContext_t qm_scroll_ctx;
            UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;
            std::string selected_display_name; // Cached display name for scrolling

            // Change detection (replaces periodic polling)
            uint32_t last_nodedb_change;   // Cached NodeDB change counter
            uint32_t last_msgstore_change; // Cached MeshDataStore change counter
        } _data;

        // Rendering
        bool _render_node_list();
        bool _render_scrolling_name(bool force = false);
        void _render_scrollbar(int panel_x, int panel_width);
        bool _render_node_detail();
        bool _render_node_detail_hint();
        bool _render_dm_view();
        void _render_signal_bars(int x, int y, int16_t rssi);
        void _refresh_dm_line_counts();
        bool _render_traceroute_log();
        bool _render_traceroute_detail();
        bool _render_traceroute_hint();
        bool _render_traceroute_detail_hint();
        bool _render_favorite_list();
        bool _render_favorite_hint();
        bool _render_ignore_list();
        bool _render_ignore_hint();
        bool _render_neighbor_list();
        bool _render_neighbor_hint();
        bool _render_quick_messages();
        bool _render_scrolling_qm(bool force = false);
        bool _render_quick_messages_hint();
        bool _render_node_map();
        bool _render_node_map_hint();

        // Input handling
        void _handle_node_list_input();
        void _handle_node_detail_input();
        void _handle_dm_input();
        void _handle_traceroute_log_input();
        void _handle_traceroute_detail_input();
        void _handle_favorite_list_input();
        void _handle_ignore_list_input();
        void _handle_neighbor_list_input();
        void _handle_quick_messages_input();
        void _handle_node_map_input();

        // Traceroute helpers
        void _start_traceroute();

        // Sorting
        void _apply_sort_order(Mesh::SortOrder new_order);

        // OSM raster tile map helpers
        static void _map_latlon_to_pixel(double lat, double lon, int zoom, double& px, double& py);
        static void _map_pixel_to_latlon(double px, double py, int zoom, double& lat, double& lon);
        bool _map_draw_tile(int tx, int ty, int zoom, int screen_x, int screen_y, int map_w, int map_h, int map_y);

        // Helpers
        bool _selected_node_valid();
        void _refresh_nodes();
        void _refresh_messages();
        void _send_message(const std::string& text);
        bool _render_list_hint();
        bool _render_dm_hint();
        std::string _format_last_seen(uint32_t last_heard);
        std::string _format_node_id(uint32_t id);
        uint32_t _get_node_color(uint32_t node_id);
        uint32_t _get_node_text_color(uint32_t node_id);
        int _get_signal_level(float snr, int16_t rssi);
        uint32_t _get_snr_color(float snr);
        uint32_t _get_rssi_color(int16_t rssi);

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppNodes_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "CONTACTS"; }
        std::string getAppDesc() override { return "Mesh contacts & DMs"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_nodes, nullptr)); }
        void* newApp() override { return new AppNodes; }
        void deleteApp(void* app) override { delete (AppNodes*)app; }
    };

} // namespace MOONCAKE::APPS
