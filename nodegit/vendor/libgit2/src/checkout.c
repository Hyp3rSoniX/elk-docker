/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "checkout.h"

#include <assert.h>

#include "git2/repository.h"
#include "git2/refs.h"
#include "git2/tree.h"
#include "git2/blob.h"
#include "git2/config.h"
#include "git2/diff.h"
#include "git2/submodule.h"
#include "git2/sys/index.h"
#include "git2/sys/filter.h"
#include "git2/merge.h"

#include "refs.h"
#include "repository.h"
#include "index.h"
#include "filter.h"
#include "blob.h"
#include "diff.h"
#include "diff_generate.h"
#include "pathspec.h"
#include "buf_text.h"
#include "diff_xdiff.h"
#include "path.h"
#include "attr.h"
#include "pool.h"
#include "strmap.h"

/* See docs/checkout-internals.md for more information */

enum {
	CHECKOUT_ACTION__NONE = 0,
	CHECKOUT_ACTION__REMOVE = 1,
	CHECKOUT_ACTION__UPDATE_BLOB = 2,
	CHECKOUT_ACTION__UPDATE_SUBMODULE = 4,
	CHECKOUT_ACTION__CONFLICT = 8,
	CHECKOUT_ACTION__REMOVE_CONFLICT = 16,
	CHECKOUT_ACTION__UPDATE_CONFLICT = 32,
	CHECKOUT_ACTION__MAX = 32,
	CHECKOUT_ACTION__REMOVE_AND_UPDATE =
		(CHECKOUT_ACTION__UPDATE_BLOB | CHECKOUT_ACTION__REMOVE),
};

typedef struct {
	git_atomic mkdir_calls;
	git_atomic stat_calls;
	git_atomic chmod_calls;
} atomic_checkout_perfdata;

typedef struct {
	git_repository *repo;
	git_iterator *target;
	git_diff *diff;
	git_checkout_options opts;
	bool opts_free_baseline;
	char *pfx;
	git_index *index;
	git_pool pool;
	git_mutex index_mutex;
	git_mutex mkpath_mutex;
	git_vector removes;
	git_vector remove_conflicts;
	git_vector update_conflicts;
	git_vector *update_reuc;
	git_vector *update_names;
	git_buf target_path;
	unsigned int strategy;
	int can_symlink;
	int respect_filemode;
	bool reload_submodules;
	size_t total_steps;
	size_t completed_steps;
	atomic_checkout_perfdata perfdata;
	git_strmap *mkdir_map;
	git_attr_session attr_session;
} checkout_data;

typedef struct {
	const git_index_entry *ancestor;
	const git_index_entry *ours;
	const git_index_entry *theirs;

	int name_collision:1,
		directoryfile:1,
		one_to_two:1,
		binary:1,
		submodule:1;
} checkout_conflictdata;

static int checkout_notify(
	checkout_data *data,
	git_checkout_notify_t why,
	const git_diff_delta *delta,
	const git_index_entry *wditem)
{
	git_diff_file wdfile;
	const git_diff_file *baseline = NULL, *target = NULL, *workdir = NULL;
	const char *path = NULL;

	if (!data->opts.notify_cb ||
		(why & data->opts.notify_flags) == 0)
		return 0;

	if (wditem) {
		memset(&wdfile, 0, sizeof(wdfile));

		git_oid_cpy(&wdfile.id, &wditem->id);
		wdfile.path = wditem->path;
		wdfile.size = wditem->file_size;
		wdfile.flags = GIT_DIFF_FLAG_VALID_ID;
		wdfile.mode = wditem->mode;

		workdir = &wdfile;

		path = wditem->path;
	}

	if (delta) {
		switch (delta->status) {
		case GIT_DELTA_UNMODIFIED:
		case GIT_DELTA_MODIFIED:
		case GIT_DELTA_TYPECHANGE:
		default:
			baseline = &delta->old_file;
			target = &delta->new_file;
			break;
		case GIT_DELTA_ADDED:
		case GIT_DELTA_IGNORED:
		case GIT_DELTA_UNTRACKED:
		case GIT_DELTA_UNREADABLE:
			target = &delta->new_file;
			break;
		case GIT_DELTA_DELETED:
			baseline = &delta->old_file;
			break;
		}

		path = delta->old_file.path;
	}

	{
		int error = data->opts.notify_cb(
			why, path, baseline, target, workdir, data->opts.notify_payload);

		return git_error_set_after_callback_function(
			error, "git_checkout notification");
	}
}

GIT_INLINE(bool) is_workdir_base_or_new(
	const git_oid *workdir_id,
	const git_diff_file *baseitem,
	const git_diff_file *newitem)
{
	return (git_oid__cmp(&baseitem->id, workdir_id) == 0 ||
		git_oid__cmp(&newitem->id, workdir_id) == 0);
}

GIT_INLINE(bool) is_filemode_changed(git_filemode_t a, git_filemode_t b, int respect_filemode)
{
	/* If core.filemode = false, ignore links in the repository and executable bit changes */
	if (!respect_filemode) {
		if (a == S_IFLNK)
			a = GIT_FILEMODE_BLOB;
		if (b == S_IFLNK)
			b = GIT_FILEMODE_BLOB;

		a &= ~0111;
		b &= ~0111;
	}

	return (a != b);
}

static bool checkout_is_workdir_modified(
	checkout_data *data,
	const git_diff_file *baseitem,
	const git_diff_file *newitem,
	const git_index_entry *wditem)
{
	git_oid oid;
	const git_index_entry *ie;

	/* handle "modified" submodule */
	if (wditem->mode == GIT_FILEMODE_COMMIT) {
		git_submodule *sm;
		unsigned int sm_status = 0;
		const git_oid *sm_oid = NULL;
		bool rval = false;

		if (git_submodule_lookup(&sm, data->repo, wditem->path) < 0) {
			git_error_clear();
			return true;
		}

		if (git_submodule_status(&sm_status, data->repo, wditem->path, GIT_SUBMODULE_IGNORE_UNSPECIFIED) < 0 ||
			GIT_SUBMODULE_STATUS_IS_WD_DIRTY(sm_status))
			rval = true;
		else if ((sm_oid = git_submodule_wd_id(sm)) == NULL)
			rval = false;
		else
			rval = (git_oid__cmp(&baseitem->id, sm_oid) != 0);

		git_submodule_free(sm);
		return rval;
	}

	/*
	 * Look at the cache to decide if the workdir is modified: if the
	 * cache contents match the workdir contents, then we do not need
	 * to examine the working directory directly, instead we can
	 * examine the cache to see if _it_ has been modified.  This allows
	 * us to avoid touching the disk.
	 */
	ie = git_index_get_bypath(data->index, wditem->path, 0);

	if (ie != NULL &&
		git_index_time_eq(&wditem->mtime, &ie->mtime) &&
		wditem->file_size == ie->file_size &&
		!is_filemode_changed(wditem->mode, ie->mode, data->respect_filemode)) {

		/* The workdir is modified iff the index entry is modified */
		return !is_workdir_base_or_new(&ie->id, baseitem, newitem) ||
			is_filemode_changed(baseitem->mode, ie->mode, data->respect_filemode);
	}

	/* depending on where base is coming from, we may or may not know
	 * the actual size of the data, so we can't rely on this shortcut.
	 */
	if (baseitem->size && wditem->file_size != baseitem->size)
		return true;

	/* if the workdir item is a directory, it cannot be a modified file */
	if (S_ISDIR(wditem->mode))
		return false;

	if (is_filemode_changed(baseitem->mode, wditem->mode, data->respect_filemode))
		return true;

	if (git_diff__oid_for_entry(&oid, data->diff, wditem, wditem->mode, NULL) < 0)
		return false;

	/* Allow the checkout if the workdir is not modified *or* if the checkout
	 * target's contents are already in the working directory.
	 */
	return !is_workdir_base_or_new(&oid, baseitem, newitem);
}

