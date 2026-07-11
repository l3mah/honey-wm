/* Configuration: defaults and the `set` command.
 *
 * Settings live in a single honey_config on the server; `set <key> <value>`
 * changes one and re-arranges. Colors are 0xRRGGBBAA. Per-workspace overrides
 * (`set-ws`) live on the workspace and win over these globals.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "honey.h"

void honey_config_defaults (struct honey_config *config) {
	config->layout = honey_layout_by_name("master");
	config->master_mfact = 0.55;
	config->master_nmaster = 1;
	config->master_orientation = HONEY_ORIENT_LEFT;
	config->spiral_ratio = 0.5;
	config->spiral_horizontal = true;
	config->grid_columns = 0; /* auto */
	config->gaps_in = 5;
	config->gaps_out = 8;
	config->smart_gaps = true;
	config->border_size = 1;
	config->border_color_active = 0xBD93F9FF;
	config->border_color_inactive = 0x595959AA;
	config->float_width = 0.33;
	config->float_height = 0.5;
	config->follow_mouse = true;
	config->mouse_follows_focus = true;
	config->warp_on_workspace_switch = false;
	config->new_window_master = false;
	config->focus_new = true;
	config->mouse_focus_new = false;
	config->exit_fullscreen_on_new = true;
	config->allow_tearing = false;
	config->drop_at_cursor = true;
	config->resize_on_border = true;
	config->scroll_workspace = true;
	config->active_opacity = 1.0;
	config->inactive_opacity = 1.0;
	config->dim_inactive = 0.0;
	config->error_window = true;
	config->suspend_hidden = true;
}

/* ------------------------------------------------------------------ parsers */

static bool parse_bool (const char *value) {
	return !strcasecmp(value, "true") || !strcasecmp(value, "1")
		|| !strcasecmp(value, "on") || !strcasecmp(value, "yes");
}

static uint32_t parse_color (const char *value) {
	if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
		value += 2;
	return (uint32_t)strtoul(value, NULL, 16);
}

bool honey_parse_orientation (
	const char *value,
	enum honey_orientation *out
) {
	if (!strcasecmp(value, "left"))
		*out = HONEY_ORIENT_LEFT;
	else if (!strcasecmp(value, "right"))
		*out = HONEY_ORIENT_RIGHT;
	else if (!strcasecmp(value, "top"))
		*out = HONEY_ORIENT_TOP;
	else if (!strcasecmp(value, "bottom"))
		*out = HONEY_ORIENT_BOTTOM;
	else
		return false;
	return true;
}

/* --------------------------------------------------------------------- set */

