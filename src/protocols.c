/* Good-citizen protocol managers.
 *
 * Each of these is fully implemented by wlroots; advertising the global is all
 * that's required. This is the low-effort "long tail" that makes standard tools
 * work: clipboard managers, screenshot/screenshare, presentation timing,
 * fractional scale, activation, idle detection, and so on. Managers that need
 * compositor logic (gamma, cursor-shape, tearing, foreign-toplevel, session
 * lock, XWayland) are wired up in their own modules.
 */
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

#include "w3ld.h"

void w3ld_protocols_setup (struct w3ld_server *server) {
	struct wl_display *display = server->display;

	/* let clients allocate dmabuf-backed buffers against our renderer */
	wlr_linux_dmabuf_v1_create_with_renderer(display, 5, server->renderer);

	/* clipboard / selections */
	wlr_primary_selection_v1_device_manager_create(display);
	wlr_data_control_manager_v1_create(display);

	/* activation, presentation timing, scaling, buffers */
	wlr_xdg_activation_v1_create(display);
	wlr_presentation_create(display, server->backend, 2);
	wlr_viewporter_create(display);
	wlr_fractional_scale_manager_v1_create(display, 1);
	wlr_single_pixel_buffer_manager_v1_create(display);
	wlr_alpha_modifier_v1_create(display);
	wlr_content_type_manager_v1_create(display, 1);

	/* input, capture, idle, security */
	wlr_relative_pointer_manager_v1_create(display);
	wlr_screencopy_manager_v1_create(display);
	wlr_export_dmabuf_manager_v1_create(display);
	wlr_idle_notifier_v1_create(display);
	wlr_security_context_manager_v1_create(display);
}