#define CHECKOUT_ACTION_IF(FLAG,YES,NO) \
	((data->strategy & GIT_CHECKOUT_##FLAG) ? CHECKOUT_ACTION__##YES : CHECKOUT_ACTION__##NO)

static int checkout_action_common(
	int *action,
	checkout_data *data,
	const git_diff_delta *delta,
	const git_index_entry *wd)
{
	git_checkout_notify_t notify = GIT_CHECKOUT_NOTIFY_NONE;

	if ((data->strategy & GIT_CHECKOUT_UPDATE_ONLY) != 0)
		*action = (*action & ~CHECKOUT_ACTION__REMOVE);

	if ((*action & CHECKOUT_ACTION__UPDATE_BLOB) != 0) {
		if (S_ISGITLINK(delta->new_file.mode))
			*action = (*action & ~CHECKOUT_ACTION__UPDATE_BLOB) |
				CHECKOUT_ACTION__UPDATE_SUBMODULE;

		/* to "update" a symlink, we must remove the old one first */
		if (delta->new_file.mode == GIT_FILEMODE_LINK && wd != NULL)
			*action |= CHECKOUT_ACTION__REMOVE;

		/* if the file is on disk and doesn't match our mode, force update */
		if (wd &&
			GIT_PERMS_IS_EXEC(wd->mode) !=
			GIT_PERMS_IS_EXEC(delta->new_file.mode))
				*action |= CHECKOUT_ACTION__REMOVE;

		notify = GIT_CHECKOUT_NOTIFY_UPDATED;
	}

	if ((*action & CHECKOUT_ACTION__CONFLICT) != 0)
		notify = GIT_CHECKOUT_NOTIFY_CONFLICT;

	return checkout_notify(data, notify, delta, wd);
}

static int checkout_action_no_wd(
	int *action,
	checkout_data *data,
	const git_diff_delta *delta)
{
	int error = 0;

	*action = CHECKOUT_ACTION__NONE;

	switch (delta->status) {
	case GIT_DELTA_UNMODIFIED: /* case 12 */
		error = checkout_notify(data, GIT_CHECKOUT_NOTIFY_DIRTY, delta, NULL);
		if (error)
			return error;
		*action = CHECKOUT_ACTION_IF(RECREATE_MISSING, UPDATE_BLOB, NONE);
		break;
	case GIT_DELTA_ADDED:    /* case 2 or 28 (and 5 but not really) */
		*action = CHECKOUT_ACTION_IF(SAFE, UPDATE_BLOB, NONE);
		break;
	case GIT_DELTA_MODIFIED: /* case 13 (and 35 but not really) */
		*action = CHECKOUT_ACTION_IF(RECREATE_MISSING, UPDATE_BLOB, CONFLICT);
		break;
	case GIT_DELTA_TYPECHANGE: /* case 21 (B->T) and 28 (T->B)*/
		if (delta->new_file.mode == GIT_FILEMODE_TREE)
			*action = CHECKOUT_ACTION_IF(SAFE, UPDATE_BLOB, NONE);
		break;
	case GIT_DELTA_DELETED: /* case 8 or 25 */
		*action = CHECKOUT_ACTION_IF(SAFE, REMOVE, NONE);
		break;
	default: /* impossible */
		break;
	}

	return checkout_action_common(action, data, delta, NULL);
}

static int build_target_fullpath(
	git_buf *out, checkout_data *data, const char *path)
{
	if (git_buf_set(out, git_buf_cstr(&data->target_path),
			git_buf_len(&data->target_path)) < 0)
		return -1;

	if (path && git_buf_puts(out, path) < 0)
		return -1;

	return 0;
}

static bool wd_item_is_removable(
	checkout_data *data, const git_index_entry *wd)
{
	git_buf fullpath = GIT_BUF_INIT;
	bool removable = false;

	if (wd->mode != GIT_FILEMODE_TREE)
		return true;

	if (build_target_fullpath(&fullpath, data, wd->path) < 0)
		return false;

	removable = !git_path_contains(&fullpath, DOT_GIT);

	git_buf_dispose(&fullpath);

	return removable;
}

static int checkout_queue_remove(checkout_data *data, const char *path)
{
	char *copy = git_pool_strdup(&data->pool, path);
	GIT_ERROR_CHECK_ALLOC(copy);
	return git_vector_insert(&data->removes, copy);
}

/* note that this advances the iterator over the wd item */
static int checkout_action_wd_only(
	checkout_data *data,
	git_iterator *workdir,
	const git_index_entry **wditem,
	git_vector *pathspec)
{
	int error = 0;
	bool remove = false;
	git_checkout_notify_t notify = GIT_CHECKOUT_NOTIFY_NONE;
	const git_index_entry *wd = *wditem;
	git_buf temp_buf = GIT_BUF_INIT;

	if (!git_pathspec__match(
			pathspec, wd->path,
			(data->strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH) != 0,
			git_iterator_ignore_case(workdir), NULL, NULL)) {
		error = git_iterator_advance(wditem, workdir);
		goto cleanup;
	}

	/* check if item is tracked in the index but not in the checkout diff */
	if (data->index != NULL) {
		size_t pos;

		error = git_index__find_pos(
			&pos, data->index, wd->path, 0, GIT_INDEX_STAGE_ANY);

		if (wd->mode != GIT_FILEMODE_TREE) {
			if (!error) { /* found by git_index__find_pos call */
				notify = GIT_CHECKOUT_NOTIFY_DIRTY;
				remove = ((data->strategy & GIT_CHECKOUT_FORCE) != 0);
			} else if (error != GIT_ENOTFOUND)
				goto cleanup;
			else
				error = 0; /* git_index__find_pos does not set error msg */
		} else {
			/* for tree entries, we have to see if there are any index
			 * entries that are contained inside that tree
			 */
			const git_index_entry *e = git_index_get_byindex(data->index, pos);

			if (e != NULL && data->diff->pfxcomp(e->path, wd->path) == 0) {
				error = git_iterator_advance_into(wditem, workdir);
				goto cleanup;
			}
		}
	}

	if (notify != GIT_CHECKOUT_NOTIFY_NONE) {
		/* if we found something in the index, notify and advance */
		if ((error = checkout_notify(data, notify, NULL, wd)) != 0)
			goto cleanup;

		if (remove && wd_item_is_removable(data, wd))
			error = checkout_queue_remove(data, wd->path);

		if (!error)
			error = git_iterator_advance(wditem, workdir);
	} else {
		/* untracked or ignored - can't know which until we advance through */
		bool over = false, removable = wd_item_is_removable(data, wd);
		git_iterator_status_t untracked_state;

		/* copy the entry for issuing notification callback later */
		git_index_entry saved_wd = *wd;
		git_buf_sets(&temp_buf, wd->path);
		saved_wd.path = git_buf_cstr(&temp_buf);

		error = git_iterator_advance_over(
			wditem, &untracked_state, workdir);
		if (error == GIT_ITEROVER)
			over = true;
		else if (error < 0)
			goto cleanup;

		if (untracked_state == GIT_ITERATOR_STATUS_IGNORED) {
			notify = GIT_CHECKOUT_NOTIFY_IGNORED;
			remove = ((data->strategy & GIT_CHECKOUT_REMOVE_IGNORED) != 0);
		} else {
			notify = GIT_CHECKOUT_NOTIFY_UNTRACKED;
			remove = ((data->strategy & GIT_CHECKOUT_REMOVE_UNTRACKED) != 0);
		}

		if ((error = checkout_notify(data, notify, NULL, &saved_wd)) != 0)
			goto cleanup;

		if (remove && removable)
			error = checkout_queue_remove(data, saved_wd.path);

		if (!error && over) /* restore ITEROVER if needed */
			error = GIT_ITEROVER;
	}

cleanup:
	git_buf_dispose(&temp_buf);
	return error;
}

static bool submodule_is_config_only(
	checkout_data *data,
	const char *path)
{
	git_submodule *sm = NULL;
	unsigned int sm_loc = 0;
	bool rval = false;

	if (git_submodule_lookup(&sm, data->repo, path) < 0)
		return true;

	if (git_submodule_location(&sm_loc, sm) < 0 ||
		sm_loc == GIT_SUBMODULE_STATUS_IN_CONFIG)
		rval = true;

	git_submodule_free(sm);

	return rval;
}

static bool checkout_is_empty_dir(checkout_data *data, const char *path)
{
	git_buf fullpath = GIT_BUF_INIT;
	bool empty;

	if (build_target_fullpath(&fullpath, data, path) < 0)
		return false;

	empty = git_path_is_empty_dir(git_buf_cstr(&fullpath));

	git_buf_dispose(&fullpath);
	return empty;
}

static int checkout_action_with_wd(
	int *action,
	checkout_data *data,
	const git_diff_delta *delta,
	git_iterator *workdir,
	const git_index_entry *wd)
{
	*action = CHECKOUT_ACTION__NONE;

	switch (delta->status) {
	case GIT_DELTA_UNMODIFIED: /* case 14/15 or 33 */
		if (checkout_is_workdir_modified(data, &delta->old_file, &delta->new_file, wd)) {
			GIT_ERROR_CHECK_ERROR(
				checkout_notify(data, GIT_CHECKOUT_NOTIFY_DIRTY, delta, wd) );
			*action = CHECKOUT_ACTION_IF(FORCE, UPDATE_BLOB, NONE);
		}
		break;
	case GIT_DELTA_ADDED: /* case 3, 4 or 6 */
		if (git_iterator_current_is_ignored(workdir))
			*action = CHECKOUT_ACTION_IF(DONT_OVERWRITE_IGNORED, CONFLICT, UPDATE_BLOB);
		else
			*action = CHECKOUT_ACTION_IF(FORCE, UPDATE_BLOB, CONFLICT);
		break;
	case GIT_DELTA_DELETED: /* case 9 or 10 (or 26 but not really) */
		if (checkout_is_workdir_modified(data, &delta->old_file, &delta->new_file, wd))
			*action = CHECKOUT_ACTION_IF(FORCE, REMOVE, CONFLICT);
		else
			*action = CHECKOUT_ACTION_IF(SAFE, REMOVE, NONE);
		break;
	case GIT_DELTA_MODIFIED: /* case 16, 17, 18 (or 36 but not really) */
		if (wd->mode != GIT_FILEMODE_COMMIT &&
			checkout_is_workdir_modified(data, &delta->old_file, &delta->new_file, wd))
			*action = CHECKOUT_ACTION_IF(FORCE, UPDATE_BLOB, CONFLICT);
		else
			*action = CHECKOUT_ACTION_IF(SAFE, UPDATE_BLOB, NONE);
		break;
	case GIT_DELTA_TYPECHANGE: /* case 22, 23, 29, 30 */
		if (delta->old_file.mode == GIT_FILEMODE_TREE) {
			if (wd->mode == GIT_FILEMODE_TREE)
				/* either deleting items in old tree will delete the wd dir,
				 * or we'll get a conflict when we attempt blob update...
				 */
				*action = CHECKOUT_ACTION_IF(SAFE, UPDATE_BLOB, NONE);
			else if (wd->mode == GIT_FILEMODE_COMMIT) {
				/* workdir is possibly a "phantom" submodule - treat as a
				 * tree if the only submodule info came from the config
				 */
				if (submodule_is_config_only(data, wd->path))
					*action = CHECKOUT_ACTION_IF(SAFE, UPDATE_BLOB, NONE);
				else
					*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, CONFLICT);
			} else
				*action = CHECKOUT_ACTION_IF(FORCE, REMOVE, CONFLICT);
		}
		else if (checkout_is_workdir_modified(data, &delta->old_file, &delta->new_file, wd))
			*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, CONFLICT);
		else
			*action = CHECKOUT_ACTION_IF(SAFE, REMOVE_AND_UPDATE, NONE);

		/* don't update if the typechange is to a tree */
		if (delta->new_file.mode == GIT_FILEMODE_TREE)
			*action = (*action & ~CHECKOUT_ACTION__UPDATE_BLOB);
		break;
	default: /* impossible */
		break;
	}

	return checkout_action_common(action, data, delta, wd);
}

static int checkout_action_with_wd_blocker(
	int *action,
	checkout_data *data,
	const git_diff_delta *delta,
	const git_index_entry *wd)
{
	*action = CHECKOUT_ACTION__NONE;

	switch (delta->status) {
	case GIT_DELTA_UNMODIFIED:
		/* should show delta as dirty / deleted */
		GIT_ERROR_CHECK_ERROR(
			checkout_notify(data, GIT_CHECKOUT_NOTIFY_DIRTY, delta, wd) );
		*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, NONE);
		break;
	case GIT_DELTA_ADDED:
	case GIT_DELTA_MODIFIED:
		*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, CONFLICT);
		break;
	case GIT_DELTA_DELETED:
		*action = CHECKOUT_ACTION_IF(FORCE, REMOVE, CONFLICT);
		break;
	case GIT_DELTA_TYPECHANGE:
		/* not 100% certain about this... */
		*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, CONFLICT);
		break;
	default: /* impossible */
		break;
	}

	return checkout_action_common(action, data, delta, wd);
}

static int checkout_action_with_wd_dir(
	int *action,
	checkout_data *data,
	const git_diff_delta *delta,
	git_iterator *workdir,
	const git_index_entry *wd)
{
	*action = CHECKOUT_ACTION__NONE;

	switch (delta->status) {
	case GIT_DELTA_UNMODIFIED: /* case 19 or 24 (or 34 but not really) */
		GIT_ERROR_CHECK_ERROR(
			checkout_notify(data, GIT_CHECKOUT_NOTIFY_DIRTY, delta, NULL));
		GIT_ERROR_CHECK_ERROR(
			checkout_notify(data, GIT_CHECKOUT_NOTIFY_UNTRACKED, NULL, wd));
		*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, NONE);
		break;
	case GIT_DELTA_ADDED:/* case 4 (and 7 for dir) */
	case GIT_DELTA_MODIFIED: /* case 20 (or 37 but not really) */
		if (delta->old_file.mode == GIT_FILEMODE_COMMIT)
			/* expected submodule (and maybe found one) */;
		else if (delta->new_file.mode != GIT_FILEMODE_TREE)
			*action = git_iterator_current_is_ignored(workdir) ?
				CHECKOUT_ACTION_IF(DONT_OVERWRITE_IGNORED, CONFLICT, REMOVE_AND_UPDATE) :
				CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, CONFLICT);
		break;
	case GIT_DELTA_DELETED: /* case 11 (and 27 for dir) */
		if (delta->old_file.mode != GIT_FILEMODE_TREE)
			GIT_ERROR_CHECK_ERROR(
				checkout_notify(data, GIT_CHECKOUT_NOTIFY_UNTRACKED, NULL, wd));
		break;
	case GIT_DELTA_TYPECHANGE: /* case 24 or 31 */
		if (delta->old_file.mode == GIT_FILEMODE_TREE) {
			/* For typechange from dir, remove dir and add blob, but it is
			 * not safe to remove dir if it contains modified files.
			 * However, safely removing child files will remove the parent
			 * directory if is it left empty, so we can defer removing the
			 * dir and it will succeed if no children are left.
			 */
			*action = CHECKOUT_ACTION_IF(SAFE, UPDATE_BLOB, NONE);
		}
		else if (delta->new_file.mode != GIT_FILEMODE_TREE)
			/* For typechange to dir, dir is already created so no action */
			*action = CHECKOUT_ACTION_IF(FORCE, REMOVE_AND_UPDATE, CONFLICT);
		break;
	default: /* impossible */
		break;
	}

	return checkout_action_common(action, data, delta, wd);
}

static int checkout_action_with_wd_dir_empty(
	int *action,
	checkout_data *data,
	const git_diff_delta *delta)
{
	int error = checkout_action_no_wd(action, data, delta);

	/* We can always safely remove an empty directory. */
	if (error == 0 && *action != CHECKOUT_ACTION__NONE)
		*action |= CHECKOUT_ACTION__REMOVE;

	return error;
}