bool honey_config_set (
	struct honey_server *server,
	const char *key,
	const char *value
) {
	struct honey_config *config = &server->config;

	if (!strcmp(key, "master-mfact")) {
		config->master_mfact = atof(value);
	} else if (!strcmp(key, "master-nmaster")) {
		config->master_nmaster = atoi(value);
	} else if (!strcmp(key, "master-orientation")) {
		if (!honey_parse_orientation(value, &config->master_orientation))
			return false;
	} else if (!strcmp(key, "gaps-in")) {
		config->gaps_in = atoi(value);
	} else if (!strcmp(key, "gaps-out")) {
		config->gaps_out = atoi(value);
	} else if (!strcmp(key, "smart-gaps")) {
		config->smart_gaps = parse_bool(value);
	} else if (!strcmp(key, "border-size")) {
		config->border_size = atoi(value);
	} else if (!strcmp(key, "border-color-active")) {
		config->border_color_active = parse_color(value);
	} else if (!strcmp(key, "border-color-inactive")) {
		config->border_color_inactive = parse_color(value);
	} else if (!strcmp(key, "float-width")) {
		config->float_width = atof(value);
	} else if (!strcmp(key, "float-height")) {
		config->float_height = atof(value);
	} else if (!strcmp(key, "float-app-size")) {
		config->float_app_size = parse_bool(value);
	} else if (!strcmp(key, "follow-mouse")) {
		config->follow_mouse = parse_bool(value);
	} else if (!strcmp(key, "mouse-follows-focus")) {
		config->mouse_follows_focus = parse_bool(value);
	} else if (!strcmp(key, "warp-on-workspace-switch")) {
		config->warp_on_workspace_switch = parse_bool(value);
	} else if (!strcmp(key, "new-window-master")) {
		config->new_window_master = parse_bool(value);
	} else if (!strcmp(key, "focus-new")) {
		config->focus_new = parse_bool(value);
	} else if (!strcmp(key, "mouse-focus-new")) {
		config->mouse_focus_new = parse_bool(value);
	} else if (!strcmp(key, "exit-fullscreen-on-new")) {
		config->exit_fullscreen_on_new = parse_bool(value);
	} else if (!strcmp(key, "allow-tearing")) {
		config->allow_tearing = parse_bool(value);
	} else if (!strcmp(key, "drop-at-cursor")) {
		config->drop_at_cursor = parse_bool(value);
	} else if (!strcmp(key, "resize-on-border")) {
		config->resize_on_border = parse_bool(value);
	} else if (!strcmp(key, "scroll-workspace")) {
		config->scroll_workspace = parse_bool(value);
	} else if (!strcmp(key, "active-opacity")) {
		config->active_opacity = atof(value);
	} else if (!strcmp(key, "inactive-opacity")) {
		config->inactive_opacity = atof(value);
	} else if (!strcmp(key, "dim-inactive")) {
		config->dim_inactive = atof(value);
	} else if (!strcmp(key, "error-window")) {
		config->error_window = parse_bool(value);
	} else if (!strcmp(key, "suspend-hidden")) {
		config->suspend_hidden = parse_bool(value);
	} else if (!strcmp(key, "xwayland-scale")) { /* xwayland-scale (removable) */
		if (!strcasecmp(value, "off")) {
			config->xwayland_scale = 0;
			config->xwayland_scale_auto = false;
		} else if (!strcasecmp(value, "auto")) {
			config->xwayland_scale_auto = true;
		} else {
			double scale = atof(value);
			if (scale < 1.0 || scale > 4.0)
				return false;
			config->xwayland_scale = scale;
			config->xwayland_scale_auto = false;
		}
		honey_xdg_output_update(server);
	} else if (!strcmp(key, "cursor-theme") || !strcmp(key, "cursor-size")) {
		if (!strcmp(key, "cursor-theme")) {
			free(server->cursor_theme);
			server->cursor_theme = strdup(value);
			setenv("XCURSOR_THEME", value, true);
		} else {
			int size = atoi(value);
			if (size < 1)
				return false;
			server->cursor_size = size;
			setenv("XCURSOR_SIZE", value, true);
		}
		wlr_xcursor_manager_destroy(server->xcursor_manager);
		server->xcursor_manager = wlr_xcursor_manager_create(
				server->cursor_theme, server->cursor_size);
		honey_cursor_reload(server); /* load the new theme at every output scale */
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
				"default");
	} else if (!strcmp(key, "layout")) {
		const struct honey_layout *layout = honey_layout_by_name(value);
		if (!layout)
			return false;
		config->layout = layout;
	} else if (!strcmp(key, "spiral-ratio")) {
		double ratio = atof(value);
		if (ratio < 0.1 || ratio > 0.9)
			return false;
		config->spiral_ratio = ratio;
	} else if (!strcmp(key, "spiral-first-split")) {
		if (!strcasecmp(value, "horizontal"))
			config->spiral_horizontal = true;
		else if (!strcasecmp(value, "vertical"))
			config->spiral_horizontal = false;
		else
			return false;
	} else if (!strcmp(key, "grid-columns")) {
		config->grid_columns = atoi(value);
	} else {
		return false;
	}

	honey_arrange(server);
	return true;
}

/* --------------------------------------------------------------------- get */

static const char *orientation_name (enum honey_orientation orientation) {
	switch (orientation) {
	case HONEY_ORIENT_LEFT: return "left";
	case HONEY_ORIENT_RIGHT: return "right";
	case HONEY_ORIENT_TOP: return "top";
	default: return "bottom";
	}
}

