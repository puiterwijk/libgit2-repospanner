/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "refs.h"
#include "hash.h"
#include "repository.h"
#include "fileops.h"
#include "filebuf.h"
#include "pack.h"
#include "reflog.h"
#include "refdb.h"
#include "iterator.h"
#include "sortedcache.h"
#include "signature.h"
#include "repospanner.h"

#include <git2/tag.h>
#include <git2/object.h>
#include <git2/refdb.h>
#include <git2/branch.h>
#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>
#include <git2/sys/reflog.h>

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

typedef struct refdb_rs_backend {
	git_refdb_backend parent;

	git_repository *repo;

	repoSpanner_client *client;

	git_sortedcache *refcache;
} refdb_rs_backend;

typedef struct refdb_rs_iter {
	git_reference_iterator backend;

	git_pool pool;
	char *glob;

	git_sortedcache *refcache;
	size_t current_pos;
} refdb_rs_iter;

struct packref {
	git_oid oid;
	git_oid peel;
	char flags;
	char name[GIT_FLEX_ARRAY];
};

struct refretrieve {
	git_buf buffer;
	git_sortedcache *target;
};

static int parse_symb_ref(git_sortedcache *target, const char *name, const char *val)
{
	struct packref *realref;
	struct packref *ref;
	int error;

	realref = git_sortedcache_lookup(target, val);
	if (!realref)  // If the real ref didn't exist... Let's skip for now
		return GIT_OK;

	ref = git__calloc(1, sizeof(struct packref));
	if (ref == NULL) {
		giterr_set_oom();
		return GIT_ERROR;
	}

	if (git_sortedcache_upsert((void **)&ref, target, name) < 0) {
		giterr_set(GITERR_ODB, "Unable to insert into target refcache");
		return GIT_ERROR;
	}

	ref->oid = realref->oid;

	// TODO: Get other flags and peeled

	return GIT_OK;
}

static int parse_ref(git_sortedcache *target, const char *start, const char *end)
{
	struct packref *ref;
	const char *type = NULL, *name = NULL, *val = NULL;
	const char *pos;

	type = start;
	for (pos = start; pos < end; pos++) {
		if (*pos != '\0') {
			continue;
		}
		if (name == NULL)
			name = pos+1;
		else {
			val = pos+1;
			break;
		}
	}

	if ((type == NULL) || (name == NULL) || (val == NULL)) {
		giterr_set(GITERR_ODB, "invalid ref parsed");
		return GIT_ERROR;
	}

	if (!strcmp(type, "real") && !strcmp(type, "symb")) {
		giterr_set(GITERR_ODB, "ref has invalid type '%s'", type);
		return GIT_ERROR;
	}

	if (!strcmp(type, "symb"))
		return parse_symb_ref(target, name, val);

	if (strcmp(type, "real") && strlen(val) != 40) {
		giterr_set(GITERR_ODB, "ref of type real has invalid val '%s'", val);
		return GIT_ERROR;
	}

	ref = git__calloc(1, sizeof(struct packref));
	if (ref == NULL) {
		giterr_set_oom();
		return GIT_ERROR;
	}

	if (git_sortedcache_upsert((void **)&ref, target, name) < 0) {
		giterr_set(GITERR_ODB, "Unable to insert into target refcache");
		return GIT_ERROR;
	}

	if (git_oid_fromstrn(&ref->oid, val, 40) != GIT_OK) {
		giterr_set(GITERR_ODB, "Could not parse oid");
		return GIT_ERROR;
	}

	// TODO: Get other flags and peeled

	return GIT_OK;
}

