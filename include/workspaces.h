/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_WORKSPACES_H
#define LABWC_WORKSPACES_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>

struct output;
struct seat;
struct server;
struct view;
struct wlr_scene_tree;

struct workspace {
	struct wl_list link; /* struct server.workspaces */

	char *name;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *view_trees[3];

	struct wlr_ext_workspace_handle_v1 *ext_workspace;

	/*
	 * Per-output workspaces (<desktops perOutput="yes">) only:
	 * @output is the output this workspace currently lives on (NULL if
	 * there is no usable output), @output_name the output it is bound
	 * to. A bound workspace moves back to its output on reconnect.
	 */
	struct output *output;
	char *output_name;
	/*
	 * Output the workspace was moved off when that output went away
	 * (disabled or disconnected); it moves back when it returns
	 */
	char *evacuated_from;
};

void workspaces_init(void);
void workspaces_switch_to(struct workspace *target, bool update_focus);
void workspaces_destroy(void);
void workspaces_osd_hide(struct seat *seat);
struct workspace *workspaces_find(struct workspace *anchor, const char *name,
	bool wrap);
void workspaces_reconfigure(void);

/* True if the workspace is shown (on its output, in per-output mode) */
bool workspaces_is_visible(struct workspace *workspace);

/* Per-output mode hooks; all of them are no-ops otherwise */
void workspaces_update_outputs(void);
void workspaces_update_views(void);
void workspaces_on_output_destroy(struct output *output);
void workspaces_on_view_output_changed(struct view *view);
void workspaces_update_focus_from_cursor(void);
bool workspaces_move_to_output(struct workspace *workspace,
	struct output *output, bool bind);

#endif /* LABWC_WORKSPACES_H */
