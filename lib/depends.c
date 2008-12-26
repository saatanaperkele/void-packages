/*-
 * Copyright (c) 2008 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include <xbps_api.h>

struct pkg_dependency {
	LIST_ENTRY(pkg_dependency) deps;
	prop_dictionary_t repo;
	const char *namever;
	char *name;
};

static LIST_HEAD(, pkg_dependency) pkg_deps_list =
    LIST_HEAD_INITIALIZER(pkg_deps_list);

int
xbps_check_is_installed_pkg(const char *pkg)
{
	prop_dictionary_t dict, pkgdict;
	prop_object_t obj;
	const char *reqver, *instver;
	char plist[PATH_MAX], *pkgname;
	int rv = 0;

	assert(pkg != NULL);

	if (!xbps_append_full_path(plist, NULL, XBPS_REGPKGDB))
		return EINVAL;

	pkgname = xbps_get_pkg_name(pkg);
	reqver = xbps_get_pkg_version(pkg);

	/* Get package dictionary from plist */
	dict = prop_dictionary_internalize_from_file(plist);
	if (dict == NULL) {
		free(pkgname);
		return 1; /* not installed */
	}

	pkgdict = xbps_find_pkg_in_dict(dict, pkgname);
	if (pkgdict == NULL) {
		prop_object_release(dict);
		free(pkgname);
		return 1; /* not installed */
	}

	/* Get version from installed package */
	obj = prop_dictionary_get(pkgdict, "version");
	assert(obj != NULL);
	assert(prop_object_type(obj) == PROP_TYPE_STRING);
	instver = prop_string_cstring_nocopy(obj);
	assert(instver != NULL);

	/* Compare installed and required version. */
	rv = xbps_cmpver_versions(instver, reqver) > 0 ? 1 : 0;

	free(pkgname);
	prop_object_release(dict);

	return rv;
}

void
xbps_clean_pkg_depslist(void)
{
	struct pkg_dependency *dep;

	LIST_FOREACH(dep, &pkg_deps_list, deps) {
		LIST_REMOVE(dep, deps);
		free(dep->name);
		prop_object_release(dep->repo);
	}
}

void
xbps_add_pkg_dependency(const char *pkg, prop_dictionary_t repo)
{
	struct pkg_dependency *dep;
	size_t len = 0;
	char *pkgname;

	assert(repo != NULL);
	assert(pkg != NULL);

	pkgname = xbps_get_pkg_name(pkg);

	LIST_FOREACH(dep, &pkg_deps_list, deps) {
		if (strcmp(dep->name, pkgname) == 0) {
			free(pkgname);
			return;
		}
	}

	dep = NULL;
	dep = malloc(sizeof(*dep));
	assert(dep != NULL);

	len = strlen(pkgname) + 1;
	dep->name = malloc(len);
	assert(dep != NULL);

	memcpy(dep->name, pkgname, len - 1);
	dep->name[len - 1] = '\0';
	free(pkgname);
	dep->repo = prop_dictionary_copy(repo);
	dep->namever = pkg;

	LIST_INSERT_HEAD(&pkg_deps_list, dep, deps);
}

static int
find_deps_in_pkg(prop_dictionary_t repo, prop_dictionary_t pkg)
{
	prop_dictionary_t pkgdict;
	prop_array_t array;
	prop_object_t obj;
	prop_object_iterator_t iter = NULL;
	const char *reqpkg;
	char *pkgname;

	array = prop_dictionary_get(pkg, "run_depends");
	if (array == NULL || prop_array_count(array) == 0)
		return 0;

	iter = prop_array_iterator(array);
	if (iter == NULL)
		return -1;

	/*
	 * Iterate over the list of required run dependencies for
	 * a package.
	 */
	while ((obj = prop_object_iterator_next(iter))) {
		/*
		 * If required package is not in repo, just pass to the
		 * next one.
		 */
		reqpkg = prop_string_cstring_nocopy(obj);
		pkgname = xbps_get_pkg_name(reqpkg);
		pkgdict = xbps_find_pkg_in_dict(repo, pkgname);
		if (pkgdict == NULL) {
			free(pkgname);
			continue;
		}

		/*
		 * Package is on repo, add it into the list.
		 */
		xbps_add_pkg_dependency(reqpkg, repo);
		free(pkgname);

		/* Iterate on required pkg to find more deps */
		if (xbps_pkg_has_rundeps(pkgdict)) {
			/* more deps? */
			if (!find_deps_in_pkg(repo, pkgdict))
				continue;
		}
	}

	prop_object_iterator_release(iter);

	return 0;
}