int ref_write_parse(struct refretrieve *data, bool final)
{
	int error;
	const char *possiblebreak, *newlinepoint, *bufval;

	// Now, see if we got a newline (end of a ref)
	while(true) {  // We might have gotten multiple refs, parse until we have nothing left
		bufval = git_buf_cstr(&data->buffer);
		newlinepoint = NULL;

		for (possiblebreak = bufval; possiblebreak < bufval + git_buf_len(&data->buffer); possiblebreak++) {
			if (*possiblebreak == '\n') {
				newlinepoint = possiblebreak;
				break;
			}
		}

		if (newlinepoint == NULL) {  // No newlines received
			if (!final)
				return GIT_OK;  // Not yet final, we might still get more data
			else {
				if (git_buf_len(&data->buffer) == 0)
					return GIT_OK;  // We exhausted the buffer, nothing to see here
				else
					return GIT_ERROR;  // We have a partial line without more data inbound
			}
		}

		if ((error = parse_ref(data->target, bufval, newlinepoint)) != GIT_OK)
			return error;

		git_buf_consume(&data->buffer, newlinepoint+1);
	}
}

size_t ref_write_callback(char *ptr, size_t UNUSED(size), size_t nmemb, void *userdata)
{
	struct refretrieve *data = (struct refretrieve *)userdata;

	// First, just add the data we just got to the buffer
	if(git_buf_put(&data->buffer, ptr, nmemb) != GIT_OK) {  // Sane would be to use "size", but not for curl
		return GIT_ERROR;
	}

	if(ref_write_parse(data, false) == GIT_OK) {
		return nmemb;
	} else {
		return GIT_ERROR;
	}
}


static int packref_cmp(const void *a_, const void *b_)
{
	const struct packref *a = a_, *b = b_;
	return strcmp(a->name, b->name);
}


static int _ensure_refs_loaded(refdb_rs_backend *backend)
{
	git_sortedcache *newcache;
	CURL *req;
	struct refretrieve *retriever;
	int error = GIT_ERROR;;

	if(backend->refcache != NULL)
		return GIT_OK;

	if ((error = git_sortedcache_new(
			&newcache, offsetof(struct packref, name), NULL, NULL,
			packref_cmp, NULL)) < 0) {
		return error;
	}

	if ((error = repospanner_prepare_request(&req, backend->client, "simple/refs")) != GIT_OK)
		goto fail;

	retriever = git__calloc(1, sizeof(struct refretrieve));
	if (!retriever)
		goto fail;
	retriever->target = newcache;
	if ((error = git_buf_init(&retriever->buffer, 0)) != GIT_OK)
		goto fail;

	curl_easy_setopt(req, CURLOPT_WRITEFUNCTION, ref_write_callback);
	curl_easy_setopt(req, CURLOPT_WRITEDATA, retriever);

	if ((error = (repospanner_check_curl(req))) != GIT_OK)
		goto fail;

	if(ref_write_parse(retriever, true) != GIT_OK) {
		giterr_set(GITERR_ODB, "Parsing failed");
		goto fail;
	}

	backend->refcache = newcache;
	return GIT_OK;

fail:
	if (req)
		curl_free(req);
	if (retriever)
		git_buf_free(&retriever->buffer);
	git_sortedcache_free(newcache);

	return error ? error : GIT_ERROR;
}

static int refdb_rs__exists(
	int *exists,
	git_refdb_backend *_backend,
	const char *ref_name)
{
	int error = GIT_OK;
	void *entry;

	refdb_rs_backend *backend = (refdb_rs_backend *)_backend;

	if ((error = _ensure_refs_loaded(backend)) != GIT_OK)
		return error;

	if((error = git_sortedcache_rlock(backend->refcache)) < 0)
		return error;

	entry = git_sortedcache_lookup(backend->refcache, ref_name);

	git_sortedcache_runlock(backend->refcache);

	*exists = entry != NULL;

	return GIT_OK;
}

static int ref_error_notfound(const char *name)
{
	giterr_set(GITERR_REFERENCE, "reference '%s' not found", name);
	return GIT_ENOTFOUND;
}

