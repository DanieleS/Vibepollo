/**
 * @file src/platform/windows/playnite_sync.cpp
 * @brief Runtime adapters for the portable Playnite sync policy.
 */

#include "playnite_sync.h"

#include "src/config_playnite.h"
#include "src/logging.h"
#include "src/platform/windows/playnite_integration.h"
#include "src/uuid.h"

#include <sstream>

namespace platf::playnite::sync {
  namespace {
    bool ensure_runtime_app_uuid(nlohmann::json &app) {
      try {
        if (app.contains("playnite-id") && app["playnite-id"].is_string() && !app["playnite-id"].get<std::string>().empty()) {
          return false;
        }
        const bool missing_uuid = !app.contains("uuid") || app["uuid"].is_null() || (app["uuid"].is_string() && app["uuid"].get<std::string>().empty());
        if (missing_uuid) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          return true;
        }
      } catch (...) {
        try {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          return true;
        } catch (...) {}
      }
      return false;
    }
  }  // namespace

  std::string image_source_signature(const std::filesystem::path &src) {
    std::error_code size_error;
    std::error_code time_error;
    const auto size = std::filesystem::file_size(src, size_error);
    const auto modified = std::filesystem::last_write_time(src, time_error);
    if (size_error || time_error) return {};
    std::ostringstream signature;
    signature << policy::normalize_path_for_match(src.string()) << '|' << size << '|' << modified.time_since_epoch().count();
    return signature.str();
  }

  bool convert_playnite_image_to_png(const std::string &src_path, const std::filesystem::path &dst) {
    if (src_path.empty()) return false;
    const auto src = std::filesystem::path(src_path);
    file_handler::make_directory(dst.parent_path().string());
    const auto signature = image_source_signature(src);
    const std::string sidecar = dst.string() + ".src";
    std::error_code exists_error;
    if (!policy::should_reconvert_playnite_image(std::filesystem::exists(dst, exists_error), file_handler::read_file(sidecar.c_str()), signature)) return true;
    if (!platf::img::convert_to_png_96dpi(src.wstring(), dst.wstring())) return false;
    if (!signature.empty()) file_handler::write_file(sidecar.c_str(), signature);
    return true;
  }

  void apply_game_metadata_to_app(const Game &game, nlohmann::json &app, const std::filesystem::path &covers_root) {
    try {
      if (!game.box_art_path.empty()) {
        const auto destination = covers_root / ("playnite_" + game.id + ".png");
        if (convert_playnite_image_to_png(game.box_art_path, destination)) policy::apply_box_art_path(app, destination.generic_string());
      }
    } catch (...) {}
    // The background is converted here but written through the shared metadata container
    // below, so a node never ends up with an image the container did not put there.
    std::string background;
    try {
      if (!game.background_path.empty()) {
        const auto destination = covers_root / ("playnite_bg_" + game.id + ".png");
        if (convert_playnite_image_to_png(game.background_path, destination)) {
          background = destination.generic_string();
        }
      }
    } catch (...) {}
    try {
      const auto destination = covers_root / ("playnite_icon_" + game.id + ".png");
      std::string install_dir = !game.install_dir.empty() ? game.install_dir : game.working_dir;
      if (install_dir.empty()) {
        std::string cached;
        if (platf::playnite::get_cached_install_dir(game.id, cached)) install_dir = cached;
      }
      platf::img::IconResolutionInfo diagnostics;
      if (platf::img::resolve_best_app_icon_png(std::filesystem::path(game.icon_path).wstring(), std::filesystem::path(game.exe).wstring(), std::filesystem::path(install_dir).wstring(), destination.wstring(), &diagnostics)) {
        policy::apply_icon_path(app, destination.generic_string());
        BOOST_LOG(debug) << "Playnite sync icon: name='" << game.name << "' installDir='" << install_dir << "' exeIcon=" << diagnostics.exe_size << " playniteIcon=" << diagnostics.icon_size << " -> width=" << platf::img::image_pixel_width(destination.wstring());
      } else {
        policy::apply_icon_path(app, {});
      }
    } catch (...) {}
    // Store identity. Playnite's own game id is a local GUID, so the owning plugin's id for
    // the game is the only thing that can be looked up in an external database later.
    try {
      const auto set_or_erase = [&app](const char *key, const std::string &value) {
        if (!value.empty()) {
          app[key] = value;
        } else {
          app.erase(key);
        }
      };
      set_or_erase("playnite-source", game.plugin_name);
      set_or_erase("playnite-source-id", game.store_id);
    } catch (...) {}
    // Descriptive metadata. Playnite writes it unless something else already claimed the app:
    // a game the user corrected by hand, or one the IGDB resolver described, must survive the
    // next library sync or the correction would last until the following one. With
    // playnite_sync_metadata off the user has said Playnite is never the source, so whatever
    // the app carries is kept as is and a new app is left for IGDB to describe.
    try {
      const auto existing = metadata::read_from_app(app);
      const bool claimed_elsewhere = existing.locked || existing.source == "igdb" || !config::playnite.sync_metadata;
      metadata::game_metadata_t meta;
      if (claimed_elsewhere) {
        meta = existing;
        // A game IGDB describes, or one the user edited, still takes Playnite's hero art when
        // it has none of its own: art from somewhere beats no art, and neither of those cases
        // said anything about images. Turning playnite_sync_metadata off did, so there the
        // field is left alone like the rest.
        if (meta.background_image_path.empty() && config::playnite.sync_metadata) {
          meta.background_image_path = background;
        }
      } else {
        meta.description = game.description;
        meta.genres = game.genres;
        meta.developers = game.developers;
        meta.publishers = game.publishers;
        meta.release_date = game.release_date;
        meta.community_score = game.community_score;
        meta.critic_score = game.critic_score;
        meta.background_image_path = background;
        meta.igdb_id = existing.igdb_id;
        meta.source = "playnite";
      }
      // Playtime and last-played are Playnite's either way: it is the thing that actually
      // watched the game run, which no database and no manual edit can stand in for.
      meta.last_played = game.last_played;
      meta.playtime_minutes = game.playtime_minutes;
      meta.present = metadata::has_any_value(meta);
      metadata::write_to_app(app, meta);
    } catch (...) {}
  }

  void apply_game_metadata_to_app(const Game &game, nlohmann::json &app) {
    apply_game_metadata_to_app(game, app, platf::appdata() / "covers");
  }

  void write_and_refresh_apps(nlohmann::json &root, const std::string &apps_path) {
    file_handler::write_file(apps_path.c_str(), root.dump(4));
    confighttp::refresh_client_apps_cache(root, false);
  }

  void autosync_reconcile(nlohmann::json &root, const std::vector<Game> &all_games, bool library_complete, int recent_count, int recent_age_days, int delete_after_days, bool require_replacement, bool sync_all_installed, const std::vector<std::string> &categories, const std::vector<std::string> &include_plugins, const std::vector<std::string> &exclude_categories, const std::vector<std::string> &exclude_ids, const std::vector<std::string> &exclude_plugins, bool remove_uninstalled, bool exclude_hidden, bool &changed, std::size_t &matched_out, bool manage_membership) {
    bool runtime_changed = false;
    if (root.contains("apps") && root["apps"].is_array()) {
      for (auto &app : root["apps"]) runtime_changed = ensure_runtime_app_uuid(app) || runtime_changed;
    }
    policy::autosync_reconcile(root, all_games, library_complete, recent_count, recent_age_days, delete_after_days, require_replacement, sync_all_installed, categories, include_plugins, exclude_categories, exclude_ids, exclude_plugins, remove_uninstalled, exclude_hidden, changed, matched_out, manage_membership, static_cast<policy::MetadataUpdater>(&apply_game_metadata_to_app));
    changed = changed || runtime_changed;
  }
}  // namespace platf::playnite::sync
