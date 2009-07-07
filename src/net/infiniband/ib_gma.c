/*
 * Copyright (C) 2009 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <byteswap.h>
#include <gpxe/infiniband.h>
#include <gpxe/iobuf.h>
#include <gpxe/ib_gma.h>

/**
 * @file
 *
 * Infiniband General Management Agent
 *
 */

/** A MAD request */
struct ib_mad_request {
	/** Associated GMA */
	struct ib_gma *gma;
	/** List of outstanding MAD requests */
	struct list_head list;
	/** Retry timer */
	struct retry_timer timer;
	/** Destination address */
	struct ib_address_vector av;
	/** MAD request */
	union ib_mad mad;
};

/** GMA number of send WQEs
 *
 * This is a policy decision.
 */
#define IB_GMA_NUM_SEND_WQES 4

/** GMA number of receive WQEs
 *
 * This is a policy decision.
 */
#define IB_GMA_NUM_RECV_WQES 2

/** GMA number of completion queue entries
 *
 * This is a policy decision
 */
#define IB_GMA_NUM_CQES 8

/** GMA TID magic signature */
#define IB_GMA_TID_MAGIC ( ( 'g' << 24 ) | ( 'P' << 16 ) | ( 'X' << 8 ) | 'E' )

/** TID to use for next MAD request */
static unsigned int next_request_tid;

/**
 * Identify attribute handler
 *
 * @v mgmt_class	Management class
 * @v class_version	Class version
 * @v method		Method
 * @v attr_id		Attribute ID (in network byte order)
 * @ret handler		Attribute handler (or NULL)
 */
static int ib_handle_mad ( struct ib_device *ibdev,
			   union ib_mad *mad ) {
	struct ib_mad_hdr *hdr = &mad->hdr;
	struct ib_mad_handler *handler;

	for_each_table_entry ( handler, IB_MAD_HANDLERS ) {
		if ( ( handler->mgmt_class == hdr->mgmt_class ) &&
		     ( handler->class_version == hdr->class_version ) &&
		     ( handler->method == hdr->method ) &&
		     ( handler->attr_id == hdr->attr_id ) ) {
			hdr->method = handler->resp_method;
			return handler->handle ( ibdev, mad );
		}
	}

	hdr->method = IB_MGMT_METHOD_TRAP;
	hdr->status = htons ( IB_MGMT_STATUS_UNSUPPORTED_METHOD_ATTR );
	return -ENOTSUP;
}

/**
 * Complete GMA receive
 *
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v av		Address vector
 * @v iobuf		I/O buffer
 * @v rc		Completion status code
 */
