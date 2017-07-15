//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Functionality related to end points.

static void nni_ep_accept_start(nni_ep *);
static void nni_ep_accept_done(void *);

static nni_idhash *nni_eps;

int
nni_ep_sys_init(void)
{
	int rv;

	if ((rv = nni_idhash_init(&nni_eps)) != 0) {
		return (rv);
	}

	nni_idhash_set_limits(
	    nni_eps, 1, 0x7fffffff, nni_random() & 0x7fffffff);

	return (0);
}

void
nni_ep_sys_fini(void)
{
	nni_idhash_fini(nni_eps);
	nni_eps = NULL;
}

uint32_t
nni_ep_id(nni_ep *ep)
{
	return (ep->ep_id);
}

static void
nni_ep_destroy(nni_ep *ep)
{
	if (ep == NULL) {
		return;
	}
	nni_aio_fini(&ep->ep_acc_aio);
	if (ep->ep_data != NULL) {
		ep->ep_ops.ep_fini(ep->ep_data);
	}
	if (ep->ep_id != 0) {
		nni_idhash_remove(nni_eps, ep->ep_id);
	}
	nni_thr_fini(&ep->ep_thr);
	nni_cv_fini(&ep->ep_cv);
	nni_mtx_fini(&ep->ep_mtx);
	NNI_FREE_STRUCT(ep);
}

int
nni_ep_create(nni_ep **epp, nni_sock *sock, const char *addr, int mode)
{
	nni_tran *tran;
	nni_ep *  ep;
	int       rv;
	uint32_t  id;

	if ((tran = nni_tran_find(addr)) == NULL) {
		return (NNG_ENOTSUP);
	}
	if (strlen(addr) >= NNG_MAXADDRLEN) {
		return (NNG_EINVAL);
	}

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	ep->ep_closed = 0;
	ep->ep_bound  = 0;
	ep->ep_pipe   = NULL;
	ep->ep_id     = id;
	ep->ep_data   = NULL;

	NNI_LIST_NODE_INIT(&ep->ep_node);

	nni_pipe_ep_list_init(&ep->ep_pipes);

	if (((rv = nni_mtx_init(&ep->ep_mtx)) != 0) ||
	    ((rv = nni_cv_init(&ep->ep_cv, &ep->ep_mtx)) != 0) ||
	    ((rv = nni_idhash_alloc(nni_eps, &ep->ep_id, ep)) != 0)) {
		nni_ep_destroy(ep);
		return (rv);
	}
	rv = nni_aio_init(&ep->ep_acc_aio, nni_ep_accept_done, ep);
	if (rv != 0) {
		nni_ep_destroy(ep);
		return (rv);
	}

	ep->ep_sock = sock;
	ep->ep_tran = tran;
	ep->ep_mode = mode;

	// Could safely use strcpy here, but this avoids discussion.
	(void) snprintf(ep->ep_addr, sizeof(ep->ep_addr), "%s", addr);

	// Make a copy of the endpoint operations.  This allows us to
	// modify them (to override NULLs for example), and avoids an extra
	// dereference on hot paths.
	ep->ep_ops = *tran->tran_ep;

	if ((rv = ep->ep_ops.ep_init(&ep->ep_data, addr, sock, mode)) != 0) {
		nni_ep_destroy(ep);
		return (rv);
	}

	*epp = ep;
	return (0);
}

void
nni_ep_close(nni_ep *ep)
{
	// Abort any in-flight operations.
	nni_aio_stop(&ep->ep_acc_aio);

	nni_mtx_lock(&ep->ep_mtx);
	if (ep->ep_closed == 0) {
		ep->ep_closed = 1;
		ep->ep_ops.ep_close(ep->ep_data);
	}
	nni_cv_wake(&ep->ep_cv);
	nni_mtx_unlock(&ep->ep_mtx);
}

static void
nni_ep_reap(nni_ep *ep)
{
	nni_pipe *pipe;

	nni_ep_close(ep); // Extra sanity.

	// Take us off the sock list.
	nni_sock_ep_remove(ep->ep_sock, ep);

	nni_ep_destroy(ep);
}

