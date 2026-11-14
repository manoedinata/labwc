// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "workspaces.h"
#include <assert.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include "buffer.h"
#include "common/font.h"
#include "common/graphic-helpers.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "input/keyboard.h"
#include "labwc.h"
#include "output.h"
#include "session-lock.h"
#include "show-desktop.h"
#include "theme.h"
#include "view.h"

#define EXT_WORKSPACES_VERSION 1

/* Internal helpers */
static size_t
parse_workspace_index(const char *name)
{
	/*
	 * We only want to get positive numbers which span the whole string.
	 *
	 * More detailed requirement:
	 *  .---------------.--------------.
	 *  |     Input     | Return value |
	 *  |---------------+--------------|
	 *  | "2nd desktop" |      0       |
	 *  |    "-50"      |      0       |
	 *  |     "0"       |      0       |
	 *  |    "124"      |     124      |
	 *  |    "1.24"     |      0       |
	 *  `------------------------------´
	 *
	 * As atoi() happily parses any numbers until it hits a non-number we
	 * can't really use it for this case. Instead, we use strtol() combined
	 * with further checks for the endptr (remaining non-number characters)
	 * and returned negative numbers.
	 */
	long index;
	char *endptr;
	errno = 0;
	index = strtol(name, &endptr, 10);
	if (errno || *endptr != '\0' || index < 0) {
		return 0;
	}
	return index;
}

static bool
per_output(void)
{
	return server.workspaces.per_output;
}

/* True if @a and @b take part in the same left/right cycle */
static bool
in_same_group(struct workspace *a, struct workspace *b)
{
	return !per_output() || a->output == b->output;
}

/* The workspace an output is showing (or the global one) */
static struct workspace *
shown_on(struct output *output)
{
	return per_output() ? output->workspace : server.workspaces.current;
}

static void
_osd_update(struct output *only)
{
	struct theme *theme = rc.theme;

	/* Settings */
	uint16_t margin = 10;
	uint16_t padding = 2;
	uint16_t rect_height = theme->osd_workspace_switcher_boxes_height;
	uint16_t rect_width = theme->osd_workspace_switcher_boxes_width;
	bool hide_boxes = theme->osd_workspace_switcher_boxes_width == 0 ||
		theme->osd_workspace_switcher_boxes_height == 0;

	cairo_t *cairo;
	cairo_surface_t *surface;
	struct workspace *workspace;

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output) || (only && output != only)) {
			continue;
		}
		struct workspace *shown = shown_on(output);
		if (!shown) {
			continue;
		}

		/* Dimensions (only this output's workspaces in per-output mode) */
		size_t workspace_count = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			if (in_same_group(workspace, shown)) {
				workspace_count++;
			}
		}
		uint16_t marker_width = workspace_count * (rect_width + padding) - padding;
		uint16_t width = margin * 2 + (marker_width < 200 ? 200 : marker_width);
		uint16_t height = margin * (hide_boxes ? 2 : 3) + rect_height
			+ font_height(&rc.font_osd);

		struct lab_data_buffer *buffer = buffer_create_cairo(width, height,
			output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for workspace OSD");
			continue;
		}

		cairo = cairo_create(buffer->surface);

		/* Background */
		set_cairo_color(cairo, theme->osd_bg_color);
		cairo_rectangle(cairo, 0, 0, width, height);
		cairo_fill(cairo);

		/* Border */
		set_cairo_color(cairo, theme->osd_border_color);
		struct wlr_fbox border_fbox = {
			.width = width,
			.height = height,
		};
		draw_cairo_border(cairo, border_fbox, theme->osd_border_width);

		/* Boxes */
		uint16_t x;
		if (!hide_boxes) {
			x = (width - marker_width) / 2;
			wl_list_for_each(workspace, &server.workspaces.all, link) {
				if (!in_same_group(workspace, shown)) {
					continue;
				}
				bool active = workspace == shown;
				set_cairo_color(cairo, rc.theme->osd_label_text_color);
				struct wlr_fbox fbox = {
					.x = x,
					.y = margin,
					.width = rect_width,
					.height = rect_height,
				};
				draw_cairo_border(cairo, fbox,
					theme->osd_workspace_switcher_boxes_border_width);
				if (active) {
					cairo_rectangle(cairo, x, margin,
						rect_width, rect_height);
					cairo_fill(cairo);
				}
				x += rect_width + padding;
			}
		}

		/* Text */
		set_cairo_color(cairo, rc.theme->osd_label_text_color);
		PangoLayout *layout = pango_cairo_create_layout(cairo);
		pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* Center workspace indicator on the x axis */
		int req_width = font_width(&rc.font_osd, shown->name);
		req_width = MIN(req_width, width - 2 * margin);
		x = (width - req_width) / 2;
		if (!hide_boxes) {
			cairo_move_to(cairo, x, margin * 2 + rect_height);
		} else {
			cairo_move_to(cairo, x, (height - font_height(&rc.font_osd)) / 2.0);
		}
		PangoFontDescription *desc = font_to_pango_desc(&rc.font_osd);
		//pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, req_width * PANGO_SCALE);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, shown->name, -1);
		pango_cairo_show_layout(cairo, layout);

		g_object_unref(layout);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		if (!output->workspace_osd) {
			output->workspace_osd = lab_wlr_scene_buffer_create(
				&server.scene->tree, NULL);
		}
		/* Position the whole thing */
		struct wlr_box output_box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &output_box);
		int lx = output_box.x + (output_box.width - width) / 2;
		int ly = output_box.y + (output_box.height - height) / 2;
		wlr_scene_node_set_position(&output->workspace_osd->node, lx, ly);
		wlr_scene_buffer_set_buffer(output->workspace_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(output->workspace_osd,
			buffer->logical_width, buffer->logical_height);

		/* And finally drop the buffer so it will get destroyed on OSD hide */
		wlr_buffer_drop(&buffer->base);
	}
}