static int checkout_action(
	int *action,
	checkout_data *data,
	git_diff_delta *delta,
	git_iterator *workdir,
	const git_index_entry **wditem,
	git_vector *pathspec)
{
	int cmp = -1, error;
	int (*strcomp)(const char *, const char *) = data->diff->strcomp;
	int (*pfxcomp)(const char *str, const char *pfx) = data->diff->pfxcomp;
	int (*advance)(const git_index_entry **, git_iterator *) = NULL;

	/* move workdir iterator to follow along with deltas */

	while (1) {
		const git_index_entry *wd = *wditem;

		if (!wd)
			return checkout_action_no_wd(action, data, delta);

		cmp = strcomp(wd->path, delta->old_file.path);

		/* 1. wd before delta ("a/a" before "a/b")
		 * 2. wd prefixes delta & should expand ("a/" before "a/b")
		 * 3. wd prefixes delta & cannot expand ("a/b" before "a/b/c")
		 * 4. wd equals delta ("a/b" and "a/b")
		 * 5. wd after delta & delta prefixes wd ("a/b/c" after "a/b/" or "a/b")
		 * 6. wd after delta ("a/c" after "a/b")
		 */

		if (cmp < 0) {
			cmp = pfxcomp(delta->old_file.path, wd->path);

			if (cmp == 0) {
				if (wd->mode == GIT_FILEMODE_TREE) {
					/* case 2 - entry prefixed by workdir tree */
					error = git_iterator_advance_into(wditem, workdir);
					if (error < 0 && error != GIT_ITEROVER)
						goto done;
					continue;
				}

				/* case 3 maybe - wd contains non-dir where dir expected */
				if (delta->old_file.path[strlen(wd->path)] == '/') {
					error = checkout_action_with_wd_blocker(
						action, data, delta, wd);
					advance = git_iterator_advance;
					goto done;
				}
			}

			/* case 1 - handle wd item (if it matches pathspec) */
			error = checkout_action_wd_only(data, workdir, wditem, pathspec);
			if (error && error != GIT_ITEROVER)
				goto done;
			continue;
		}

		if (cmp == 0) {
			/* case 4 */
			error = checkout_action_with_wd(action, data, delta, workdir, wd);
			advance = git_iterator_advance;
			goto done;
		}

		cmp = pfxcomp(wd->path, delta->old_file.path);

		if (cmp == 0) { /* case 5 */
			if (wd->path[strlen(delta->old_file.path)] != '/')
				return checkout_action_no_wd(action, data, delta);

			if (delta->status == GIT_DELTA_TYPECHANGE) {
				if (delta->old_file.mode == GIT_FILEMODE_TREE) {
					error = checkout_action_with_wd(action, data, delta, workdir, wd);
					advance = git_iterator_advance_into;
					goto done;
				}

				if (delta->new_file.mode == GIT_FILEMODE_TREE ||
					delta->new_file.mode == GIT_FILEMODE_COMMIT ||
					delta->old_file.mode == GIT_FILEMODE_COMMIT)
				{
					error = checkout_action_with_wd(action, data, delta, workdir, wd);
					advance = git_iterator_advance;
					goto done;
				}
			}

			return checkout_is_empty_dir(data, wd->path) ?
				checkout_action_with_wd_dir_empty(action, data, delta) :
				checkout_action_with_wd_dir(action, data, delta, workdir, wd);
		}

		/* case 6 - wd is after delta */
		return checkout_action_no_wd(action, data, delta);
	}

done:
	if (!error && advance != NULL &&
		(error = advance(wditem, workdir)) < 0) {
		*wditem = NULL;
		if (error == GIT_ITEROVER)
			error = 0;
	}

	return error;
}

static int checkout_remaining_wd_items(
	checkout_data *data,
	git_iterator *workdir,
	const git_index_entry *wd,
	git_vector *spec)
{
	int error = 0;

	while (wd && !error)
		error = checkout_action_wd_only(data, workdir, &wd, spec);

	if (error == GIT_ITEROVER)
		error = 0;

	return error;
}

GIT_INLINE(int) checkout_idxentry_cmp(
	const git_index_entry *a,
	const git_index_entry *b)
{
	if (!a && !b)
		return 0;
	else if (!a && b)
		return -1;
	else if(a && !b)
		return 1;
	else
		return strcmp(a->path, b->path);
}

static int checkout_conflictdata_cmp(const void *a, const void *b)
{
	const checkout_conflictdata *ca = a;
	const checkout_conflictdata *cb = b;
	int diff;

	if ((diff = checkout_idxentry_cmp(ca->ancestor, cb->ancestor)) == 0 &&
		(diff = checkout_idxentry_cmp(ca->ours, cb->theirs)) == 0)
		diff = checkout_idxentry_cmp(ca->theirs, cb->theirs);

	return diff;
}

int checkout_conflictdata_empty(
	const git_vector *conflicts, size_t idx, void *payload)
{
	checkout_conflictdata *conflict;

	GIT_UNUSED(payload);

	if ((conflict = git_vector_get(conflicts, idx)) == NULL)
		return -1;

	if (conflict->ancestor || conflict->ours || conflict->theirs)
		return 0;

	git__free(conflict);
	return 1;
}

GIT_INLINE(bool) conflict_pathspec_match(
	checkout_data *data,
	git_iterator *workdir,
	git_vector *pathspec,
	const git_index_entry *ancestor,
	const git_index_entry *ours,
	const git_index_entry *theirs)
{
	/* if the pathspec matches ours *or* theirs, proceed */
	if (ours && git_pathspec__match(pathspec, ours->path,
		(data->strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH) != 0,
		git_iterator_ignore_case(workdir), NULL, NULL))
		return true;

	if (theirs && git_pathspec__match(pathspec, theirs->path,
		(data->strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH) != 0,
		git_iterator_ignore_case(workdir), NULL, NULL))
		return true;

	if (ancestor && git_pathspec__match(pathspec, ancestor->path,
		(data->strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH) != 0,
		git_iterator_ignore_case(workdir), NULL, NULL))
		return true;

	return false;
}

GIT_INLINE(int) checkout_conflict_detect_submodule(checkout_conflictdata *conflict)
{
	conflict->submodule = ((conflict->ancestor && S_ISGITLINK(conflict->ancestor->mode)) ||
		(conflict->ours && S_ISGITLINK(conflict->ours->mode)) ||
		(conflict->theirs && S_ISGITLINK(conflict->theirs->mode)));
	return 0;
}

GIT_INLINE(int) checkout_conflict_detect_binary(git_repository *repo, checkout_conflictdata *conflict)
{
	git_blob *ancestor_blob = NULL, *our_blob = NULL, *their_blob = NULL;
	int error = 0;

	if (conflict->submodule)
		return 0;

	if (conflict->ancestor) {
		if ((error = git_blob_lookup(&ancestor_blob, repo, &conflict->ancestor->id)) < 0)
			goto done;

		conflict->binary = git_blob_is_binary(ancestor_blob);
	}

	if (!conflict->binary && conflict->ours) {
		if ((error = git_blob_lookup(&our_blob, repo, &conflict->ours->id)) < 0)
			goto done;

		conflict->binary = git_blob_is_binary(our_blob);
	}

	if (!conflict->binary && conflict->theirs) {
		if ((error = git_blob_lookup(&their_blob, repo, &conflict->theirs->id)) < 0)
			goto done;

		conflict->binary = git_blob_is_binary(their_blob);
	}

done:
	git_blob_free(ancestor_blob);
	git_blob_free(our_blob);
	git_blob_free(their_blob);

	return error;
}

static int checkout_conflict_append_update(
	const git_index_entry *ancestor,
	const git_index_entry *ours,
	const git_index_entry *theirs,
	void *payload)
{
	checkout_data *data = payload;
	checkout_conflictdata *conflict;
	int error;

	conflict = git__calloc(1, sizeof(checkout_conflictdata));
	GIT_ERROR_CHECK_ALLOC(conflict);

	conflict->ancestor = ancestor;
	conflict->ours = ours;
	conflict->theirs = theirs;

	if ((error = checkout_conflict_detect_submodule(conflict)) < 0 ||
		(error = checkout_conflict_detect_binary(data->repo, conflict)) < 0)
	{
		git__free(conflict);
		return error;
	}

	if (git_vector_insert(&data->update_conflicts, conflict))
		return -1;

	return 0;
}

static int checkout_conflicts_foreach(
	checkout_data *data,
	git_index *index,
	git_iterator *workdir,
	git_vector *pathspec,
	int (*cb)(const git_index_entry *, const git_index_entry *, const git_index_entry *, void *),
	void *payload)
{
	git_index_conflict_iterator *iterator = NULL;
	const git_index_entry *ancestor, *ours, *theirs;
	int error = 0;

	if ((error = git_index_conflict_iterator_new(&iterator, index)) < 0)
		goto done;

	/* Collect the conflicts */
	while ((error = git_index_conflict_next(&ancestor, &ours, &theirs, iterator)) == 0) {
		if (!conflict_pathspec_match(data, workdir, pathspec, ancestor, ours, theirs))
			continue;

		if ((error = cb(ancestor, ours, theirs, payload)) < 0)
			goto done;
	}

	if (error == GIT_ITEROVER)
		error = 0;

done:
	git_index_conflict_iterator_free(iterator);

	return error;
}

static int checkout_conflicts_load(checkout_data *data, git_iterator *workdir, git_vector *pathspec)
{
	git_index *index;

	/* Only write conficts from sources that have them: indexes. */
	if ((index = git_iterator_index(data->target)) == NULL)
		return 0;

	data->update_conflicts._cmp = checkout_conflictdata_cmp;

	if (checkout_conflicts_foreach(data, index, workdir, pathspec, checkout_conflict_append_update, data) < 0)
		return -1;

	/* Collect the REUC and NAME entries */
	data->update_reuc = &index->reuc;
	data->update_names = &index->names;

	return 0;
}

GIT_INLINE(int) checkout_conflicts_cmp_entry(
	const char *path,
	const git_index_entry *entry)
{
	return strcmp((const char *)path, entry->path);
}

static int checkout_conflicts_cmp_ancestor(const void *p, const void *c)
{
	const char *path = p;
	const checkout_conflictdata *conflict = c;

	if (!conflict->ancestor)
		return 1;

	return checkout_conflicts_cmp_entry(path, conflict->ancestor);
}

static checkout_conflictdata *checkout_conflicts_search_ancestor(
	checkout_data *data,
	const char *path)
{
	size_t pos;

	if (git_vector_bsearch2(&pos, &data->update_conflicts, checkout_conflicts_cmp_ancestor, path) < 0)
		return NULL;

	return git_vector_get(&data->update_conflicts, pos);
}

static checkout_conflictdata *checkout_conflicts_search_branch(
	checkout_data *data,
	const char *path)
{
	checkout_conflictdata *conflict;
	size_t i;

	git_vector_foreach(&data->update_conflicts, i, conflict) {
		int cmp = -1;

		if (conflict->ancestor)
			break;

		if (conflict->ours)
			cmp = checkout_conflicts_cmp_entry(path, conflict->ours);
		else if (conflict->theirs)
			cmp = checkout_conflicts_cmp_entry(path, conflict->theirs);

		if (cmp == 0)
			return conflict;
	}

	return NULL;
}

static int checkout_conflicts_load_byname_entry(
	checkout_conflictdata **ancestor_out,
	checkout_conflictdata **ours_out,
	checkout_conflictdata **theirs_out,
	checkout_data *data,
	const git_index_name_entry *name_entry)
{
	checkout_conflictdata *ancestor, *ours = NULL, *theirs = NULL;
	int error = 0;

	*ancestor_out = NULL;
	*ours_out = NULL;
	*theirs_out = NULL;

	if (!name_entry->ancestor) {
		git_error_set(GIT_ERROR_INDEX, "a NAME entry exists without an ancestor");
		error = -1;
		goto done;
	}

	if (!name_entry->ours && !name_entry->theirs) {
		git_error_set(GIT_ERROR_INDEX, "a NAME entry exists without an ours or theirs");
		error = -1;
		goto done;
	}

	if ((ancestor = checkout_conflicts_search_ancestor(data,
		name_entry->ancestor)) == NULL) {
		git_error_set(GIT_ERROR_INDEX,
			"a NAME entry referenced ancestor entry '%s' which does not exist in the main index",
			name_entry->ancestor);
		error = -1;
		goto done;
	}

	if (name_entry->ours) {
		if (strcmp(name_entry->ancestor, name_entry->ours) == 0)
			ours = ancestor;
		else if ((ours = checkout_conflicts_search_branch(data, name_entry->ours)) == NULL ||
			ours->ours == NULL) {
			git_error_set(GIT_ERROR_INDEX,
				"a NAME entry referenced our entry '%s' which does not exist in the main index",
				name_entry->ours);
			error = -1;
			goto done;
		}
	}

	if (name_entry->theirs) {
		if (strcmp(name_entry->ancestor, name_entry->theirs) == 0)
			theirs = ancestor;
		else if (name_entry->ours && strcmp(name_entry->ours, name_entry->theirs) == 0)
			theirs = ours;
		else if ((theirs = checkout_conflicts_search_branch(data, name_entry->theirs)) == NULL ||
			theirs->theirs == NULL) {
			git_error_set(GIT_ERROR_INDEX,
				"a NAME entry referenced their entry '%s' which does not exist in the main index",
				name_entry->theirs);
			error = -1;
			goto done;
		}
	}

	*ancestor_out = ancestor;
	*ours_out = ours;
	*theirs_out = theirs;

done:
	return error;
}