int
xbps_install_pkg_deps(prop_dictionary_t pkg)
{
	prop_dictionary_t repolistd, repod, pkgd;
	prop_array_t array;
	prop_object_iterator_t iter;
	prop_object_t obj;
	struct pkg_dependency *dep;
	size_t required_deps = 0, deps_found = 0;
	const char *reqpkg, *version, *pkgname, *desc;
	char plist[PATH_MAX], *namestr;
	int rv = 0;
	bool dep_found = false;

	assert(pkg != NULL);

	if (!xbps_append_full_path(plist, NULL, XBPS_REPOLIST))
		return EINVAL;

	repolistd = prop_dictionary_internalize_from_file(plist);
	if (repolistd == NULL)
		return EINVAL;

	array = prop_dictionary_get(repolistd, "repository-list");
	assert(array != NULL);

	iter = prop_array_iterator(array);
	if (iter == NULL) {
		prop_object_release(repolistd);
		return ENOMEM;
	}

	/*
	 * Iterate over the repository list and find out if we have
	 * all required dependencies.
	 */
	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		memset(plist, 0, sizeof(&plist));
		if (!xbps_append_full_path(plist,
		    prop_string_cstring_nocopy(obj), XBPS_PKGINDEX)) {
			xbps_clean_pkg_depslist();
			prop_object_iterator_release(iter);
			rv = EINVAL;
			goto out;
		}

		repod = prop_dictionary_internalize_from_file(plist);
		if (repod == NULL) {
			prop_object_iterator_release(iter);
			rv = errno;
			goto out;
		}

		rv = find_deps_in_pkg(repod, pkg);
		if (rv == -1) {
			prop_object_release(repod);
			prop_object_iterator_release(iter);
			goto out;
		}

		prop_object_release(repod);
	}
	prop_object_iterator_release(iter);

	/*
	 * Find out if all required dependencies and binary packages
	 * from repositories are there.
	 */
	array = prop_dictionary_get(pkg, "run_depends");
	iter = prop_array_iterator(array);
	if (iter == NULL) {
		rv = ENOMEM;
		goto out;
	}

	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		dep_found = false;
		required_deps++;
		reqpkg = prop_string_cstring_nocopy(obj);
		namestr = xbps_get_pkg_name(reqpkg);
		version = xbps_get_pkg_version(reqpkg);

		LIST_FOREACH(dep, &pkg_deps_list, deps) {
			if (strcmp(dep->name, namestr) == 0) {
				deps_found++;
				dep_found = true;
				break;
			}
		}
		if (dep_found == false) {
			printf("Cannot find %s >= %s in repository list.\n",
			    namestr, version);
			(void)fflush(stdout);
		}
		free(namestr);
	}
	prop_object_iterator_release(iter);

	if (required_deps != deps_found) {
		rv = XBPS_PKG_ENOTINREPO;
		goto out;
	}

	/*
	 * Iterate over the list of dependencies and install them.
	 */
	LIST_FOREACH(dep, &pkg_deps_list, deps) {
		pkgd = xbps_find_pkg_in_dict(dep->repo, dep->name);
		if (pkgd == NULL) {
			rv = EINVAL;
			break;
		}

		rv = xbps_check_is_installed_pkg(dep->namever);
		if (rv == -1) {
			rv = EINVAL;
			break;
		} else if (rv == 0) {
			/* Dependency is already installed. */
			continue;
		}

		prop_dictionary_get_cstring_nocopy(pkgd, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(pkgd, "version", &version);
		prop_dictionary_get_cstring_nocopy(pkgd, "short_desc", &desc);

		printf("Required package: %s >= %s\n", pkgname, version);
		(void)fflush(stdout);

		rv = xbps_unpack_binary_pkg(dep->repo, pkgd,
		    xbps_unpack_archive_cb);
		if (rv != 0)
			break;

		rv = xbps_register_pkg(pkgname, version, desc);
		if (rv != 0)
			break;
	}

out:
	prop_object_release(repolistd);
	xbps_clean_pkg_depslist();

	return rv;
}