static struct workspace *
workspace_find_by_name(const char *name)
{
	struct workspace *workspace;

	/* by index */
	size_t parsed_index = parse_workspace_index(name);
	if (parsed_index) {
		size_t index = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			if (parsed_index == ++index) {
				return workspace;
			}
		}
	}

	/* by name */
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (!strcmp(workspace->name, name)) {
			return workspace;
		}
	}

	wlr_log(WLR_ERROR, "Workspace '%s' not found", name);
	return NULL;
}

static void
handle_ext_workspace_commit(struct wl_listener *listener, void *data)
{
	struct wlr_ext_workspace_v1_commit_event *event = data;

	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
			if (!req->activate.workspace) {
				continue;
			}
			struct workspace *workspace = req->activate.workspace->data;
			workspaces_switch_to(workspace, /* update_focus */ true);
			wlr_log(WLR_INFO, "activating workspace %s", workspace->name);
		} else if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN
				&& per_output()) {
			if (!req->assign.workspace || !req->assign.group) {
				continue;
			}
			struct workspace *workspace = req->assign.workspace->data;
			struct output *output = req->assign.group->data;
			if (!output) {
				continue;
			}
			wlr_log(WLR_INFO, "assigning workspace %s to output %s",
				workspace->name, output->wlr_output->name);
			if (workspaces_move_to_output(workspace, output,
					/* bind */ true)) {
				workspaces_switch_to(workspace,
					/* update_focus */ true);
			}
		}
	}
}

