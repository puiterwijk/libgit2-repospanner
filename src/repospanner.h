/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_repospanner_h__
#define INCLUDE_repospanner_h__

#include "common.h"

#include <curl/curl.h>

typedef struct repoSpanner_client repoSpanner_client;

extern int repospanner_global_init(void);
extern int repospanner_get_client(repoSpanner_client **out, git_repository *repo);
extern int repospanner_prepare_request(CURL **out, repoSpanner_client *client, const char *path);
extern int repospanner_check_curl(CURL *req);

#endif
