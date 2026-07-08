/* Gamma / night-light.
 *
 * Two paths, both native:
 *   - the gamma-control protocol, handed to the scene, so external tools like
 *     wlsunset/gammastep can drive the screen temperature.
 *   - an integrated `gamma <temp> [brightness]` command that builds a
 *     per-channel LUT from a colour temperature (Kelvin) and a brightness
 *     multiplier. The LUT rides the output state, which the DRM backend
 *     offloads to CRTC gamma hardware — renderer-agnostic. The kernel requires
 *     the LUT to be exactly the CRTC's gamma size, which differs per output, so
 *     each output carries its own transform.
 */
#include <math.h>
#include <stdlib.h>

#include <wlr/render/color.h>
#include <wlr/types/wlr_gamma_control_v1.h>

#include "honey.h"

static double clamp01 (double value) {
	return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

/* Colour-temperature -> per-channel multiplier (Tanner Helland approximation). */
static void temperature_rgb (
	double kelvin,
	double *red,
	double *green,
	double *blue
) {
	double t = kelvin / 100.0;
	double r, g, b;

	r = t <= 66 ? 255.0 : 329.698727446 * pow(t - 60, -0.1332047592);
	g = t <= 66 ? 99.4708025861 * log(t) - 161.1195681661
		: 288.1221695283 * pow(t - 60, -0.0755148492);
	b = t >= 66 ? 255.0
		: t <= 19 ? 0.0 : 138.5177312231 * log(t - 10) - 305.0447927307;

	*red = clamp01(r / 255.0);
	*green = clamp01(g / 255.0);
	*blue = clamp01(b / 255.0);
}

/* Rebuild this output's transform for the server's current gamma target, sized
 * to the CRTC's gamma ramp. */
void honey_gamma_update_output (struct honey_output *output) {
	struct honey_server *server = output->server;

	if (output->gamma_transform) {
		wlr_color_transform_unref(output->gamma_transform);
		output->gamma_transform = NULL;
	}

	size_t dim = wlr_output_get_gamma_size(output->wlr_output);
	if (server->gamma_temperature > 0 && dim > 0) {
		double rm, gm, bm;
		temperature_rgb(server->gamma_temperature, &rm, &gm, &bm);
		double brightness = clamp01(server->gamma_brightness);
		rm *= brightness;
		gm *= brightness;
		bm *= brightness;

		uint16_t *r = malloc(dim * sizeof *r);
		uint16_t *g = malloc(dim * sizeof *g);
		uint16_t *b = malloc(dim * sizeof *b);
		for (size_t i = 0; i < dim; i++) {
			double base = (double)i / (dim - 1);
			r[i] = (uint16_t)(clamp01(base * rm) * 65535);
			g[i] = (uint16_t)(clamp01(base * gm) * 65535);
			b[i] = (uint16_t)(clamp01(base * bm) * 65535);
		}
		output->gamma_transform =
			wlr_color_transform_init_lut_3x1d(dim, r, g, b);
		free(r);
		free(g);
		free(b);
	}

	/* CRTC gamma is a sticky atomic property: one property-only commit applies
	 * it until the next change — the frame path never touches it. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_color_transform(&state, output->gamma_transform);
	if (!wlr_output_commit_state(output->wlr_output, &state))
		LOG("gamma: %s rejected the transform", output->wlr_output->name);
	wlr_output_state_finish(&state);
}

/* temperature <= 0 resets to neutral (no transform). */
void honey_gamma_set (
	struct honey_server *server,
	double temperature,
	double brightness
) {
	server->gamma_temperature = temperature;
	server->gamma_brightness = brightness;

	struct honey_output *output;
	wl_list_for_each(output, &server->outputs, link)
		honey_gamma_update_output(output);

	/* One source of truth: any gamma change (scroll, hotkey, direct command)
	 * flows through here, so subscribers stay in sync however it was driven. */
	honey_status_broadcast_gamma(server);
}

void honey_gamma_setup (struct honey_server *server) {
	server->gamma_brightness = 1.0;
	server->gamma_brightness_min = 0.05;
	server->gamma_brightness_max = 1.0;
	struct wlr_gamma_control_manager_v1 *manager =
		wlr_gamma_control_manager_v1_create(server->display);
	wlr_scene_set_gamma_control_manager_v1(server->scene, manager);
}