/* Internal API */
static void
add_workspace(const char *name)
{
	struct workspace *workspace = znew(*workspace);
	workspace->name = xstrdup(name);
	workspace->tree = lab_wlr_scene_tree_create(server.workspace_tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_BOTTOM] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_NORMAL] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_TOP] =
		lab_wlr_scene_tree_create(workspace->tree);
	wl_list_append(&server.workspaces.all, &workspace->link);
	wlr_scene_node_set_enabled(&workspace->tree->node, false);

	uint32_t caps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
	if (per_output()) {
		caps |= EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN;
	}
	workspace->ext_workspace = wlr_ext_workspace_handle_v1_create(
		server.workspaces.ext_manager, /*id*/ NULL, caps);
	workspace->ext_workspace->data = workspace;
	/* In per-output mode the group is that of the workspace's output */
	if (!per_output()) {
		wlr_ext_workspace_handle_v1_set_group(
			workspace->ext_workspace, server.workspaces.ext_group);
	}
	wlr_ext_workspace_handle_v1_set_name(workspace->ext_workspace, name);
}

static bool
workspace_has_views(struct workspace *workspace)
{
	struct view *view;

	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NO_OMNIPRESENT) {
		if (view->workspace == workspace) {
			return true;
		}
	}
	return false;
}

/*
 * Find the next workspace before/after @current. In per-output mode only
 * workspaces on the same output as @current are considered.
 */
static struct workspace *
get_adjacent(struct workspace *current, struct wl_list *workspaces,
		bool wrap, bool reverse, bool occupied)
{
	struct wl_list *start = &current->link;
	struct wl_list *link = reverse ? start->prev : start->next;
	bool has_wrapped = false;

	while (link != start) {
		/* Handle list boundaries */
		if (link == workspaces) {
			if (!wrap || has_wrapped) {
				break;
			}
			link = reverse ? workspaces->prev : workspaces->next;
			has_wrapped = true;
			continue;
		}

		struct workspace *target = wl_container_of(link, target, link);
		if (in_same_group(target, current)
				&& (!occupied || workspace_has_views(target))) {
			return target;
		}

		link = reverse ? link->prev : link->next;
	}

	return NULL;
}

static struct workspace *
get_prev(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent(current, workspaces, wrap, true, false);
}

static struct workspace *
get_next(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent(current, workspaces, wrap, false, false);
}

static struct workspace *
get_prev_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent(current, workspaces, wrap, true, true);
}

static struct workspace *
get_next_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent(current, workspaces, wrap, false, true);
}

static int
_osd_handle_timeout(void *data)
{
	struct seat *seat = data;
	workspaces_osd_hide(seat);
	/* Don't re-check */
	return 0;
}

/* @only: show on this output only, or on all outputs if NULL */
static void
_osd_show(struct output *only)
{
	if (!rc.workspace_config.popuptime) {
		return;
	}

	_osd_update(only);
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->workspace_osd) {
			continue;
		}
		bool show = output_is_usable(output) && (!only || output == only);
		wlr_scene_node_set_enabled(&output->workspace_osd->node, show);
	}
	if (keyboard_get_all_modifiers(&server.seat)) {
		/* Hidden by release of all modifiers */
		server.seat.workspace_osd_shown_by_modifier = true;
	} else {
		/* Hidden by timer */
		if (!server.seat.workspace_osd_timer) {
			server.seat.workspace_osd_timer = wl_event_loop_add_timer(
				server.wl_event_loop, _osd_handle_timeout, &server.seat);
		}
		wl_event_source_timer_update(server.seat.workspace_osd_timer,
			rc.workspace_config.popuptime);
	}
}

/* Per-output workspaces */
static struct workspace *
first_on_output(struct output *output, struct workspace *exclude)
{
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (workspace->output == output && workspace != exclude) {
			return workspace;
		}
	}
	return NULL;
}

static void
refresh_visibility(void)
{
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		bool visible = workspaces_is_visible(workspace);
		wlr_scene_node_set_enabled(&workspace->tree->node, visible);
		wlr_ext_workspace_handle_v1_set_active(
			workspace->ext_workspace, visible);
	}
}