static int refdb_rs__lookup(
	git_reference **out,
	git_refdb_backend *_backend,
	const char *ref_name)
{
	struct packref *entry;
	int error = GIT_OK;

	refdb_rs_backend *backend = (refdb_rs_backend *)_backend;

	if ((error = _ensure_refs_loaded(backend)) != GIT_OK)
		return error;

	if((error = git_sortedcache_rlock(backend->refcache)) < 0)
		return error;

	entry = git_sortedcache_lookup(backend->refcache, ref_name);
	if (!entry)
		error = ref_error_notfound(ref_name);
	else {
		*out = git_reference__alloc(ref_name, &entry->oid, &entry->peel);
		if (!*out)
			error = -1;
	}

	git_sortedcache_runlock(backend->refcache);

	return error;
}

static int refdb_rs__iterator_next(
	git_reference **out, git_reference_iterator *_iter)
{
	int error = GIT_ITEROVER;
	refdb_rs_iter *iter = (refdb_rs_iter *)_iter;
	struct packref *ref;

	while(iter->current_pos < git_sortedcache_entrycount(iter->refcache)) {
		ref = git_sortedcache_entry(iter->refcache, iter->current_pos++);
		if (!ref)  // If we have NULL even though there should be something, the world blew up
			break;

		if (iter->glob && p_fnmatch(iter->glob, ref->name, 0) != 0)
			continue;

		*out = git_reference__alloc(ref->name, &ref->oid, &ref->peel);
		error = (*out != NULL) ? GIT_OK : GIT_ERROR;
		break;
	}

	return error;
}

static int refdb_rs__iterator_next_name(
	const char **out, git_reference_iterator *_iter)
{
	int error = GIT_ITEROVER;
	refdb_rs_iter *iter = (refdb_rs_iter *)_iter;
	struct packref *ref;

	while(iter->current_pos < git_sortedcache_entrycount(iter->refcache)) {
		ref = git_sortedcache_entry(iter->refcache, iter->current_pos++);
		if (!ref)  // If we have NULL even though there should be something, the world blew up
			break;

		if (iter->glob && p_fnmatch(iter->glob, ref->name, 0) != 0)
			continue;

		*out = ref->name;
		error = GIT_OK;
		break;
	}

	return error;
}

static void refdb_rs__iterator_free(git_reference_iterator *_iter)
{
	refdb_rs_iter *iter = (refdb_rs_iter *)_iter;

	git_pool_clear(&iter->pool);
	git_sortedcache_free(iter->refcache);
	git__free(iter);
}

static int refdb_rs__iterator(
	git_reference_iterator **out,
	git_refdb_backend *_backend,
	const char *glob)
{
	refdb_rs_iter *iter;
	int error = GIT_OK;

	refdb_rs_backend *backend = (refdb_rs_backend *)_backend;

	if ((error = _ensure_refs_loaded(backend)) != GIT_OK)
		return error;

	iter = git__calloc(1, sizeof(refdb_rs_iter));
	GITERR_CHECK_ALLOC(iter);

	iter->current_pos = 0;

	if ((error = git_sortedcache_copy(&iter->refcache, backend->refcache, 1, NULL, NULL)) < 0)
		goto fail;

	git_pool_init(&iter->pool, 1);
	if (glob != NULL &&
		(iter->glob = git_pool_strdup(&iter->pool, glob)) == NULL)
		goto fail;

	iter->backend.next = refdb_rs__iterator_next;
	iter->backend.next_name = refdb_rs__iterator_next_name;
	iter->backend.free = refdb_rs__iterator_free;

	*out = (git_reference_iterator *)iter;

	return GIT_OK;

fail:
	refdb_rs__iterator_free((git_reference_iterator *)iter);
	return -1;
}

static int rs_not_implemented(const char *fname)
{
	giterr_set(GITERR_INVALID, "function %s not implemented for repoSpanner", fname);
	return GIT_EINVALID;
}

static int refdb_rs__write(
	git_refdb_backend *UNUSED(backend),
	const git_reference *UNUSED(ref),
	int UNUSED(force),
	const git_signature *UNUSED(Who),
	const char *UNUSED(message),
	const git_oid *UNUSED(old),
	const char *UNUSED(old_target))
{
	return rs_not_implemented("write");
}