static int checkout_conflicts_coalesce_renames(
	checkout_data *data)
{
	git_index *index;
	const git_index_name_entry *name_entry;
	checkout_conflictdata *ancestor_conflict, *our_conflict, *their_conflict;
	size_t i, names;
	int error = 0;

	if ((index = git_iterator_index(data->target)) == NULL)
		return 0;

	/* Juggle entries based on renames */
	names = git_index_name_entrycount(index);

	for (i = 0; i < names; i++) {
		name_entry = git_index_name_get_byindex(index, i);

		if ((error = checkout_conflicts_load_byname_entry(
			&ancestor_conflict, &our_conflict, &their_conflict,
			data, name_entry)) < 0)
			goto done;

		if (our_conflict && our_conflict != ancestor_conflict) {
			ancestor_conflict->ours = our_conflict->ours;
			our_conflict->ours = NULL;

			if (our_conflict->theirs)
				our_conflict->name_collision = 1;

			if (our_conflict->name_collision)
				ancestor_conflict->name_collision = 1;
		}

		if (their_conflict && their_conflict != ancestor_conflict) {
			ancestor_conflict->theirs = their_conflict->theirs;
			their_conflict->theirs = NULL;

			if (their_conflict->ours)
				their_conflict->name_collision = 1;

			if (their_conflict->name_collision)
				ancestor_conflict->name_collision = 1;
		}

		if (our_conflict && our_conflict != ancestor_conflict &&
			their_conflict && their_conflict != ancestor_conflict)
			ancestor_conflict->one_to_two = 1;
	}

	git_vector_remove_matching(
		&data->update_conflicts, checkout_conflictdata_empty, NULL);

done:
	return error;
}

static int checkout_conflicts_mark_directoryfile(
	checkout_data *data)
{
	git_index *index;
	checkout_conflictdata *conflict;
	const git_index_entry *entry;
	size_t i, j, len;
	const char *path;
	int prefixed, error = 0;

	if ((index = git_iterator_index(data->target)) == NULL)
		return 0;

	len = git_index_entrycount(index);

	/* Find d/f conflicts */
	git_vector_foreach(&data->update_conflicts, i, conflict) {
		if ((conflict->ours && conflict->theirs) ||
			(!conflict->ours && !conflict->theirs))
			continue;

		path = conflict->ours ?
			conflict->ours->path : conflict->theirs->path;

		if ((error = git_index_find(&j, index, path)) < 0) {
			if (error == GIT_ENOTFOUND)
				git_error_set(GIT_ERROR_INDEX,
					"index inconsistency, could not find entry for expected conflict '%s'", path);

			goto done;
		}

		for (; j < len; j++) {
			if ((entry = git_index_get_byindex(index, j)) == NULL) {
				git_error_set(GIT_ERROR_INDEX,
					"index inconsistency, truncated index while loading expected conflict '%s'", path);
				error = -1;
				goto done;
			}

			prefixed = git_path_equal_or_prefixed(path, entry->path, NULL);

			if (prefixed == GIT_PATH_EQUAL)
				continue;

			if (prefixed == GIT_PATH_PREFIX)
				conflict->directoryfile = 1;

			break;
		}
	}

done:
	return error;
}

static int checkout_get_update_conflicts(
	checkout_data *data,
	git_iterator *workdir,
	git_vector *pathspec)
{
	int error = 0;

	if (data->strategy & GIT_CHECKOUT_SKIP_UNMERGED)
		return 0;

	if ((error = checkout_conflicts_load(data, workdir, pathspec)) < 0 ||
		(error = checkout_conflicts_coalesce_renames(data)) < 0 ||
		(error = checkout_conflicts_mark_directoryfile(data)) < 0)
		goto done;

done:
	return error;
}

static int checkout_conflict_append_remove(
	const git_index_entry *ancestor,
	const git_index_entry *ours,
	const git_index_entry *theirs,
	void *payload)
{
	checkout_data *data = payload;
	const char *name;

	assert(ancestor || ours || theirs);

	if (ancestor)
		name = git__strdup(ancestor->path);
	else if (ours)
		name = git__strdup(ours->path);
	else if (theirs)
		name = git__strdup(theirs->path);
	else
		abort();

	GIT_ERROR_CHECK_ALLOC(name);

	return git_vector_insert(&data->remove_conflicts, (char *)name);
}

static int checkout_get_remove_conflicts(
	checkout_data *data,
	git_iterator *workdir,
	git_vector *pathspec)
{
	if ((data->strategy & GIT_CHECKOUT_DONT_UPDATE_INDEX) != 0)
		return 0;

	return checkout_conflicts_foreach(data, data->index, workdir, pathspec, checkout_conflict_append_remove, data);
}

static int checkout_verify_paths(
	git_repository *repo,
	int action,
	git_diff_delta *delta)
{
	unsigned int flags = GIT_PATH_REJECT_WORKDIR_DEFAULTS;

	if (action & CHECKOUT_ACTION__REMOVE) {
		if (!git_path_isvalid(repo, delta->old_file.path, delta->old_file.mode, flags)) {
			git_error_set(GIT_ERROR_CHECKOUT, "cannot remove invalid path '%s'", delta->old_file.path);
			return -1;
		}
	}

	if (action & ~CHECKOUT_ACTION__REMOVE) {
		if (!git_path_isvalid(repo, delta->new_file.path, delta->new_file.mode, flags)) {
			git_error_set(GIT_ERROR_CHECKOUT, "cannot checkout to invalid path '%s'", delta->new_file.path);
			return -1;
		}
	}

	return 0;
}

static int checkout_get_actions(
	uint32_t **actions_ptr,
	size_t **counts_ptr,
	checkout_data *data,
	git_iterator *workdir)
{
	int error = 0, act;
	const git_index_entry *wditem;
	git_vector pathspec = GIT_VECTOR_INIT, *deltas;
	git_pool pathpool;
	git_diff_delta *delta;
	size_t i, *counts = NULL;
	uint32_t *actions = NULL;

	git_pool_init(&pathpool, 1);

	if (data->opts.paths.count > 0 &&
		git_pathspec__vinit(&pathspec, &data->opts.paths, &pathpool) < 0)
		return -1;

	if ((error = git_iterator_current(&wditem, workdir)) < 0 &&
		error != GIT_ITEROVER)
		goto fail;

	deltas = &data->diff->deltas;

	*counts_ptr = counts = git__calloc(CHECKOUT_ACTION__MAX+1, sizeof(size_t));
	*actions_ptr = actions = git__calloc(
		deltas->length ? deltas->length : 1, sizeof(uint32_t));
	if (!counts || !actions) {
		error = -1;
		goto fail;
	}

	git_vector_foreach(deltas, i, delta) {
		if ((error = checkout_action(&act, data, delta, workdir, &wditem, &pathspec)) == 0)
			error = checkout_verify_paths(data->repo, act, delta);

		if (error != 0)
			goto fail;

		actions[i] = act;

		if (act & CHECKOUT_ACTION__REMOVE)
			counts[CHECKOUT_ACTION__REMOVE]++;
		if (act & CHECKOUT_ACTION__UPDATE_BLOB)
			counts[CHECKOUT_ACTION__UPDATE_BLOB]++;
		if (act & CHECKOUT_ACTION__UPDATE_SUBMODULE)
			counts[CHECKOUT_ACTION__UPDATE_SUBMODULE]++;
		if (act & CHECKOUT_ACTION__CONFLICT)
			counts[CHECKOUT_ACTION__CONFLICT]++;
	}

	error = checkout_remaining_wd_items(data, workdir, wditem, &pathspec);
	if (error)
		goto fail;

	counts[CHECKOUT_ACTION__REMOVE] += data->removes.length;

	if (counts[CHECKOUT_ACTION__CONFLICT] > 0 &&
		(data->strategy & GIT_CHECKOUT_ALLOW_CONFLICTS) == 0)
	{
		git_error_set(GIT_ERROR_CHECKOUT, "%"PRIuZ" %s checkout",
			counts[CHECKOUT_ACTION__CONFLICT],
			counts[CHECKOUT_ACTION__CONFLICT] == 1 ?
			"conflict prevents" : "conflicts prevent");
		error = GIT_ECONFLICT;
		goto fail;
	}


	if ((error = checkout_get_remove_conflicts(data, workdir, &pathspec)) < 0 ||
		(error = checkout_get_update_conflicts(data, workdir, &pathspec)) < 0)
		goto fail;

	counts[CHECKOUT_ACTION__REMOVE_CONFLICT] = git_vector_length(&data->remove_conflicts);
	counts[CHECKOUT_ACTION__UPDATE_CONFLICT] = git_vector_length(&data->update_conflicts);

	git_pathspec__vfree(&pathspec);
	git_pool_clear(&pathpool);

	return 0;

fail:
	*counts_ptr = NULL;
	git__free(counts);
	*actions_ptr = NULL;
	git__free(actions);

	git_pathspec__vfree(&pathspec);
	git_pool_clear(&pathpool);

	return error;
}

static bool should_remove_existing(checkout_data *data)
{
	int ignorecase;

	if (git_repository__configmap_lookup(&ignorecase, data->repo, GIT_CONFIGMAP_IGNORECASE) < 0) {
		ignorecase = 0;
	}

	return (ignorecase &&
		(data->strategy & GIT_CHECKOUT_DONT_REMOVE_EXISTING) == 0);
}

#define MKDIR_NORMAL \
	GIT_MKDIR_PATH | GIT_MKDIR_VERIFY_DIR
#define MKDIR_REMOVE_EXISTING \
	MKDIR_NORMAL | GIT_MKDIR_REMOVE_FILES | GIT_MKDIR_REMOVE_SYMLINKS

/* not thread safe */
static int checkout_mkdir(
	checkout_data *data,
	const char *path,
	const char *base,
	mode_t mode,
	unsigned int flags)
{
	struct git_futils_mkdir_options mkdir_opts = {0};
	int error;

	mkdir_opts.dir_map = data->mkdir_map;
	mkdir_opts.pool = &data->pool;

	error = git_futils_mkdir_relative(
		path, base, mode, flags, &mkdir_opts);

	git_atomic_add(&data->perfdata.mkdir_calls, (int)mkdir_opts.perfdata.mkdir_calls);
	git_atomic_add(&data->perfdata.stat_calls, (int)mkdir_opts.perfdata.stat_calls);
	git_atomic_add(&data->perfdata.chmod_calls, (int)mkdir_opts.perfdata.chmod_calls);

	return error;
}

static int mkpath2file(
	checkout_data *data, const char *path, unsigned int mode)
{
	struct stat st;
	bool remove_existing;
	unsigned int flags;
	int error;

	git_mutex_lock(&data->mkpath_mutex);

	remove_existing = should_remove_existing(data);
	flags = (remove_existing ? MKDIR_REMOVE_EXISTING : MKDIR_NORMAL) |
		GIT_MKDIR_SKIP_LAST;

	if ((error = checkout_mkdir(data, path, data->opts.target_directory, mode,
		flags)) < 0)
		goto cleanup;

	if (remove_existing) {
		git_atomic_inc(&data->perfdata.stat_calls);

		if (p_lstat(path, &st) == 0) {

			/* Some file, symlink or folder already exists at this name.
			 * We would have removed it in remove_the_old unless we're on
			 * a case inensitive filesystem (or the user has asked us not
			 * to).  Remove the similarly named file to write the new.
			 */
			error = git_futils_rmdir_r(path, NULL, GIT_RMDIR_REMOVE_FILES);
		} else if (errno != ENOENT) {
			git_error_set(GIT_ERROR_OS, "failed to stat '%s'", path);
			error = GIT_EEXISTS;
		} else {
			git_error_clear();
		}
	}

cleanup:
	git_mutex_unlock(&data->mkpath_mutex);
	return error;
}

struct checkout_stream {
	git_writestream base;
	const char *path;
	int fd;
	int open;
};

static int checkout_stream_write(
	git_writestream *s, const char *buffer, size_t len)
{
	struct checkout_stream *stream = (struct checkout_stream *)s;
	int ret;

	if ((ret = p_write(stream->fd, buffer, len)) < 0)
		git_error_set(GIT_ERROR_OS, "could not write to '%s'", stream->path);

	return ret;
}

static int checkout_stream_close(git_writestream *s)
{
	struct checkout_stream *stream = (struct checkout_stream *)s;
	assert(stream && stream->open);

	stream->open = 0;
	return p_close(stream->fd);
}