/*
 * Only updates bookkeeping; views are moved by the caller (or by the
 * layout change code when outputs come and go).
 */
static void
workspace_set_output(struct workspace *workspace, struct output *output)
{
	struct output *old = workspace->output;
	if (old == output) {
		return;
	}
	workspace->output = output;
	wlr_ext_workspace_handle_v1_set_group(workspace->ext_workspace,
		output ? output->ext_group : NULL);

	if (old && old->workspace == workspace) {
		old->workspace = first_on_output(old, NULL);
	}
	if (output && (!output->workspace
			|| workspace == server.workspaces.current)) {
		output->workspace = workspace;
	}
}

/* Move the views of @workspace to the output it now lives on */
static void
move_views_with_workspace(struct workspace *workspace, struct output *old)
{
	struct output *output = workspace->output;
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		if (view->workspace != workspace || view->output == output
				|| wlr_box_empty(&view->pending)) {
			continue;
		}
		if (view->visible_on_all_workspaces && old && old->workspace
				&& view->output == old) {
			/* Omnipresent views stay on their output */
			view_move_to_workspace(view, old->workspace);
			continue;
		}
		view_move_to_output(view, output);
	}
}

static bool
output_is_internal(struct output *output)
{
	const char *name = output->wlr_output->name;
	return !strncmp(name, "eDP", 3) || !strncmp(name, "LVDS", 4)
		|| !strncmp(name, "DSI", 3);
}

/* Where workspaces without a (connected) bound output go */
static struct output *
fallback_output(void)
{
	struct output *output, *first = NULL;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		if (output_is_internal(output)) {
			return output;
		}
		if (!first) {
			first = output;
		}
	}
	return first;
}

/* A hidden workspace that is not bound to a connected output */
static struct workspace *
find_spare_workspace(bool empty_only)
{
	struct workspace *workspace;
	wl_list_for_each_reverse(workspace, &server.workspaces.all, link) {
		if (workspaces_is_visible(workspace)) {
			continue;
		}
		if (workspace->output_name
				&& output_from_name(workspace->output_name)) {
			continue;
		}
		if (empty_only && workspace_has_views(workspace)) {
			continue;
		}
		return workspace;
	}
	return NULL;
}

static struct workspace *
find_workspace_quiet(const char *name)
{
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (!strcmp(workspace->name, name)) {
			return workspace;
		}
	}
	size_t parsed_index = parse_workspace_index(name);
	size_t index = 0;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (parsed_index && parsed_index == ++index) {
			return workspace;
		}
	}
	return NULL;
}

static void
apply_config_binds(void)
{
	struct workspace_output_bind *bind;
	wl_list_for_each(bind, &rc.workspace_config.output_binds, link) {
		struct workspace *workspace = find_workspace_quiet(bind->workspace);
		if (!workspace) {
			wlr_log(WLR_ERROR, "<desktops><outputs>: no workspace '%s'",
				bind->workspace);
			continue;
		}
		xstrdup_replace(workspace->output_name, bind->output);
	}
}

/* Focus the topmost view of @target, or nothing if it is empty */
static void
focus_workspace(struct workspace *target)
{
	struct output *output = target->output;
	struct view *active_view = server.active_view;
	if (active_view && (active_view->workspace == target
			|| (active_view->visible_on_all_workspaces
				&& active_view->output == output))) {
		return;
	}
	struct view *view, *topmost = NULL;
	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (!view->minimized && view->workspace == target) {
			topmost = view;
			break;
		}
	}
	if (topmost) {
		desktop_focus_view(topmost, /*raise*/ true);
	} else {
		/* Empty workspace: focus nothing, like sway */
		seat_focus_surface(&server.seat, NULL);
	}
}

