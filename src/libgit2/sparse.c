/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "sparse.h"
#include "attrcache.h"
#include "git2/sparse.h"
#include "config.h"
#include "filebuf.h"
#include "index.h"
#include "ignore.h"
#include "status.h"

#define HAS_FLAG(entity, flag) (((entity->flags & flag) != 0))

static bool pattern_is_cone(const git_attr_fnmatch *match) {
	size_t i;

	if (match->length == 0) {
		return false;
	}

	if (match->length == 1 && match->pattern[0] == '*') {
		// NOTE: "/*" and "!/*/" both parse to "*", either to:
		//   positive, wildcard, non-directory, or
		//   negative, wildcard, directory.
		bool is_dir = HAS_FLAG(match, GIT_ATTR_FNMATCH_DIRECTORY);
		bool is_negative =  HAS_FLAG(match, GIT_ATTR_FNMATCH_NEGATIVE);
		return is_dir == is_negative;
	}

	if (!HAS_FLAG(match, GIT_ATTR_FNMATCH_DIRECTORY)) {
		return false;
	};

	if (HAS_FLAG(match, GIT_ATTR_FNMATCH_HASWILD)) {
		for (i = 0; i < match->length - 1; i++) {
			if (match->pattern[i] == '*') {
				// NOTE: The ony acceptable wildcard is at the end.
				return false;
			}
		}
	}

	return true;
}

static bool is_top_level_file(git_attr_path *path) {
	return !path->is_dir && (strchr(path->path, '/') == NULL);
}

static bool pattern_matches_path(git_attr_fnmatch *match, git_attr_path *path, size_t path_length) {
	// This is the number of characters that must match exactly.
	// Ex: "A/B"  -> 3,
	//     "A/B/C/*" -> 5
	size_t exact_match_length = match->length;

	size_t i;

	// If we have a pattern like "A/B/C/*", we have to match "A/B/C" exactly, then have a slash,
	// then have at least one more slash (or be a directory).
	bool expected_extra_nesting = false;

	if (HAS_FLAG(match, GIT_ATTR_FNMATCH_HASWILD)) {
		expected_extra_nesting = true;
		if (match->length <= 1) {
			// Top level wildcard always matches
			return true;
		}
		exact_match_length = match->length - 2; // Cut off the trailing "/*"
	}

	if (path_length < exact_match_length) {
		return false;
	}

	if (expected_extra_nesting) {
		if (path_length < exact_match_length + 2) {
			return false;
		}

		if (!path->is_dir) {
			bool found_slash = false;
			for (i = exact_match_length + 1; i < path_length; i++) {
				if (path->path[i] == '/') {
					found_slash = true;
					break;
				}
			}

			if (!found_slash) {
				return false;
			}
		}
	}

	if (path_length > exact_match_length && path->path[exact_match_length] != '/') {
		return false;
	}

	for (i = 0; i < exact_match_length; i++) {
		if (path->path[i] != match->pattern[i]) {
			return false;
		}
	}

	return true;
}

static int sparse_lookup_in_rules(
        int *checkout,
        git_attr_file *file,
        git_attr_path *path)
{
	size_t j;
	git_attr_fnmatch *match;
	
	int path_length = strlen(path->path);
	if (is_top_level_file(path) ) {
		*checkout = GIT_SPARSE_CHECKOUT;
		return 0;
	}


	git_vector_rforeach(&file->rules, j, match) {
		if (pattern_matches_path(match, path, path_length)) {
			*checkout = HAS_FLAG(match, GIT_ATTR_FNMATCH_NEGATIVE)
				? GIT_SPARSE_NOCHECKOUT
				: GIT_SPARSE_CHECKOUT;
			return 0;
		}
	}

	*checkout = GIT_SPARSE_NOCHECKOUT;
	return 0;
}

