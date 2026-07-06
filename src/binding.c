/* Keybindings and action dispatch.
 *
 * Bindings are a runtime list edited by the `map`/`unmap` IPC commands, so they
 * come from the config init script rather than being compiled in. An action is
 * a string ("spawn thunar", "workspace 2", "close", ...) parsed and dispatched
 * to the action handlers — the same grammar the IPC uses for direct commands.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/types/wlr_keyboard.h>

#include "w3ld.h"

/* --------------------------------------------------------------- combo parse */

/* Parse "super+shift+q" into a modifier mask and a keysym. */
static bool parse_combo (
	const char *combo,
	uint32_t *out_modifiers,
	xkb_keysym_t *out_sym
) {
	uint32_t modifiers = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;

	char buffer[256];
	strncpy(buffer, combo, sizeof buffer - 1);
	buffer[sizeof buffer - 1] = '\0';

	char *save = NULL;
	char *token = strtok_r(buffer, "+", &save);
	while (token) {
		char *next = strtok_r(NULL, "+", &save);
		if (!next) {
			sym = xkb_keysym_from_name(token, XKB_KEYSYM_CASE_INSENSITIVE);
		} else if (!strcasecmp(token, "super") || !strcasecmp(token, "logo")
				|| !strcasecmp(token, "mod4")) {
			modifiers |= WLR_MODIFIER_LOGO;
		} else if (!strcasecmp(token, "shift")) {
			modifiers |= WLR_MODIFIER_SHIFT;
		} else if (!strcasecmp(token, "ctrl") || !strcasecmp(token, "control")) {
			modifiers |= WLR_MODIFIER_CTRL;
		} else if (!strcasecmp(token, "alt") || !strcasecmp(token, "mod1")) {
			modifiers |= WLR_MODIFIER_ALT;
		} else {
			return false;
		}
		token = next;
	}

	if (sym == XKB_KEY_NoSymbol)
		return false;
	*out_modifiers = modifiers;
	*out_sym = sym;
	return true;
}

/* ------------------------------------------------------------------ keybinds */

bool w3ld_binding_add (
	struct w3ld_server *server,
	const char *combo,
	const char *action
) {
	uint32_t modifiers;
	xkb_keysym_t sym;
	if (!parse_combo(combo, &modifiers, &sym))
		return false;

	struct w3ld_keybind *keybind;
	wl_list_for_each(keybind, &server->keybinds, link) {
		if (keybind->modifiers == modifiers && keybind->sym == sym) {
			free(keybind->action);
			keybind->action = strdup(action);
			return true;
		}
	}

	keybind = calloc(1, sizeof *keybind);
	keybind->modifiers = modifiers;
	keybind->sym = sym;
	keybind->action = strdup(action);
	wl_list_insert(&server->keybinds, &keybind->link);
	return true;
}

bool w3ld_binding_remove (
	struct w3ld_server *server,
	const char *combo
) {
	uint32_t modifiers;
	xkb_keysym_t sym;
	if (!parse_combo(combo, &modifiers, &sym))
		return false;

	struct w3ld_keybind *keybind;
	wl_list_for_each(keybind, &server->keybinds, link) {
		if (keybind->modifiers == modifiers && keybind->sym == sym) {
			wl_list_remove(&keybind->link);
			free(keybind->action);
			free(keybind);
			return true;
		}
	}
	return false;
}

bool w3ld_binding_run (
	struct w3ld_server *server,
	uint32_t modifiers,
	xkb_keysym_t sym
) {
	uint32_t relevant = modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT
			| WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT);
	struct w3ld_keybind *keybind;
	wl_list_for_each(keybind, &server->keybinds, link) {
		if (keybind->sym == sym && keybind->modifiers == relevant) {
			w3ld_action_run(server, keybind->action);
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------- action string */

static enum w3ld_direction parse_direction (const char *arg) {
	if (arg) {
		if (!strcasecmp(arg, "right"))
			return W3LD_DIR_RIGHT;
		if (!strcasecmp(arg, "up"))
			return W3LD_DIR_UP;
		if (!strcasecmp(arg, "down"))
			return W3LD_DIR_DOWN;
	}
	return W3LD_DIR_LEFT;
}

bool w3ld_action_run (
	struct w3ld_server *server,
	const char *action
) {
	char buffer[256];
	strncpy(buffer, action, sizeof buffer - 1);
	buffer[sizeof buffer - 1] = '\0';

	char *verb = buffer;
	char *arg = NULL;
	char *space = strpbrk(buffer, " \t");
	if (space) {
		*space = '\0';
		arg = space + 1;
		while (*arg == ' ' || *arg == '\t')
			arg++;
		if (*arg == '\0')
			arg = NULL;
	}

	if (!strcmp(verb, "spawn")) {
		if (arg)
			w3ld_spawn(arg);
	} else if (!strcmp(verb, "close")) {
		w3ld_action_close(server);
	} else if (!strcmp(verb, "exit")) {
		wl_display_terminate(server->display);
	} else if (!strcmp(verb, "focus-next")) {
		w3ld_action_focus(server, +1);
	} else if (!strcmp(verb, "focus-prev")) {
		w3ld_action_focus(server, -1);
	} else if (!strcmp(verb, "workspace")) {
		if (arg)
			w3ld_action_workspace(server, atoi(arg));
	} else if (!strcmp(verb, "move-to-workspace")) {
		if (arg)
			w3ld_action_move_to_workspace(server, atoi(arg));
	} else if (!strcmp(verb, "workspace-back")) {
		w3ld_action_workspace_back(server);
	} else if (!strcmp(verb, "workspace-next")) {
		w3ld_action_workspace_cycle(server, +1);
	} else if (!strcmp(verb, "workspace-prev")) {
		w3ld_action_workspace_cycle(server, -1);
	} else if (!strcmp(verb, "focus-dir")) {
		w3ld_action_focus_dir(server, parse_direction(arg));
	} else if (!strcmp(verb, "move-to-output")) {
		w3ld_action_move_to_output(server, parse_direction(arg));
	} else {
		LOG("unknown action: %s", verb);
		return false;
	}
	return true;
}

/* -------------------------------------------------------------------- setup */

void w3ld_binding_setup (struct w3ld_server *server) {
	wl_list_init(&server->keybinds);
}