bool honey_config_get (
	struct honey_server *server,
	const char *key,
	char *reply,
	size_t reply_size
) {
	struct honey_config *config = &server->config;

	if (!strcmp(key, "xwayland-scale")) {
		if (config->xwayland_scale_auto)
			snprintf(reply, reply_size, "auto");
		else if (config->xwayland_scale > 0)
			snprintf(reply, reply_size, "%g", config->xwayland_scale);
		else
			snprintf(reply, reply_size, "off");
		return true;
	}

	struct {
		const char *key;
		enum { V_BOOL, V_INT, V_DOUBLE, V_COLOR, V_STRING } kind;
		const void *value;
	} keys[] = {
		{ "layout", V_STRING, config->layout->name },
		{ "master-mfact", V_DOUBLE, &config->master_mfact },
		{ "master-nmaster", V_INT, &config->master_nmaster },
		{ "master-orientation", V_STRING,
			orientation_name(config->master_orientation) },
		{ "spiral-ratio", V_DOUBLE, &config->spiral_ratio },
		{ "spiral-first-split", V_STRING,
			config->spiral_horizontal ? "horizontal" : "vertical" },
		{ "grid-columns", V_INT, &config->grid_columns },
		{ "gaps-in", V_INT, &config->gaps_in },
		{ "gaps-out", V_INT, &config->gaps_out },
		{ "smart-gaps", V_BOOL, &config->smart_gaps },
		{ "border-size", V_INT, &config->border_size },
		{ "border-color-active", V_COLOR, &config->border_color_active },
		{ "border-color-inactive", V_COLOR, &config->border_color_inactive },
		{ "float-width", V_DOUBLE, &config->float_width },
		{ "float-height", V_DOUBLE, &config->float_height },
		{ "float-app-size", V_BOOL, &config->float_app_size },
		{ "follow-mouse", V_BOOL, &config->follow_mouse },
		{ "mouse-follows-focus", V_BOOL, &config->mouse_follows_focus },
		{ "warp-on-workspace-switch", V_BOOL, &config->warp_on_workspace_switch },
		{ "new-window-master", V_BOOL, &config->new_window_master },
		{ "focus-new", V_BOOL, &config->focus_new },
		{ "mouse-focus-new", V_BOOL, &config->mouse_focus_new },
		{ "exit-fullscreen-on-new", V_BOOL, &config->exit_fullscreen_on_new },
		{ "allow-tearing", V_BOOL, &config->allow_tearing },
		{ "drop-at-cursor", V_BOOL, &config->drop_at_cursor },
		{ "resize-on-border", V_BOOL, &config->resize_on_border },
		{ "scroll-workspace", V_BOOL, &config->scroll_workspace },
		{ "active-opacity", V_DOUBLE, &config->active_opacity },
		{ "inactive-opacity", V_DOUBLE, &config->inactive_opacity },
		{ "dim-inactive", V_DOUBLE, &config->dim_inactive },
		{ "error-window", V_BOOL, &config->error_window },
		{ "suspend-hidden", V_BOOL, &config->suspend_hidden },
	};

	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		if (strcmp(key, keys[i].key) != 0)
			continue;
		switch (keys[i].kind) {
		case V_BOOL:
			snprintf(reply, reply_size, "%s",
					*(const bool *)keys[i].value ? "true" : "false");
			break;
		case V_INT:
			snprintf(reply, reply_size, "%d", *(const int *)keys[i].value);
			break;
		case V_DOUBLE:
			snprintf(reply, reply_size, "%g", *(const double *)keys[i].value);
			break;
		case V_COLOR:
			snprintf(reply, reply_size, "0x%08X",
					*(const uint32_t *)keys[i].value);
			break;
		case V_STRING:
			snprintf(reply, reply_size, "%s", (const char *)keys[i].value);
			break;
		}
		return true;
	}
	return false;
}