static int parse_sparse_file(
        git_repository *repo,
        git_attr_file *attrs,
        const char *data,
        bool allow_macros)
{
	int error = parse_ignore_file(
			repo,
			attrs,
			data,
			NULL,
			allow_macros);

	if (error != 0 ) {
		return error;
	}

	size_t j;
	git_attr_fnmatch *match;
	git_vector_rforeach(&attrs->rules, j, match) {
		if (!pattern_is_cone(match)) {
			git_error_set(GIT_ERROR_INVALID, "sparse-checkout patterns must be in cone format");
			return -1;
		}
	}

	// Check for positive patterns that have their parent excluded.
	// Ex: "A/B/C" without "A/B" is considered invalid.
	git_vector_foreach(&attrs->rules, j, match) {
		if (HAS_FLAG(match, GIT_ATTR_FNMATCH_NEGATIVE)) {
			continue;
		}

		char *parent_end_slash = strrchr(match->pattern, '/');
		if (parent_end_slash == NULL) {
			continue;
		}

		size_t parent_length = parent_end_slash - match->pattern;
		char* parent_pathname = git__strndup(match->pattern, parent_length);

		git_attr_path parent_path;
		if ((error = git_attr_path__init(&parent_path, parent_pathname, git_repository_workdir(repo), GIT_DIR_FLAG_TRUE))) {
			git__free(parent_pathname);
			return error;
		}

		bool matched = false;
		size_t k;
		git_attr_fnmatch *parent_match;
		git_vector_rforeach(&attrs->rules, k, parent_match) {
			if (pattern_matches_path(parent_match, &parent_path, parent_length)) {
				matched = !HAS_FLAG(parent_match, GIT_ATTR_FNMATCH_NEGATIVE);
				break;
			}
		}

		git__free(parent_pathname);
		git_attr_path__free(&parent_path);

		if (!matched) {
			git_error_set(GIT_ERROR_INVALID, "sparse-checkout requires that deeply-nested includes have their parents included as well.");
			return -1;
		}
	}

	return 0;
}

int git_sparse_attr_file__init_(
		int *file_exists,
        git_repository *repo,
        git_sparse *sparse)
{
    int error = 0;
	git_str infopath = GIT_STR_INIT;
    const char *filename = GIT_SPARSE_CHECKOUT_FILE;
	git_attr_file_source source = { GIT_ATTR_FILE_SOURCE_FILE, git_str_cstr(&infopath), filename, NULL };
	git_str filepath = GIT_STR_INIT;

	if ((error = git_str_joinpath(&infopath, repo->gitdir, "info")) < 0) {
		if (error != GIT_ENOTFOUND)
			goto done;
		error = 0;
	}

	source.base = git_str_cstr(&infopath);
	source.filename = filename;

	git_str_joinpath(&filepath, infopath.ptr, filename);

    /* Don't overwrite any existing sparse-checkout file */
	*file_exists = git_fs_path_exists(git_str_cstr(&filepath));
    if (!*file_exists) {
        if ((error = git_futils_creat_withpath(git_str_cstr(&filepath), 0777, 0666)) < 0)
			goto done;
    }

		// printf("init attr file\n");
    error = git_attr_cache__get(&sparse->sparse, repo, NULL, &source, parse_sparse_file, false);

done:
	git_str_dispose(&infopath);
    return error;
}

int git_sparse_attr_file__init(
		git_repository *repo,
		git_sparse *sparse)
{
	int b = false;
	int error = git_sparse_attr_file__init_(&b, repo, sparse);
	return error;
}

int git_sparse__init_(
		int *file_exists,
        git_repository *repo,
        git_sparse *sparse)
{
	int error = 0;
	
	assert(repo && sparse);
	
	memset(sparse, 0, sizeof(*sparse));
	sparse->repo = repo;
	
	/* Read the ignore_case flag */
	if ((error = git_repository__configmap_lookup(
			&sparse->ignore_case, repo, GIT_CONFIGMAP_IGNORECASE)) < 0)
		goto cleanup;
	
	if ((error = git_attr_cache__init(repo)) < 0)
		goto cleanup;

    if ((error = git_sparse_attr_file__init_(file_exists, repo, sparse)) < 0) {
        if (error != GIT_ENOTFOUND)
            goto cleanup;
        error = 0;
    }
	
cleanup:
	if (error < 0)
		git_sparse__free(sparse);
	
	return error;
}

