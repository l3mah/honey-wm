/* Input configuration.
 *
 * Keyboards build their xkb keymap from the stored RMLVO config (kb-layout) and
 * take the repeat rate/delay (kb-repeat). Pointers/touchpads are configured with
 * libinput directly from `input <device> <option> <value>` rules, applied as
 * devices appear and when a rule is added. Device matching is a case-insensitive
 * substring of the device name, or "*" for all.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libinput.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>

#include "honey.h"

/* ------------------------------------------------------------------- keyboard */

void honey_input_apply_keyboard (
	struct honey_server *server,
	struct wlr_keyboard *keyboard
) {
	struct xkb_rule_names names = {
		.rules = server->kb_rules,
		.model = server->kb_model,
		.layout = server->kb_layout,
		.variant = server->kb_variant,
		.options = server->kb_options,
	};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap =
		xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap) {
		wlr_keyboard_set_keymap(keyboard, keymap);
		xkb_keymap_unref(keymap);
	}
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, server->kb_repeat_rate,
			server->kb_repeat_delay);
}

static char *dup_or_null (const char *value) {
	return value ? strdup(value) : NULL;
}

bool honey_kb_layout (
	struct honey_server *server,
	const char *layout,
	const char *variant,
	const char *model,
	const char *options,
	const char *rules
) {
	/* Validate by compiling the keymap before storing anything. */
	struct xkb_rule_names names = {
		.rules = rules, .model = model, .layout = layout,
		.variant = variant, .options = options,
	};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap =
		xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(context);
	if (!keymap)
		return false;
	xkb_keymap_unref(keymap);

	free(server->kb_layout);
	free(server->kb_variant);
	free(server->kb_model);
	free(server->kb_options);
	free(server->kb_rules);
	server->kb_layout = dup_or_null(layout);
	server->kb_variant = dup_or_null(variant);
	server->kb_model = dup_or_null(model);
	server->kb_options = dup_or_null(options);
	server->kb_rules = dup_or_null(rules);

	struct honey_keyboard *keyboard;
	wl_list_for_each(keyboard, &server->keyboards, link)
		honey_input_apply_keyboard(server, keyboard->wlr_keyboard);
	return true;
}

bool honey_kb_repeat (
	struct honey_server *server,
	int rate,
	int delay
) {
	if (rate < 0 || delay < 0)
		return false;
	server->kb_repeat_rate = rate;
	server->kb_repeat_delay = delay;

	struct honey_keyboard *keyboard;
	wl_list_for_each(keyboard, &server->keyboards, link)
		wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, rate, delay);
	return true;
}

/* -------------------------------------------------------------- libinput config */

static bool parse_bool (const char *value) {
	return !strcasecmp(value, "true") || !strcasecmp(value, "1")
		|| !strcasecmp(value, "on") || !strcasecmp(value, "yes");
}

/* Returns false if the option name is unknown. */
static bool apply_option (
	struct libinput_device *device,
	const char *option,
	const char *value
) {
	if (!strcmp(option, "natural-scroll")) {
		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device,
					parse_bool(value));
	} else if (!strcmp(option, "tap")) {
		if (libinput_device_config_tap_get_finger_count(device) > 0)
			libinput_device_config_tap_set_enabled(device, parse_bool(value)
					? LIBINPUT_CONFIG_TAP_ENABLED
					: LIBINPUT_CONFIG_TAP_DISABLED);
	} else if (!strcmp(option, "accel-speed")) {
		if (libinput_device_config_accel_is_available(device))
			libinput_device_config_accel_set_speed(device, atof(value));
	} else if (!strcmp(option, "accel-profile")) {
		if (libinput_device_config_accel_is_available(device))
			libinput_device_config_accel_set_profile(device,
					!strcmp(value, "flat")
					? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
					: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE);
	} else if (!strcmp(option, "scroll-method")) {
		enum libinput_config_scroll_method method =
			!strcmp(value, "edge") ? LIBINPUT_CONFIG_SCROLL_EDGE
			: !strcmp(value, "button") ? LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
			: !strcmp(value, "none") ? LIBINPUT_CONFIG_SCROLL_NO_SCROLL
			: LIBINPUT_CONFIG_SCROLL_2FG;
		libinput_device_config_scroll_set_method(device, method);
	} else if (!strcmp(option, "disable-while-typing")) {
		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, parse_bool(value)
					? LIBINPUT_CONFIG_DWT_ENABLED
					: LIBINPUT_CONFIG_DWT_DISABLED);
	} else if (!strcmp(option, "left-handed")) {
		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, parse_bool(value));
	} else if (!strcmp(option, "middle-emulation")) {
		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device,
					parse_bool(value)
					? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
					: LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	} else {
		return false;
	}
	return true;
}

static void configure_device (
	struct honey_server *server,
	struct wlr_input_device *input_device
) {
	if (!wlr_input_device_is_libinput(input_device))
		return;
	struct libinput_device *device =
		wlr_libinput_get_device_handle(input_device);

	struct honey_input_rule *rule;
	wl_list_for_each(rule, &server->input_rules, link) {
		if (strcmp(rule->device, "*") != 0
				&& !strcasestr(input_device->name, rule->device))
			continue;
		apply_option(device, rule->option, rule->value);
	}
}

/* -------------------------------------------------------------- device tracking */

static void input_device_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct honey_input_device *device =
		wl_container_of(listener, device, destroy);
	wl_list_remove(&device->destroy.link);
	wl_list_remove(&device->link);
	free(device);
}

void honey_input_add_device (
	struct honey_server *server,
	struct wlr_input_device *input_device
) {
	struct honey_input_device *device = calloc(1, sizeof *device);
	device->server = server;
	device->device = input_device;
	device->destroy.notify = input_device_destroy;
	wl_signal_add(&input_device->events.destroy, &device->destroy);
	wl_list_insert(&server->input_devices, &device->link);

	configure_device(server, input_device);
}

bool honey_input_rule_add (
	struct honey_server *server,
	const char *device,
	const char *option,
	const char *value
) {
	/* Reject unknown options up front against a throwaway probe is not
	 * possible without a device, so validate the name here. */
	static const char *known[] = { "natural-scroll", "tap", "accel-speed",
		"accel-profile", "scroll-method", "disable-while-typing",
		"left-handed", "middle-emulation", NULL };
	bool valid = false;
	for (int i = 0; known[i]; i++) {
		if (!strcmp(option, known[i])) {
			valid = true;
			break;
		}
	}
	if (!valid)
		return false;

	/* Replace an existing rule with the same device + option. */
	struct honey_input_rule *rule;
	wl_list_for_each(rule, &server->input_rules, link) {
		if (!strcmp(rule->device, device) && !strcmp(rule->option, option)) {
			free(rule->value);
			rule->value = strdup(value);
			goto apply;
		}
	}
	rule = calloc(1, sizeof *rule);
	rule->device = strdup(device);
	rule->option = strdup(option);
	rule->value = strdup(value);
	wl_list_insert(&server->input_rules, &rule->link);

apply:;
	struct honey_input_device *tracked;
	wl_list_for_each(tracked, &server->input_devices, link)
		configure_device(server, tracked->device);
	return true;
}

/* -------------------------------------------------------------------- setup */

void honey_input_setup (struct honey_server *server) {
	wl_list_init(&server->input_rules);
	wl_list_init(&server->input_devices);
	server->kb_repeat_rate = 25;
	server->kb_repeat_delay = 600;
}
