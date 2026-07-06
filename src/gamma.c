/* Gamma / night-light.
 *
 * Two paths, both native:
 *   - the gamma-control protocol, handed to the scene, so external tools like
 *     wlsunset/gammastep can drive the screen temperature.
 *   - an integrated `gamma <temp> [brightness]` command that builds a per-channel
 *     LUT from a colour temperature (Kelvin) and a brightness multiplier and
 *     applies it as the scene's colour transform every frame — no external
 *     process needed.
 */
#include <math.h>
#include <stdlib.h>

#include <wlr/render/color.h>
#include <wlr/types/wlr_gamma_control_v1.h>

#include "w3ld.h"

#define LUT_DIM 1024

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

/* temperature <= 0 resets to neutral (no transform). */
void w3ld_gamma_set (
	struct w3ld_server *server,
	double temperature,
	double brightness
) {
	if (server->gamma_transform) {
		wlr_color_transform_unref(server->gamma_transform);
		server->gamma_transform = NULL;
	}

	if (temperature > 0) {
		double rm, gm, bm;
		temperature_rgb(temperature, &rm, &gm, &bm);
		brightness = clamp01(brightness);
		rm *= brightness;
		gm *= brightness;
		bm *= brightness;

		uint16_t *r = malloc(LUT_DIM * sizeof *r);
		uint16_t *g = malloc(LUT_DIM * sizeof *g);
		uint16_t *b = malloc(LUT_DIM * sizeof *b);
		for (size_t i = 0; i < LUT_DIM; i++) {
			double base = (double)i / (LUT_DIM - 1);
			r[i] = (uint16_t)(clamp01(base * rm) * 65535);
			g[i] = (uint16_t)(clamp01(base * gm) * 65535);
			b[i] = (uint16_t)(clamp01(base * bm) * 65535);
		}
		server->gamma_transform =
			wlr_color_transform_init_lut_3x1d(LUT_DIM, r, g, b);
		free(r);
		free(g);
		free(b);
	}

	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link)
		wlr_output_schedule_frame(output->wlr_output);
}

void w3ld_gamma_setup (struct w3ld_server *server) {
	struct wlr_gamma_control_manager_v1 *manager =
		wlr_gamma_control_manager_v1_create(server->display);
	wlr_scene_set_gamma_control_manager_v1(server->scene, manager);
}