static int refdb_rs__rename(
	git_reference **UNUSED(out),
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(old_name),
	const char *UNUSED(new_name),
	int UNUSED(force),
	const git_signature *UNUSED(who),
	const char *UNUSED(message))
{
	return rs_not_implemented("rename");
}

static int refdb_rs__del(
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(ref_name),
	const git_oid *UNUSED(old_id),
	const char *UNUSED(old_target))
{
	return rs_not_implemented("del");
}

static int refdb_rs__compress(git_refdb_backend *UNUSED(backend))
{
	return GIT_OK;
}

static int refdb_reflog_rs__has_log(
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(refname))
{
	return 0;
}

static int refdb_reflog_rs__ensure_log(
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(refname))
{
	return rs_not_implemented("ensure_log");
}

static int refdb_reflog_rs__read(
	git_reflog **UNUSED(out),
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(refname))
{
	return rs_not_implemented("reflog_read");
}

static int refdb_reflog_rs__write(
	git_refdb_backend *UNUSED(backend),
	git_reflog *UNUSED(reflog))
{
	return rs_not_implemented("reflog_write");
}

static int refdb_reflog_rs__rename(
	git_refdb_backend *UNUSED(_backend),
	const char *UNUSED(old_name),
	const char *UNUSED(new_name))
{
	return rs_not_implemented("reflog_rename");
}


static int refdb_reflog_rs__delete(
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(refname))
{
	return rs_not_implemented("reflog_delete");
}

static int refdb_rs__lock(
	void **UNUSED(payload_out),
	git_refdb_backend *UNUSED(backend),
	const char *UNUSED(refname))
{
	return rs_not_implemented("lock");
}

static int refdb_rs__unlock(
	git_refdb_backend *UNUSED(backend),
	void *UNUSED(payload),
	int UNUSED(success),
	int UNUSED(update_reflog),
	const git_reference *UNUSED(ref),
	const git_signature *UNUSED(sig),
	const char *UNUSED(message))
{
	return rs_not_implemented("unlock");
}

static void refdb_rs__free(git_refdb_backend *_backend)
{
	refdb_rs_backend *backend = (refdb_rs_backend *)_backend;

	assert(backend);

	if (backend->refcache != NULL)
		git_sortedcache_free(backend->refcache);
	git__free(backend);
}

int git_refdb_backend_repospanner(
	git_refdb_backend **backend_out,
	git_repository *repository)
{
	int error = GIT_OK;
	refdb_rs_backend *backend;
	repoSpanner_client *client;

	if ((error = repospanner_get_client(&client, repository)) != 0)
		return error;

	backend = git__calloc(1, sizeof(refdb_rs_backend));
	GITERR_CHECK_ALLOC(backend);

	backend->client = client;
	backend->repo = repository;
	backend->refcache = NULL;

	backend->parent.exists = &refdb_rs__exists;
	backend->parent.lookup = &refdb_rs__lookup;
	backend->parent.iterator = &refdb_rs__iterator;
	backend->parent.write = &refdb_rs__write;
	backend->parent.del = &refdb_rs__del;
	backend->parent.rename = &refdb_rs__rename;
	backend->parent.compress = &refdb_rs__compress;
	backend->parent.lock = &refdb_rs__lock;
	backend->parent.unlock = &refdb_rs__unlock;
	backend->parent.has_log = &refdb_reflog_rs__has_log;
	backend->parent.ensure_log = &refdb_reflog_rs__ensure_log;
	backend->parent.free = &refdb_rs__free;
	backend->parent.reflog_read = &refdb_reflog_rs__read;
	backend->parent.reflog_write = &refdb_reflog_rs__write;
	backend->parent.reflog_rename = &refdb_reflog_rs__rename;
	backend->parent.reflog_delete = &refdb_reflog_rs__delete;

	*backend_out = (git_refdb_backend *)backend;
	return GIT_OK;
}