int git_sparse__init(
		git_repository *repo,
		git_sparse *sparse)
{
	int b = false;
	int error = git_sparse__init_(&b, repo, sparse);
	return error;
}

int git_sparse__lookup(
        git_sparse_status* status,
        git_sparse *sparse,
        const char* pathname,
        git_dir_flag dir_flag)
{
	git_attr_path path;
	const char *workdir;
	int error;

	GIT_ASSERT_ARG(status);
	GIT_ASSERT_ARG(sparse);
	GIT_ASSERT_ARG(pathname);

	*status = GIT_SPARSE_CHECKOUT;

	workdir = git_repository_workdir(sparse->repo);
	if ((error = git_attr_path__init(&path, pathname, workdir, dir_flag)))
		return error;

	error = sparse_lookup_in_rules(status, sparse->sparse, &path);

	git_attr_path__free(&path);

	return error;
}

void git_sparse__free(git_sparse *sparse)
{
	git_attr_file__free(sparse->sparse);
}

int git_sparse_checkout__list(
        git_vector *patterns,
        git_sparse *sparse)
{
    int error = 0;
    git_str data = GIT_STR_INIT;
    char *scan, *buf;

    GIT_ASSERT_ARG(patterns);
    GIT_ASSERT_ARG(sparse);

    if ((error = git_futils_readbuffer(&data, sparse->sparse->entry->fullpath)) < 0)
        return error;

    scan = (char *)git_str_cstr(&data);
    while (!error && *scan) {

		buf = git__strtok(&scan, "\r\n");
		if (buf)
			error = git_vector_insert(patterns, buf);
    }

    return error;
}

int git_sparse_checkout_list(git_strarray *patterns, git_repository *repo) {

    int error = 0;
    git_sparse sparse;
    git_vector patternlist;

    GIT_ASSERT_ARG(patterns);
    GIT_ASSERT_ARG(repo);

    if ((error = git_sparse__init(repo, &sparse)))
        goto done;

    if ((error = git_vector_init(&patternlist, 0, NULL)) < 0)
        goto done;

    if ((error = git_sparse_checkout__list(&patternlist, &sparse)))
        goto done;

    patterns->strings = (char **) git_vector_detach(&patterns->count, NULL, &patternlist);

done:
    git_sparse__free(&sparse);
    git_vector_free(&patternlist);


    return error;
}

int git_sparse_checkout__apply_sparse_to_index(git_repository *repo, git_sparse *sparse) {

	int error = 0;
	git_index *index;
	size_t i = 0;
	git_index_entry *entry;

printf("git_sparse_checkout__apply_sparse_to_index\n");

	if ((error = git_repository_index(&index, repo)) < 0)
		goto done;

	git_vector_foreach(&index->entries, i, entry)
	{
		int is_submodule = false;
		int has_conflict = false;
		unsigned int status_flags;
		int checkout = GIT_SPARSE_CHECKOUT;
		if ((error = git_sparse__lookup(&checkout, sparse, entry->path, GIT_DIR_FLAG_FALSE)) < 0)
			goto done;

		uint16_t new_extended_flags = entry->flags_extended;
		if (checkout == GIT_SPARSE_NOCHECKOUT) {
			printf("Excluding:             %s", entry->path);
			new_extended_flags |= GIT_INDEX_ENTRY_SKIP_WORKTREE;
		} else {
			printf("Including:      %s", entry->path);
			new_extended_flags &= ~GIT_INDEX_ENTRY_SKIP_WORKTREE;
		}
		if (new_extended_flags != entry->flags_extended) {
			printf("*\n");
			entry->flags_extended = new_extended_flags;
		} else {
			printf("\n");
		}
	}

	error = git_index_write(index);

	printf("Wrote index back...\n");
done:
	git_index_free(index);

	return error;
}