void
nni_ep_stop(nni_ep *ep)
{
	nni_mtx_lock(&ep->ep_mtx);

	// Protection against recursion.
	if (ep->ep_stop) {
		nni_mtx_unlock(&ep->ep_mtx);
		return;
	}
	ep->ep_stop = 1;
	nni_taskq_ent_init(&ep->ep_reap_tqe, (nni_cb) nni_ep_reap, ep);
	nni_taskq_dispatch(NULL, &ep->ep_reap_tqe);
	nni_mtx_unlock(&ep->ep_mtx);
}

static int
nni_ep_connect_aio(nni_ep *ep, void **pipep)
{
	nni_aio aio;
	int     rv;

	nni_aio_init(&aio, NULL, NULL);
	aio.a_endpt = ep->ep_data;
	ep->ep_ops.ep_connect(ep->ep_data, &aio);
	nni_aio_wait(&aio);

	if ((rv = nni_aio_result(&aio)) == 0) {
		*pipep = aio.a_pipe;
	}
	nni_aio_fini(&aio);
	return (rv);
}

static int
nni_ep_connect_sync(nni_ep *ep)
{
	nni_pipe *pipe;
	int       rv;

	nni_mtx_lock(&ep->ep_mtx);
	if (ep->ep_closed) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_ECLOSED);
	}
	rv = nni_pipe_create(&pipe, ep->ep_sock, ep->ep_tran);
	if (rv != 0) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (rv);
	}
	pipe->p_ep = ep;
	nni_list_append(&ep->ep_pipes, pipe);
	nni_mtx_unlock(&ep->ep_mtx);

	rv = nni_ep_connect_aio(ep, &pipe->p_tran_data);
	if (rv != 0) {
		if (rv != NNG_ECLOSED) { // HACK ALERT
			nni_pipe_stop(pipe);
		}
		return (rv);
	}
	nni_pipe_start(pipe);
	nni_mtx_lock(&ep->ep_mtx);
	ep->ep_pipe = pipe;
	nni_mtx_unlock(&ep->ep_mtx);
	return (0);
}

// nni_dialer is the thread worker that dials in the background.
static void
nni_dialer(void *arg)
{
	nni_ep *     ep = arg;
	int          rv;
	nni_time     cooldown;
	nni_duration maxrtime = 0, nmaxrtime;
	nni_duration defrtime = 0, ndefrtime;
	nni_duration rtime;

	nni_sock_reconntimes(ep->ep_sock, &defrtime, &maxrtime);
	rtime = defrtime;

	for (;;) {
		nni_sock_reconntimes(ep->ep_sock, &ndefrtime, &nmaxrtime);
		if ((defrtime != ndefrtime) || (maxrtime != nmaxrtime)) {
			// Times changed, so reset them.
			defrtime = ndefrtime;
			maxrtime = nmaxrtime;
			rtime    = defrtime;
		}

		nni_mtx_lock(&ep->ep_mtx);
		while ((!ep->ep_closed) && (ep->ep_pipe != NULL)) {
			rtime = defrtime;
			nni_cv_wait(&ep->ep_cv);
		}
		if (ep->ep_closed) {
			nni_mtx_unlock(&ep->ep_mtx);
			return;
		}
		nni_mtx_unlock(&ep->ep_mtx);

		rv = nni_ep_connect_sync(ep);
		switch (rv) {
		case 0:
			// good connection
			continue;
		case NNG_ECLOSED:
			return;

		default:
			cooldown = nni_clock() + rtime;
			rtime *= 2;
			if ((maxrtime >= defrtime) && (rtime > maxrtime)) {
				rtime = maxrtime;
			}
			break;
		}
		// we inject a delay so we don't just spin hard on
		// errors like connection refused.
		nni_mtx_lock(&ep->ep_mtx);
		while (!ep->ep_closed) {
			rv = nni_cv_until(&ep->ep_cv, cooldown);
			if (rv == NNG_ETIMEDOUT) {
				break;
			}
		}
		nni_mtx_unlock(&ep->ep_mtx);
	}
}