static void checkout_stream_free(git_writestream *s)
{
	GIT_UNUSED(s);
}

static int blob_content_to_file(
	checkout_data *data,
	struct stat *st,
	git_blob *blob,
	const char *path,
	const char *hint_path,
	mode_t entry_filemode)
{
	int flags = data->opts.file_open_flags;
	mode_t file_mode = data->opts.file_mode ?
		data->opts.file_mode : entry_filemode;
	git_filter_options filter_opts = GIT_FILTER_OPTIONS_INIT;
	struct checkout_stream writer;
	mode_t mode;
	git_filter_list *fl = NULL;
	int fd;
	int error = 0;
	git_buf temp_buf = GIT_BUF_INIT;

	if (hint_path == NULL)
		hint_path = path;

	if ((error = mkpath2file(data, path, data->opts.dir_mode)) < 0)
		return error;

	if (flags <= 0)
		flags = O_CREAT | O_TRUNC | O_WRONLY;
	if (!(mode = file_mode))
		mode = GIT_FILEMODE_BLOB;

	if ((fd = p_open(path, flags, mode)) < 0) {
		git_error_set(GIT_ERROR_OS, "could not open '%s' for writing", path);
		return fd;
	}

	filter_opts.attr_session = &data->attr_session;
	filter_opts.temp_buf = &temp_buf;

	if (!data->opts.disable_filters) {
		git_mutex_lock(&data->index_mutex);

		error = git_filter_list__load_ext(
			&fl, data->repo, blob, hint_path,
			GIT_FILTER_TO_WORKTREE, &filter_opts);

		git_mutex_unlock(&data->index_mutex);

		if (error) {
			p_close(fd);
			return error;
		}
	}

	/* setup the writer */
	memset(&writer, 0, sizeof(struct checkout_stream));
	writer.base.write = checkout_stream_write;
	writer.base.close = checkout_stream_close;
	writer.base.free = checkout_stream_free;
	writer.path = path;
	writer.fd = fd;
	writer.open = 1;

	error = git_filter_list_stream_blob(fl, blob, &writer.base);

	assert(writer.open == 0);

	git_filter_list_free(fl);

	git_buf_dispose(&temp_buf);

	if (error < 0)
		return error;

	if (st) {
		git_atomic_inc(&data->perfdata.stat_calls);

		if ((error = p_stat(path, st)) < 0) {
			git_error_set(GIT_ERROR_OS, "failed to stat '%s'", path);
			return error;
		}

		st->st_mode = entry_filemode;
	}

	return 0;
}

static int blob_content_to_link(
	checkout_data *data,
	struct stat *st,
	git_blob *blob,
	const char *path)
{
	git_buf linktarget = GIT_BUF_INIT;
	int error;

	if ((error = mkpath2file(data, path, data->opts.dir_mode)) < 0)
		return error;

	if ((error = git_blob__getbuf(&linktarget, blob)) < 0)
		return error;

	if (data->can_symlink) {
		if ((error = p_symlink(git_buf_cstr(&linktarget), path)) < 0)
			git_error_set(GIT_ERROR_OS, "could not create symlink %s", path);
	} else {
		error = git_futils_fake_symlink(git_buf_cstr(&linktarget), path);
	}

	if (!error) {
		git_atomic_inc(&data->perfdata.stat_calls);

		if ((error = p_lstat(path, st)) < 0)
			git_error_set(GIT_ERROR_CHECKOUT, "could not stat symlink %s", path);

		st->st_mode = GIT_FILEMODE_LINK;
	}

	git_buf_dispose(&linktarget);

	return error;
}

static int checkout_update_index(
	checkout_data *data,
	const git_diff_file *file,
	struct stat *st)
{
	git_index_entry entry;

	if (!data->index)
		return 0;

	memset(&entry, 0, sizeof(entry));
	entry.path = (char *)file->path; /* cast to prevent warning */
	git_index_entry__init_from_stat(&entry, st, true);
	git_oid_cpy(&entry.id, &file->id);

	return git_index_add(data->index, &entry);
}

static int checkout_submodule_update_index(
	checkout_data *data,
	const git_diff_file *file)
{
	git_buf fullpath = GIT_BUF_INIT;
	int error;
	struct stat st;

	/* update the index unless prevented */
	if ((data->strategy & GIT_CHECKOUT_DONT_UPDATE_INDEX) != 0)
		return 0;

	if (build_target_fullpath(&fullpath, data, file->path) < 0)
		return -1;

	git_atomic_inc(&data->perfdata.stat_calls);
	if (p_stat(git_buf_cstr(&fullpath), &st) < 0) {
		git_error_set(
			GIT_ERROR_CHECKOUT, "could not stat submodule %s\n", file->path);
		error = GIT_ENOTFOUND;
		goto cleanup;
	}

	st.st_mode = GIT_FILEMODE_COMMIT;

	error = checkout_update_index(data, file, &st);

cleanup:
	git_buf_dispose(&fullpath);
	return error;
}

static int checkout_submodule(
	checkout_data *data,
	const git_diff_file *file)
{
	bool remove_existing = should_remove_existing(data);
	int error = 0;

	/* Until submodules are supported, UPDATE_ONLY means do nothing here */
	if ((data->strategy & GIT_CHECKOUT_UPDATE_ONLY) != 0)
		return 0;

	if ((error = checkout_mkdir(
			data,
			file->path, data->opts.target_directory, data->opts.dir_mode,
			remove_existing ? MKDIR_REMOVE_EXISTING : MKDIR_NORMAL)) < 0)
		return error;

	if ((error = git_submodule_lookup(NULL, data->repo, file->path)) < 0) {
		/* I've observed repos with submodules in the tree that do not
		 * have a .gitmodules - core Git just makes an empty directory
		 */
		if (error == GIT_ENOTFOUND) {
			git_error_clear();
			return checkout_submodule_update_index(data, file);
		}

		return error;
	}

	/* TODO: Support checkout_strategy options.  Two circumstances:
	 * 1 - submodule already checked out, but we need to move the HEAD
	 *     to the new OID, or
	 * 2 - submodule not checked out and we should recursively check it out
	 *
	 * Checkout will not execute a pull on the submodule, but a clone
	 * command should probably be able to.  Do we need a submodule callback?
	 */

	return checkout_submodule_update_index(data, file);
}

static void report_progress(
	checkout_data *data,
	const char *path)
{
	if (data->opts.progress_cb)
		data->opts.progress_cb(
			path, data->completed_steps, data->total_steps,
			data->opts.progress_payload);
}

static int checkout_safe_for_update_only(
	checkout_data *data, const char *path, mode_t expected_mode)
{
	struct stat st;

	git_atomic_inc(&data->perfdata.stat_calls);

	if (p_lstat(path, &st) < 0) {
		/* if doesn't exist, then no error and no update */
		if (errno == ENOENT || errno == ENOTDIR)
			return 0;

		/* otherwise, stat error and no update */
		git_error_set(GIT_ERROR_OS, "failed to stat '%s'", path);
		return -1;
	}

	/* only safe for update if this is the same type of file */
	if ((st.st_mode & ~0777) == (expected_mode & ~0777))
		return 1;

	return 0;
}

static int checkout_write_content(
	checkout_data *data,
	const git_oid *oid,
	const char *full_path,
	const char *hint_path,
	unsigned int mode,
	struct stat *st)
{
	int error = 0;
	git_blob *blob;

	if ((error = git_blob_lookup(&blob, data->repo, oid)) < 0)
		return error;

	if (S_ISLNK(mode))
		error = blob_content_to_link(data, st, blob, full_path);
	else
		error = blob_content_to_file(data, st, blob, full_path, hint_path, mode);

	git_blob_free(blob);

	/* if we try to create the blob and an existing directory blocks it from
	 * being written, then there must have been a typechange conflict in a
	 * parent directory - suppress the error and try to continue.
	 */
	if ((data->strategy & GIT_CHECKOUT_ALLOW_CONFLICTS) != 0 &&
		(error == GIT_ENOTFOUND || error == GIT_EEXISTS))
	{
		git_error_clear();
		error = 0;
	}

	return error;
}

static int checkout_blob(
	checkout_data *data,
	const git_diff_file *file)
{
	git_buf fullpath = GIT_BUF_INIT;
	struct stat st;
	int error = 0;

	if (build_target_fullpath(&fullpath, data, file->path) < 0)
		return -1;

	if ((data->strategy & GIT_CHECKOUT_UPDATE_ONLY) != 0) {
		int rval = checkout_safe_for_update_only(
			data, fullpath.ptr, file->mode);

		if (rval <= 0) {
			git_buf_dispose(&fullpath);
			return rval;
		}
	}

	error = checkout_write_content(
		data, &file->id, fullpath.ptr, NULL, file->mode, &st);

	/* update the index unless prevented */
	if (!error && (data->strategy & GIT_CHECKOUT_DONT_UPDATE_INDEX) == 0) {
		git_mutex_lock(&data->index_mutex);
		error = checkout_update_index(data, file, &st);
		git_mutex_unlock(&data->index_mutex);
	}

	/* update the submodule data if this was a new .gitmodules file */
	if (!error && strcmp(file->path, ".gitmodules") == 0)
		data->reload_submodules = true;

	git_buf_dispose(&fullpath);
	return error;
}

static int checkout_remove_the_old(
	unsigned int *actions,
	checkout_data *data)
{
	int error = 0;
	git_diff_delta *delta;
	const char *str;
	size_t i;
	git_buf fullpath = GIT_BUF_INIT;
	uint32_t flg = GIT_RMDIR_EMPTY_PARENTS |
		GIT_RMDIR_REMOVE_FILES | GIT_RMDIR_REMOVE_BLOCKERS;

	if (data->opts.checkout_strategy & GIT_CHECKOUT_SKIP_LOCKED_DIRECTORIES)
		flg |= GIT_RMDIR_SKIP_NONEMPTY;

	if (build_target_fullpath(&fullpath, data, NULL) < 0)
		return -1;

	git_vector_foreach(&data->diff->deltas, i, delta) {
		if (actions[i] & CHECKOUT_ACTION__REMOVE) {
			error = git_futils_rmdir_r(
				delta->old_file.path, fullpath.ptr, flg);

			if (error < 0)
				goto cleanup;

			data->completed_steps++;
			report_progress(data, delta->old_file.path);

			if ((actions[i] & CHECKOUT_ACTION__UPDATE_BLOB) == 0 &&
				(data->strategy & GIT_CHECKOUT_DONT_UPDATE_INDEX) == 0 &&
				data->index != NULL)
			{
				(void)git_index_remove(data->index, delta->old_file.path, 0);
			}
		}
	}

	git_vector_foreach(&data->removes, i, str) {
		error = git_futils_rmdir_r(str, fullpath.ptr, flg);
		if (error < 0)
			goto cleanup;

		data->completed_steps++;
		report_progress(data, str);

		if ((data->strategy & GIT_CHECKOUT_DONT_UPDATE_INDEX) == 0 &&
			data->index != NULL)
		{
			if (str[strlen(str) - 1] == '/')
				(void)git_index_remove_directory(data->index, str, 0);
			else
				(void)git_index_remove(data->index, str, 0);
		}
	}

cleanup:
	git_buf_dispose(&fullpath);
	return error;
}

enum {
	NO_SYMLINKS = 0,
	SYMLINKS_ONLY = 1
};

static int checkout_create_perform(
	checkout_data *data,
	unsigned int action,
	git_diff_delta *delta,
  unsigned int checkout_option)
{
	int error = 0;

	if (action & CHECKOUT_ACTION__UPDATE_BLOB) {
		if (checkout_option == NO_SYMLINKS && S_ISLNK(delta->new_file.mode))
			return 0;

		if (checkout_option == SYMLINKS_ONLY && !S_ISLNK(delta->new_file.mode))
			return 0;

		error = checkout_blob(data, &delta->new_file);
		if (error < 0)
			return error;

		data->completed_steps++;
		report_progress(data, delta->new_file.path);
	}

	return 0;
}

static int checkout_create_the_new__single(
	unsigned int *actions,
	checkout_data *data)
{
	int error = 0;
	git_diff_delta *delta;
	size_t i;

	git_vector_foreach(&data->diff->deltas, i, delta) {
		if ((error = checkout_create_perform(data, actions[i], delta, NO_SYMLINKS)) < 0)
			return error;
	}

	git_vector_foreach(&data->diff->deltas, i, delta) {
		if ((error = checkout_create_perform(data, actions[i], delta, SYMLINKS_ONLY)) < 0)
			return error;
	}

	return 0;
}