int git_sparse_checkout__reapply(git_repository *repo, git_sparse *sparse)
{
	int error = 0;
	git_index *index;
	size_t i = 0;
	git_index_entry *entry;
	git_vector paths_to_checkout;
	git_checkout_options copts;
	git_status_list *status = NULL;
	const git_status_entry *status_entry;
	git_status_options status_opts = GIT_STATUS_OPTIONS_INIT;

	const char *workdir = repo->workdir;


	if ((error = git_vector_init(&paths_to_checkout, 0, NULL)) < 0)
		goto done;

	if ((error = git_sparse_checkout__apply_sparse_to_index(repo, sparse)) < 0)
		goto done;

	// status_opts.flags |= GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
	if ((error = git_status_list_new(&status, repo, &status_opts)) < 0) {
		return error;
	}

	printf("Status: %d\n", git_status_list_entrycount(status));

	git_vector_foreach(&status->paired, i, status_entry) {
			const char *path = status_entry->head_to_index ?
				status_entry->head_to_index->old_file.path :
				status_entry->index_to_workdir->old_file.path;
			printf("\n\nPath: %s\n", path);
			if (status_entry->head_to_index) {
				printf("Head to index:\n");
				switch (status_entry->head_to_index->status) {
					case GIT_STATUS_CURRENT:
						printf("  Current\n");
						break;
					case GIT_STATUS_INDEX_NEW:
						printf("  Index New\n");
						break;
					case GIT_STATUS_INDEX_MODIFIED:
						printf("  Index Modified\n");
						break;
					case GIT_STATUS_INDEX_DELETED:
						printf("  Index Deleted\n");
						break;
					case GIT_STATUS_INDEX_RENAMED:
						printf("  Index Renamed\n");
						break;
					case GIT_STATUS_INDEX_TYPECHANGE:
						printf("  Index Typechange\n");
						break;
					case GIT_STATUS_WT_NEW:
						printf("  WT New\n");
						break;
					case GIT_STATUS_WT_MODIFIED:
						printf("  WT Modified\n");
						break;
					case GIT_STATUS_WT_DELETED:
						printf("  WT Deleted\n");
						break;
					case GIT_STATUS_WT_TYPECHANGE:
						printf("  WT Typechange\n");
						break;
					case GIT_STATUS_WT_RENAMED:
						printf("  WT Renamed\n");
						break;
					case GIT_STATUS_CONFLICTED:
						printf("  Conflict\n");
						break;
					default:
						printf("  Unknown\n");
						break;
				}
			} else {
				printf("Head to index is null\n");
			}


		// Error out if conflicts
		// Delete file if it shouldn't exist
		// Add file to paths_to_checkout if it should exist
		// Otherwise do nothing
	}

	if ((error = git_repository_index(&index, repo)) < 0)
		goto done;

	if ((error = git_checkout_options_init(&copts, GIT_CHECKOUT_OPTIONS_VERSION)) < 0)
		goto done;

	copts.paths.strings = (char**) git_vector_detach(&copts.paths.count, NULL, &paths_to_checkout);

	copts.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_RECREATE_MISSING;
	if ((error = git_checkout_index(repo, index, &copts)) < 0)
		goto done;

	error = git_index_write(index);

done:
	git_index_free(index);
	git_vector_free(&paths_to_checkout);
	git_status_list_free(status);

	return error;
}

int git_sparse_checkout__set(
		git_vector *patterns,
		git_repository *repo,
		git_sparse *sparse)
{
	int error;
	size_t i;
	const char *pattern;
	git_str content = GIT_STR_INIT;

	git_vector_foreach(patterns, i, pattern) {
		git_str_join(&content, '\n', git_str_cstr(&content), pattern);
	}

	if ((error = git_futils_truncate(sparse->sparse->entry->fullpath, 0777)) < 0)
		goto done;

	if ((error = git_futils_writebuffer(&content, sparse->sparse->entry->fullpath, O_WRONLY, 0644)) < 0)
		goto done;

	/* Refresh the rules in the sparse info */
	git_vector_clear(&sparse->sparse->rules);
	if ((error = git_sparse_attr_file__init(repo, sparse)) < 0)
		goto done;

done:
	git_str_dispose(&content);

	return error;
}