int
nni_ep_dial(nni_ep *ep, int flags)
{
	int rv = 0;

	nni_mtx_lock(&ep->ep_mtx);
	if (ep->ep_mode != NNI_EP_MODE_DIAL) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_ENOTSUP);
	}
	if (ep->ep_started) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_EBUSY);
	}
	if (ep->ep_closed) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_ECLOSED);
	}

	if ((rv = nni_thr_init(&ep->ep_thr, nni_dialer, ep)) != 0) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (rv);
	}
	ep->ep_started = 1;

	if (flags & NNG_FLAG_SYNCH) {
		nni_mtx_unlock(&ep->ep_mtx);
		rv = nni_ep_connect_sync(ep);
		if (rv != 0) {
			nni_thr_fini(&ep->ep_thr);
			ep->ep_started = 0;
			return (rv);
		}
		nni_mtx_lock(&ep->ep_mtx);
	}

	nni_thr_run(&ep->ep_thr);
	nni_mtx_unlock(&ep->ep_mtx);

	return (rv);
}

static void nni_ep_accept_start(nni_ep *ep);

static void
nni_ep_accept_done(void *arg)
{
	nni_ep *             ep  = arg;
	nni_aio *            aio = &ep->ep_acc_aio;
	void *               tpipe;
	nni_pipe *           pipe;
	int                  rv;
	const nni_tran_pipe *ops;

	ops = ep->ep_tran->tran_pipe;

	nni_mtx_lock(&ep->ep_mtx);
	if ((rv = nni_aio_result(aio)) != 0) {
		goto done;
	}
	NNI_ASSERT((tpipe = aio->a_pipe) != NULL);

	rv = nni_pipe_create(&pipe, ep->ep_sock, ep->ep_tran);
	if (rv != 0) {
		ops->p_fini(tpipe);
		goto done;
	}

done:

	switch (rv) {
	case 0:
		pipe->p_tran_ops  = *ops;
		pipe->p_tran_data = tpipe;
		nni_pipe_start(pipe);
		nni_ep_accept_start(ep);
		break;
	case NNG_ECLOSED:
	case NNG_ECANCELED:
		// Canceled or closed, no furhter action.
		break;
	case NNG_ECONNABORTED:
	case NNG_ECONNRESET:
		// These are remote conditions, no cool down.
		// cooldown = 0;
		nni_ep_accept_start(ep);
		break;
	case NNG_ENOMEM:
		// We're running low on memory, so its best to wait
		// a whole second to give the system a chance to
		// recover memory.
		// cooldown = 1000000;
		nni_ep_accept_start(ep);
		break;
	default:
		// other cases... sleep a tiny bit then try again.
		// cooldown = 1000; 10msec
		// Add a timeout here instead to avoid spinning.
		nni_ep_accept_start(ep);
		break;
	}

	nni_mtx_unlock(&ep->ep_mtx);
}

static void
nni_ep_accept_start(nni_ep *ep)
{
	nni_aio *aio = &ep->ep_acc_aio;

	// Call with the Endpoint lock held.
	if (ep->ep_closed) {
		return;
	}

	aio->a_endpt = ep->ep_data;
	ep->ep_ops.ep_accept(ep->ep_data, aio);
}

int
nni_ep_listen(nni_ep *ep, int flags)
{
	int rv = 0;

	nni_mtx_lock(&ep->ep_mtx);
	if (ep->ep_mode != NNI_EP_MODE_LISTEN) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_ENOTSUP);
	}
	if (ep->ep_started) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_EBUSY);
	}
	if (ep->ep_closed) {
		nni_mtx_unlock(&ep->ep_mtx);
		return (NNG_ECLOSED);
	}

	ep->ep_started = 1;

	rv = ep->ep_ops.ep_bind(ep->ep_data);
	if (rv != 0) {
		ep->ep_started = 0;
		nni_mtx_unlock(&ep->ep_mtx);
		return (rv);
	}
	ep->ep_bound = 1;

	nni_ep_accept_start(ep);
	nni_mtx_unlock(&ep->ep_mtx);

	return (0);
}

void
nni_ep_list_init(nni_list *list)
{
	NNI_LIST_INIT(list, nni_ep, ep_node);
}