#if defined(GIT_THREADS)

static int paths_cmp(const void *a, const void *b) { return git__strcmp((char*)a, (char*)b); }

typedef struct {
	int error;
	size_t index;
	bool skipped;
} checkout_progress_pair;

typedef struct {
	git_thread thread;
	const unsigned int *actions;
	checkout_data *cd;

	git_cond *cond;
	git_mutex *mutex;

	git_atomic *delta_index;
	git_atomic *error;
	git_vector *progress_pairs;
} thread_params;

static void *checkout_create_the_new__thread(void *arg)
{
	thread_params *worker = arg;
	size_t i;

	while ((i = git_atomic_inc(worker->delta_index)) <
			git_vector_length(&worker->cd->diff->deltas)) {
		checkout_progress_pair *progress_pair;
		git_diff_delta *delta = git_vector_get(&worker->cd->diff->deltas, i);

		if (delta == NULL || git_atomic_get(worker->error) != 0)
			return NULL;

		progress_pair = (checkout_progress_pair *)git__malloc(
			sizeof(checkout_progress_pair));
		if (progress_pair == NULL) {
			git_atomic_set(worker->error, -1);
			git_cond_signal(worker->cond);
			return NULL;
		}

		/* We skip symlink operations, because we handle them
		 * in the main thread to avoid a symlink security flaw.
		 */
		if (!S_ISLNK(delta->new_file.mode) &&
		    worker->actions[i] & CHECKOUT_ACTION__UPDATE_BLOB) {
			/* We will retry failed operations in the calling thread to handle
			 * the case where might encounter a file locking error due to
			 * multithreading and name collisions.
			 */
			progress_pair->index = i;
			progress_pair->error = checkout_blob(worker->cd, &delta->new_file);
			progress_pair->skipped = false;
		} else {
			progress_pair->index = i;
			progress_pair->error = 0;
			progress_pair->skipped = true;
		}

		git_mutex_lock(worker->mutex);
		git_vector_insert(worker->progress_pairs, progress_pair);
		git_cond_signal(worker->cond);
		git_mutex_unlock(worker->mutex);
	}

	return NULL;
}

static int checkout_create_the_new__parallel(
	unsigned int *actions,
	checkout_data *data)
{
	thread_params *p;
	size_t i, num_threads = git_online_cpus(), last_index = 0, current_index = 0,
		num_deltas = git_vector_length(&data->diff->deltas);
	int ret;
	checkout_progress_pair *progress_pair;
	git_atomic delta_index, error;
	git_diff_delta *delta;
	git_vector errored_pairs, progress_pairs;
	git_cond cond;
	git_mutex mutex;

	if (
		(ret = git_vector_init(&progress_pairs, num_deltas, paths_cmp)) < 0 ||
		(ret = git_vector_init(&errored_pairs, num_deltas, paths_cmp)) < 0)
		return ret;

	p = git__mallocarray(num_threads, sizeof(*p));
	GIT_ERROR_CHECK_ALLOC(p);

	git_cond_init(&cond);
	git_mutex_init(&mutex);
	git_mutex_lock(&mutex);

	git_atomic_set(&delta_index, -1);
	git_atomic_set(&error, 0);

	/* Initialize worker threads */
	for (i = 0; i < num_threads; ++i) {
		p[i].actions = actions;
		p[i].cd = data;
		p[i].cond = &cond;
		p[i].mutex = &mutex;
		p[i].error = &error;
		p[i].delta_index = &delta_index;
		p[i].progress_pairs = &progress_pairs;
	}

	/* Start worker threads */
	for (i = 0; i < num_threads; ++i) {
		ret = git_thread_create(&p[i].thread, checkout_create_the_new__thread, &p[i]);

		/* On error, we will cleanly exit any started worker threads,
		 * and then return with our error code */
		if (ret) {
			git_atomic_set(&error, -1);
			git_error_set(GIT_ERROR_THREAD, "unable to create thread");
			git_mutex_unlock(&mutex);
			/* Only clean up the number of threads we have started */
			num_threads = i;
			ret = -1;
			goto cleanup;
		}
	}

	while (last_index < num_deltas) {
		if ((ret = git_atomic_get(&error)) != 0) {
			git_mutex_unlock(&mutex);
			goto cleanup;
		}

		current_index = git_vector_length(&progress_pairs);

		if (last_index == current_index) {
			git_cond_wait(&cond, &mutex);
			current_index = git_vector_length(&progress_pairs);
		}

		git_mutex_unlock(&mutex);

		for (; last_index < current_index; ++last_index) {
			progress_pair = git_vector_get(&progress_pairs,
				last_index);
			delta = git_vector_get(&data->diff->deltas, last_index);

			if (progress_pair->skipped)
				continue;

			/* We will retry errored checkouts synchronously after all the workers
			 * complete
			 */
			if (progress_pair->error < 0) {
				git_vector_insert(&errored_pairs, progress_pair);
				continue;
			}

			data->completed_steps++;
			report_progress(data, delta->new_file.path);
		}
		git_mutex_lock(&mutex);
	}

	git_vector_foreach(&errored_pairs, i, progress_pair) {
		delta = git_vector_get(&data->diff->deltas, progress_pair->index);
		if ((ret = checkout_create_perform(data, actions[progress_pair->index], delta,
			NO_SYMLINKS)) < 0)
				goto cleanup;
	}

	/* After we create everything else, we need to create all the symlinks
	 * to ensure that we don't accidentally write data through symlinks into
	 * the .git directory.
	 */
	git_vector_foreach(&data->diff->deltas, i, delta) {
		if (S_ISLNK(delta->new_file.mode) &&
				actions[i] & CHECKOUT_ACTION__UPDATE_BLOB &&
				(ret = checkout_create_perform(data, actions[i], delta, SYMLINKS_ONLY)) < 0)
			goto cleanup;
	}

cleanup:
	for (i = 0; i < num_threads; ++i) {
		git_thread_join(&p[i].thread, NULL);
	}

	git__free(p);
	git_vector_free(&errored_pairs);
	git_vector_free_deep(&progress_pairs);
	git_cond_free(&cond);
	git_mutex_free(&mutex);

	return ret;
}

#endif

static int checkout_create_the_new(
	unsigned int *actions,
	checkout_data *data)
{
#ifdef GIT_THREADS
	if (git_online_cpus() > 1)
		return checkout_create_the_new__parallel(actions, data);
	else
#endif
	return checkout_create_the_new__single(actions, data);
}

static int checkout_create_submodules(
	unsigned int *actions,
	checkout_data *data)
{
	git_diff_delta *delta;
	size_t i;

	git_vector_foreach(&data->diff->deltas, i, delta) {
		if (actions[i] & CHECKOUT_ACTION__UPDATE_SUBMODULE) {
			int error = checkout_submodule(data, &delta->new_file);
			if (error < 0)
				return error;

			data->completed_steps++;
			report_progress(data, delta->new_file.path);
		}
	}

	return 0;
}

static int checkout_lookup_head_tree(git_tree **out, git_repository *repo)
{
	int error = 0;
	git_reference *ref = NULL;
	git_object *head;

	if (!(error = git_repository_head(&ref, repo)) &&
		!(error = git_reference_peel(&head, ref, GIT_OBJECT_TREE)))
		*out = (git_tree *)head;

	git_reference_free(ref);

	return error;
}


static int conflict_entry_name(
	git_buf *out,
	const char *side_name,
	const char *filename)
{
	if (git_buf_puts(out, side_name) < 0 ||
		git_buf_putc(out, ':') < 0 ||
		git_buf_puts(out, filename) < 0)
		return -1;

	return 0;
}

static int checkout_path_suffixed(git_buf *path, const char *suffix)
{
	size_t path_len;
	int i = 0, error = 0;

	if ((error = git_buf_putc(path, '~')) < 0 || (error = git_buf_puts(path, suffix)) < 0)
		return -1;

	path_len = git_buf_len(path);

	while (git_path_exists(git_buf_cstr(path)) && i < INT_MAX) {
		git_buf_truncate(path, path_len);

		if ((error = git_buf_putc(path, '_')) < 0 ||
			(error = git_buf_printf(path, "%d", i)) < 0)
			return error;

		i++;
	}

	if (i == INT_MAX) {
		git_buf_truncate(path, path_len);

		git_error_set(GIT_ERROR_CHECKOUT, "could not write '%s': working directory file exists", path->ptr);
		return GIT_EEXISTS;
	}

	return 0;
}

static int checkout_write_entry(
	checkout_data *data,
	checkout_conflictdata *conflict,
	const git_index_entry *side)
{
	const char *hint_path = NULL, *suffix;
	git_buf fullpath = GIT_BUF_INIT;
	struct stat st;
	int error = 0;

	assert (side == conflict->ours || side == conflict->theirs);

	if (build_target_fullpath(&fullpath, data, side->path) < 0)
		return -1;

	if ((conflict->name_collision || conflict->directoryfile) &&
		(data->strategy & GIT_CHECKOUT_USE_OURS) == 0 &&
		(data->strategy & GIT_CHECKOUT_USE_THEIRS) == 0) {

		if (side == conflict->ours)
			suffix = data->opts.our_label ? data->opts.our_label :
				"ours";
		else
			suffix = data->opts.their_label ? data->opts.their_label :
				"theirs";

		if ((error = checkout_path_suffixed(&fullpath, suffix)) < 0)
			goto cleanup;

		hint_path = side->path;
	}

	if ((data->strategy & GIT_CHECKOUT_UPDATE_ONLY) != 0 &&
		(error = checkout_safe_for_update_only(data, fullpath.ptr, side->mode)) <= 0)
		goto cleanup;

	if (!S_ISGITLINK(side->mode) &&
	    (error = checkout_write_content(data,
					    &side->id, fullpath.ptr, hint_path, side->mode, &st)) <= 0)
	    goto cleanup;

cleanup:
	git_buf_dispose(&fullpath);
	return 0;
}

static int checkout_write_entries(
	checkout_data *data,
	checkout_conflictdata *conflict)
{
	int error = 0;

	if ((error = checkout_write_entry(data, conflict, conflict->ours)) >= 0)
		error = checkout_write_entry(data, conflict, conflict->theirs);

	return error;
}

static int checkout_merge_path(
	git_buf *out,
	checkout_data *data,
	checkout_conflictdata *conflict,
	git_merge_file_result *result)
{
	const char *our_label_raw, *their_label_raw, *suffix;
	int error = 0;

	if ((error = git_buf_joinpath(out, git_repository_workdir(data->repo), result->path)) < 0)
		return error;

	/* Most conflicts simply use the filename in the index */
	if (!conflict->name_collision)
		return 0;

	/* Rename 2->1 conflicts need the branch name appended */
	our_label_raw = data->opts.our_label ? data->opts.our_label : "ours";
	their_label_raw = data->opts.their_label ? data->opts.their_label : "theirs";
	suffix = strcmp(result->path, conflict->ours->path) == 0 ? our_label_raw : their_label_raw;

	if ((error = checkout_path_suffixed(out, suffix)) < 0)
		return error;

	return 0;
}