int git_sparse_checkout__enable(git_repository *repo, git_sparse_checkout_init_options *opts)
{
	int error = 0;
	git_config *cfg;

	/* Can be used once cone mode is supported */
	GIT_UNUSED(opts);

	if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
		return error;

	if ((error = git_config_set_bool(cfg, "core.sparseCheckout", true)) < 0)
		goto done;

done:
	git_config_free(cfg);
	return error;
}

int git_sparse_checkout_init(git_repository *repo, git_sparse_checkout_init_options *opts)
{
    int error = 0;
    git_sparse sparse;
	int file_exists = false;
	git_vector default_patterns = GIT_VECTOR_INIT;

    GIT_ASSERT_ARG(repo);
    GIT_ASSERT_ARG(opts);

    if ((error = git_sparse_checkout__enable(repo, opts)) < 0)
		return error;

    if ((error = git_sparse__init_(&file_exists, repo, &sparse)) < 0)
        goto cleanup;

	if (!file_exists) {

		/* Default patterns that match every file in the root directory and no other directories */
		git_vector_insert(&default_patterns, "/*");
		git_vector_insert(&default_patterns, "!/*/");

		if ((error = git_sparse_checkout__set(&default_patterns, repo, &sparse)) < 0)
			goto cleanup;
	}

	if ((error = git_sparse_checkout__reapply(repo, &sparse)) < 0)
		goto cleanup;

cleanup:
    git_sparse__free(&sparse);
    return error;
}

int git_sparse_checkout_set(
        git_repository *repo,
        git_strarray *patterns)
{
    int error;
    git_config *cfg;
    git_sparse sparse;

    size_t i = 0;
    git_vector patternlist;

	git_sparse_checkout_init_options opts = GIT_SPARSE_CHECKOUT_INIT_OPTIONS_INIT;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(patterns);

    if ((error = git_repository_config(&cfg, repo)) < 0)
        goto done;

	if ((error = git_sparse_checkout__enable(repo, &opts) < 0))
		goto done;

    if ((error = git_sparse__init(repo, &sparse)) < 0)
        goto done;

    if ((error = git_vector_init(&patternlist, 0, NULL)) < 0)
        goto done;

    for (i = 0; i < patterns->count; i++) {
        git_vector_insert(&patternlist, patterns->strings[i]);
    }

    if ((error = git_sparse_checkout__set(&patternlist, repo, &sparse)) < 0)
        goto done;

	if ((error = git_sparse_checkout__reapply(repo, &sparse)) < 0)
		goto done;

done:
    git_config_free(cfg);
    git_sparse__free(&sparse);
    git_vector_free(&patternlist);

    return error;
}

int git_sparse_checkout__restore_wd(git_repository *repo)
{
	int error = 0;
	git_sparse sparse;
	git_vector old_patterns, patterns = GIT_VECTOR_INIT;

	if ((error = git_sparse__init(repo, &sparse)) < 0)
		return error;

	/* Store the old patterns so that we can put them back later */
	if ((error = git_vector_init(&old_patterns, 0 ,NULL)) < 0)
		goto done;

	if ((error = git_sparse_checkout__list(&old_patterns, &sparse)) < 0)
		goto done;

	/* Write down a pattern that will include everything */
	if ((error = git_vector_init(&patterns, 0 ,NULL)) < 0)
		goto done;

	if ((error = git_vector_insert(&patterns, "/*")) < 0)
		goto done;

	if ((error = git_sparse_checkout__set(&patterns, repo, &sparse)) < 0)
		goto done;

	/* Re-apply sparsity with our catch-all pattern */
	if ((error = git_sparse_checkout__reapply(repo, &sparse)) < 0)
		goto done;

	/* Restore the sparse-checkout patterns to how they were before */
	if ((error = git_sparse_checkout__set(&old_patterns, repo, &sparse)) < 0)
		goto done;

done:
	git_sparse__free(&sparse);
	git_vector_free(&old_patterns);
	git_vector_free(&patterns);
	return error;
}

