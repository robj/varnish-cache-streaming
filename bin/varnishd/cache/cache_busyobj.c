/*-
 * Copyright (c) 2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Handle backend connections and backend request structures.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include "cache.h"

struct vbo {
	unsigned		magic;
#define VBO_MAGIC		0xde3d8223
	struct lock		mtx;
	pthread_cond_t		cond;
	unsigned		refcount;
	uint16_t		nhttp;
	struct busyobj		bo;
};

static struct lock vbo_mtx;
static struct vbo *nvbo;

void
VBO_Init(void)
{
	Lck_New(&vbo_mtx, lck_busyobj);
	nvbo = NULL;
}

/*--------------------------------------------------------------------
 * BusyObj handling
 */

static struct vbo *
vbo_New(void)
{
	struct vbo *vbo;
	uint16_t nhttp;
	ssize_t http_space;

	assert(cache_param->http_max_hdr < 65536);
	nhttp = (uint16_t)cache_param->http_max_hdr;

	http_space = HTTP_estimate(nhttp);

	vbo = malloc(sizeof *vbo + 2 * http_space);
	AN(vbo);

	memset(vbo, 0, sizeof *vbo);
	vbo->magic = VBO_MAGIC;
	vbo->nhttp = nhttp;
	Lck_New(&vbo->mtx, lck_busyobj);
	AZ(pthread_cond_init(&vbo->cond, NULL));
	return (vbo);
}

void
VBO_Free(struct vbo **vbop)
{
	struct vbo *vbo;

	AN(vbop);
	vbo = *vbop;
	*vbop = NULL;
	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	AZ(vbo->refcount);
	Lck_Delete(&vbo->mtx);
	AZ(pthread_cond_destroy(&vbo->cond));
	FREE_OBJ(vbo);
}

struct busyobj *
VBO_GetBusyObj(struct worker *wrk)
{
	struct vbo *vbo = NULL;
	char *p;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);

	if (wrk->nvbo != NULL) {
		vbo = wrk->nvbo;
		wrk->nvbo = NULL;
	}

	if (vbo == NULL) {
		Lck_Lock(&vbo_mtx);

		vbo = nvbo;
		nvbo = NULL;

		if (vbo == NULL)
			VSC_C_main->busyobj_alloc++;

		Lck_Unlock(&vbo_mtx);
	}

	if (vbo != NULL && vbo->nhttp != cache_param->http_max_hdr)
		VBO_Free(&vbo);

	if (vbo == NULL)
		vbo = vbo_New();

	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	AZ(vbo->refcount);

	AZ(vbo->bo.magic);
	vbo->refcount = 1;
	vbo->bo.magic = BUSYOBJ_MAGIC;
	vbo->bo.vbo = vbo;

	p = (void*)(vbo + 1);
	vbo->bo.bereq = HTTP_create(p, vbo->nhttp);
	p += HTTP_estimate(vbo->nhttp);
	vbo->bo.beresp = HTTP_create(p, vbo->nhttp);

	return (&vbo->bo);
}

struct busyobj *
VBO_RefBusyObj(struct busyobj *busyobj)
{
	struct vbo *vbo;

	CHECK_OBJ_NOTNULL(busyobj, BUSYOBJ_MAGIC);
	vbo = busyobj->vbo;
	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	if (busyobj->use_locks)
		Lck_Lock(&vbo->mtx);
	assert(vbo->refcount > 0);
	vbo->refcount++;
	if (busyobj->use_locks)
		Lck_Unlock(&vbo->mtx);

	return (busyobj);
}