static int checkout_write_merge(
	checkout_data *data,
	checkout_conflictdata *conflict)
{
	git_buf our_label = GIT_BUF_INIT, their_label = GIT_BUF_INIT,
		path_suffixed = GIT_BUF_INIT, path_workdir = GIT_BUF_INIT,
		in_data = GIT_BUF_INIT, out_data = GIT_BUF_INIT;
	git_merge_file_options opts = GIT_MERGE_FILE_OPTIONS_INIT;
	git_merge_file_result result = {0};
	git_filebuf output = GIT_FILEBUF_INIT;
	git_filter_list *fl = NULL;
	git_filter_options filter_opts = GIT_FILTER_OPTIONS_INIT;
	int error = 0;
	git_buf temp_buf = GIT_BUF_INIT;

	if (data->opts.checkout_strategy & GIT_CHECKOUT_CONFLICT_STYLE_DIFF3)
		opts.flags |= GIT_MERGE_FILE_STYLE_DIFF3;

	opts.ancestor_label = data->opts.ancestor_label ?
		data->opts.ancestor_label : "ancestor";
	opts.our_label = data->opts.our_label ?
		data->opts.our_label : "ours";
	opts.their_label = data->opts.their_label ?
		data->opts.their_label : "theirs";

	/* If all the paths are identical, decorate the diff3 file with the branch
	 * names.  Otherwise, append branch_name:path.
	 */
	if (conflict->ours && conflict->theirs &&
		strcmp(conflict->ours->path, conflict->theirs->path) != 0) {

		if ((error = conflict_entry_name(
			&our_label, opts.our_label, conflict->ours->path)) < 0 ||
			(error = conflict_entry_name(
			&their_label, opts.their_label, conflict->theirs->path)) < 0)
			goto done;

		opts.our_label = git_buf_cstr(&our_label);
		opts.their_label = git_buf_cstr(&their_label);
	}

	if ((error = git_merge_file_from_index(&result, data->repo,
		conflict->ancestor, conflict->ours, conflict->theirs, &opts)) < 0)
		goto done;

	if (result.path == NULL || result.mode == 0) {
		git_error_set(GIT_ERROR_CHECKOUT, "could not merge contents of file");
		error = GIT_ECONFLICT;
		goto done;
	}

	if ((error = checkout_merge_path(&path_workdir, data, conflict, &result)) < 0)
		goto done;

	if ((data->strategy & GIT_CHECKOUT_UPDATE_ONLY) != 0 &&
		(error = checkout_safe_for_update_only(data, git_buf_cstr(&path_workdir), result.mode)) <= 0)
		goto done;

	if (!data->opts.disable_filters) {
		in_data.ptr = (char *)result.ptr;
		in_data.size = result.len;

		filter_opts.attr_session = &data->attr_session;
		filter_opts.temp_buf = &temp_buf;

		if ((error = git_filter_list__load_ext(
				&fl, data->repo, NULL, git_buf_cstr(&path_workdir),
				GIT_FILTER_TO_WORKTREE, &filter_opts)) < 0 ||
			(error = git_filter_list_apply_to_data(&out_data, fl, &in_data)) < 0)
			goto done;
	} else {
		out_data.ptr = (char *)result.ptr;
		out_data.size = result.len;
	}

	if ((error = mkpath2file(data, path_workdir.ptr, data->opts.dir_mode)) < 0 ||
		(error = git_filebuf_open(&output, git_buf_cstr(&path_workdir), GIT_FILEBUF_DO_NOT_BUFFER, result.mode)) < 0 ||
		(error = git_filebuf_write(&output, out_data.ptr, out_data.size)) < 0 ||
		(error = git_filebuf_commit(&output)) < 0)
		goto done;

done:
	git_filter_list_free(fl);

	git_buf_dispose(&out_data);
	git_buf_dispose(&our_label);
	git_buf_dispose(&their_label);

	git_merge_file_result_free(&result);
	git_buf_dispose(&path_workdir);
	git_buf_dispose(&path_suffixed);
	git_buf_dispose(&temp_buf);

	return error;
}

static int checkout_conflict_add(
	checkout_data *data,
	const git_index_entry *conflict)
{
	int error = git_index_remove(data->index, conflict->path, 0);

	if (error == GIT_ENOTFOUND)
		git_error_clear();
	else if (error < 0)
		return error;

	return git_index_add(data->index, conflict);
}

static int checkout_conflict_update_index(
	checkout_data *data,
	checkout_conflictdata *conflict)
{
	int error = 0;

	if (conflict->ancestor)
		error = checkout_conflict_add(data, conflict->ancestor);

	if (!error && conflict->ours)
		error = checkout_conflict_add(data, conflict->ours);

	if (!error && conflict->theirs)
		error = checkout_conflict_add(data, conflict->theirs);

	return error;
}

static int checkout_create_conflicts(checkout_data *data)
{
	checkout_conflictdata *conflict;
	size_t i;
	int error = 0;

	git_vector_foreach(&data->update_conflicts, i, conflict) {

		/* Both deleted: nothing to do */
		if (conflict->ours == NULL && conflict->theirs == NULL)
			error = 0;

		else if ((data->strategy & GIT_CHECKOUT_USE_OURS) &&
			conflict->ours)
			error = checkout_write_entry(data, conflict, conflict->ours);
		else if ((data->strategy & GIT_CHECKOUT_USE_THEIRS) &&
			conflict->theirs)
			error = checkout_write_entry(data, conflict, conflict->theirs);

		/* Ignore the other side of name collisions. */
		else if ((data->strategy & GIT_CHECKOUT_USE_OURS) &&
			!conflict->ours && conflict->name_collision)
			error = 0;
		else if ((data->strategy & GIT_CHECKOUT_USE_THEIRS) &&
			!conflict->theirs && conflict->name_collision)
			error = 0;

		/* For modify/delete, name collisions and d/f conflicts, write
		 * the file (potentially with the name mangled.
		 */
		else if (conflict->ours != NULL && conflict->theirs == NULL)
			error = checkout_write_entry(data, conflict, conflict->ours);
		else if (conflict->ours == NULL && conflict->theirs != NULL)
			error = checkout_write_entry(data, conflict, conflict->theirs);

		/* Add/add conflicts and rename 1->2 conflicts, write the
		 * ours/theirs sides (potentially name mangled).
		 */
		else if (conflict->one_to_two)
			error = checkout_write_entries(data, conflict);

		/* If all sides are links, write the ours side */
		else if (S_ISLNK(conflict->ours->mode) &&
			S_ISLNK(conflict->theirs->mode))
			error = checkout_write_entry(data, conflict, conflict->ours);
		/* Link/file conflicts, write the file side */
		else if (S_ISLNK(conflict->ours->mode))
			error = checkout_write_entry(data, conflict, conflict->theirs);
		else if (S_ISLNK(conflict->theirs->mode))
			error = checkout_write_entry(data, conflict, conflict->ours);

		/* If any side is a gitlink, do nothing. */
		else if (conflict->submodule)
			error = 0;

		/* If any side is binary, write the ours side */
		else if (conflict->binary)
			error = checkout_write_entry(data, conflict, conflict->ours);

		else if (!error)
			error = checkout_write_merge(data, conflict);

		/* Update the index extensions (REUC and NAME) if we're checking
		 * out a different index. (Otherwise just leave them there.)
		 */
		if (!error && (data->strategy & GIT_CHECKOUT_DONT_UPDATE_INDEX) == 0)
			error = checkout_conflict_update_index(data, conflict);

		if (error)
			break;

		data->completed_steps++;
		report_progress(data,
			conflict->ours ? conflict->ours->path :
			(conflict->theirs ? conflict->theirs->path : conflict->ancestor->path));
	}

	return error;
}

static int checkout_remove_conflicts(checkout_data *data)
{
	const char *conflict;
	size_t i;

	git_vector_foreach(&data->remove_conflicts, i, conflict) {
		if (git_index_conflict_remove(data->index, conflict) < 0)
			return -1;

		data->completed_steps++;
	}

	return 0;
}

static int checkout_extensions_update_index(checkout_data *data)
{
	const git_index_reuc_entry *reuc_entry;
	const git_index_name_entry *name_entry;
	size_t i;
	int error = 0;

	if ((data->strategy & GIT_CHECKOUT_UPDATE_ONLY) != 0)
		return 0;

	if (data->update_reuc) {
		git_vector_foreach(data->update_reuc, i, reuc_entry) {
			if ((error = git_index_reuc_add(data->index, reuc_entry->path,
				reuc_entry->mode[0], &reuc_entry->oid[0],
				reuc_entry->mode[1], &reuc_entry->oid[1],
				reuc_entry->mode[2], &reuc_entry->oid[2])) < 0)
				goto done;
		}
	}

	if (data->update_names) {
		git_vector_foreach(data->update_names, i, name_entry) {
			if ((error = git_index_name_add(data->index, name_entry->ancestor,
				name_entry->ours, name_entry->theirs)) < 0)
				goto done;
		}
	}

done:
	return error;
}

static void checkout_data_clear(checkout_data *data)
{
	if (data->opts_free_baseline) {
		git_tree_free(data->opts.baseline);
		data->opts.baseline = NULL;
	}

	git_vector_free(&data->removes);
	git_pool_clear(&data->pool);

	git_vector_free_deep(&data->remove_conflicts);
	git_vector_free_deep(&data->update_conflicts);

	git__free(data->pfx);
	data->pfx = NULL;

	git_buf_dispose(&data->target_path);

	git_index_free(data->index);
	data->index = NULL;

	git_strmap_free(data->mkdir_map);
	data->mkdir_map = NULL;

	git_attr_session__free(&data->attr_session);

	git_mutex_free(&data->index_mutex);
	git_mutex_free(&data->mkpath_mutex);
}

static int checkout_data_init(
	checkout_data *data,
	git_iterator *target,
	const git_checkout_options *proposed)
{
	int error = 0;
	git_repository *repo = git_iterator_owner(target);

	memset(data, 0, sizeof(*data));

	if (!repo) {
		git_error_set(GIT_ERROR_CHECKOUT, "cannot checkout nothing");
		return -1;
	}

	if ((!proposed || !proposed->target_directory) &&
		(error = git_repository__ensure_not_bare(repo, "checkout")) < 0)
		return error;

	data->repo = repo;
	data->target = target;
	git_mutex_init(&data->index_mutex);
	git_mutex_init(&data->mkpath_mutex);

	GIT_ERROR_CHECK_VERSION(
		proposed, GIT_CHECKOUT_OPTIONS_VERSION, "git_checkout_options");

	if (!proposed)
		GIT_INIT_STRUCTURE(&data->opts, GIT_CHECKOUT_OPTIONS_VERSION);
	else
		memmove(&data->opts, proposed, sizeof(git_checkout_options));

	if (!data->opts.target_directory)
		data->opts.target_directory = git_repository_workdir(repo);
	else if (!git_path_isdir(data->opts.target_directory) &&
			 (error = checkout_mkdir(data,
				data->opts.target_directory, NULL,
				GIT_DIR_MODE, GIT_MKDIR_VERIFY_DIR)) < 0)
		goto cleanup;

	if ((error = git_repository_index(&data->index, data->repo)) < 0)
		goto cleanup;

	/* refresh config and index content unless NO_REFRESH is given */
	if ((data->opts.checkout_strategy & GIT_CHECKOUT_NO_REFRESH) == 0) {
		git_config *cfg;

		if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
			goto cleanup;

		/* Reload the repository index (unless we're checking out the
		 * index; then it has the changes we're trying to check out
		 * and those should not be overwritten.)
		 */
		if (data->index != git_iterator_index(target)) {
			if (data->opts.checkout_strategy & GIT_CHECKOUT_FORCE) {
				/* When forcing, we can blindly re-read the index */
				if ((error = git_index_read(data->index, false)) < 0)
					goto cleanup;
			} else {
				/*
				 * When not being forced, we need to check for unresolved
				 * conflicts and unsaved changes in the index before
				 * proceeding.
				 */
				if (git_index_has_conflicts(data->index)) {
					error = GIT_ECONFLICT;
					git_error_set(GIT_ERROR_CHECKOUT,
						"unresolved conflicts exist in the index");
					goto cleanup;
				}

				if ((error = git_index_read_safely(data->index)) < 0)
					goto cleanup;
			}

			/* clean conflict data in the current index */
			git_index_name_clear(data->index);
			git_index_reuc_clear(data->index);
		}
	}

	/* if you are forcing, allow all safe updates, plus recreate missing */
	if ((data->opts.checkout_strategy & GIT_CHECKOUT_FORCE) != 0)
		data->opts.checkout_strategy |= GIT_CHECKOUT_SAFE |
			GIT_CHECKOUT_RECREATE_MISSING;

	/* if the repository does not actually have an index file, then this
	 * is an initial checkout (perhaps from clone), so we allow safe updates
	 */
	if (!data->index->on_disk &&
		(data->opts.checkout_strategy & GIT_CHECKOUT_SAFE) != 0)
		data->opts.checkout_strategy |= GIT_CHECKOUT_RECREATE_MISSING;

	data->strategy = data->opts.checkout_strategy;

	/* opts->disable_filters is false by default */

	if (!data->opts.dir_mode)
		data->opts.dir_mode = GIT_DIR_MODE;

	if (!data->opts.file_open_flags)
		data->opts.file_open_flags = O_CREAT | O_TRUNC | O_WRONLY;

	data->pfx = git_pathspec_prefix(&data->opts.paths);

	if ((error = git_repository__configmap_lookup(
			 &data->can_symlink, repo, GIT_CONFIGMAP_SYMLINKS)) < 0)
		goto cleanup;

	if ((error = git_repository__configmap_lookup(
			 &data->respect_filemode, repo, GIT_CONFIGMAP_FILEMODE)) < 0)
		goto cleanup;

	if (!data->opts.baseline && !data->opts.baseline_index) {
		data->opts_free_baseline = true;
		error = 0;

		/* if we don't have an index, this is an initial checkout and
		 * should be against an empty baseline
		 */
		if (data->index->on_disk)
			error = checkout_lookup_head_tree(&data->opts.baseline, repo);

		if (error == GIT_EUNBORNBRANCH) {
			error = 0;
			git_error_clear();
		}

		if (error < 0)
			goto cleanup;
	}

	if ((data->opts.checkout_strategy &
		(GIT_CHECKOUT_CONFLICT_STYLE_MERGE | GIT_CHECKOUT_CONFLICT_STYLE_DIFF3)) == 0) {
		git_config_entry *conflict_style = NULL;
		git_config *cfg = NULL;

		if ((error = git_repository_config__weakptr(&cfg, repo)) < 0 ||
			(error = git_config_get_entry(&conflict_style, cfg, "merge.conflictstyle")) < 0 ||
			error == GIT_ENOTFOUND)
			;
		else if (error)
			goto cleanup;
		else if (strcmp(conflict_style->value, "merge") == 0)
			data->opts.checkout_strategy |= GIT_CHECKOUT_CONFLICT_STYLE_MERGE;
		else if (strcmp(conflict_style->value, "diff3") == 0)
			data->opts.checkout_strategy |= GIT_CHECKOUT_CONFLICT_STYLE_DIFF3;
		else {
			git_error_set(GIT_ERROR_CHECKOUT, "unknown style '%s' given for 'merge.conflictstyle'",
				conflict_style->value);
			error = -1;
			git_config_entry_free(conflict_style);
			goto cleanup;
		}
		git_config_entry_free(conflict_style);
	}

	git_pool_init(&data->pool, 1);

	if ((error = git_vector_init(&data->removes, 0, git__strcmp_cb)) < 0 ||
	    (error = git_vector_init(&data->remove_conflicts, 0, NULL)) < 0 ||
	    (error = git_vector_init(&data->update_conflicts, 0, NULL)) < 0 ||
	    (error = git_buf_puts(&data->target_path, data->opts.target_directory)) < 0 ||
	    (error = git_path_to_dir(&data->target_path)) < 0 ||
	    (error = git_strmap_new(&data->mkdir_map)) < 0)
		goto cleanup;

	git_attr_session__init(&data->attr_session, data->repo);

	git_atomic_set(&data->perfdata.mkdir_calls, 0);
	git_atomic_set(&data->perfdata.stat_calls, 0);
	git_atomic_set(&data->perfdata.chmod_calls, 0);

cleanup:
	if (error < 0)
		checkout_data_clear(data);

	return error;
}

