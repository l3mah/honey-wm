/* Output management.
 *
 * Each wlr_output is enabled at its preferred mode, added to the output layout,
 * given a wlr_scene_output that composites the scene every frame, and seeded
 * with workspace 1. Its usable box is the layout geometry (layer-shell will
 * reserve exclusive zones). Removal reassigns the output's windows to a
 * surviving output.
 */
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_tearing_control_v1.h>

#include "honey.h"

/* ------------------------------------------------------------------- listeners */

/* Tear when allowed and the focused window on this output fills it and hints
 * async presentation (a fullscreen game). */
static bool tearing_wanted (struct honey_output *output) {
	struct honey_server *server = output->server;
	if (!server->config.allow_tearing || !server->focused)
		return false;
	struct honey_window *window = server->focused;
	if (window->workspace->output != output)
		return false;
	if (window->geom.width < output->usable.width
			|| window->geom.height < output->usable.height)
		return false;
	return wlr_tearing_control_manager_v1_surface_hint_from_surface(
			server->tearing_control, honey_window_surface(window))
		== WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

static void output_frame (
	struct wl_listener *listener,
	void *data
) {
	struct honey_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(output->server->scene, output->wlr_output);

	/* Tearing needs a manual async page-flip; everything else goes through
	 * wlr_scene_output_commit, which owns both the render and the commit and so
	 * can fall back to a composited frame when a direct-scanout buffer can't be
	 * imported. The old manual build+commit didn't tell wlr_scene the commit
	 * failed, so it re-picked the same unscannable buffer every vblank and
	 * busy-looped a static output at full refresh (a wallpaper with an AMD
	 * tiled modifier that can't be scanned out). */
	bool committed = false;
	if (tearing_wanted(output)) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		if (wlr_scene_output_build_state(scene_output, &state, NULL)) {
			state.tearing_page_flip = true;
			committed = wlr_output_commit_state(output->wlr_output, &state);
		} else {
			committed = true; /* no damage this frame */
		}
		wlr_output_state_finish(&state);
	}
	if (!committed)
		wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct honey_output *output = wl_container_of(listener, output, destroy);
	struct honey_server *server = output->server;

	struct honey_output *other;
	struct honey_output *fallback = NULL;
	wl_list_for_each(other, &server->outputs, link) {
		if (other != output) {
			fallback = other;
			break;
		}
	}

	struct honey_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (window->workspace && window->workspace->output == output)
			window->workspace = fallback ? fallback->active : NULL;
	}
	if (server->focused_output == output)
		server->focused_output = fallback;

	struct honey_workspace *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &output->workspaces, link) {
		wl_list_remove(&workspace->link);
		if (workspace->ext)
			wlr_ext_workspace_handle_v1_destroy(workspace->ext);
		free(workspace->name);
		free(workspace);
	}
	if (output->ext_group)
		wlr_ext_workspace_group_handle_v1_destroy(output->ext_group);
	if (output->gamma_transform)
		wlr_color_transform_unref(output->gamma_transform);
	honey_xdg_output_output_destroyed(server, output->wlr_output);
	free(output->status_workspaces);
	free(output->status_window);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);

	if (fallback) {
		honey_arrange(server);
		honey_focus_output_active(fallback);
	} else {
		server->focused = NULL;
	}
}

/* --------------------------------------------------------------- new outputs */

static void handle_new_output (
	struct wl_listener *listener,
	void *data
) {
	struct honey_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct honey_output *output = calloc(1, sizeof *output);
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output; /* for wlr_output -> honey_output lookups */
	wl_list_init(&output->workspaces);
	wl_list_init(&output->layer_surfaces);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
			scene_output);

	wlr_output_layout_get_box(server->output_layout, wlr_output,
			&output->usable);

	/* Load the cursor at this output's scale for a hardware cursor here. */
	honey_cursor_reload(server);

	output->active = honey_workspace_get(output, 1);
	if (!server->focused_output)
		server->focused_output = output;

	if (server->gamma_temperature > 0)
		honey_gamma_update_output(output); /* hotplugged during night light */

	LOG("new output %s %dx%d at %d,%d", wlr_output->name,
			output->usable.width, output->usable.height,
			output->usable.x, output->usable.y);
}

/* -------------------------------------------------------------------- setup */

void honey_output_setup (struct honey_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
}