unsigned
VBO_DerefBusyObj(struct worker *wrk, struct busyobj **pbo)
{
	struct busyobj *bo;
	struct vbo *vbo;
	unsigned r;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	AN(pbo);
	bo = *pbo;
	*pbo = NULL;
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vbo = bo->vbo;
	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	if (bo->use_locks)
		Lck_Lock(&vbo->mtx);
	assert(vbo->refcount > 0);
	r = --vbo->refcount;
	if (bo->use_locks)
		Lck_Unlock(&vbo->mtx);

	if (r == 0) {
		/* XXX: Sanity checks & cleanup */
		memset(&vbo->bo, 0, sizeof vbo->bo);

		if (cache_param->bo_cache && wrk->nvbo == NULL) {
			wrk->nvbo = vbo;
		} else {
			Lck_Lock(&vbo_mtx);
			if (nvbo == NULL) {
				nvbo = vbo;
				vbo = NULL;
			} else
				VSC_C_main->busyobj_free++;
			Lck_Unlock(&vbo_mtx);

			if (vbo != NULL)
				VBO_Free(&vbo);
		}
	}

	return (r);
}

/* Signal that the fetch thread has stopped */
void
VBO_StreamStopped(struct busyobj *busyobj)
{
	if (!busyobj->use_locks) {
		busyobj->stream_stopped = 1;
		return;
	}
	Lck_Lock(&busyobj->vbo->mtx);
	busyobj->stream_stopped = 1;
	AZ(pthread_cond_broadcast(&busyobj->vbo->cond));
	Lck_Unlock(&busyobj->vbo->mtx);
}

/* Wait for the fetch thread to finish reading the pipeline buffer */
void
VBO_StreamWait(struct busyobj *busyobj)
{
	if (!busyobj->use_locks)
		return;
	Lck_Lock(&busyobj->vbo->mtx);
	while (busyobj->htc.pipeline.b != NULL && busyobj->stream_stopped == 0)
		Lck_CondWait(&busyobj->vbo->cond, &busyobj->vbo->mtx, NULL);
	Lck_Unlock(&busyobj->vbo->mtx);
}

/* Signal additional data available */
void
VBO_StreamData(struct busyobj *busyobj)
{
	CHECK_OBJ_NOTNULL(busyobj, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(busyobj->fetch_obj, OBJECT_MAGIC);

	if (busyobj->use_locks)
		Lck_Lock(&busyobj->vbo->mtx);
	assert(busyobj->fetch_obj->len >= busyobj->stream_max);
	if (busyobj->fetch_obj->len > busyobj->stream_max) {
		busyobj->stream_max = busyobj->fetch_obj->len;
		if (busyobj->use_locks)
			AZ(pthread_cond_broadcast(&busyobj->vbo->cond));
	}
	if (busyobj->use_locks)
		Lck_Unlock(&busyobj->vbo->mtx);
}

/* Sync the client's stream_ctx with the busyobj, and block on no more
 * data available */
void
VBO_StreamSync(struct worker *wrk)
{
	struct busyobj *busyobj;
	struct stream_ctx *sctx;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
	busyobj = wrk->busyobj;
	CHECK_OBJ_NOTNULL(wrk->sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->sp->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->sp->req->obj, OBJECT_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->sctx, STREAM_CTX_MAGIC);
	sctx = wrk->sctx;

	if (busyobj->use_locks)
		Lck_Lock(&busyobj->vbo->mtx);
	assert(sctx->stream_max <= busyobj->stream_max);

	if (wrk->sp->req->obj->objcore == NULL ||
	    (wrk->sp->req->obj->objcore->flags & OC_F_PASS)) {
		/* Give notice to backend fetch that we are finished
		 * with all chunks before this one */
		busyobj->stream_frontchunk = sctx->stream_frontchunk;
	}

	sctx->stream_stopped = busyobj->stream_stopped;
	sctx->stream_max = busyobj->stream_max;

	if (busyobj->use_locks && !sctx->stream_stopped &&
	    sctx->stream_next == sctx->stream_max) {
		while (!busyobj->stream_stopped &&
		       sctx->stream_max == busyobj->stream_max) {
			Lck_CondWait(&busyobj->vbo->cond, &busyobj->vbo->mtx,
				     NULL);
		}
		sctx->stream_stopped = busyobj->stream_stopped;
		sctx->stream_max = busyobj->stream_max;
	}

	if (busyobj->use_locks)
		Lck_Unlock(&busyobj->vbo->mtx);
}
