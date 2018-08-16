/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "git2/object.h"
#include "git2/sys/odb_backend.h"
#include "fileops.h"
#include "hash.h"
#include "odb.h"
#include "array.h"
#include "oidmap.h"

#include "repospanner.h"

#include "git2/odb_backend.h"
#include "git2/types.h"
#include "git2/pack.h"

struct repospanner_odb {
	git_odb_backend parent;

	repoSpanner_client *client;

	git_odb_backend *fsdb;
	const char *objects_dir;
	size_t objects_dirlen;
	git_repository *repo;
};

static int object_file_name(
	git_buf *name, const struct repospanner_odb *be, const git_oid *id)
{
	size_t alloclen;
	size_t objects_dirlen = be->objects_dirlen;

	/* expand length for object root + 40 hex sha1 chars + 2 * '/' + '\0' */
	GITERR_CHECK_ALLOC_ADD(&alloclen, objects_dirlen, GIT_OID_HEXSZ);
	GITERR_CHECK_ALLOC_ADD(&alloclen, alloclen, 3);
	if (git_buf_grow(name, alloclen) < 0)
		return -1;

	git_buf_set(name, be->objects_dir, objects_dirlen);
	git_path_to_dir(name);

	/* loose object filename: aa/aaa... (41 bytes) */
	git_oid_pathfmt(name->ptr + name->size, id);
	name->size += GIT_OID_HEXSZ + 1;
	name->ptr[name->size] = '\0';

	return git_futils_mkdir_relative(
		name->ptr + objects_dirlen, be->objects_dir, 0755,
		GIT_MKDIR_PATH | GIT_MKDIR_SKIP_LAST | GIT_MKDIR_VERIFY_DIR, NULL);
}


static int rs_odb_not_implemented(const char *fname)
{
	giterr_set(GITERR_INVALID, "function %s not implemented for repoSpanner", fname);
	return GIT_EINVALID;
}

static int get_request_for_object(CURL **out, struct repospanner_odb *backend, const git_oid *oid)
{
	git_buf pathbuf = GIT_BUF_INIT;

	if (git_buf_puts(&pathbuf, "simple/object/") != GIT_OK)
		return GIT_ERROR;
	if (git_buf_puts(&pathbuf, git_oid_tostr_s(oid)) != GIT_OK)
		return GIT_ERROR;

	return repospanner_prepare_request(out, backend->client, git_buf_cstr(&pathbuf));
}

static int impl__write(
	git_odb_backend *_backend,
	const git_oid *oid,
	const void *data,
	size_t len,
	git_otype type)
{
	(void)_backend;
	(void)oid;
	(void)data;
	(void)len;
	(void)type;
	return rs_odb_not_implemented("write");
}

static int retrieve_file(struct repospanner_odb *backend, const git_oid *oid)
{
	int error;
	CURL *req;
	git_buf final_path = GIT_BUF_INIT;
	FILE *outfile;

	if ((error = get_request_for_object(&req, backend, oid)) != GIT_OK)
		return error;

	if ((error = object_file_name(&final_path, backend, oid)) != GIT_OK)
		return error;

	outfile = fopen(git_buf_cstr(&final_path), "wb");
	if (outfile == NULL) {
		giterr_set(GITERR_NOMEMORY, "Could not open file buffer at %s", git_buf_cstr(&final_path));
		return GIT_ERROR;
	}

	curl_easy_setopt(req, CURLOPT_WRITEDATA, outfile);
	if ((error = repospanner_check_curl(req)) != GIT_OK)
		goto fail;

	if (fclose(outfile))
		goto fail;

	return GIT_OK;
fail:
	fclose(outfile);
	unlink(git_buf_cstr(&final_path));

	return error;
}


static int impl__exists(git_odb_backend *_backend, const git_oid *oid)
{
	int error;
	struct repospanner_odb *backend = (struct repospanner_odb *)_backend;

	if ((error = retrieve_file(backend, oid)) != GIT_OK) {
		if (error == GIT_ENOTFOUND)
			return 0;
		else
			return error;
	}

	return 1;
}

static int impl__read(void **buffer_p, size_t *len_p, git_otype *type_p, git_odb_backend *_backend, const git_oid *oid)
{
	int error;
	struct repospanner_odb *backend = (struct repospanner_odb *)_backend;

	if ((error = retrieve_file(backend, oid)) != GIT_OK)
		return error;

	return backend->fsdb->read(buffer_p, len_p, type_p, backend->fsdb, oid);
}

static int impl__read_header(size_t *len_p, git_otype *type_p, git_odb_backend *_backend, const git_oid *oid)
{
	int error;
	struct repospanner_odb *backend = (struct repospanner_odb *)_backend;

	if ((error = retrieve_file(backend, oid)) != GIT_OK)
		return error;

	return backend->fsdb->read_header(len_p, type_p, backend->fsdb, oid);
}

static void impl__free(git_odb_backend *_backend)
{
	struct repospanner_odb *backend = (struct repospanner_odb *)_backend;

	git__free(backend);
}

int git_odb_backend_repospanner(
	git_odb_backend **out, git_odb_backend *fsbackend,
	const char *objects_dir, git_repository *repository)
{
	struct repospanner_odb *db;
	int error = GIT_OK;
	repoSpanner_client *client;
	size_t objects_dirlen;
	char *new_objects_dir;

	assert(out);

	objects_dirlen = strlen(objects_dir);
	new_objects_dir = git__calloc(objects_dirlen + 1, sizeof(char));
	memcpy(new_objects_dir, objects_dir, objects_dirlen);
	// Make sure to null-terminate it, since fileops won't behave otherwise
	new_objects_dir[objects_dirlen] = '\0';

	if ((error = repospanner_get_client(&client, repository)) != 0)
		return error;

	db = git__calloc(1, sizeof(struct repospanner_odb));
	GITERR_CHECK_ALLOC(db);

	db->client = client;
	db->repo = repository;
	db->fsdb = fsbackend;
	db->objects_dir = new_objects_dir;
	db->objects_dirlen = objects_dirlen;

	db->parent.version = GIT_ODB_BACKEND_VERSION;
	db->parent.read = &impl__read;
	db->parent.write = &impl__write;
	db->parent.read_header = &impl__read_header;
	db->parent.exists = &impl__exists;
	db->parent.free = &impl__free;

	*out = (git_odb_backend *)db;
	return 0;
}
