/**
 * @file app_map.cpp
 * @brief Dedicated Interactive MAP application for MeshCore UI
 */
#include "app_map.h"
#include "esp_log.h"
#include "apps/utils/ui/draw_helper.h"
#include "mesh/mesh_service.h"
#include "common_define.h"
#include "lgfx/v1/misc/enum.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

using namespace MOONCAKE::APPS;

static const char* TAG = "APP_MAP";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAP_TILE_PX 256
#define MAP_MIN_ZOOM 1
#define MAP_MAX_ZOOM 18
#define MAP_DEFAULT_ZOOM 14
#define MAP_BASE_DIR "/sdcard/map"

struct MapStyleConfig
{
    const char* name;
    uint32_t bg_fallback;
    uint32_t text_color;
    uint32_t text_bg;
    uint32_t marker_outline;
    uint32_t crosshair;
};

static const MapStyleConfig MAP_STYLES[] = {
    {"dark", TFT_BLACK, TFT_WHITE, TFT_BLACK, TFT_WHITE, TFT_CYAN},
    {"osm", TFT_WHITE, TFT_BLACK, TFT_WHITE, TFT_BLACK, TFT_RED},
    {"satellite", TFT_BLACK, TFT_YELLOW, TFT_BLACK, TFT_WHITE, TFT_GREEN}
};
static const int NUM_MAP_STYLES = sizeof(MAP_STYLES) / sizeof(MAP_STYLES[0]);

static void map_latlon_to_pixel(double lat, double lon, int zoom, double& px, double& py)
{
    double n = (double)(1 << zoom);
    px = (lon + 180.0) / 360.0 * n * MAP_TILE_PX;
    double lat_rad = lat * M_PI / 180.0;
    py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * MAP_TILE_PX;
}

static bool map_draw_tile(HAL::Hal* hal, const char* tile_dir, int tx, int ty, int zoom, int screen_x, int screen_y, int map_w, int map_h, int map_y)
{
    int n = 1 << zoom;
    if (tx < 0 || tx >= n || ty < 0 || ty >= n)
        return false;

    char path[128];
    bool found = false;
    bool is_png = false;

    const char* candidates[6][2] = {
        { "%s/%d/%d/%d.png", tile_dir },
        { "%s/%d/%d/%d.jpg", tile_dir },
        { "/sdcard/map/%d/%d/%d.png", nullptr },
        { "/sdcard/map/%d/%d/%d.jpg", nullptr },
        { "/sdcard/map/osm/%d/%d/%d.png", nullptr },
        { "/sdcard/map/osm/%d/%d/%d.jpg", nullptr }
    };

    for (int i = 0; i < 6; i++)
    {
        if (candidates[i][1])
            snprintf(path, sizeof(path), candidates[i][0], candidates[i][1], zoom, tx, ty);
        else
            snprintf(path, sizeof(path), candidates[i][0], zoom, tx, ty);

        FILE* f = fopen(path, "rb");
        if (f)
        {
            fclose(f);
            found = true;
            const char* ext = strrchr(path, '.');
            if (ext && strcmp(ext, ".png") == 0)
                is_png = true;
            break;
        }
    }

    if (!found)
        return false;

    auto* canvas = hal->canvas();
    int src_x = 0, src_y = 0;
    int dst_x = screen_x, dst_y = screen_y;
    int draw_w = MAP_TILE_PX, draw_h = MAP_TILE_PX;

    if (dst_x < 0)
    {
        src_x = -dst_x;
        draw_w += dst_x;
        dst_x = 0;
    }
    if (dst_y < map_y)
    {
        src_y = map_y - dst_y;
        draw_h -= (map_y - dst_y);
        dst_y = map_y;
    }
    if (dst_x + draw_w > map_w)
        draw_w = map_w - dst_x;
    if (dst_y + draw_h > map_y + map_h)
        draw_h = map_y + map_h - dst_y;
    if (draw_w <= 0 || draw_h <= 0)
        return false;

    if (is_png)
        return canvas->drawPngFile(path, dst_x, dst_y, draw_w, draw_h, src_x, src_y);
    else
        return canvas->drawJpgFile(path, dst_x, dst_y, draw_w, draw_h, src_x, src_y);
}

