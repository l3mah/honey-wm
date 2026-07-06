/* Configuration: defaults and the `set` command.
 *
 * Settings live in a single w3ld_config on the server; `set <key> <value>`
 * changes one and re-arranges. Colors are 0xRRGGBBAA. Per-workspace overrides
 * (`set-ws`) live on the workspace and win over these globals.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "w3ld.h"

void w3ld_config_defaults (struct w3ld_config *config) {
	config->layout = w3ld_layout_by_name("master");
	config->master_mfact = 0.55;
	config->master_nmaster = 1;
	config->master_orientation = W3LD_ORIENT_LEFT;
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

bool w3ld_parse_orientation (
	const char *value,
	enum w3ld_orientation *out
) {
	if (!strcasecmp(value, "left"))
		*out = W3LD_ORIENT_LEFT;
	else if (!strcasecmp(value, "right"))
		*out = W3LD_ORIENT_RIGHT;
	else if (!strcasecmp(value, "top"))
		*out = W3LD_ORIENT_TOP;
	else if (!strcasecmp(value, "bottom"))
		*out = W3LD_ORIENT_BOTTOM;
	else
		return false;
	return true;
}

/* --------------------------------------------------------------------- set */

bool w3ld_config_set (
	struct w3ld_server *server,
	const char *key,
	const char *value
) {
	struct w3ld_config *config = &server->config;

	if (!strcmp(key, "master-mfact")) {
		config->master_mfact = atof(value);
	} else if (!strcmp(key, "master-nmaster")) {
		config->master_nmaster = atoi(value);
	} else if (!strcmp(key, "master-orientation")) {
		if (!w3ld_parse_orientation(value, &config->master_orientation))
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
	} else if (!strcmp(key, "layout")) {
		const struct w3ld_layout *layout = w3ld_layout_by_name(value);
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

	w3ld_arrange(server);
	return true;
}
