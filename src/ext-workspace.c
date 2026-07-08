/* ext-workspace protocol: standardized workspace info for generic bars.
 *
 * Mirrors the compositor's workspace model onto ext-workspace-v1: one group per
 * output, one workspace handle per honey_workspace (id "OUTPUT:N", name = label
 * or number, active state tracked). Client activate requests switch the
 * workspace on its output. Sync runs after every arrange, creating handles
 * lazily, so the mirror can never drift from the real state.
 */
#include <stdio.h>

#include <wlr/types/wlr_ext_workspace_v1.h>

#include "honey.h"

/* --------------------------------------------------------------------- sync */

void honey_ext_workspace_sync (struct honey_server *server) {
	if (!server->ext_workspace_manager)
		return;

	struct honey_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output->ext_group) {
			output->ext_group = wlr_ext_workspace_group_handle_v1_create(
					server->ext_workspace_manager, 0);
			wlr_ext_workspace_group_handle_v1_output_enter(output->ext_group,
					output->wlr_output);
		}

		struct honey_workspace *workspace;
		wl_list_for_each(workspace, &output->workspaces, link) {
			if (!workspace->ext) {
				char id[64];
				snprintf(id, sizeof id, "%s:%d", output->wlr_output->name,
						workspace->number);
				workspace->ext = wlr_ext_workspace_handle_v1_create(
						server->ext_workspace_manager, id,
						EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
				workspace->ext->data = workspace;
				wlr_ext_workspace_handle_v1_set_group(workspace->ext,
						output->ext_group);
			}

			char label[64];
			if (!workspace->name)
				snprintf(label, sizeof label, "%d", workspace->number);
			wlr_ext_workspace_handle_v1_set_name(workspace->ext,
					workspace->name ? workspace->name : label);
			wlr_ext_workspace_handle_v1_set_active(workspace->ext,
					workspace == output->active);
		}
	}
}

/* ----------------------------------------------------------------- requests */

static void handle_commit (
	struct wl_listener *listener,
	void *data
) {
	struct honey_server *server =
		wl_container_of(listener, server, ext_workspace_commit);
	struct wlr_ext_workspace_v1_commit_event *event = data;

	struct wlr_ext_workspace_v1_request *request;
	wl_list_for_each(request, event->requests, link) {
		if (request->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE
				|| !request->activate.workspace)
			continue;
		struct honey_workspace *workspace = request->activate.workspace->data;
		if (workspace)
			honey_switch_workspace(workspace->output, workspace->number);
	}
}

/* -------------------------------------------------------------------- setup */

void honey_ext_workspace_setup (struct honey_server *server) {
	server->ext_workspace_manager =
		wlr_ext_workspace_manager_v1_create(server->display, 1);
	server->ext_workspace_commit.notify = handle_commit;
	wl_signal_add(&server->ext_workspace_manager->events.commit,
			&server->ext_workspace_commit);
}
