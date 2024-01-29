#include "clar_libgit2.h"
#include "futils.h"
#include "git2/attr.h"
#include "sparse.h"
#include "status/status_helpers.h"

static git_repository *g_repo = NULL;

void test_sparse_paths__initialize(void)
{
}

void test_sparse_paths__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

static void assert_checkout_(
	bool expected, const char *filepath,
	const char *file, const char *func, int line)
{
	int checkout = 0;
	cl_git_expect(
	 git_sparse_check_path(&checkout, g_repo, filepath), 0, file, func, line);
	clar__assert(
	 (expected != 0) == (checkout != 0),
	 file, func, line, expected ? "should be included"  :"should be excluded", filepath, 0);
}

#define assert_checkout(expected, filepath) \
assert_checkout_(expected, filepath, __FILE__, __func__, __LINE__)
#define assert_is_checkout(filepath) \
assert_checkout_(true, filepath, __FILE__, __func__, __LINE__)
#define refute_is_checkout(filepath) \
assert_checkout_(false, filepath, __FILE__, __func__, __LINE__)

void test_sparse_paths__check_path(void)
{
	git_sparse_checkout_init_options scopts = GIT_SPARSE_CHECKOUT_INIT_OPTIONS_INIT;
	g_repo = cl_git_sandbox_init("sparse");

	cl_git_pass(git_sparse_checkout_init(g_repo, &scopts));
	{
		char *pattern_strings[] = {
			 "/*",
			"!/*/",
			 "/A/",
			"!/A/*/",
			 "/A/B/",
			"!/A/B/*/",
			 "/A/B/C/",
			"!/A/B/C/*/",
			 "/A/B/D/",
		};
		git_strarray patterns = { pattern_strings, ARRAY_SIZE(pattern_strings) };
		cl_git_pass(git_sparse_checkout_add(g_repo, &patterns));
	}

	char *matches[] = {
		// Folder prefixes match
		"A/",
		"A/B/",
		"A/B/C/",
		"A/B/D/",
		"A/B/D/E/",
		"A/B/D/E/F/",
		// Direct children
		"A/_",
		"A/B/_",
		"A/B/C/_",
		"A/B/D/_",
		"A/B/D/E/_",
		"A/B/D/E/F/_",
	};

	char * non_matches[] = {
		"M/",
		"A/N/",
		"A/B/O/",
		"A/B/CP/",
		"A/B/C/P/",
		"A/B/C/P/Q/",
		"M/_",
		"A/N/_",
		"A/B/O/_",
		"A/B/CP/_",
		"A/B/C/P/_",
		"A/B/C/P/Q/_",
	};

	size_t j;
	for ( j = 0; j < ARRAY_SIZE(matches); j++) {
		assert_is_checkout(matches[j]);
	}

	for ( j = 0; j < ARRAY_SIZE(non_matches); j++) {
		refute_is_checkout(non_matches[j]);
	}
}

void test_sparse_paths__check_toplevel(void)
{
	git_sparse_checkout_init_options scopts = GIT_SPARSE_CHECKOUT_INIT_OPTIONS_INIT;
	g_repo = cl_git_sandbox_init("sparse");

	cl_git_pass(git_sparse_checkout_init(g_repo, &scopts));
	{
		char *pattern_strings[] = {};
		git_strarray patterns = { pattern_strings, ARRAY_SIZE(pattern_strings) };
		cl_git_pass(git_sparse_checkout_add(g_repo, &patterns));
	}

	char *matches[] = {
		"_", // Even with no include patterns, toplevel files are included.
	};

	char * non_matches[] = {
		"A/",
		"A/_",
	};

	size_t j;
	for ( j = 0; j < ARRAY_SIZE(matches); j++) {
		assert_is_checkout(matches[j]);
	}

	for ( j = 0; j < ARRAY_SIZE(non_matches); j++) {
		refute_is_checkout(non_matches[j]);
	}
}

void test_sparse_paths__validate_cone(void)
{
	size_t i;

	git_sparse_checkout_init_options scopts = GIT_SPARSE_CHECKOUT_INIT_OPTIONS_INIT;

	char *good_patterns[] = {
		"/*",
		"!/*/",
		"/A/",
		"!/A/B/C/*/",
		"/A/\n/A/B/C/", // To allow /A/B/C/, it needs to be included by a parent pattern.
	};

	char *bad_patterns[] = {
		"/*/",
		"!/*",
		"!/A/B/C/*",
		"/A/B/C/*",
		"/A/*/C/",
		"/A/B*/C/",
		"/A/B/C",
		"A/B/C",
		// Using extra paths here to prevent parse_ignore_file from stripping out an
		// "unneeded" negative pattern.
		"/A/\n/A/B/C/\n!/A/B/C",
	};

	char *missing_parent_patterns[] = {
		"/A/B/",
		"/A/B/C/",
		"/*\n!/A/B/*/\n/A/B/C/"
	};

	for (i = 0; i < ARRAY_SIZE(good_patterns); i++) {
		g_repo = cl_git_sandbox_init("sparse");
		cl_git_pass(git_sparse_checkout_init(g_repo, &scopts));
		git_strarray patterns = { &good_patterns[i], 1 };
		int error = git_sparse_checkout_set(g_repo, &patterns);
		clar__assert(error == 0, __FILE__, __func__, __LINE__, "Expected success on:", good_patterns[i], 0);
		cl_git_sandbox_cleanup();
	}

	for (i = 0; i < ARRAY_SIZE(bad_patterns); i++) {
		g_repo = cl_git_sandbox_init("sparse");
		cl_git_pass(git_sparse_checkout_init(g_repo, &scopts));
		git_error_clear();
		git_strarray patterns = { &bad_patterns[i], 1 };
		int error = git_sparse_checkout_set(g_repo, &patterns);
		clar__assert(error != 0, __FILE__, __func__, __LINE__, "Expected rejection on:", bad_patterns[i], 0);
		if (error != 0) {
			clar__assert(strstr(git_error_last()->message, "cone format") != NULL, __FILE__, __func__, __LINE__, "Expected error message to complain about syntax", bad_patterns[i], 0);
		}
		cl_git_sandbox_cleanup();
	}

	for (i = 0; i < ARRAY_SIZE(missing_parent_patterns); i++) {
		g_repo = cl_git_sandbox_init("sparse");
		cl_git_pass(git_sparse_checkout_init(g_repo, &scopts));
		git_error_clear();
		git_strarray patterns = { &missing_parent_patterns[i], 1 };
		int error = git_sparse_checkout_set(g_repo, &patterns);
		clar__assert(error != 0, __FILE__, __func__, __LINE__, "Expected parent rejection on:", missing_parent_patterns[i], 0);
		if (error != 0) {
			clar__assert(strstr(git_error_last()->message, "deeply-nested") != NULL, __FILE__, __func__, __LINE__, "Expected error message to complain about nesting", missing_parent_patterns[i], 0);
		}
		cl_git_sandbox_cleanup();
	}
}