int git_sparse_checkout_disable(git_repository *repo)
{
    int error = 0;
    git_config *cfg;

    GIT_ASSERT_ARG(repo);

    if ((error = git_repository_config(&cfg, repo)) < 0)
        return error;

    if ((error = git_config_set_bool(cfg, "core.sparseCheckout", false)) < 0)
        goto done;

	if ((error = git_sparse_checkout__restore_wd(repo)) < 0)
		goto done;

done:
    git_config_free(cfg);

    return error;
}

int git_sparse_checkout__add(
		git_repository *repo,
        git_vector *patterns,
        git_sparse *sparse)
{
    int error = 0;
    size_t i = 0;
    git_vector existing_patterns;
    git_vector new_patterns;
    char* pattern;

    if ((error = git_vector_init(&existing_patterns, 0, NULL)) < 0)
        goto done;

    if ((error = git_vector_init(&new_patterns, 0, NULL)) < 0)
        goto done;

    if ((error = git_sparse_checkout__list(&existing_patterns, sparse)) < 0)
        goto done;

    git_vector_foreach(&existing_patterns, i, pattern) {
        git_vector_insert(&new_patterns, pattern);
    }

    git_vector_foreach(patterns, i, pattern) {
        git_vector_insert(&new_patterns, pattern);
    }

    if ((error = git_sparse_checkout__set(&new_patterns, repo, sparse)) < 0)
        goto done;

done:
    git_vector_free(&existing_patterns);
    git_vector_free(&new_patterns);

    return error;
}

int git_sparse_checkout_add(
        git_repository *repo,
        git_strarray *patterns)
{
    int error;
    int is_enabled = false;
    git_config *cfg;
    git_sparse sparse;
    git_vector patternlist;
	size_t i;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(patterns);

    if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
        return error;

	error = git_config_get_bool(&is_enabled, cfg, "core.sparseCheckout");
    if (error < 0 && error != GIT_ENOTFOUND)
        goto done;

    if (!is_enabled)
	{
		git_error_set(GIT_ERROR_INVALID, "sparse checkout is not enabled");
		git_config_free(cfg);
		return -1;
	}

    if ((error = git_sparse__init(repo, &sparse)) < 0)
        goto done;

    if ((error = git_vector_init(&patternlist, 0, NULL)))
        goto done;

	for (i = 0; i < patterns->count; i++) {
		if ((error = git_vector_insert(&patternlist, patterns->strings[i])) < 0)
			return error;
	}

    if ((error = git_sparse_checkout__add(repo, &patternlist,  &sparse)) < 0)
        goto done;

	if ((error = git_sparse_checkout__reapply(repo, &sparse)) < 0)
		goto done;

done:
    git_config_free(cfg);
	git_sparse__free(&sparse);
	git_vector_free(&patternlist);

    return error;
}

int git_sparse_checkout_reapply(git_repository *repo) {
	int error;
	git_sparse sparse;

	GIT_ASSERT_ARG(repo);

	if ((error = git_sparse__init(repo, &sparse)) < 0)
		return error;

	if ((error = git_sparse_checkout__reapply(repo, &sparse)) < 0)
		goto done;

done:
	git_sparse__free(&sparse);
	return error;
}

int git_sparse_check_path(
        int *checkout,
        git_repository *repo,
        const char *pathname)
{
    int error;
    int sparse_checkout_enabled = false;
    git_sparse sparse;
    git_dir_flag dir_flag = GIT_DIR_FLAG_FALSE;

    GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(checkout);
	GIT_ASSERT_ARG(pathname);

    *checkout = GIT_SPARSE_CHECKOUT;

    if ((error = git_repository__configmap_lookup(&sparse_checkout_enabled, repo, GIT_CONFIGMAP_SPARSECHECKOUT)) < 0 ||
        sparse_checkout_enabled == false)
        return 0;

    if ((error = git_sparse__init(repo, &sparse)) < 0)
        goto cleanup;

    if (!git__suffixcmp(pathname, "/"))
        dir_flag = GIT_DIR_FLAG_TRUE;
    else if (git_repository_is_bare(repo))
        dir_flag = GIT_DIR_FLAG_FALSE;

    error = git_sparse__lookup(checkout, &sparse, pathname, dir_flag);

    cleanup:
    git_sparse__free(&sparse);
    return error;
}