static void
switch_to_per_output(struct workspace *target, bool update_focus)
{
	/* The focused workspace changes even if it's already visible */
	server.workspaces.current = target;

	struct output *output = target->output;
	if (!output_is_usable(output)) {
		return;
	}
	if (output->workspace == target) {
		/* Already shown, e.g. on the other output: just focus it */
		if (update_focus) {
			focus_workspace(target);
		}
		return;
	}

	struct workspace *old = output->workspace;
	output->workspace = target;
	if (old) {
		server.workspaces.last = old;
	}

	/* Omnipresent views on this output follow the switch */
	struct view *view;
	wl_list_for_each_reverse(view, &server.views, link) {
		if (view->visible_on_all_workspaces && view->output == output) {
			view_move_to_workspace(view, target);
		}
	}
	struct view *grabbed_view = server.grabbed_view;
	if (grabbed_view && grabbed_view->output == output) {
		view_move_to_workspace(grabbed_view, target);
	}

	refresh_visibility();

	if (update_focus) {
		focus_workspace(target);
	}

	_osd_show(output);
	cursor_update_focus();
	desktop_update_top_layer_visibility();
	show_desktop_reset();
}

/* Public API */
void
workspaces_init(void)
{
	server.workspaces.ext_manager = wlr_ext_workspace_manager_v1_create(
		server.wl_display, EXT_WORKSPACES_VERSION);

	/*
	 * Switching between modes needs a restart; it is too disruptive
	 * to do on reconfigure.
	 */
	server.workspaces.per_output = rc.workspace_config.per_output;
	if (!server.workspaces.per_output) {
		server.workspaces.ext_group = wlr_ext_workspace_group_handle_v1_create(
			server.workspaces.ext_manager, /*caps*/ 0);
	}

	server.workspaces.on_ext_manager.commit.notify = handle_ext_workspace_commit;
	wl_signal_add(&server.workspaces.ext_manager->events.commit,
		&server.workspaces.on_ext_manager.commit);

	wl_list_init(&server.workspaces.all);

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		add_workspace(conf->name);
	}

	/*
	 * After adding workspaces, check if there is an initial workspace
	 * selected and set that as the initial workspace.
	 */
	char *initial_name = rc.workspace_config.initial_workspace_name;
	struct workspace *initial = NULL;
	struct workspace *first = wl_container_of(
		server.workspaces.all.next, first, link);

	if (initial_name) {
		initial = workspace_find_by_name(initial_name);
	}
	if (!initial) {
		initial = first;
	}

	server.workspaces.current = initial;
	if (per_output()) {
		/* Shown once outputs appear, see workspaces_update_outputs() */
		apply_config_binds();
		return;
	}
	wlr_scene_node_set_enabled(&initial->tree->node, true);
	wlr_ext_workspace_handle_v1_set_active(initial->ext_workspace, true);
}

/*
 * update_focus should normally be set to true. It is set to false only
 * when this function is called from desktop_focus_view(), in order to
 * avoid unnecessary extra focus changes and possible recursion.
 */
void
workspaces_switch_to(struct workspace *target, bool update_focus)
{
	assert(target);
	if (per_output()) {
		switch_to_per_output(target, update_focus);
		return;
	}
	if (target == server.workspaces.current) {
		return;
	}

	/* Disable the old workspace */
	wlr_scene_node_set_enabled(
		&server.workspaces.current->tree->node, false);

	wlr_ext_workspace_handle_v1_set_active(
		server.workspaces.current->ext_workspace, false);

	/*
	 * Move Omnipresent views to new workspace.
	 * Not using for_each_view() since it skips views that
	 * view_is_focusable() returns false (e.g. Conky).
	 */
	struct view *view;
	wl_list_for_each_reverse(view, &server.views, link) {
		if (view->visible_on_all_workspaces) {
			view_move_to_workspace(view, target);
		}
	}

	/* Enable the new workspace */
	wlr_scene_node_set_enabled(&target->tree->node, true);

	/* Save the last visited workspace */
	server.workspaces.last = server.workspaces.current;

	/* Make sure new views will spawn on the new workspace */
	server.workspaces.current = target;

	struct view *grabbed_view = server.grabbed_view;
	if (grabbed_view) {
		view_move_to_workspace(grabbed_view, target);
	}

	/*
	 * Make sure we are focusing what the user sees. Only refocus if
	 * the focus is not already on an omnipresent view.
	 */
	if (update_focus) {
		struct view *active_view = server.active_view;
		if (!(active_view && active_view->visible_on_all_workspaces)) {
			desktop_focus_topmost_view();
		}
	}

	/* And finally show the OSD */
	_osd_show(NULL);

	/*
	 * Make sure we are not carrying around a
	 * cursor image from the previous desktop
	 */
	cursor_update_focus();

	/* Ensure that only currently visible fullscreen windows hide the top layer */
	desktop_update_top_layer_visibility();

	wlr_ext_workspace_handle_v1_set_active(target->ext_workspace, true);

	show_desktop_reset();
}