#define CHECKOUT_INDEX_DONT_WRITE_MASK \
	(GIT_CHECKOUT_DONT_UPDATE_INDEX | GIT_CHECKOUT_DONT_WRITE_INDEX)

int git_checkout_iterator(
	git_iterator *target,
	git_index *index,
	const git_checkout_options *opts)
{
	int error = 0;
	git_iterator *baseline = NULL, *workdir = NULL;
	git_iterator_options baseline_opts = GIT_ITERATOR_OPTIONS_INIT,
		workdir_opts = GIT_ITERATOR_OPTIONS_INIT;
	checkout_data data = {0};
	git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
	uint32_t *actions = NULL;
	size_t *counts = NULL;

	/* initialize structures and options */
	error = checkout_data_init(&data, target, opts);
	if (error < 0)
		return error;

	diff_opts.flags =
		GIT_DIFF_INCLUDE_UNMODIFIED |
		GIT_DIFF_INCLUDE_UNREADABLE |
		GIT_DIFF_INCLUDE_UNTRACKED |
		GIT_DIFF_RECURSE_UNTRACKED_DIRS | /* needed to match baseline */
		GIT_DIFF_INCLUDE_IGNORED |
		GIT_DIFF_INCLUDE_TYPECHANGE |
		GIT_DIFF_INCLUDE_TYPECHANGE_TREES |
		GIT_DIFF_SKIP_BINARY_CHECK |
		GIT_DIFF_INCLUDE_CASECHANGE;
	if (data.opts.checkout_strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH)
		diff_opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
	if (data.opts.paths.count > 0)
		diff_opts.pathspec = data.opts.paths;

	/* set up iterators */

	workdir_opts.flags = git_iterator_ignore_case(target) ?
		GIT_ITERATOR_IGNORE_CASE : GIT_ITERATOR_DONT_IGNORE_CASE;
	workdir_opts.flags |= GIT_ITERATOR_DONT_AUTOEXPAND;
	workdir_opts.start = data.pfx;
	workdir_opts.end = data.pfx;

	if ((error = git_iterator_reset_range(target, data.pfx, data.pfx)) < 0 ||
		(error = git_iterator_for_workdir_ext(
			&workdir, data.repo, data.opts.target_directory, index, NULL,
			&workdir_opts)) < 0)
		goto cleanup;

	baseline_opts.flags = git_iterator_ignore_case(target) ?
		GIT_ITERATOR_IGNORE_CASE : GIT_ITERATOR_DONT_IGNORE_CASE;
	baseline_opts.start = data.pfx;
	baseline_opts.end = data.pfx;
	if (opts && (opts->checkout_strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH)) {
		baseline_opts.pathlist.count = opts->paths.count;
		baseline_opts.pathlist.strings = opts->paths.strings;
	}

	if (data.opts.baseline_index) {
		if ((error = git_iterator_for_index(
				&baseline, git_index_owner(data.opts.baseline_index),
				data.opts.baseline_index, &baseline_opts)) < 0)
			goto cleanup;
	} else {
		if ((error = git_iterator_for_tree(
				&baseline, data.opts.baseline, &baseline_opts)) < 0)
			goto cleanup;
	}

	/* Should not have case insensitivity mismatch */
	assert(git_iterator_ignore_case(workdir) == git_iterator_ignore_case(baseline));

	/* Generate baseline-to-target diff which will include an entry for
	 * every possible update that might need to be made.
	 */
	if ((error = git_diff__from_iterators(
			&data.diff, data.repo, baseline, target, &diff_opts)) < 0)
		goto cleanup;

	/* Loop through diff (and working directory iterator) building a list of
	 * actions to be taken, plus look for conflicts and send notifications,
	 * then loop through conflicts.
	 */
	if ((error = checkout_get_actions(&actions, &counts, &data, workdir)) != 0)
		goto cleanup;

	data.total_steps = counts[CHECKOUT_ACTION__REMOVE] +
		counts[CHECKOUT_ACTION__REMOVE_CONFLICT] +
		counts[CHECKOUT_ACTION__UPDATE_BLOB] +
		counts[CHECKOUT_ACTION__UPDATE_SUBMODULE] +
		counts[CHECKOUT_ACTION__UPDATE_CONFLICT];

	report_progress(&data, NULL); /* establish 0 baseline */

	/* To deal with some order dependencies, perform remaining checkout
	 * in three passes: removes, then update blobs, then update submodules.
	 */
	if (counts[CHECKOUT_ACTION__REMOVE] > 0 &&
		(error = checkout_remove_the_old(actions, &data)) < 0)
		goto cleanup;

	if (counts[CHECKOUT_ACTION__REMOVE_CONFLICT] > 0 &&
		(error = checkout_remove_conflicts(&data)) < 0)
		goto cleanup;

	if (counts[CHECKOUT_ACTION__UPDATE_BLOB] > 0 &&
		(error = checkout_create_the_new(actions, &data)) < 0)
		goto cleanup;

	if (counts[CHECKOUT_ACTION__UPDATE_SUBMODULE] > 0 &&
		(error = checkout_create_submodules(actions, &data)) < 0)
		goto cleanup;

	if (counts[CHECKOUT_ACTION__UPDATE_CONFLICT] > 0 &&
		(error = checkout_create_conflicts(&data)) < 0)
		goto cleanup;

	if (data.index != git_iterator_index(target) &&
		(error = checkout_extensions_update_index(&data)) < 0)
		goto cleanup;

	assert(data.completed_steps == data.total_steps);

	if (data.opts.perfdata_cb) {
		git_checkout_perfdata perfdata;
		perfdata.mkdir_calls = (size_t)git_atomic_get(&data.perfdata.mkdir_calls);
		perfdata.stat_calls = (size_t)git_atomic_get(&data.perfdata.stat_calls);
		perfdata.chmod_calls = (size_t)git_atomic_get(&data.perfdata.chmod_calls);
		data.opts.perfdata_cb(&perfdata, data.opts.perfdata_payload);
	}

cleanup:
	if (!error && data.index != NULL &&
		(data.strategy & CHECKOUT_INDEX_DONT_WRITE_MASK) == 0)
		error = git_index_write(data.index);

	git_diff_free(data.diff);
	git_iterator_free(workdir);
	git_iterator_free(baseline);
	git__free(actions);
	git__free(counts);
	checkout_data_clear(&data);

	return error;
}

int git_checkout_index(
	git_repository *repo,
	git_index *index,
	const git_checkout_options *opts)
{
	int error, owned = 0;
	git_iterator *index_i;

	if (!index && !repo) {
		git_error_set(GIT_ERROR_CHECKOUT,
			"must provide either repository or index to checkout");
		return -1;
	}

	if (index && repo &&
		git_index_owner(index) &&
		git_index_owner(index) != repo) {
		git_error_set(GIT_ERROR_CHECKOUT,
			"index to checkout does not match repository");
		return -1;
	} else if(index && repo && !git_index_owner(index)) {
		GIT_REFCOUNT_OWN(index, repo);
		owned = 1;
	}

	if (!repo)
		repo = git_index_owner(index);

	if (!index && (error = git_repository_index__weakptr(&index, repo)) < 0)
		return error;
	GIT_REFCOUNT_INC(index);

	if (!(error = git_iterator_for_index(&index_i, repo, index, NULL)))
		error = git_checkout_iterator(index_i, index, opts);

	if (owned)
		GIT_REFCOUNT_OWN(index, NULL);

	git_iterator_free(index_i);
	git_index_free(index);

	return error;
}

int git_checkout_tree(
	git_repository *repo,
	const git_object *treeish,
	const git_checkout_options *opts)
{
	int error;
	git_index *index;
	git_tree *tree = NULL;
	git_iterator *tree_i = NULL;
	git_iterator_options iter_opts = GIT_ITERATOR_OPTIONS_INIT;

	if (!treeish && !repo) {
		git_error_set(GIT_ERROR_CHECKOUT,
			"must provide either repository or tree to checkout");
		return -1;
	}
	if (treeish && repo && git_object_owner(treeish) != repo) {
		git_error_set(GIT_ERROR_CHECKOUT,
			"object to checkout does not match repository");
		return -1;
	}

	if (!repo)
		repo = git_object_owner(treeish);

	if (treeish) {
		if (git_object_peel((git_object **)&tree, treeish, GIT_OBJECT_TREE) < 0) {
			git_error_set(
				GIT_ERROR_CHECKOUT, "provided object cannot be peeled to a tree");
			return -1;
		}
	}
	else {
		if ((error = checkout_lookup_head_tree(&tree, repo)) < 0) {
			if (error != GIT_EUNBORNBRANCH)
				git_error_set(
					GIT_ERROR_CHECKOUT,
					"HEAD could not be peeled to a tree and no treeish given");
			return error;
		}
	}

	if ((error = git_repository_index(&index, repo)) < 0)
		return error;

	if (opts && (opts->checkout_strategy & GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH)) {
		iter_opts.pathlist.count = opts->paths.count;
		iter_opts.pathlist.strings = opts->paths.strings;
	}

	if (!(error = git_iterator_for_tree(&tree_i, tree, &iter_opts)))
		error = git_checkout_iterator(tree_i, index, opts);

	git_iterator_free(tree_i);
	git_index_free(index);
	git_tree_free(tree);

	return error;
}

int git_checkout_head(
	git_repository *repo,
	const git_checkout_options *opts)
{
	assert(repo);
	return git_checkout_tree(repo, NULL, opts);
}

int git_checkout_options_init(git_checkout_options *opts, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_checkout_options, GIT_CHECKOUT_OPTIONS_INIT);
	return 0;
}

int git_checkout_init_options(git_checkout_options *opts, unsigned int version)
{
	return git_checkout_options_init(opts, version);
}
