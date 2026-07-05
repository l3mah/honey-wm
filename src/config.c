/* Configuration: defaults and the `set` command.
 *
 * Settings live in a single w3ld_config on the server. The lakectl-style grammar
 * is mirrored: `set <key> <value>`. Layout keys re-tile on change; behavior keys
 * just update the flag. Colors are 0xRRGGBBAA.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "w3ld.h"

void w3ld_config_defaults (struct w3ld_config *config) {
	config->master_mfact = 0.55;
	config->master_nmaster = 1;
	config->master_orientation = W3LD_ORIENT_LEFT;
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

static bool parse_orientation (
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
		if (!parse_orientation(value, &config->master_orientation))
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
	} else if (!strcmp(key, "layout")) {
		/* master is the only layout so far; spiral/grid arrive in M5. */
		if (strcmp(value, "master") != 0)
			return false;
	} else {
		return false;
	}

	w3ld_arrange(server);
	return true;
}