void
workspaces_osd_hide(struct seat *seat)
{
	assert(seat);
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->workspace_osd) {
			continue;
		}
		wlr_scene_node_set_enabled(&output->workspace_osd->node, false);
		wlr_scene_buffer_set_buffer(output->workspace_osd, NULL);
	}
	seat->workspace_osd_shown_by_modifier = false;

	/* Update the cursor focus in case it was on top of the OSD before */
	cursor_update_focus();
}

struct workspace *
workspaces_find(struct workspace *anchor, const char *name, bool wrap)
{
	assert(anchor);
	if (!name) {
		return NULL;
	}
	struct wl_list *workspaces = &server.workspaces.all;

	if (!strcasecmp(name, "current")) {
		return anchor;
	} else if (!strcasecmp(name, "last")) {
		return server.workspaces.last;
	} else if (!strcasecmp(name, "left")) {
		return get_prev(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right")) {
		return get_next(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "left-occupied")) {
		return get_prev_occupied(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right-occupied")) {
		return get_next_occupied(anchor, workspaces, wrap);
	}
	return workspace_find_by_name(name);
}

static void
destroy_workspace(struct workspace *workspace)
{
	wlr_scene_node_destroy(&workspace->tree->node);
	zfree(workspace->name);
	zfree(workspace->output_name);
	zfree(workspace->evacuated_from);
	wl_list_remove(&workspace->link);

	wlr_ext_workspace_handle_v1_destroy(workspace->ext_workspace);
	free(workspace);
}

void
workspaces_reconfigure(void)
{
	/*
	 * Compare actual workspace list with the new desired configuration to:
	 *   - Update names
	 *   - Add workspaces if more workspaces are desired
	 *   - Destroy workspaces if fewer workspace are desired
	 */

	struct wl_list *workspace_link = server.workspaces.all.next;

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		if (workspace_link == &server.workspaces.all) {
			/* # of configured workspaces increased */
			wlr_log(WLR_DEBUG, "Adding workspace \"%s\"",
				conf->name);
			add_workspace(conf->name);
			continue;
		}
		if (strcmp(workspace->name, conf->name)) {
			/* Workspace is renamed */
			wlr_log(WLR_DEBUG, "Renaming workspace \"%s\" to \"%s\"",
				workspace->name, conf->name);
			xstrdup_replace(workspace->name, conf->name);
			wlr_ext_workspace_handle_v1_set_name(
				workspace->ext_workspace, workspace->name);
		}
		workspace_link = workspace_link->next;
	}

	if (workspace_link == &server.workspaces.all) {
		if (per_output()) {
			apply_config_binds();
			workspaces_update_outputs();
			workspaces_update_views();
		}
		return;
	}

	/* # of configured workspaces decreased */
	overlay_finish(&server.seat);
	struct workspace *first_workspace =
		wl_container_of(server.workspaces.all.next, first_workspace, link);

	while (workspace_link != &server.workspaces.all) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		wlr_log(WLR_DEBUG, "Destroying workspace \"%s\"",
			workspace->name);

		struct view *view;
		wl_list_for_each(view, &server.views, link) {
			if (view->workspace == workspace) {
				view_move_to_workspace(view, first_workspace);
			}
		}

		if (per_output()) {
			/* Outputs left without a workspace are fixed below */
			struct output *output = workspace->output;
			if (output && output->workspace == workspace) {
				output->workspace = NULL;
			}
			workspace->output = NULL;
			if (server.workspaces.current == workspace) {
				server.workspaces.current = first_workspace;
			}
		} else if (server.workspaces.current == workspace) {
			workspaces_switch_to(first_workspace,
				/* update_focus */ true);
		}
		if (server.workspaces.last == workspace) {
			server.workspaces.last = first_workspace;
		}

		workspace_link = workspace_link->next;
		destroy_workspace(workspace);
	}

	if (per_output()) {
		apply_config_binds();
		workspaces_update_outputs();
		workspaces_update_views();
		desktop_focus_topmost_view();
	}
}