static void ib_gma_complete_recv ( struct ib_device *ibdev,
				   struct ib_queue_pair *qp,
				   struct ib_address_vector *av,
				   struct io_buffer *iobuf, int rc ) {
	struct ib_gma *gma = ib_qp_get_ownerdata ( qp );
	struct ib_mad_request *request;
	union ib_mad *mad;
	struct ib_mad_hdr *hdr;
	unsigned int hop_pointer;
	unsigned int hop_count;

	/* Ignore errors */
	if ( rc != 0 ) {
		DBGC ( gma, "GMA %p RX error: %s\n", gma, strerror ( rc ) );
		goto out;
	}

	/* Sanity checks */
	if ( iob_len ( iobuf ) != sizeof ( *mad ) ) {
		DBGC ( gma, "GMA %p RX bad size (%zd bytes)\n",
		       gma, iob_len ( iobuf ) );
		DBGC_HDA ( gma, 0, iobuf->data, iob_len ( iobuf ) );
		goto out;
	}
	mad = iobuf->data;
	hdr = &mad->hdr;
	if ( hdr->base_version != IB_MGMT_BASE_VERSION ) {
		DBGC ( gma, "GMA %p unsupported base version %x\n",
		       gma, hdr->base_version );
		DBGC_HDA ( gma, 0, mad, sizeof ( *mad ) );
		goto out;
	}
	DBGC ( gma, "GMA %p RX TID %08x%08x (%02x,%02x,%02x,%04x) status "
	       "%04x\n", gma, ntohl ( hdr->tid[0] ), ntohl ( hdr->tid[1] ),
	       hdr->mgmt_class, hdr->class_version, hdr->method,
	       ntohs ( hdr->attr_id ), ntohs ( hdr->status ) );
	DBGC2_HDA ( gma, 0, mad, sizeof ( *mad ) );

	/* Dequeue request if applicable */
	list_for_each_entry ( request, &gma->requests, list ) {
		if ( memcmp ( &request->mad.hdr.tid, &hdr->tid,
			      sizeof ( request->mad.hdr.tid ) ) == 0 ) {
			stop_timer ( &request->timer );
			list_del ( &request->list );
			free ( request );
			break;
		}
	}

	/* Handle MAD, if possible */
	if ( ( rc = ib_handle_mad ( ibdev, mad ) ) != 0 ) {
		DBGC ( gma, "GMA %p could not handle TID %08x%08x: %s\n",
		       gma, ntohl ( hdr->tid[0] ), ntohl ( hdr->tid[1] ),
		       strerror ( rc ) );
		/* Do not abort; we may want to send an error response */
	}

	/* Finish processing if we have no response to send */
	if ( ! hdr->method )
		goto out;

	DBGC ( gma, "GMA %p TX TID %08x%08x (%02x,%02x,%02x,%04x)\n", gma,
	       ntohl ( hdr->tid[0] ), ntohl ( hdr->tid[1] ), hdr->mgmt_class,
	       hdr->class_version, hdr->method, ntohs ( hdr->attr_id ) );
	DBGC2_HDA ( gma, 0, mad, sizeof ( *mad ) );

	/* Set response fields for directed route SMPs */
	if ( hdr->mgmt_class == IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE ) {
		struct ib_mad_smp *smp = &mad->smp;

		hdr->status |= htons ( IB_SMP_STATUS_D_INBOUND );
		hop_pointer = smp->mad_hdr.class_specific.smp.hop_pointer;
		hop_count = smp->mad_hdr.class_specific.smp.hop_count;
		assert ( hop_count == hop_pointer );
		if ( hop_pointer < ( sizeof ( smp->return_path.hops ) /
				     sizeof ( smp->return_path.hops[0] ) ) ) {
			smp->return_path.hops[hop_pointer] = ibdev->port;
		} else {
			DBGC ( gma, "GMA %p invalid hop pointer %d\n",
			       gma, hop_pointer );
			goto out;
		}
	}

	/* Construct return address */
	av->qkey = ( ( av->qpn == IB_QPN_SMA ) ? IB_QKEY_SMA : IB_QKEY_GMA );
	av->rate = IB_RATE_2_5;

	/* Send MAD response, if applicable */
	if ( ( rc = ib_post_send ( ibdev, qp, av,
				   iob_disown ( iobuf ) ) ) != 0 ) {
		DBGC ( gma, "GMA %p could not send MAD response: %s\n",
		       gma, strerror ( rc ) );
		goto out;
	}

 out:
	free_iob ( iobuf );
}

/**
 * Complete GMA send
 *
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v iobuf		I/O buffer
 * @v rc		Completion status code
 */
static void ib_gma_complete_send ( struct ib_device *ibdev __unused,
				   struct ib_queue_pair *qp,
				   struct io_buffer *iobuf, int rc ) {
	struct ib_gma *gma = ib_qp_get_ownerdata ( qp );

	if ( rc != 0 ) {
		DBGC ( gma, "GMA %p send completion error: %s\n",
		       gma, strerror ( rc ) );
	}
	free_iob ( iobuf );
}

/** GMA completion operations */
static struct ib_completion_queue_operations ib_gma_completion_ops = {
	.complete_send = ib_gma_complete_send,
	.complete_recv = ib_gma_complete_recv,
};

/**
 * Handle MAD request timer expiry
 *
 * @v timer		Retry timer
 * @v expired		Failure indicator
 */
static void ib_gma_timer_expired ( struct retry_timer *timer, int expired ) {
	struct ib_mad_request *request =
		container_of ( timer, struct ib_mad_request, timer );
	struct ib_gma *gma = request->gma;
	struct ib_device *ibdev = gma->ibdev;
	struct io_buffer *iobuf;
	int rc;

	/* Abandon TID if we have tried too many times */
	if ( expired ) {
		DBGC ( gma, "GMA %p abandoning TID %08x%08x\n",
		       gma, ntohl ( request->mad.hdr.tid[0] ),
		       ntohl ( request->mad.hdr.tid[1] ) );
		list_del ( &request->list );
		free ( request );
		return;
	}

	DBGC ( gma, "GMA %p TX TID %08x%08x (%02x,%02x,%02x,%04x)\n",
	       gma, ntohl ( request->mad.hdr.tid[0] ),
	       ntohl ( request->mad.hdr.tid[1] ), request->mad.hdr.mgmt_class,
	       request->mad.hdr.class_version, request->mad.hdr.method,
	       ntohs ( request->mad.hdr.attr_id ) );
	DBGC2_HDA ( gma, 0, &request->mad, sizeof ( request->mad ) );

	/* Restart retransmission timer */
	start_timer ( timer );

	/* Construct I/O buffer */
	iobuf = alloc_iob ( sizeof ( request->mad ) );
	if ( ! iobuf ) {
		DBGC ( gma, "GMA %p could not allocate buffer for TID "
		       "%08x%08x\n", gma, ntohl ( request->mad.hdr.tid[0] ),
		       ntohl ( request->mad.hdr.tid[1] ) );
		return;
	}
	memcpy ( iob_put ( iobuf, sizeof ( request->mad ) ), &request->mad,
		 sizeof ( request->mad ) );

	/* Post send request */
	if ( ( rc = ib_post_send ( ibdev, gma->qp, &request->av,
				   iobuf ) ) != 0 ) {
		DBGC ( gma, "GMA %p could not send TID %08x%08x: %s\n",
		       gma,  ntohl ( request->mad.hdr.tid[0] ),
		       ntohl ( request->mad.hdr.tid[1] ), strerror ( rc ) );
		free_iob ( iobuf );
		return;
	}
}