void AppMap::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.map_zoom = MAP_DEFAULT_ZOOM;
    _data.map_style_idx = 0;
    snprintf(_data.map_tile_dir, sizeof(_data.map_tile_dir), "%s/dark", MAP_BASE_DIR);
}

void AppMap::onResume()
{
    _data.hal->canvas()->fillScreen(TFT_BLACK);
    _init_map_center();
    _render_map_view();
    _data.hal->canvas_update();
}

void AppMap::_init_map_center()
{
    bool found = false;

#if HAL_USE_GPS
    if (_data.hal->gps() && _data.hal->gps()->hasFix())
    {
        _data.map_center_lat = _data.hal->gps()->getLatitude();
        _data.map_center_lon = _data.hal->gps()->getLongitude();
        found = true;
    }
#endif

    if (!found && _data.hal->nodedb())
    {
        const auto& index = _data.hal->nodedb()->getIndex();
        for (const auto& entry : index)
        {
            if (entry.latitude_i != 0 || entry.longitude_i != 0)
            {
                _data.map_center_lat = entry.latitude_i * 1e-7;
                _data.map_center_lon = entry.longitude_i * 1e-7;
                _data.selected_node_id = entry.node_id;
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        // Default center: 30.556, -89.446
        _data.map_center_lat = 30.556;
        _data.map_center_lon = -89.446;
    }
}

bool AppMap::_render_map_view()
{
    auto* canvas = _data.hal->canvas();
    const auto& mstyle = MAP_STYLES[_data.map_style_idx];
    canvas->fillScreen(mstyle.bg_fallback);
    canvas->setFont(FONT_12);

    const int map_y = 0;
    const int map_w = canvas->width();
    const int map_h = canvas->height() - 9;

    int z = std::clamp(_data.map_zoom, MAP_MIN_ZOOM, MAP_MAX_ZOOM);

    double center_px, center_py;
    map_latlon_to_pixel(_data.map_center_lat, _data.map_center_lon, z, center_px, center_py);

    double vp_left = center_px - map_w / 2.0;
    double vp_top = center_py - map_h / 2.0;

    int tx_min = (int)floor(vp_left / MAP_TILE_PX);
    int tx_max = (int)floor((vp_left + map_w - 1) / MAP_TILE_PX);
    int ty_min = (int)floor(vp_top / MAP_TILE_PX);
    int ty_max = (int)floor((vp_top + map_h - 1) / MAP_TILE_PX);

    // Draw tile grid
    for (int ty = ty_min; ty <= ty_max; ty++)
    {
        for (int tx = tx_min; tx <= tx_max; tx++)
        {
            int scr_x = (int)(tx * MAP_TILE_PX - vp_left);
            int scr_y = map_y + (int)(ty * MAP_TILE_PX - vp_top);
            map_draw_tile(_data.hal, _data.map_tile_dir, tx, ty, z, scr_x, scr_y, map_w, map_h, map_y);
        }
    }

    // Render nodes from NodeDB
    auto* nodedb = _data.hal->nodedb();
    std::string focused_label;

    if (nodedb)
    {
        uint32_t our_id = _data.hal->mesh() ? _data.hal->mesh()->getNodeId() : 0;
        const auto& index = nodedb->getIndex();

        for (const auto& entry : index)
        {
            if (entry.latitude_i == 0 && entry.longitude_i == 0)
                continue;

            double nlat = entry.latitude_i * 1e-7;
            double nlon = entry.longitude_i * 1e-7;

            double npx, npy;
            map_latlon_to_pixel(nlat, nlon, z, npx, npy);
            int nx = (int)(npx - vp_left);
            int ny = map_y + (int)(npy - vp_top);

            if (nx < -10 || nx > map_w + 10 || ny < map_y - 10 || ny > map_y + map_h + 10)
                continue;

            bool is_selected = (entry.node_id == _data.selected_node_id);
            bool is_ours = (entry.node_id == our_id);

            uint32_t marker_color = UTILS::UI::node_color(entry.node_id);
            int marker_r = is_selected ? 4 : (is_ours ? 3 : 2);

            canvas->fillCircle(nx, ny, marker_r + 1, mstyle.marker_outline);
            canvas->fillCircle(nx, ny, marker_r, marker_color);

            if (is_selected)
            {
                canvas->drawLine(nx - 7, ny, nx - 3, ny, mstyle.crosshair);
                canvas->drawLine(nx + 3, ny, nx + 7, ny, mstyle.crosshair);
                canvas->drawLine(nx, ny - 7, nx, ny - 3, mstyle.crosshair);
                canvas->drawLine(nx, ny + 3, nx, ny + 7, mstyle.crosshair);

                Mesh::NodeInfo ni;
                if (nodedb->getNode(entry.node_id, ni))
                    focused_label = Mesh::NodeDB::getLongLabel(ni);
                else
                    focused_label = entry.short_name[0] ? entry.short_name : "Node";
            }
        }
    }

    // Top overlay bar
    char status_buf[96];
    if (!focused_label.empty())
    {
        snprintf(status_buf, sizeof(status_buf), "[z%d] %s (%.4f, %.4f)", z, focused_label.c_str(), _data.map_center_lat, _data.map_center_lon);
    }
    else
    {
        snprintf(status_buf, sizeof(status_buf), "[z%d] (%.4f, %.4f)", z, _data.map_center_lat, _data.map_center_lon);
    }

    canvas->fillRect(0, 0, map_w, 14, TFT_BLACK);
    canvas->setTextColor(TFT_GREEN, TFT_BLACK);
    canvas->drawString(status_buf, 4, 1);

    // Bottom hint bar
    canvas->fillRect(0, canvas->height() - 9, map_w, 9, TFT_BLACK);
    canvas->setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas->drawString("Arrows:Pan  , / .:Zoom  N:Next  C:Center  ESC:Exit", 2, canvas->height() - 8);

    _data.hal->canvas_update();
    return true;
}

void AppMap::_handle_input()
{
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (_data.hal->keyboard()->isPressed())
    {
        double pan_step = 120.0 / (1 << _data.map_zoom);

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            destroyApp();
            return;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            _data.map_center_lon -= pan_step;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            _data.map_center_lon += pan_step;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            _data.map_center_lat += pan_step * 0.7;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            _data.map_center_lat -= pan_step * 0.7;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_EQUAL) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_UNDERSCORE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_EQUAL);
            if (_data.map_zoom < MAP_MAX_ZOOM)
                _data.map_zoom++;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_SPACE) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_SPACE);
            if (_data.map_zoom > MAP_MIN_ZOOM)
                _data.map_zoom--;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_C) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_C);
            _init_map_center();
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_N))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_N);

            if (_data.hal->nodedb())
            {
                const auto& index = _data.hal->nodedb()->getIndex();
                if (!index.empty())
                {
                    _data.focused_node_idx = (_data.focused_node_idx + 1) % index.size();
                    const auto& entry = index[_data.focused_node_idx];
                    if (entry.latitude_i != 0 || entry.longitude_i != 0)
                    {
                        _data.selected_node_id = entry.node_id;
                        _data.map_center_lat = entry.latitude_i * 1e-7;
                        _data.map_center_lon = entry.longitude_i * 1e-7;
                    }
                }
            }
        }
    }
}

void AppMap::onRunning()
{
    uint32_t now = millis();
    _handle_input();
    if (now - _data.last_update > 50)
    {
        _data.last_update = now;
        _render_map_view();
    }
}

void AppMap::onDestroy()
{
    ESP_LOGI(TAG, "MAP app closed");
}