bool
workspaces_is_visible(struct workspace *workspace)
{
	if (!workspace) {
		return false;
	}
	if (per_output()) {
		return workspace->output
			&& workspace->output->workspace == workspace;
	}
	return workspace == server.workspaces.current;
}

/*
 * Called on output layout changes, before views are re-arranged:
 * give every workspace an output (its bound one if connected) and every
 * output a workspace.
 */
void
workspaces_update_outputs(void)
{
	if (!per_output()) {
		return;
	}

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output) && !output->ext_group) {
			output->ext_group = wlr_ext_workspace_group_handle_v1_create(
				server.workspaces.ext_manager, /*caps*/ 0);
			output->ext_group->data = output;
			wlr_ext_workspace_group_handle_v1_output_enter(
				output->ext_group, output->wlr_output);
		}
	}

	struct output *fallback = fallback_output();
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		struct output *want = NULL;
		if (workspace->output_name) {
			want = output_from_name(workspace->output_name);
		}
		if (!want && workspace->evacuated_from) {
			/* Back to the output it was moved off */
			want = output_from_name(workspace->evacuated_from);
		}
		if (want) {
			zfree(workspace->evacuated_from);
		}
		if (!want && output_is_usable(workspace->output)) {
			want = workspace->output;
		}
		if (!want) {
			if (workspace->output && !workspace->evacuated_from) {
				workspace->evacuated_from =
					xstrdup(workspace->output->wlr_output->name);
			}
			want = fallback;
		}
		workspace_set_output(workspace, want);
	}

	/* Outputs without any workspace take a spare one */
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output) || output->workspace) {
			continue;
		}
		workspace = find_spare_workspace(/* empty_only */ true);
		if (!workspace) {
			workspace = find_spare_workspace(/* empty_only */ false);
		}
		if (!workspace) {
			wlr_log(WLR_INFO, "no spare workspace for output %s; "
				"add more workspaces", output->wlr_output->name);
			continue;
		}
		workspace_set_output(workspace, output);
	}

	/* Groups of outputs that went away are empty now */
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output) && output->ext_group) {
			wlr_ext_workspace_group_handle_v1_destroy(output->ext_group);
			output->ext_group = NULL;
			output->workspace = NULL;
		}
	}

	if (!workspaces_is_visible(server.workspaces.current)) {
		output = output_nearest_to_cursor();
		if (!output_is_usable(output) || !output->workspace) {
			output = fallback;
		}
		if (output && output->workspace) {
			server.workspaces.current = output->workspace;
		}
	}

	refresh_visibility();
}

/*
 * Called after views have been re-arranged for a layout change: make
 * sure each view is on the output of its workspace.
 */