/**
 * Issue MAD request
 *
 * @v gma		General management agent
 * @v mad		MAD request
 * @v av		Destination address, or NULL for SM
 * @ret rc		Return status code
 */
int ib_gma_request ( struct ib_gma *gma, union ib_mad *mad,
		     struct ib_address_vector *av ) {
	struct ib_device *ibdev = gma->ibdev;
	struct ib_mad_request *request;

	/* Allocate and initialise structure */
	request = zalloc ( sizeof ( *request ) );
	if ( ! request ) {
		DBGC ( gma, "GMA %p could not allocate MAD request\n", gma );
		return -ENOMEM;
	}
	request->gma = gma;
	list_add ( &request->list, &gma->requests );
	request->timer.expired = ib_gma_timer_expired;

	/* Determine address vector */
	if ( av ) {
		memcpy ( &request->av, av, sizeof ( request->av ) );
	} else {
		request->av.lid = ibdev->sm_lid;
		request->av.sl = ibdev->sm_sl;
		request->av.qpn = IB_QPN_GMA;
		request->av.qkey = IB_QKEY_GMA;
	}

	/* Copy MAD body */
	memcpy ( &request->mad, mad, sizeof ( request->mad ) );

	/* Allocate TID */
	request->mad.hdr.tid[0] = htonl ( IB_GMA_TID_MAGIC );
	request->mad.hdr.tid[1] = htonl ( ++next_request_tid );

	/* Start timer to initiate transmission */
	start_timer_nodelay ( &request->timer );

	return 0;
}

/**
 * Create GMA
 *
 * @v gma		General management agent
 * @v ibdev		Infiniband device
 * @v qkey		Queue key
 * @ret rc		Return status code
 */
int ib_create_gma ( struct ib_gma *gma, struct ib_device *ibdev,
		    unsigned long qkey ) {
	int rc;

	/* Initialise fields */
	memset ( gma, 0, sizeof ( *gma ) );
	gma->ibdev = ibdev;
	INIT_LIST_HEAD ( &gma->requests );

	/* Create completion queue */
	gma->cq = ib_create_cq ( ibdev, IB_GMA_NUM_CQES,
				 &ib_gma_completion_ops );
	if ( ! gma->cq ) {
		DBGC ( gma, "GMA %p could not allocate completion queue\n",
		       gma );
		rc = -ENOMEM;
		goto err_create_cq;
	}

	/* Create queue pair */
	gma->qp = ib_create_qp ( ibdev, IB_GMA_NUM_SEND_WQES, gma->cq,
				 IB_GMA_NUM_RECV_WQES, gma->cq, qkey );
	if ( ! gma->qp ) {
		DBGC ( gma, "GMA %p could not allocate queue pair\n", gma );
		rc = -ENOMEM;
		goto err_create_qp;
	}
	ib_qp_set_ownerdata ( gma->qp, gma );

	DBGC ( gma, "GMA %p running on QPN %#lx\n", gma, gma->qp->qpn );

	/* Fill receive ring */
	ib_refill_recv ( ibdev, gma->qp );
	return 0;

	ib_destroy_qp ( ibdev, gma->qp );
 err_create_qp:
	ib_destroy_cq ( ibdev, gma->cq );
 err_create_cq:
	return rc;
}

/**
 * Destroy GMA
 *
 * @v gma		General management agent
 */
void ib_destroy_gma ( struct ib_gma *gma ) {
	struct ib_device *ibdev = gma->ibdev;
	struct ib_mad_request *request;
	struct ib_mad_request *tmp;

	/* Flush any outstanding requests */
	list_for_each_entry_safe ( request, tmp, &gma->requests, list ) {
		stop_timer ( &request->timer );
		list_del ( &request->list );
		free ( request );
	}

	ib_destroy_qp ( ibdev, gma->qp );
	ib_destroy_cq ( ibdev, gma->cq );
}