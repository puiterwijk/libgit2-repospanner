/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "repository.h"
#include "fileops.h"
#include "filebuf.h"
#include "iterator.h"
#include "sortedcache.h"
#include "signature.h"
#include "repospanner.h"

#include <git2/version.h>
#include <git2/tag.h>
#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>
#include <git2/sys/reflog.h>

#ifndef GIT_CURL
# error "GIT_CURL required for repoSpanner support"
#endif

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif


typedef struct repoSpanner_client {
	CURL *basehandle;
	// TODO: At some point move the Share object globally so cross-repo can also
	// use the same TLS cache?
	// Will need to see about whether sharing depends on the client cert used.
	CURLSH *share;

	git_buf baseurl;

	const char gitdir[GIT_FLEX_ARRAY];
} repoSpanner_client;

git_sortedcache *global_clients;

static int client_cmp(const void *a_, const void *b_)
{
	const struct repoSpanner_client *a = a_, *b = b_;
	return strcmp(a->gitdir, b->gitdir);
}

int repospanner_global_init()
{
	return git_sortedcache_new(
		&global_clients, offsetof(struct repoSpanner_client, gitdir),
		NULL, NULL, client_cmp, NULL
	);
}

/* Returns GIT_ENOTFOUND if not repospanner repo, GIT_OK if so */
int repo_check_repospanner(git_repository *repo)
{
	int enabled, error;

	if (repo->_config == NULL)
		return GIT_ENOTFOUND;  // This only happens with partial inits, like test suite

	error = git_config_get_bool(&enabled, repo->_config, "repospanner.enabled");
	if (error != GIT_OK) {
		if (error == GIT_ENOTFOUND) {
			giterr_clear();
		}
		return error;
	}

	if (!enabled)
		return GIT_ENOTFOUND;

	return GIT_OK;
}

GIT_INLINE(int) repospanner_user_agent(git_buf *buf)
{
	return git_buf_printf(buf, "git/2.0 (libgit2 %s) repospanner/1", LIBGIT2_VERSION);
}

int repospanner_get_client(repoSpanner_client **out, git_repository *repo)
{
	git_buf baseurlbuf = GIT_BUF_INIT;
	git_buf bufval = GIT_BUF_INIT;
	int error = GIT_ERROR;
	repoSpanner_client *client;

	if((error = repo_check_repospanner(repo)) != GIT_OK)
		return error;

	if (git_sortedcache_wlock(global_clients) != GIT_OK)
		return GIT_ERROR;

	client = git_sortedcache_lookup(global_clients, repo->gitdir);
	if (client) {
		*out = client;
		git_sortedcache_wunlock(global_clients);
		return GIT_OK;
	}

	if ((error = git_sortedcache_upsert((void**)&client, global_clients, repo->gitdir)) != GIT_OK)
		goto fail;

	client->basehandle = curl_easy_init();
	if (!client->basehandle)
		goto fail;

	client->share = curl_share_init();
	if (!client->share)
		goto fail;

	// Debugging
	if (getenv("REPOSPANNER_CURL_DEBUG"))
		curl_easy_setopt(client->basehandle, CURLOPT_VERBOSE, 1L);

	curl_easy_setopt(client->basehandle, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(client->basehandle, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(client->basehandle, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(client->basehandle, CURLOPT_SHARE, client->share);
	// These should be default, but still going to set it explicitly
	curl_easy_setopt(client->basehandle, CURLOPT_USE_SSL, CURLUSESSL_ALL);
	curl_easy_setopt(client->basehandle, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(client->basehandle, CURLOPT_SSL_VERIFYPEER, 1L);

	curl_share_setopt(client->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS |
													  CURL_LOCK_DATA_SSL_SESSION |
													  CURL_LOCK_DATA_CONNECT);

	if ((error = repospanner_user_agent(&bufval)) != GIT_OK)
		goto fail;
	curl_easy_setopt(client->basehandle, CURLOPT_USERAGENT, git_buf_cstr(&bufval));
	git_buf_clear(&bufval);

	client->baseurl = baseurlbuf;
	if ((error = git_config_get_string_buf(&client->baseurl, repo->_config, "repospanner.url")) != GIT_OK) {
		if (error == GIT_ENOTFOUND) {
			giterr_set(GITERR_ODB, "Required config option url missing");
			error = GIT_ERROR;
		}
		goto fail;
	}

	if ((error = git_config_get_string_buf(&bufval, repo->_config, "repospanner.cert")) != GIT_OK) {
		if (error == GIT_ENOTFOUND) {
			giterr_set(GITERR_ODB, "Required config option cert missing");
			error = GIT_ERROR;
		}
		goto fail;
	}
	curl_easy_setopt(client->basehandle, CURLOPT_SSLCERT, git_buf_cstr(&bufval));
	git_buf_clear(&bufval);

	if ((error = git_config_get_string_buf(&bufval, repo->_config, "repospanner.key")) != GIT_OK) {
		if (error == GIT_ENOTFOUND) {
			giterr_set(GITERR_ODB, "Required config option key missing");
			error = GIT_ERROR;
		}
		goto fail;
	}
	curl_easy_setopt(client->basehandle, CURLOPT_SSLKEY, git_buf_cstr(&bufval));
	git_buf_clear(&bufval);

	if ((error = git_config_get_string_buf(&bufval, repo->_config, "repospanner.cacert")) != GIT_OK) {
		if (error == GIT_ENOTFOUND) {
			giterr_set(GITERR_ODB, "Required config option cacert missing");
			error = GIT_ERROR;
		}
		goto fail;
	}
	curl_easy_setopt(client->basehandle, CURLOPT_CAINFO, git_buf_cstr(&bufval));
	git_buf_clear(&bufval);

	// Data normalizaton
	if (git_buf_cstr(&client->baseurl)[git_buf_len(&client->baseurl)] == '/')
		git_buf_shorten(&client->baseurl, 1);


	git_sortedcache_wunlock(global_clients);

	*out = client;
	return GIT_OK;

fail:
	git_sortedcache_wunlock(global_clients);

	git_buf_free(&client->baseurl);
	git_buf_free(&bufval);
	git__free(client);
	return error;
}

int repospanner_prepare_request(CURL **out, repoSpanner_client *client, const char *path)
{
	int error;
	CURL *newreq;
	git_buf pathbuf = GIT_BUF_INIT;

	if ((error = git_buf_joinpath(&pathbuf, git_buf_cstr(&client->baseurl), path)) != GIT_OK)
		return error;

	newreq = curl_easy_duphandle(client->basehandle);
	if(newreq == NULL)
		return GIT_ERROR;
	if (curl_easy_setopt(newreq, CURLOPT_URL, git_buf_cstr(&pathbuf)))
		return GIT_ERROR;
	git_buf_free(&pathbuf);

	*out = newreq;
	return GIT_OK;
}

int repospanner_check_curl(CURL *req)
{
	int error;
	long response_code;

	error = curl_easy_perform(req);

	if (error == CURLE_OK)
		return GIT_OK;

	if (error == CURLE_HTTP_RETURNED_ERROR) {
		// Check for 404
		curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &response_code);
		giterr_set(GITERR_NET, "Error received from repoSpanner: %ld", response_code);
		if (response_code == 404)
			return GIT_ENOTFOUND;
	} else {
		giterr_set(GITERR_ODB, "Error performing curl request: (%d): %s", error, curl_easy_strerror(error));
	}

	return GIT_ERROR;
}