void
workspaces_update_views(void)
{
	if (!per_output()) {
		return;
	}
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		struct workspace *workspace = view->workspace;
		if (!workspace || view->output == workspace->output
				|| !output_is_usable(view->output)
				|| wlr_box_empty(&view->pending)) {
			continue;
		}
		if (view->visible_on_all_workspaces
				|| !output_is_usable(workspace->output)) {
			if (view->output->workspace) {
				view_move_to_workspace(view, view->output->workspace);
			}
			continue;
		}
		view_move_to_output(view, workspace->output);
	}
	desktop_update_top_layer_visibility();
}

void
workspaces_on_output_destroy(struct output *output)
{
	if (!per_output()) {
		return;
	}
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (workspace->output == output) {
			if (!workspace->evacuated_from) {
				workspace->evacuated_from =
					xstrdup(output->wlr_output->name);
			}
			workspace->output = NULL;
			wlr_ext_workspace_handle_v1_set_group(
				workspace->ext_workspace, NULL);
		}
	}
	output->workspace = NULL;
	if (output->ext_group) {
		wlr_ext_workspace_group_handle_v1_destroy(output->ext_group);
		output->ext_group = NULL;
	}
}

/* A view moved to another output: put it on that output's workspace */
void
workspaces_on_view_output_changed(struct view *view)
{
	if (!per_output() || !view->workspace
			|| view->adjusting_for_layout_change) {
		return;
	}
	struct output *output = view->output;
	if (!output_is_usable(output) || !output->workspace
			|| view->workspace->output == output) {
		return;
	}
	view_move_to_workspace(view, output->workspace);
}

/* The output under the cursor becomes the focused one */
void
workspaces_update_focus_from_cursor(void)
{
	if (!per_output()) {
		return;
	}
	struct workspace *current = server.workspaces.current;
	struct output *output = output_nearest_to_cursor();
	if (!output || !output->workspace
			|| (current && current->output == output)) {
		return;
	}
	server.workspaces.current = output->workspace;

	/*
	 * Keyboard focus follows the cursor to the other output, so keys
	 * go to what is under the user's eyes without an extra click.
	 * Not while dragging, holding a button, or with a layer-shell
	 * surface (launcher, lock screen) or the window switcher focused.
	 */
	struct seat *seat = &server.seat;
	if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH
			|| seat->focused_layer
			|| seat->wlr_seat->pointer_state.button_count
			|| server.session_lock_manager->locked) {
		return;
	}
	struct view *active_view = server.active_view;
	if (active_view && active_view->output == output) {
		return;
	}
	struct view *view;
	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (!view->minimized && view->output == output) {
			desktop_focus_view(view, /*raise*/ false);
			return;
		}
	}
}

/*
 * Move @workspace with its views to @output. With @bind, the workspace
 * is also bound to the output so it returns there after a reconnect.
 * Fails if the workspace is the last one on its output.
 */
bool
workspaces_move_to_output(struct workspace *workspace, struct output *output,
		bool bind)
{
	if (!per_output() || !output_is_usable(output)) {
		return false;
	}
	struct output *old = workspace->output;
	if (old != output && output_is_usable(old)
			&& !first_on_output(old, workspace)) {
		wlr_log(WLR_ERROR, "not moving workspace %s: it is the only "
			"one on %s", workspace->name, old->wlr_output->name);
		return false;
	}
	if (bind) {
		xstrdup_replace(workspace->output_name, output->wlr_output->name);
		zfree(workspace->evacuated_from);
	}
	if (old == output) {
		return true;
	}

	workspace_set_output(workspace, output);
	move_views_with_workspace(workspace, old);
	refresh_visibility();
	desktop_update_top_layer_visibility();
	cursor_update_focus();
	return true;
}

void
workspaces_destroy(void)
{
	struct workspace *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &server.workspaces.all, link) {
		destroy_workspace(workspace);
	}
	assert(wl_list_empty(&server.workspaces.all));
	wl_list_remove(&server.workspaces.on_ext_manager.commit.link);
}
