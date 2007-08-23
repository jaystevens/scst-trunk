/*
 *  Copyright (C) 2002-2003 Ardis Technolgies <roman@ardistech.com>
 *  Copyright (C) 2007 Vladislav Bolkhovitin
 *  Copyright (C) 2007 CMS Distribution Limited
 * 
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/hash.h>
#include <linux/kthread.h>
#include <net/tcp.h>
#include <scsi/scsi.h>

#include "iscsi.h"
#include "digest.h"

#ifndef NET_PAGE_CALLBACKS_DEFINED
#warning Patch put_page_callback.patch not applied on your kernel. ISCSI-SCST \
	will run in the performance degraded mode. Refer README file for \
	details.
#endif

#define ISCSI_INIT_WRITE_WAKE		0x1
#define ISCSI_INIT_WRITE_REMOVE_HASH	0x2

static int ctr_major;
static char ctr_name[] = "iscsi-scst-ctl";
static int iscsi_template_registered;

#if defined(DEBUG) || defined(TRACING)
unsigned long iscsi_trace_flag = ISCSI_DEFAULT_LOG_FLAGS;
#endif

static struct kmem_cache *iscsi_cmnd_cache;

spinlock_t iscsi_rd_lock = SPIN_LOCK_UNLOCKED;
LIST_HEAD(iscsi_rd_list);
DECLARE_WAIT_QUEUE_HEAD(iscsi_rd_waitQ);

spinlock_t iscsi_wr_lock = SPIN_LOCK_UNLOCKED;
LIST_HEAD(iscsi_wr_list);
DECLARE_WAIT_QUEUE_HEAD(iscsi_wr_waitQ);

static char dummy_data[1024];

struct iscsi_thread_t {
	struct task_struct *thr;
	struct list_head threads_list_entry;
};

static LIST_HEAD(iscsi_threads_list);

static void cmnd_remove_hash(struct iscsi_cmnd *cmnd);
static void iscsi_send_task_mgmt_resp(struct iscsi_cmnd *req, int status);
static void cmnd_prepare_skip_pdu(struct iscsi_cmnd *cmnd);

static inline u32 cmnd_write_size(struct iscsi_cmnd *cmnd)
{
	struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);

	if (hdr->flags & ISCSI_CMD_WRITE)
		return be32_to_cpu(hdr->data_length);
	return 0;
}

static inline u32 cmnd_read_size(struct iscsi_cmnd *cmnd)
{
	struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);

	if (hdr->flags & ISCSI_CMD_READ) {
		struct iscsi_rlength_ahdr *ahdr =
			(struct iscsi_rlength_ahdr *)cmnd->pdu.ahs;

		if (!(hdr->flags & ISCSI_CMD_WRITE))
			return be32_to_cpu(hdr->data_length);
		if (ahdr && ahdr->ahstype == ISCSI_AHSTYPE_RLENGTH)
			return be32_to_cpu(ahdr->read_length);
	}
	return 0;
}

static inline void iscsi_restart_cmnd(struct iscsi_cmnd *cmnd)
{
	cmnd->scst_state = ISCSI_CMD_STATE_RESTARTED;
	scst_restart_cmd(cmnd->scst_cmd, SCST_PREPROCESS_STATUS_SUCCESS,
		SCST_CONTEXT_THREAD);
}

struct iscsi_cmnd *cmnd_alloc(struct iscsi_conn *conn, struct iscsi_cmnd *parent)
{
	struct iscsi_cmnd *cmnd;

	/* ToDo: __GFP_NOFAIL?? */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	cmnd = kmem_cache_alloc(iscsi_cmnd_cache, GFP_KERNEL|__GFP_NOFAIL);
	memset(cmnd, 0, sizeof(*cmnd));
#else
	cmnd = kmem_cache_zalloc(iscsi_cmnd_cache, GFP_KERNEL|__GFP_NOFAIL);
#endif

	atomic_set(&cmnd->ref_cnt, 1);
	cmnd->scst_state = ISCSI_CMD_STATE_NEW;
	cmnd->conn = conn;
	cmnd->parent_req = parent;
	init_waitqueue_head(&cmnd->scst_waitQ);

	if (parent == NULL) {
		atomic_inc(&conn->conn_ref_cnt);
#ifdef NET_PAGE_CALLBACKS_DEFINED
		atomic_set(&cmnd->net_ref_cnt, 0);
#endif		
		cmnd->target = conn->target;
		spin_lock_init(&cmnd->rsp_cmd_lock);
		INIT_LIST_HEAD(&cmnd->rsp_cmd_list);
		INIT_LIST_HEAD(&cmnd->rx_ddigest_cmd_list);

		spin_lock_bh(&conn->cmd_list_lock);
		list_add_tail(&cmnd->cmd_list_entry, &conn->cmd_list);
		spin_unlock_bh(&conn->cmd_list_lock);
	}

	TRACE_DBG("conn %p, parent %p, cmnd %p", conn, parent, cmnd);
	return cmnd;
}

/* Frees a command. Also frees the additional header. */
void cmnd_free(struct iscsi_cmnd *cmnd)
{
	TRACE_DBG("%p", cmnd);

	/* Catch users from cmd_list or rsp_cmd_list */
	EXTRACHECKS_BUG_ON(atomic_read(&cmnd->ref_cnt) != 0);

	kfree(cmnd->pdu.ahs);

	if (unlikely(cmnd->on_write_list)) {
		struct iscsi_scsi_cmd_hdr *req = cmnd_hdr(cmnd);

		PRINT_ERROR_PR("cmnd %p still on some list?, %x, %x, %x, %x, %x, %x, %x",
			cmnd, req->opcode, req->scb[0], req->flags, req->itt,
			be32_to_cpu(req->data_length),
			req->cmd_sn, be32_to_cpu(cmnd->pdu.datasize));

		if (cmnd->parent_req) {
			struct iscsi_scsi_cmd_hdr *req =
					cmnd_hdr(cmnd->parent_req);
			PRINT_ERROR_PR("%p %x %u", req, req->opcode, req->scb[0]);
		}
		sBUG();
	}

	kmem_cache_free(iscsi_cmnd_cache, cmnd);
	return;
}

void cmnd_done(struct iscsi_cmnd *cmnd)
{
	TRACE_DBG("%p", cmnd);

	if (unlikely(cmnd->tmfabort)) {
		TRACE_MGMT_DBG("Done aborted cmd %p (scst cmd %p, state %d)",
			cmnd, cmnd->scst_cmd, cmnd->scst_state);
	}

	if (cmnd->parent_req == NULL) {
		struct iscsi_conn *conn = cmnd->conn;
		TRACE_DBG("Deleting req %p from conn %p", cmnd, conn);
		spin_lock_bh(&conn->cmd_list_lock);
		list_del(&cmnd->cmd_list_entry);
		spin_unlock_bh(&conn->cmd_list_lock);

		smp_mb__before_atomic_dec();
		atomic_dec(&conn->conn_ref_cnt);

		EXTRACHECKS_BUG_ON(!list_empty(&cmnd->rsp_cmd_list));
		EXTRACHECKS_BUG_ON(!list_empty(&cmnd->rx_ddigest_cmd_list));

		/* Order between above and below code is important! */

		if (cmnd->scst_cmd) {
			switch(cmnd->scst_state) {
			case ISCSI_CMD_STATE_AFTER_PREPROC:
				TRACE_DBG("%s", "AFTER_PREPROC");
				cmnd->scst_state = ISCSI_CMD_STATE_RESTARTED;
				scst_restart_cmd(cmnd->scst_cmd,
					SCST_PREPROCESS_STATUS_ERROR_FATAL,
					SCST_CONTEXT_THREAD);
				break;
			case ISCSI_CMD_STATE_PROCESSED:
				TRACE_DBG("%s", "PROCESSED");
				scst_tgt_cmd_done(cmnd->scst_cmd);
				break;
			default:
				PRINT_ERROR_PR("Unexpected cmnd scst state %d",
					cmnd->scst_state);
				sBUG();
				break;
			}
		}
	} else {
		EXTRACHECKS_BUG_ON(cmnd->scst_cmd != NULL);

		spin_lock_bh(&cmnd->parent_req->rsp_cmd_lock);
		TRACE_DBG("Deleting rsp %p from parent %p", cmnd,
			cmnd->parent_req);
		list_del(&cmnd->rsp_cmd_list_entry);
		spin_unlock_bh(&cmnd->parent_req->rsp_cmd_lock);

		cmnd_put(cmnd->parent_req);
	}

	/* Order between above and below code is important! */

	if (cmnd->own_sg) {
		TRACE_DBG("%s", "own_sg");
		scst_free(cmnd->sg, cmnd->sg_cnt);
#ifdef DEBUG
		cmnd->own_sg = 0;
		cmnd->sg = NULL;
		cmnd->sg_cnt = -1;
#endif
	}

	cmnd_free(cmnd);
	return;
}

void req_cmnd_release_force(struct iscsi_cmnd *req, int flags)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_conn *conn = req->conn;

	TRACE_ENTRY();

	TRACE_DBG("%p", req);

	if (flags & ISCSI_FORCE_RELEASE_WRITE) {
		spin_lock(&conn->write_list_lock);
		while(!list_empty(&conn->write_list)) {
			rsp = list_entry(conn->write_list.next,
				struct iscsi_cmnd, write_list_entry);
			cmd_del_from_write_list(rsp);
			spin_unlock(&conn->write_list_lock);

			cmnd_put(rsp);

			spin_lock(&conn->write_list_lock);
		}
		spin_unlock(&conn->write_list_lock);
	}

again:
	spin_lock_bh(&req->rsp_cmd_lock);
	list_for_each_entry(rsp, &req->rsp_cmd_list, rsp_cmd_list_entry) {
		int f;

		if (rsp->on_write_list || rsp->write_processing_started ||
		    rsp->force_cleanup_done)
			continue;

		spin_unlock_bh(&req->rsp_cmd_lock);

		/* 
		 * Recheck is necessary to not take write_list_lock under
		 * rsp_cmd_lock.
		 */
		spin_lock(&conn->write_list_lock);
		f = rsp->on_write_list || rsp->write_processing_started ||
			rsp->force_cleanup_done;
		spin_unlock(&conn->write_list_lock);
		if (f)
			goto again;

		rsp->force_cleanup_done = 1;
		cmnd_put(rsp);

		goto again;
	}
	spin_unlock_bh(&req->rsp_cmd_lock);

	req_cmnd_release(req);

	TRACE_EXIT();
	return;
}

void req_cmnd_release(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *c, *t;

	TRACE_ENTRY();

	TRACE_DBG("%p", req);

#ifdef EXTRACHECKS
	sBUG_ON(req->release_called);
	req->release_called = 1;
#endif

	if (unlikely(req->tmfabort)) {
		TRACE_MGMT_DBG("Release aborted req cmd %p (scst cmd %p, "
			"state %d)", req, req->scst_cmd, req->scst_state);
	}

	sBUG_ON(req->parent_req != NULL);

	list_for_each_entry_safe(c, t, &req->rx_ddigest_cmd_list,
				rx_ddigest_cmd_list_entry) {
		TRACE_DBG("Deleting RX ddigest cmd %p from digest "
			"list of req %p", c, req);
		list_del(&c->rx_ddigest_cmd_list_entry);
		cmnd_put(c);
	}

	if (req->hashed)
		cmnd_remove_hash(req);

	cmnd_put(req);

	TRACE_EXIT();
	return;
}

void rsp_cmnd_release(struct iscsi_cmnd *cmnd)
{
	TRACE_DBG("%p", cmnd);

#ifdef EXTRACHECKS
	sBUG_ON(cmnd->release_called);
	cmnd->release_called = 1;
#endif

	sBUG_ON(cmnd->hashed);
	sBUG_ON(cmnd->parent_req == NULL);

	if (unlikely(cmnd->tmfabort)) {
		TRACE_MGMT_DBG("Release aborted rsp cmd %p (parent req %p, "
			"scst cmd %p, state %d)", cmnd, cmnd->parent_req,
			cmnd->parent_req->scst_cmd,
			cmnd->parent_req->scst_state);
	}

	cmnd_put(cmnd);
	return;
}

/**
 * create a new command used as response.
 *
 * iscsi_cmnd_create_rsp_cmnd - 
 * @cmnd: ptr to request command
 *
 * @return    ptr to response command or NULL
 */
static struct iscsi_cmnd *iscsi_cmnd_create_rsp_cmnd(struct iscsi_cmnd *parent)
{
	struct iscsi_cmnd *rsp = cmnd_alloc(parent->conn, parent);

	spin_lock_bh(&parent->rsp_cmd_lock);
	TRACE_DBG("Adding rsp %p to parent %p", rsp, parent);
	list_add_tail(&rsp->rsp_cmd_list_entry, &parent->rsp_cmd_list);
	spin_unlock_bh(&parent->rsp_cmd_lock);
	cmnd_get(parent);
	return rsp;
}

static inline struct iscsi_cmnd *get_rsp_cmnd(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *res;

	/* Currently this lock isn't needed, but just in case.. */
	spin_lock_bh(&req->rsp_cmd_lock);
	res = list_entry(req->rsp_cmd_list.prev, struct iscsi_cmnd,
		rsp_cmd_list_entry);
	spin_unlock_bh(&req->rsp_cmd_lock);

	return res;
}

static void iscsi_cmnds_init_write(struct list_head *send, int flags)
{
	struct iscsi_cmnd *cmnd = list_entry(send->next, struct iscsi_cmnd, 
						write_list_entry);
	struct iscsi_conn *conn = cmnd->conn;
	struct list_head *pos, *next;

	/*
	 * If we don't remove hashed req cmd from the hash list here, before
	 * submitting it for transmittion, we will have a race, when for
	 * some reason cmd's release is delayed after transmittion and
	 * initiator sends cmd with the same ITT => this command will be
	 * erroneously rejected as a duplicate.
	 */
	if ((flags & ISCSI_INIT_WRITE_REMOVE_HASH) && cmnd->parent_req->hashed &&
	    (cmnd->parent_req->outstanding_r2t == 0))
		cmnd_remove_hash(cmnd->parent_req);

	list_for_each_safe(pos, next, send) {
		cmnd = list_entry(pos, struct iscsi_cmnd, write_list_entry);

		TRACE_DBG("%p:%x", cmnd, cmnd_opcode(cmnd));

		sBUG_ON(conn != cmnd->conn);

		if (!(conn->ddigest_type & DIGEST_NONE) &&
		    (cmnd->pdu.datasize != 0))
			digest_tx_data(cmnd);

		list_del(&cmnd->write_list_entry);

		spin_lock(&conn->write_list_lock);
		cmd_add_on_write_list(conn, cmnd);
		spin_unlock(&conn->write_list_lock);
	}
	

	if (flags & ISCSI_INIT_WRITE_WAKE)
		iscsi_make_conn_wr_active(conn);
}

static void iscsi_cmnd_init_write(struct iscsi_cmnd *cmnd, int flags)
{
	LIST_HEAD(head);

	if (unlikely(cmnd->on_write_list)) {
		PRINT_ERROR_PR("cmd already on write list (%x %x %x %x %u %u "
			"%u %u %u %u %u %d %d",
			cmnd_itt(cmnd), cmnd_ttt(cmnd), cmnd_opcode(cmnd),
			cmnd_scsicode(cmnd), cmnd->r2t_sn,
			cmnd->r2t_length, cmnd->is_unsolicited_data,
			cmnd->target_task_tag, cmnd->outstanding_r2t,
			cmnd->hdigest, cmnd->ddigest,
			list_empty(&cmnd->rsp_cmd_list), cmnd->hashed);
		sBUG();
	}
	list_add(&cmnd->write_list_entry, &head);
	iscsi_cmnds_init_write(&head, flags);
}

static void iscsi_set_datasize(struct iscsi_cmnd *cmnd, u32 offset, u32 size)
{
	cmnd->pdu.datasize = size;

	if (cmnd->pdu.datasize & 3) {
	    	int idx = (offset + cmnd->pdu.datasize) >> PAGE_SHIFT;
		u8 *p = (u8 *)page_address(cmnd->sg[idx].page) + 
			((offset + cmnd->pdu.datasize) & ~PAGE_MASK);
		int i = 4 - (cmnd->pdu.datasize & 3);
		while (i--) 
		    *p++ = 0;
	}
}

static void send_data_rsp(struct iscsi_cmnd *req, u8 status, int send_status)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);
	struct iscsi_data_in_hdr *rsp_hdr;
	u32 pdusize, expsize, scsisize, size, offset, sn;
	LIST_HEAD(send);

	TRACE_DBG("req %p", req);
	pdusize = req->conn->session->sess_param.max_xmit_data_length;
	expsize = cmnd_read_size(req);
	size = min(expsize, (u32)req->bufflen);
	offset = 0;
	sn = 0;

	while (1) {
		rsp = iscsi_cmnd_create_rsp_cmnd(req);
		TRACE_DBG("rsp %p", rsp);
		rsp->sg = req->sg;
		rsp->bufflen = req->bufflen;
		rsp_hdr = (struct iscsi_data_in_hdr *)&rsp->pdu.bhs;

		rsp_hdr->opcode = ISCSI_OP_SCSI_DATA_IN;
		rsp_hdr->itt = req_hdr->itt;
		rsp_hdr->ttt = cpu_to_be32(ISCSI_RESERVED_TAG);
		rsp_hdr->buffer_offset = cpu_to_be32(offset);
		rsp_hdr->data_sn = cpu_to_be32(sn);

		if (size <= pdusize) {
			iscsi_set_datasize(rsp, offset, size);
			if (send_status) {
				TRACE_DBG("status %x", status);
				rsp_hdr->flags =
					ISCSI_FLG_FINAL | ISCSI_FLG_STATUS;
				rsp_hdr->cmd_status = status;
			}
			scsisize = req->bufflen;
			if (scsisize < expsize) {
				rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
				size = expsize - scsisize;
			} else if (scsisize > expsize) {
				rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_OVERFLOW;
				size = scsisize - expsize;
			} else
				size = 0;
			rsp_hdr->residual_count = cpu_to_be32(size);
			list_add_tail(&rsp->write_list_entry, &send);
			break;
		}

		iscsi_set_datasize(rsp, offset, pdusize);

		size -= pdusize;
		offset += pdusize;
		sn++;

		list_add_tail(&rsp->write_list_entry, &send);
	}
	iscsi_cmnds_init_write(&send, ISCSI_INIT_WRITE_REMOVE_HASH);
}

static struct iscsi_cmnd *create_status_rsp(struct iscsi_cmnd *req, int status,
	const u8 *sense_buf, int sense_len)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_scsi_rsp_hdr *rsp_hdr;
	struct iscsi_sense_data *sense;
	struct scatterlist *sg;

	rsp = iscsi_cmnd_create_rsp_cmnd(req);
	TRACE_DBG("%p", rsp);

	rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;
	rsp_hdr->opcode = ISCSI_OP_SCSI_RSP;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->response = ISCSI_RESPONSE_COMMAND_COMPLETED;
	rsp_hdr->cmd_status = status;
	rsp_hdr->itt = cmnd_hdr(req)->itt;

	if (status == SAM_STAT_CHECK_CONDITION) {
		TRACE_DBG("%s", "CHECK_CONDITION");
		/* ToDo: __GFP_NOFAIL ?? */
		sg = rsp->sg = scst_alloc(PAGE_SIZE, GFP_KERNEL|__GFP_NOFAIL, 0,
					&rsp->sg_cnt);
		if (sg == NULL) {
			/* ToDo(); */
		}
		rsp->own_sg = 1;
		sense = (struct iscsi_sense_data *)page_address(sg[0].page);
		sense->length = cpu_to_be16(sense_len);
		memcpy(sense->data, sense_buf, sense_len);
		rsp->pdu.datasize = sizeof(struct iscsi_sense_data) + sense_len;
		rsp->bufflen = (rsp->pdu.datasize + 3) & -4;
		if (rsp->bufflen - rsp->pdu.datasize) {
		    int i = rsp->pdu.datasize;
		    u8 *p = (u8 *)sense + i;
		    
		    while (i < rsp->bufflen) {
			*p ++ = 0;
			i++;
		    }
		}
	} else {
		rsp->pdu.datasize = 0;
		rsp->bufflen = 0;
	}

	return rsp;
}

static struct iscsi_cmnd *create_sense_rsp(struct iscsi_cmnd *req,
	u8 sense_key, u8 asc, u8 ascq)
{
	u8 sense[14];
	memset(sense, 0, sizeof(sense));
	sense[0] = 0xf0;
	sense[2] = sense_key;
	sense[7] = 6;	// Additional sense length
	sense[12] = asc;
	sense[13] = ascq;
	return create_status_rsp(req, SAM_STAT_CHECK_CONDITION, sense,
		sizeof(sense));
}

static void iscsi_cmnd_reject(struct iscsi_cmnd *req, int reason)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_reject_hdr *rsp_hdr;
	struct scatterlist *sg;
	char *addr;

	TRACE_MGMT_DBG("Reject: req %p, reason %x", req, reason);

	rsp = iscsi_cmnd_create_rsp_cmnd(req);
	rsp_hdr = (struct iscsi_reject_hdr *)&rsp->pdu.bhs;

	rsp_hdr->opcode = ISCSI_OP_REJECT;
	rsp_hdr->ffffffff = ISCSI_RESERVED_TAG;
	rsp_hdr->reason = reason;

	/* ToDo: __GFP_NOFAIL ?? */
	sg = rsp->sg = scst_alloc(PAGE_SIZE, GFP_KERNEL|__GFP_NOFAIL, 0,
				&rsp->sg_cnt);
	if (sg == NULL) {
		/* ToDo(); */
	}
	rsp->own_sg = 1;
	addr = page_address(sg[0].page);
	clear_page(addr);
	memcpy(addr, &req->pdu.bhs, sizeof(struct iscsi_hdr));
	rsp->bufflen = rsp->pdu.datasize = sizeof(struct iscsi_hdr);
	cmnd_prepare_skip_pdu(req);

	req->pdu.bhs.opcode = ISCSI_OP_PDU_REJECT;
}

static u32 cmnd_set_sn(struct iscsi_cmnd *cmnd, int set_stat_sn)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct iscsi_session *sess = conn->session;
	u32 res;

	spin_lock(&sess->sn_lock);

	if (set_stat_sn)
		cmnd->pdu.bhs.sn = cpu_to_be32(conn->stat_sn++);
	cmnd->pdu.bhs.exp_sn = cpu_to_be32(sess->exp_cmd_sn);
	cmnd->pdu.bhs.max_sn = cpu_to_be32(sess->exp_cmd_sn + sess->max_queued_cmnds);

	res = cpu_to_be32(conn->stat_sn);

	spin_unlock(&sess->sn_lock);
	return res;
}

/* Called under sn_lock */
static void __update_stat_sn(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	u32 exp_stat_sn;

	cmnd->pdu.bhs.exp_sn = exp_stat_sn = be32_to_cpu(cmnd->pdu.bhs.exp_sn);
	TRACE_DBG("%x,%x", cmnd_opcode(cmnd), exp_stat_sn);
	if ((int)(exp_stat_sn - conn->exp_stat_sn) > 0 &&
	    (int)(exp_stat_sn - conn->stat_sn) <= 0) {
		// free pdu resources
		cmnd->conn->exp_stat_sn = exp_stat_sn;
	}
}

static inline void update_stat_sn(struct iscsi_cmnd *cmnd)
{
	spin_lock(&cmnd->conn->session->sn_lock);
	__update_stat_sn(cmnd);
	spin_unlock(&cmnd->conn->session->sn_lock);
}

/* Called under sn_lock */
static int check_cmd_sn(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	u32 cmd_sn;

	cmnd->pdu.bhs.sn = cmd_sn = be32_to_cpu(cmnd->pdu.bhs.sn);
	TRACE_DBG("%d(%d)", cmd_sn, session->exp_cmd_sn);
	if ((s32)(cmd_sn - session->exp_cmd_sn) >= 0)
		return 0;
	PRINT_ERROR_PR("sequence error (%x,%x)", cmd_sn, session->exp_cmd_sn);
	return -ISCSI_REASON_PROTOCOL_ERROR;
}

static inline struct iscsi_cmnd *__cmnd_find_hash(struct iscsi_session *session,
	u32 itt, u32 ttt)
{
	struct list_head *head;
	struct iscsi_cmnd *cmnd;

	head = &session->cmnd_hash[cmnd_hashfn(itt)];

	list_for_each_entry(cmnd, head, hash_list_entry) {
		if (cmnd->pdu.bhs.itt == itt) {
			if ((ttt != ISCSI_RESERVED_TAG) && (ttt != cmnd->target_task_tag))
				continue;
			return cmnd;
		}
	}
	return NULL;
}

static struct iscsi_cmnd *cmnd_find_hash(struct iscsi_session *session,
	u32 itt, u32 ttt)
{
	struct iscsi_cmnd *cmnd;

	spin_lock(&session->cmnd_hash_lock);
	cmnd = __cmnd_find_hash(session, itt, ttt);
	spin_unlock(&session->cmnd_hash_lock);

	return cmnd;
}

static struct iscsi_cmnd *cmnd_find_hash_get(struct iscsi_session *session,
	u32 itt, u32 ttt)
{
	struct iscsi_cmnd *cmnd;

	spin_lock(&session->cmnd_hash_lock);
	cmnd = __cmnd_find_hash(session, itt, ttt);
	cmnd_get(cmnd);
	spin_unlock(&session->cmnd_hash_lock);

	return cmnd;
}

static int cmnd_insert_hash(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	struct iscsi_cmnd *tmp;
	struct list_head *head;
	int err = 0;
	u32 itt = cmnd->pdu.bhs.itt;

	TRACE_DBG("%p:%x", cmnd, itt);
	if (itt == ISCSI_RESERVED_TAG) {
		err = -ISCSI_REASON_PROTOCOL_ERROR;
		goto out;
	}

	spin_lock(&session->cmnd_hash_lock);

	head = &session->cmnd_hash[cmnd_hashfn(cmnd->pdu.bhs.itt)];

	tmp = __cmnd_find_hash(session, itt, ISCSI_RESERVED_TAG);
	if (!tmp) {
		list_add_tail(&cmnd->hash_list_entry, head);
		cmnd->hashed = 1;
	} else
		err = -ISCSI_REASON_TASK_IN_PROGRESS;

	spin_unlock(&session->cmnd_hash_lock);

	if (!err) {
		spin_lock(&session->sn_lock);
		__update_stat_sn(cmnd);
		err = check_cmd_sn(cmnd);
		spin_unlock(&session->sn_lock);
	}

out:
	return err;
}

static void cmnd_remove_hash(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	struct iscsi_cmnd *tmp;

	spin_lock(&session->cmnd_hash_lock);

	tmp = __cmnd_find_hash(session, cmnd->pdu.bhs.itt, ISCSI_RESERVED_TAG);

	if (tmp && tmp == cmnd) {
		list_del(&cmnd->hash_list_entry);
		cmnd->hashed = 0;
	} else {
		PRINT_ERROR_PR("%p:%x not found", cmnd, cmnd_itt(cmnd));
	}

	spin_unlock(&session->cmnd_hash_lock);
}

static void cmnd_prepare_skip_pdu(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct scatterlist *sg = cmnd->sg;
	char *addr;
	u32 size;
	int i;

	TRACE_MGMT_DBG("Skipping (%p, %x %x %x %u, %p, scst state %d)", cmnd,
		cmnd_itt(cmnd), cmnd_opcode(cmnd), cmnd_hdr(cmnd)->scb[0],
		cmnd->pdu.datasize, cmnd->scst_cmd, cmnd->scst_state);

	iscsi_extracheck_is_rd_thread(conn);

	if (!(size = cmnd->pdu.datasize))
		return;

	if (sg == NULL) {
		/* ToDo: __GFP_NOFAIL ?? */
		sg = cmnd->sg = scst_alloc(PAGE_SIZE, GFP_KERNEL|__GFP_NOFAIL,
					0, &cmnd->sg_cnt);
		if (sg == NULL) {
			/* ToDo(); */
		}
		cmnd->own_sg = 1;
		cmnd->bufflen = PAGE_SIZE;
	}

	addr = page_address(sg[0].page);
	sBUG_ON(addr == NULL);
	size = (size + 3) & -4;
	conn->read_size = size;
	for (i = 0; size > PAGE_SIZE; i++, size -= cmnd->bufflen) {
		sBUG_ON(i >= ISCSI_CONN_IOV_MAX);
		conn->read_iov[i].iov_base = addr;
		conn->read_iov[i].iov_len = cmnd->bufflen;
	}
	conn->read_iov[i].iov_base = addr;
	conn->read_iov[i].iov_len = size;
	conn->read_msg.msg_iov = conn->read_iov;
	conn->read_msg.msg_iovlen = ++i;
}

static void cmnd_prepare_skip_pdu_set_resid(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_scsi_rsp_hdr *rsp_hdr;
	u32 size;

	TRACE_DBG("%p", req);

	rsp = get_rsp_cmnd(req);
	rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;
	if (cmnd_opcode(rsp) != ISCSI_OP_SCSI_RSP) {
		PRINT_ERROR_PR("unexpected response command %u", cmnd_opcode(rsp));
		return;
	}

	size = cmnd_write_size(req);
	if (size) {
		rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
		rsp_hdr->residual_count = cpu_to_be32(size);
	}
	size = cmnd_read_size(req);
	if (size) {
		if (cmnd_hdr(req)->flags & ISCSI_CMD_WRITE) {
			rsp_hdr->flags |= ISCSI_FLG_BIRESIDUAL_UNDERFLOW;
			rsp_hdr->bi_residual_count = cpu_to_be32(size);
		} else {
			rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
			rsp_hdr->residual_count = cpu_to_be32(size);
		}
	}
	req->pdu.bhs.opcode =
		(req->pdu.bhs.opcode & ~ISCSI_OPCODE_MASK) | ISCSI_OP_SCSI_REJECT;

	cmnd_prepare_skip_pdu(req);
}

static int cmnd_prepare_recv_pdu(struct iscsi_conn *conn,
	struct iscsi_cmnd *cmd,	u32 offset, u32 size)
{
	struct scatterlist *sg = cmd->sg;
	int bufflen = cmd->bufflen;
	int idx, i;
	char *addr;
	int res = 0;

	TRACE_DBG("%p %u,%u", cmd->sg, offset, size);

	iscsi_extracheck_is_rd_thread(conn);

	if ((offset >= bufflen) ||
	    (offset + size > bufflen)) {
		PRINT_ERROR_PR("Wrong ltn (%u %u %u)", offset, size, bufflen);
		mark_conn_closed(conn);
		res = -EIO;
		goto out;
	}

	offset += sg[0].offset;
	idx = offset >> PAGE_SHIFT;
	offset &= ~PAGE_MASK;

	conn->read_msg.msg_iov = conn->read_iov;
	conn->read_size = size = (size + 3) & -4;

	i = 0;
	while (1) {
		sBUG_ON(sg[idx].page == NULL);
		addr = page_address(sg[idx].page);
		sBUG_ON(addr == NULL);
		conn->read_iov[i].iov_base =  addr + offset;
		if (offset + size <= PAGE_SIZE) {
			TRACE_DBG("idx=%d, offset=%u, size=%d, addr=%p",
				idx, offset, size, addr);
			conn->read_iov[i].iov_len = size;
			conn->read_msg.msg_iovlen = ++i;
			break;
		}
		conn->read_iov[i].iov_len = PAGE_SIZE - offset;
		TRACE_DBG("idx=%d, offset=%u, size=%d, iov_len=%d, addr=%p",
			idx, offset, size, conn->read_iov[i].iov_len, addr);
		size -= conn->read_iov[i].iov_len;
		offset = 0;
		if (++i >= ISCSI_CONN_IOV_MAX) {
			PRINT_ERROR_PR("Initiator %s violated negotiated "
				"parameters by sending too much data (size "
				"left %d)", conn->session->initiator_name, size);
			mark_conn_closed(conn);
			res = -EINVAL;
			break;
		}
		idx++;
	}
	TRACE_DBG("msg_iov=%p, msg_iovlen=%d",
		conn->read_msg.msg_iov, conn->read_msg.msg_iovlen);

out:
	return res;
}

static void send_r2t(struct iscsi_cmnd *req)
{
	struct iscsi_session *session = req->conn->session;
	struct iscsi_cmnd *rsp;
	struct iscsi_r2t_hdr *rsp_hdr;
	u32 length, offset, burst;
	LIST_HEAD(send);

	/*
	 * There is no race with data_out_start() and __cmnd_abort(), since
	 * all functions called from single read thread
	 */
	iscsi_extracheck_is_rd_thread(req->conn);

	length = req->r2t_length;
	burst = session->sess_param.max_burst_length;
	offset = be32_to_cpu(cmnd_hdr(req)->data_length) - length;

	do {
		rsp = iscsi_cmnd_create_rsp_cmnd(req);
		rsp->pdu.bhs.ttt = req->target_task_tag;
		rsp_hdr = (struct iscsi_r2t_hdr *)&rsp->pdu.bhs;
		rsp_hdr->opcode = ISCSI_OP_R2T;
		rsp_hdr->flags = ISCSI_FLG_FINAL;
		memcpy(rsp_hdr->lun, cmnd_hdr(req)->lun, 8);
		rsp_hdr->itt = cmnd_hdr(req)->itt;
		rsp_hdr->r2t_sn = cpu_to_be32(req->r2t_sn++);
		rsp_hdr->buffer_offset = cpu_to_be32(offset);
		if (length > burst) {
			rsp_hdr->data_length = cpu_to_be32(burst);
			length -= burst;
			offset += burst;
		} else {
			rsp_hdr->data_length = cpu_to_be32(length);
			length = 0;
		}

		TRACE(TRACE_D_WRITE, "%x %u %u %u %u", cmnd_itt(req),
			be32_to_cpu(rsp_hdr->data_length),
			be32_to_cpu(rsp_hdr->buffer_offset),
			be32_to_cpu(rsp_hdr->r2t_sn), req->outstanding_r2t);

		list_add_tail(&rsp->write_list_entry, &send);

		if (++req->outstanding_r2t >= session->sess_param.max_outstanding_r2t)
			break;

	} while (length);

	iscsi_cmnds_init_write(&send, ISCSI_INIT_WRITE_WAKE);

	req->data_waiting = 1;
}

static int iscsi_pre_exec(struct scst_cmd *scst_cmd)
{
	int res = SCST_PREPROCESS_STATUS_SUCCESS;
	struct iscsi_cmnd *req = (struct iscsi_cmnd*)
		scst_cmd_get_tgt_priv(scst_cmd);
	struct iscsi_cmnd *c, *t;

	TRACE_ENTRY();

	EXTRACHECKS_BUG_ON(scst_cmd_atomic(scst_cmd));

	/* If data digest isn't used this list will be empty */
	list_for_each_entry_safe(c, t, &req->rx_ddigest_cmd_list,
				rx_ddigest_cmd_list_entry) {
		TRACE_DBG("Checking digest of RX ddigest cmd %p", c);
		if (digest_rx_data(c) != 0) {
			scst_set_cmd_error(scst_cmd,
				SCST_LOAD_SENSE(iscsi_sense_crc_error));
			res = SCST_PREPROCESS_STATUS_ERROR_SENSE_SET;
			/*
			 * The rest of rx_ddigest_cmd_list will be freed 
			 * in req_cmnd_release()
			 */
			goto out;
		}
		TRACE_DBG("Deleting RX digest cmd %p from digest list", c);
		list_del(&c->rx_ddigest_cmd_list_entry);
		cmnd_put(c);
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}


static void scsi_cmnd_exec(struct iscsi_cmnd *cmnd)
{
	if (cmnd->r2t_length) {
		if (!cmnd->is_unsolicited_data)
			send_r2t(cmnd);
	} else {
		/*
		 * There is no race with send_r2t() and __cmnd_abort(),
		 * since all functions called from single read thread
		 */
		cmnd->data_waiting = 0;
		iscsi_restart_cmnd(cmnd);
	}
}

static int noop_out_start(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	u32 size, tmp;
	int i, err = 0;

	TRACE_DBG("%p", cmnd);

	iscsi_extracheck_is_rd_thread(conn);

	if (cmnd_ttt(cmnd) != cpu_to_be32(ISCSI_RESERVED_TAG)) {
		/*
		 * We don't request a NOP-Out by sending a NOP-In.
		 * See 10.18.2 in the draft 20.
		 */
		PRINT_ERROR_PR("initiator bug %x", cmnd_itt(cmnd));
		err = -ISCSI_REASON_PROTOCOL_ERROR;
		goto out;
	}

	if (cmnd_itt(cmnd) == cpu_to_be32(ISCSI_RESERVED_TAG)) {
		if (!(cmnd->pdu.bhs.opcode & ISCSI_OP_IMMEDIATE))
			PRINT_ERROR_PR("%s","initiator bug!");
		spin_lock(&conn->session->sn_lock);
		__update_stat_sn(cmnd);
		err = check_cmd_sn(cmnd);
		spin_unlock(&conn->session->sn_lock);
		if (err)
			goto out;
	} else if ((err = cmnd_insert_hash(cmnd)) < 0) {
		PRINT_ERROR_PR("Can't insert in hash: ignore this request %x",
			cmnd_itt(cmnd));
		goto out;
	}

	if ((size = cmnd->pdu.datasize)) {
		size = (size + 3) & -4;
		conn->read_msg.msg_iov = conn->read_iov;
		if (cmnd->pdu.bhs.itt != cpu_to_be32(ISCSI_RESERVED_TAG)) {
			struct scatterlist *sg;

			/* ToDo: __GFP_NOFAIL ?? */
			cmnd->sg = sg = scst_alloc(size,
				GFP_KERNEL|__GFP_NOFAIL, 0, &cmnd->sg_cnt);
			if (sg == NULL) {
				/* ToDo(); */
			}
			if (cmnd->sg_cnt > ISCSI_CONN_IOV_MAX) {
				/* ToDo(); */
			}
			cmnd->own_sg = 1;
			cmnd->bufflen = size;

			for (i = 0; i < cmnd->sg_cnt; i++) {
				conn->read_iov[i].iov_base =
					page_address(sg[i].page);
				tmp = min_t(u32, size, PAGE_SIZE);
				conn->read_iov[i].iov_len = tmp;
				conn->read_size += tmp;
				size -= tmp;
			}
		} else {
			/*
			 * There are no problems with the safety from concurrent
			 * accesses to dummy_data, since for ISCSI_RESERVED_TAG
			 * the data only read and then discarded.
			 */
			for (i = 0; i < ISCSI_CONN_IOV_MAX; i++) {
				conn->read_iov[i].iov_base = dummy_data;
				tmp = min_t(u32, size, sizeof(dummy_data));
				conn->read_iov[i].iov_len = tmp;
				conn->read_size += tmp;
				size -= tmp;
			}
		}
		sBUG_ON(size == 0);
		conn->read_msg.msg_iovlen = i;
		TRACE_DBG("msg_iov=%p, msg_iovlen=%d", conn->read_msg.msg_iov,
			conn->read_msg.msg_iovlen);
	}
out:
	return err;
}

static inline u32 get_next_ttt(struct iscsi_conn *conn)
{
	u32 ttt;
	struct iscsi_session *session = conn->session;

	iscsi_extracheck_is_rd_thread(conn);

	if (session->next_ttt == ISCSI_RESERVED_TAG)
		session->next_ttt++;
	ttt = session->next_ttt++;

	return cpu_to_be32(ttt);
}

static int scsi_cmnd_start(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn = req->conn;
	struct iscsi_session *session = conn->session;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);
	struct scst_cmd *scst_cmd;
	scst_data_direction dir;
	int res = 0;

	TRACE_ENTRY();

	TRACE_DBG("scsi command: %02x", req_hdr->scb[0]);

	scst_cmd = scst_rx_cmd(session->scst_sess,
		(uint8_t*)req_hdr->lun, sizeof(req_hdr->lun),
		req_hdr->scb, sizeof(req_hdr->scb), SCST_NON_ATOMIC);
	if (scst_cmd == NULL) {
		create_status_rsp(req, SAM_STAT_BUSY, NULL, 0);
		cmnd_prepare_skip_pdu_set_resid(req);
		goto out;
	}

	req->scst_cmd = scst_cmd;
	scst_cmd_set_tag(scst_cmd, req_hdr->itt);
	scst_cmd_set_tgt_priv(scst_cmd, req);
#ifndef NET_PAGE_CALLBACKS_DEFINED
	scst_cmd_set_data_buf_tgt_alloc(scst_cmd);
#endif

	if (req_hdr->flags & ISCSI_CMD_READ)
		dir = SCST_DATA_READ;
	else if (req_hdr->flags & ISCSI_CMD_WRITE)
		dir = SCST_DATA_WRITE;
	else
		dir = SCST_DATA_NONE;
	scst_cmd_set_expected(scst_cmd, dir, be32_to_cpu(req_hdr->data_length));

	switch(req_hdr->flags & ISCSI_CMD_ATTR_MASK) {
	case ISCSI_CMD_SIMPLE:
		scst_cmd->queue_type = SCST_CMD_QUEUE_SIMPLE;
		break;
	case ISCSI_CMD_HEAD_OF_QUEUE:
		scst_cmd->queue_type = SCST_CMD_QUEUE_HEAD_OF_QUEUE;
		break;
	case ISCSI_CMD_ORDERED:
		scst_cmd->queue_type = SCST_CMD_QUEUE_ORDERED;
		break;
	case ISCSI_CMD_ACA:
		scst_cmd->queue_type = SCST_CMD_QUEUE_ACA;
		break;
	case ISCSI_CMD_UNTAGGED:
		scst_cmd->queue_type = SCST_CMD_QUEUE_UNTAGGED;
		break;
	default:
		PRINT_ERROR_PR("Unknown task code %x, use ORDERED instead",
			req_hdr->flags & ISCSI_CMD_ATTR_MASK);
		scst_cmd->queue_type = SCST_CMD_QUEUE_ORDERED;
		break;
	}

	TRACE_DBG("START Command (tag %d, queue_type %d)",
		req_hdr->itt, scst_cmd->queue_type);
	req->scst_state = ISCSI_CMD_STATE_RX_CMD;
	scst_cmd_init_stage1_done(scst_cmd, SCST_CONTEXT_DIRECT, 0);

	wait_event(req->scst_waitQ, (req->scst_state != ISCSI_CMD_STATE_RX_CMD));

	if (unlikely(req->scst_state != ISCSI_CMD_STATE_AFTER_PREPROC)) {
		TRACE_DBG("req %p is in %x state", req, req->scst_state);
		if (req->scst_state == ISCSI_CMD_STATE_PROCESSED) {
			/* Response is already prepared */
			cmnd_prepare_skip_pdu_set_resid(req);
			goto out;
		}
		if (unlikely(req->tmfabort)) {
			TRACE_MGMT_DBG("req %p (scst_cmd %p) aborted", req,
				req->scst_cmd);
			cmnd_prepare_skip_pdu(req);
			goto out;
		}
		sBUG();
	}

	dir = scst_cmd_get_data_direction(scst_cmd);
	if (dir != SCST_DATA_WRITE) {
		if (!(req_hdr->flags & ISCSI_CMD_FINAL) || req->pdu.datasize) {
			PRINT_ERROR_PR("Unexpected unsolicited data (ITT %x "
				"CDB %x", cmnd_itt(req), req_hdr->scb[0]);
			create_sense_rsp(req, ABORTED_COMMAND, 0xc, 0xc);
			cmnd_prepare_skip_pdu_set_resid(req);
			goto out;
		}
	}

	if (dir == SCST_DATA_WRITE) {
		req->is_unsolicited_data = !(req_hdr->flags & ISCSI_CMD_FINAL);
		req->r2t_length = be32_to_cpu(req_hdr->data_length) - req->pdu.datasize;
	}
	req->target_task_tag = get_next_ttt(conn);
	req->sg = scst_cmd_get_sg(scst_cmd);
	req->bufflen = scst_cmd_get_bufflen(scst_cmd);
	if (req->r2t_length > req->bufflen) {
		PRINT_ERROR_PR("req->r2t_length %d > req->bufflen %d",
			req->r2t_length, req->bufflen);
		req->r2t_length = req->bufflen;
	}

	TRACE_DBG("req=%p, dir=%d, is_unsolicited_data=%d, "
		"r2t_length=%d, bufflen=%d", req, dir,
		req->is_unsolicited_data, req->r2t_length, req->bufflen);

	if (!session->sess_param.immediate_data &&
	    req->pdu.datasize) {
		PRINT_ERROR_PR("Initiator %s violated negotiated paremeters: "
			"forbidden immediate data sent (ITT %x, op  %x)",
			session->initiator_name, cmnd_itt(req), req_hdr->scb[0]);
		res = -EINVAL;
		goto out;
	}

	if (session->sess_param.initial_r2t &&
	    !(req_hdr->flags & ISCSI_CMD_FINAL)) {
		PRINT_ERROR_PR("Initiator %s violated negotiated paremeters: "
			"initial R2T is required (ITT %x, op  %x)",
			session->initiator_name, cmnd_itt(req), req_hdr->scb[0]);
		res = -EINVAL;
		goto out;
	}

	if (req->pdu.datasize) {
		if (dir != SCST_DATA_WRITE) {
			PRINT_ERROR_PR("pdu.datasize(%d) >0, but dir(%x) isn't WRITE",
				req->pdu.datasize, dir);
			create_sense_rsp(req, ABORTED_COMMAND, 0xc, 0xc);
			cmnd_prepare_skip_pdu_set_resid(req);
		} else
			res = cmnd_prepare_recv_pdu(conn, req, 0, req->pdu.datasize);
	}
out:
	/* Aborted commands will be freed in cmnd_rx_end() */
	TRACE_EXIT_RES(res);
	return res;
}

static int data_out_start(struct iscsi_conn *conn, struct iscsi_cmnd *cmnd)
{
	struct iscsi_data_out_hdr *req_hdr = (struct iscsi_data_out_hdr *)&cmnd->pdu.bhs;
	struct iscsi_cmnd *req = NULL;
	u32 offset = be32_to_cpu(req_hdr->buffer_offset);
	int res = 0;

	TRACE_ENTRY();

	/*
	 * There is no race with send_r2t() and __cmnd_abort(), since
	 * all functions called from single read thread
	 */
	iscsi_extracheck_is_rd_thread(cmnd->conn);

	update_stat_sn(cmnd);

	cmnd->cmd_req = req = cmnd_find_hash(conn->session, req_hdr->itt,
					req_hdr->ttt);
	if (!req) {
		PRINT_ERROR_PR("unable to find scsi task %x %x",
			cmnd_itt(cmnd), cmnd_ttt(cmnd));
		goto skip_pdu;
	}

	if (req->r2t_length < cmnd->pdu.datasize) {
		PRINT_ERROR_PR("Invalid data len %x %u %u", cmnd_itt(req),
			cmnd->pdu.datasize, req->r2t_length);
		mark_conn_closed(conn);
		res = -EINVAL;
		goto out;
	}

	if (req->r2t_length + offset != cmnd_write_size(req)) {
		PRINT_ERROR_PR("Wrong cmd lengths (%x %u %u %u)",
			cmnd_itt(req), req->r2t_length,
			offset,	cmnd_write_size(req));
		mark_conn_closed(conn);
		res = -EINVAL;
		goto out;
	}

	req->r2t_length -= cmnd->pdu.datasize;

	/* Check unsolicited burst data */
	if ((req_hdr->ttt == cpu_to_be32(ISCSI_RESERVED_TAG)) &&
	    (req->pdu.bhs.flags & ISCSI_FLG_FINAL)) {
		PRINT_ERROR_PR("unexpected data from %x %x",
			cmnd_itt(cmnd), cmnd_ttt(cmnd));
		mark_conn_closed(conn);
		res = -EINVAL;
		goto out;
	}

	TRACE(TRACE_D_WRITE, "%u %p %p %u %u", req_hdr->ttt, cmnd, req,
		offset, cmnd->pdu.datasize);

	res = cmnd_prepare_recv_pdu(conn, req, offset, cmnd->pdu.datasize);

out:
	TRACE_EXIT_RES(res);
	return res;

skip_pdu:
	cmnd->pdu.bhs.opcode = ISCSI_OP_DATA_REJECT;
	cmnd_prepare_skip_pdu(cmnd);
	goto out;
}

static void data_out_end(struct iscsi_cmnd *cmnd)
{
	struct iscsi_data_out_hdr *req_hdr = (struct iscsi_data_out_hdr *)&cmnd->pdu.bhs;
	struct iscsi_cmnd *req;

	sBUG_ON(cmnd == NULL);
	req = cmnd->cmd_req;
	sBUG_ON(req == NULL);

	TRACE_DBG("cmnd %p, req %p", cmnd, req);

	iscsi_extracheck_is_rd_thread(cmnd->conn);

	if (!(cmnd->conn->ddigest_type & DIGEST_NONE)) {
		TRACE_DBG("Adding RX ddigest cmd %p to digest list "
			"of req %p", cmnd, req);
		list_add_tail(&cmnd->rx_ddigest_cmd_list_entry,
			&req->rx_ddigest_cmd_list);
		cmnd_get(cmnd);
	}

	if (req_hdr->ttt == cpu_to_be32(ISCSI_RESERVED_TAG)) {
		TRACE_DBG("ISCSI_RESERVED_TAG, FINAL %x",
			req_hdr->flags & ISCSI_FLG_FINAL);
		if (req_hdr->flags & ISCSI_FLG_FINAL) {
			req->is_unsolicited_data = 0;
			if (!req->pending)
				scsi_cmnd_exec(req);
		}
	} else {
		TRACE_DBG("FINAL %x, outstanding_r2t %d, "
			"r2t_length %d", req_hdr->flags & ISCSI_FLG_FINAL,
			req->outstanding_r2t, req->r2t_length);
		/* ToDo : proper error handling */
		if (!(req_hdr->flags & ISCSI_FLG_FINAL) && (req->r2t_length == 0))
			PRINT_ERROR_PR("initiator error %x", cmnd_itt(req));

		if (!(req_hdr->flags & ISCSI_FLG_FINAL))
			goto out;

		req->outstanding_r2t--;

		scsi_cmnd_exec(req);
	}

out:
	cmnd_put(cmnd);
	return;
}

/*
 * Called under cmd_list_lock, but may drop it inside.
 * Returns >0 if cmd_list_lock was dropped inside, 0 otherwise.
 */
static inline int __cmnd_abort(struct iscsi_cmnd *cmnd)
{
	int res = 0;

	TRACE(TRACE_MGMT, "Aborting cmd %p, scst_cmd %p (scst state %x, "
		"itt %x, op %x, r2t_len %x, CDB op %x, size to write %u, "
		"is_unsolicited_data %u, outstanding_r2t %u)",
		cmnd, cmnd->scst_cmd, cmnd->scst_state, cmnd_itt(cmnd),
		cmnd_opcode(cmnd), cmnd->r2t_length, cmnd_scsicode(cmnd),
		cmnd_write_size(cmnd), cmnd->is_unsolicited_data,
		cmnd->outstanding_r2t);

	iscsi_extracheck_is_rd_thread(cmnd->conn);

	cmnd->tmfabort = 1;

	if (cmnd->data_waiting) {
		struct iscsi_conn *conn = cmnd->conn;
		res = 1;
		spin_unlock_bh(&conn->cmd_list_lock);
		TRACE_MGMT_DBG("Releasing data waiting cmd %p", cmnd);
		req_cmnd_release_force(cmnd, ISCSI_FORCE_RELEASE_WRITE);
		spin_lock_bh(&conn->cmd_list_lock);
	}

	return res;
}

static int cmnd_abort(struct iscsi_session *session, u32 itt)
{
	struct iscsi_cmnd *cmnd;
	int err;

	if ((cmnd = cmnd_find_hash_get(session, itt, ISCSI_RESERVED_TAG))) {
		struct iscsi_conn *conn = cmnd->conn;
		spin_lock_bh(&conn->cmd_list_lock);
		__cmnd_abort(cmnd);
		spin_unlock_bh(&conn->cmd_list_lock);
		cmnd_put(cmnd);
		err = 0;
	} else
		err = ISCSI_RESPONSE_UNKNOWN_TASK;

	return err;
}

static int target_abort(struct iscsi_cmnd *req, u16 *lun, int all)
{
	struct iscsi_target *target = req->conn->session->target;
	struct iscsi_session *session;
	struct iscsi_conn *conn;
	struct iscsi_cmnd *cmnd;

	mutex_lock(&target->target_mutex);

	list_for_each_entry(session, &target->session_list, session_list_entry) {
		list_for_each_entry(conn, &session->conn_list, conn_list_entry) {
			spin_lock_bh(&conn->cmd_list_lock);
again:
			list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
				int again = 0;
				if (cmnd == req)
					continue;
				if (all)
					again = __cmnd_abort(cmnd);
				else if (memcmp(lun, &cmnd_hdr(cmnd)->lun,
						sizeof(cmnd_hdr(cmnd)->lun)) == 0)
					again = __cmnd_abort(cmnd);
				if (again)
					goto again;
			}
			spin_unlock_bh(&conn->cmd_list_lock);
		}
	}

	mutex_unlock(&target->target_mutex);
	return 0;
}

static void task_set_abort(struct iscsi_cmnd *req)
{
	struct iscsi_session *session = req->conn->session;
	struct iscsi_target *target = session->target;
	struct iscsi_conn *conn;
	struct iscsi_cmnd *cmnd;

	mutex_lock(&target->target_mutex);

	list_for_each_entry(conn, &session->conn_list, conn_list_entry) {
		spin_lock_bh(&conn->cmd_list_lock);
again:
		list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
			if (cmnd != req)
				if (__cmnd_abort(cmnd))
					goto again;
		}
		spin_unlock_bh(&conn->cmd_list_lock);
	}

	mutex_unlock(&target->target_mutex);
	return;
}

void conn_abort(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *cmnd;

	TRACE_MGMT_DBG("Aborting conn %p", conn);

	spin_lock_bh(&conn->cmd_list_lock);
again:
	list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
		if (__cmnd_abort(cmnd))
			goto again;
	}
	spin_unlock_bh(&conn->cmd_list_lock);
}

static void execute_task_management(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn = req->conn;
	struct iscsi_task_mgt_hdr *req_hdr = (struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	int err = 0, function = req_hdr->function & ISCSI_FUNCTION_MASK;

	TRACE(TRACE_MGMT, "TM cmd: req %p, itt %x, fn %d, rtt %x", req, cmnd_itt(req),
		function, req_hdr->rtt);

	/* 
	 * ToDo: relevant TM functions shall affect only commands with
	 * CmdSN lower req_hdr->cmd_sn (see RFC 3720 section 10.5).
	 * 
	 * I suppose, iscsi_session_push_cmnd() should be updated to keep
	 * commands with higher CmdSN in the session->pending_list until
	 * executing TM command finished. Although, if higher CmdSN commands
	 * might be already sent to SCST for execution, it could get much more
	 * complicated and should be implemented on SCST level.
	 */

	switch (function) {
	case ISCSI_FUNCTION_ABORT_TASK:
		err = cmnd_abort(conn->session, req_hdr->rtt);
		if (err == 0) {
			err = scst_rx_mgmt_fn_tag(conn->session->scst_sess,
				SCST_ABORT_TASK, req_hdr->rtt, SCST_NON_ATOMIC,
				req);
		}
		break;
	case ISCSI_FUNCTION_ABORT_TASK_SET:
		task_set_abort(req);
		err = scst_rx_mgmt_fn_lun(conn->session->scst_sess,
			SCST_ABORT_TASK_SET, (uint8_t *)req_hdr->lun,
			sizeof(req_hdr->lun), SCST_NON_ATOMIC, req);
		break;
	case ISCSI_FUNCTION_CLEAR_TASK_SET:
		task_set_abort(req);
		err = scst_rx_mgmt_fn_lun(conn->session->scst_sess,
			SCST_CLEAR_TASK_SET, (uint8_t *)req_hdr->lun,
			sizeof(req_hdr->lun), SCST_NON_ATOMIC, req);
		break;
	case ISCSI_FUNCTION_CLEAR_ACA:
		err = scst_rx_mgmt_fn_lun(conn->session->scst_sess,
			SCST_CLEAR_ACA,	(uint8_t *)req_hdr->lun,
			sizeof(req_hdr->lun), SCST_NON_ATOMIC, req);
		break;
	case ISCSI_FUNCTION_TARGET_COLD_RESET:
	case ISCSI_FUNCTION_TARGET_WARM_RESET:
		target_abort(req, 0, 1);
		err = scst_rx_mgmt_fn_lun(conn->session->scst_sess,
			SCST_TARGET_RESET, (uint8_t *)req_hdr->lun,
			sizeof(req_hdr->lun), SCST_NON_ATOMIC, req);
		break;
	case ISCSI_FUNCTION_LOGICAL_UNIT_RESET:
		target_abort(req, req_hdr->lun, 0);
		err = scst_rx_mgmt_fn_lun(conn->session->scst_sess,
			SCST_LUN_RESET, (uint8_t *)req_hdr->lun,
			sizeof(req_hdr->lun), SCST_NON_ATOMIC, req);
		break;
	case ISCSI_FUNCTION_TASK_REASSIGN:
		iscsi_send_task_mgmt_resp(req, 
			ISCSI_RESPONSE_FUNCTION_UNSUPPORTED);
		break;
	default:
		iscsi_send_task_mgmt_resp(req,
			ISCSI_RESPONSE_FUNCTION_REJECTED);
		break;
	}

	if (err != 0) {
		iscsi_send_task_mgmt_resp(req,
			ISCSI_RESPONSE_FUNCTION_REJECTED);
	}
}

static void noop_out_exec(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_nop_in_hdr *rsp_hdr;

	TRACE_DBG("%p", req);

	if (cmnd_itt(req) != cpu_to_be32(ISCSI_RESERVED_TAG)) {
		rsp = iscsi_cmnd_create_rsp_cmnd(req);

		rsp_hdr = (struct iscsi_nop_in_hdr *)&rsp->pdu.bhs;
		rsp_hdr->opcode = ISCSI_OP_NOOP_IN;
		rsp_hdr->flags = ISCSI_FLG_FINAL;
		rsp_hdr->itt = req->pdu.bhs.itt;
		rsp_hdr->ttt = cpu_to_be32(ISCSI_RESERVED_TAG);

		if (req->pdu.datasize)
			sBUG_ON(req->sg == NULL);
		else
			sBUG_ON(req->sg != NULL);

		if (req->sg) {
			rsp->sg = req->sg;
			rsp->bufflen = req->bufflen;
		}

		sBUG_ON(get_pgcnt(req->pdu.datasize, 0) > ISCSI_CONN_IOV_MAX);
		rsp->pdu.datasize = req->pdu.datasize;
		iscsi_cmnd_init_write(rsp,
			ISCSI_INIT_WRITE_REMOVE_HASH | ISCSI_INIT_WRITE_WAKE);
		req_cmnd_release(req);
	} else
		cmnd_put(req);
}

static void logout_exec(struct iscsi_cmnd *req)
{
	struct iscsi_logout_req_hdr *req_hdr;
	struct iscsi_cmnd *rsp;
	struct iscsi_logout_rsp_hdr *rsp_hdr;

	PRINT_INFO_PR("Logout received from initiator %s",
		req->conn->session->initiator_name);
	TRACE_DBG("%p", req);

	req_hdr = (struct iscsi_logout_req_hdr *)&req->pdu.bhs;
	rsp = iscsi_cmnd_create_rsp_cmnd(req);
	rsp_hdr = (struct iscsi_logout_rsp_hdr *)&rsp->pdu.bhs;
	rsp_hdr->opcode = ISCSI_OP_LOGOUT_RSP;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->itt = req_hdr->itt;
	rsp->should_close_conn = 1;
	iscsi_cmnd_init_write(rsp,
		ISCSI_INIT_WRITE_REMOVE_HASH | ISCSI_INIT_WRITE_WAKE);
	req_cmnd_release(req);
}

static void iscsi_cmnd_exec(struct iscsi_cmnd *cmnd)
{
	TRACE_DBG("%p,%x,%u", cmnd, cmnd_opcode(cmnd), cmnd->pdu.bhs.sn);

	if (unlikely(cmnd->tmfabort)) {
		TRACE_MGMT_DBG("cmnd %p (scst_cmd %p) aborted", cmnd,
			cmnd->scst_cmd);
		req_cmnd_release_force(cmnd, ISCSI_FORCE_RELEASE_WRITE);
		goto out;
	}

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOOP_OUT:
		noop_out_exec(cmnd);
		break;
	case ISCSI_OP_SCSI_CMD:
		scsi_cmnd_exec(cmnd);
		break;
	case ISCSI_OP_SCSI_TASK_MGT_MSG:
		execute_task_management(cmnd);
		break;
	case ISCSI_OP_LOGOUT_CMD:
		logout_exec(cmnd);
		break;
	case ISCSI_OP_SCSI_REJECT:
		TRACE_MGMT_DBG("REJECT cmnd %p (scst_cmd %p)", cmnd,
			cmnd->scst_cmd);
		iscsi_cmnd_init_write(get_rsp_cmnd(cmnd),
			ISCSI_INIT_WRITE_REMOVE_HASH | ISCSI_INIT_WRITE_WAKE);
		req_cmnd_release(cmnd);
		break;
	default:
		PRINT_ERROR_PR("unexpected cmnd op %x", cmnd_opcode(cmnd));
		req_cmnd_release(cmnd);
		break;
	}
out:
	return;
}

static void __cmnd_send_pdu(struct iscsi_conn *conn, struct iscsi_cmnd *cmnd,
	u32 offset, u32 size)
{
	TRACE_DBG("%p %u,%u,%u", cmnd, offset, size, cmnd->bufflen);

	iscsi_extracheck_is_wr_thread(conn);

	sBUG_ON(offset > cmnd->bufflen);
	sBUG_ON(offset + size > cmnd->bufflen);

	conn->write_offset = offset;
	conn->write_size += size;
}

static void cmnd_send_pdu(struct iscsi_conn *conn, struct iscsi_cmnd *cmnd)
{
	u32 size;

	if (!cmnd->pdu.datasize)
		return;

	size = (cmnd->pdu.datasize + 3) & -4;
	sBUG_ON(cmnd->sg == NULL);
	sBUG_ON(cmnd->bufflen != size);
	__cmnd_send_pdu(conn, cmnd, 0, size);
}

static void set_cork(struct socket *sock, int on)
{
	int opt = on;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());
	sock->ops->setsockopt(sock, SOL_TCP, TCP_CORK, (void *)&opt, sizeof(opt));
	set_fs(oldfs);
}

void cmnd_tx_start(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;

	TRACE_DBG("%p:%p:%x", conn, cmnd, cmnd_opcode(cmnd));
	iscsi_cmnd_set_length(&cmnd->pdu);

	iscsi_extracheck_is_wr_thread(conn);

	set_cork(conn->sock, 1);

	conn->write_iop = conn->write_iov;
	conn->write_iop->iov_base = &cmnd->pdu.bhs;
	conn->write_iop->iov_len = sizeof(cmnd->pdu.bhs);
	conn->write_iop_used = 1;
	conn->write_size = sizeof(cmnd->pdu.bhs);

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOOP_IN:
		cmnd_set_sn(cmnd, 1);
		cmnd_send_pdu(conn, cmnd);
		break;
	case ISCSI_OP_SCSI_RSP:
		cmnd_set_sn(cmnd, 1);
		cmnd_send_pdu(conn, cmnd);
		break;
	case ISCSI_OP_SCSI_TASK_MGT_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_TEXT_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_SCSI_DATA_IN:
	{
		struct iscsi_data_in_hdr *rsp = (struct iscsi_data_in_hdr *)&cmnd->pdu.bhs;
		u32 offset = cpu_to_be32(rsp->buffer_offset);

		cmnd_set_sn(cmnd, (rsp->flags & ISCSI_FLG_FINAL) ? 1 : 0);
		__cmnd_send_pdu(conn, cmnd, offset, cmnd->pdu.datasize);
		break;
	}
	case ISCSI_OP_LOGOUT_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_R2T:
		cmnd->pdu.bhs.sn = cmnd_set_sn(cmnd, 0);
		break;
	case ISCSI_OP_ASYNC_MSG:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_REJECT:
		cmnd_set_sn(cmnd, 1);
		cmnd_send_pdu(conn, cmnd);
		break;
	default:
		PRINT_ERROR_PR("unexpected cmnd op %x", cmnd_opcode(cmnd));
		break;
	}

	// move this?
	conn->write_size = (conn->write_size + 3) & -4;
	iscsi_dump_pdu(&cmnd->pdu);
}

void cmnd_tx_end(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;

	TRACE_DBG("%p:%x (should_close_conn %d)", cmnd, cmnd_opcode(cmnd),
		cmnd->should_close_conn);

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOOP_IN:
	case ISCSI_OP_SCSI_RSP:
	case ISCSI_OP_SCSI_TASK_MGT_RSP:
	case ISCSI_OP_TEXT_RSP:
	case ISCSI_OP_R2T:
	case ISCSI_OP_ASYNC_MSG:
	case ISCSI_OP_REJECT:
	case ISCSI_OP_SCSI_DATA_IN:
	case ISCSI_OP_LOGOUT_RSP:
		break;
	default:
		PRINT_ERROR_PR("unexpected cmnd op %x", cmnd_opcode(cmnd));
		sBUG();
		break;
	}

	if (cmnd->should_close_conn) {
		PRINT_INFO_PR("Closing connection at initiator %s request",
			conn->session->initiator_name);
		mark_conn_closed(conn);
	}

	set_cork(cmnd->conn->sock, 0);
}

/*
 * Push the command for execution. This functions reorders the commands.
 * Called from the read thread.
 */
static void iscsi_session_push_cmnd(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	struct list_head *entry;
	u32 cmd_sn;

	TRACE_DBG("%p:%x %u,%u",
		cmnd, cmnd_opcode(cmnd), cmnd->pdu.bhs.sn, session->exp_cmd_sn);

	iscsi_extracheck_is_rd_thread(cmnd->conn);

	if (cmnd->pdu.bhs.opcode & ISCSI_OP_IMMEDIATE) {
		iscsi_cmnd_exec(cmnd);
		return;
	}

	spin_lock(&session->sn_lock);

	cmd_sn = cmnd->pdu.bhs.sn;
	if (cmd_sn == session->exp_cmd_sn) {
		while (1) {
			session->exp_cmd_sn = ++cmd_sn;

			spin_unlock(&session->sn_lock);

			iscsi_cmnd_exec(cmnd);

			if (list_empty(&session->pending_list))
				break;
			cmnd = list_entry(session->pending_list.next, struct iscsi_cmnd,
						pending_list_entry);
			if (cmnd->pdu.bhs.sn != cmd_sn)
				break;
			list_del(&cmnd->pending_list_entry);
			cmnd->pending = 0;

			spin_lock(&session->sn_lock);
		}
	} else {
		cmnd->pending = 1;
		if (before(cmd_sn, session->exp_cmd_sn)) { /* close the conn */
			PRINT_ERROR_PR("unexpected cmd_sn (%u,%u)", cmd_sn,
				session->exp_cmd_sn);
		}

		if (after(cmd_sn, session->exp_cmd_sn + session->max_queued_cmnds)) {
			PRINT_ERROR_PR("too large cmd_sn (%u,%u)", cmd_sn,
				session->exp_cmd_sn);
		}

		spin_unlock(&session->sn_lock);

		list_for_each(entry, &session->pending_list) {
			struct iscsi_cmnd *tmp = list_entry(entry, struct iscsi_cmnd,
							pending_list_entry);
			if (before(cmd_sn, tmp->pdu.bhs.sn))
				break;
		}

		list_add_tail(&cmnd->pending_list_entry, entry);
	}

	return;
}

static int check_segment_length(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct iscsi_session *session = conn->session;

	if (cmnd->pdu.datasize > session->sess_param.max_recv_data_length) {
		PRINT_ERROR_PR("Initiator %s violated negotiated parameters: "
			"data too long (ITT %x, datasize %u, "
			"max_recv_data_length %u", session->initiator_name,
			cmnd_itt(cmnd), cmnd->pdu.datasize,
			session->sess_param.max_recv_data_length);
		mark_conn_closed(conn);
		return -EINVAL;
	}
	return 0;
}

int cmnd_rx_start(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	int res, rc;

	iscsi_dump_pdu(&cmnd->pdu);

	res = check_segment_length(cmnd);
	if (res != 0)
		goto out;

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOOP_OUT:
		rc = noop_out_start(cmnd);
		break;
	case ISCSI_OP_SCSI_CMD:
		rc = cmnd_insert_hash(cmnd);
		if (likely(rc == 0)) {
			res = scsi_cmnd_start(cmnd);
			if (unlikely(res != 0))
				goto out;
		}
		break;
	case ISCSI_OP_SCSI_TASK_MGT_MSG:
		rc = cmnd_insert_hash(cmnd);
		break;
	case ISCSI_OP_SCSI_DATA_OUT:
		res = data_out_start(conn, cmnd);
		rc = 0; /* to avoid compiler warning */
		if (unlikely(res != 0))
			goto out;
		break;
	case ISCSI_OP_LOGOUT_CMD:
		rc = cmnd_insert_hash(cmnd);
		break;
	case ISCSI_OP_TEXT_CMD:
	case ISCSI_OP_SNACK_CMD:
		rc = -ISCSI_REASON_UNSUPPORTED_COMMAND;
		break;
	default:
		rc = -ISCSI_REASON_UNSUPPORTED_COMMAND;
		break;
	}

	if (unlikely(rc < 0)) {
		struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);
		PRINT_ERROR_PR("Error %d (iSCSI opcode %x, ITT %x, op %x)", rc,
			cmnd_opcode(cmnd), cmnd_itt(cmnd),
			(cmnd_opcode(cmnd) == ISCSI_OP_SCSI_CMD ?
				hdr->scb[0] : -1));
		iscsi_cmnd_reject(cmnd, -rc);
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

void cmnd_rx_end(struct iscsi_cmnd *cmnd)
{
	if (unlikely(cmnd->tmfabort)) {
		TRACE_MGMT_DBG("cmnd %p (scst_cmd %p) aborted", cmnd,
			cmnd->scst_cmd);
		req_cmnd_release_force(cmnd, ISCSI_FORCE_RELEASE_WRITE);
		return;
	}

	TRACE_DBG("%p:%x", cmnd, cmnd_opcode(cmnd));
	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_SCSI_REJECT:
	case ISCSI_OP_NOOP_OUT:
	case ISCSI_OP_SCSI_CMD:
	case ISCSI_OP_SCSI_TASK_MGT_MSG:
	case ISCSI_OP_LOGOUT_CMD:
		iscsi_session_push_cmnd(cmnd);
		break;
	case ISCSI_OP_SCSI_DATA_OUT:
		data_out_end(cmnd);
		break;
	case ISCSI_OP_PDU_REJECT:
		iscsi_cmnd_init_write(get_rsp_cmnd(cmnd),
			ISCSI_INIT_WRITE_REMOVE_HASH | ISCSI_INIT_WRITE_WAKE);
		req_cmnd_release(cmnd);
		break;
	case ISCSI_OP_DATA_REJECT:
		req_cmnd_release(cmnd);
		break;
	default:
		PRINT_ERROR_PR("unexpected cmnd op %x", cmnd_opcode(cmnd));
		req_cmnd_release(cmnd);
		break;
	}
}

#ifndef NET_PAGE_CALLBACKS_DEFINED
static int iscsi_alloc_data_buf(struct scst_cmd *cmd)
{
	if (scst_cmd_get_data_direction(cmd) == SCST_DATA_READ) {
		/*
		 * sock->ops->sendpage() is async zero copy operation,
		 * so we must be sure not to free and reuse
		 * the command's buffer before the sending was completed
		 * by the network layers. It is possible only if we
		 * don't use SGV cache.
		 */
		scst_cmd_set_no_sgv(cmd);
	}
	return 1;
}
#endif

static inline void iscsi_set_state_wake_up(struct iscsi_cmnd *req,
	int new_state)
{
	/*
	 * We use wait_event() to wait for the state change, but it checks its
	 * condition without any protection, so without cmnd_get() it is
	 * possible that req will die "immediately" after the state assignment
	 * and wake_up() will operate on dead data.
	 */
	cmnd_get_ordered(req);
	req->scst_state = new_state;
	wake_up(&req->scst_waitQ);
	cmnd_put(req);
	return;
}

static void iscsi_preprocessing_done(struct scst_cmd *scst_cmd)
{
	struct iscsi_cmnd *req = (struct iscsi_cmnd*)
				scst_cmd_get_tgt_priv(scst_cmd);

	TRACE_DBG("req %p", req);

	EXTRACHECKS_BUG_ON(req->scst_state != ISCSI_CMD_STATE_RX_CMD);

	iscsi_set_state_wake_up(req, ISCSI_CMD_STATE_AFTER_PREPROC);
	return;
}

static void iscsi_try_local_processing(struct iscsi_conn *conn)
{
	int local;

	TRACE_ENTRY();

	spin_lock_bh(&iscsi_wr_lock);
	switch(conn->wr_state) {
	case ISCSI_CONN_WR_STATE_IN_LIST:
		list_del(&conn->wr_list_entry);
		/* go through */
	case ISCSI_CONN_WR_STATE_IDLE:
#ifdef EXTRACHECKS
		conn->wr_task = current;
#endif
		conn->wr_state = ISCSI_CONN_WR_STATE_PROCESSING;
		conn->wr_space_ready = 0;
		local = 1;
		break;
	default:
		local = 0;
		break;
	}
	spin_unlock_bh(&iscsi_wr_lock);

	if (local) {
		int rc = 1;
		while(test_write_ready(conn)) {
			rc = iscsi_send(conn);
			if (rc <= 0) {
				break;
			}
		}

		spin_lock_bh(&iscsi_wr_lock);
#ifdef EXTRACHECKS
		conn->wr_task = NULL;
#endif
		if ((rc <= 0) || test_write_ready(conn)) {
			list_add_tail(&conn->wr_list_entry, &iscsi_wr_list);
			conn->wr_state = ISCSI_CONN_WR_STATE_IN_LIST;
			wake_up(&iscsi_wr_waitQ);
		} else
			conn->wr_state = ISCSI_CONN_WR_STATE_IDLE;
		spin_unlock_bh(&iscsi_wr_lock);
	}

	TRACE_EXIT();
	return;
}

static int iscsi_xmit_response(struct scst_cmd *scst_cmd)
{
	int resp_flags = scst_cmd_get_tgt_resp_flags(scst_cmd);
	struct iscsi_cmnd *req = (struct iscsi_cmnd*)
					scst_cmd_get_tgt_priv(scst_cmd);
	struct iscsi_conn *conn = req->conn;
	int status = scst_cmd_get_status(scst_cmd);
	u8 *sense = scst_cmd_get_sense_buffer(scst_cmd);
	int sense_len = scst_cmd_get_sense_buffer_len(scst_cmd);
	int old_state = req->scst_state;

	scst_cmd_set_tgt_priv(scst_cmd, NULL);

	req->tmfabort |= scst_cmd_aborted(scst_cmd) ? 1 : 0;
	if (unlikely(req->tmfabort)) {
		TRACE_MGMT_DBG("req %p (scst_cmd %p) aborted", req,
			req->scst_cmd);
		if (old_state == ISCSI_CMD_STATE_RESTARTED) {
			req->scst_state = ISCSI_CMD_STATE_PROCESSED;
			req_cmnd_release_force(req, ISCSI_FORCE_RELEASE_WRITE);
		} else
			iscsi_set_state_wake_up(req, ISCSI_CMD_STATE_PROCESSED);
		goto out;
	}

	if (unlikely(old_state != ISCSI_CMD_STATE_RESTARTED)) {
		TRACE_DBG("req %p on %d state", req, old_state);
		create_status_rsp(req, status, sense, sense_len);
		switch(old_state) {
		case ISCSI_CMD_STATE_RX_CMD:
		case ISCSI_CMD_STATE_AFTER_PREPROC:
			break;
		default:
			sBUG();
		}
		iscsi_set_state_wake_up(req, ISCSI_CMD_STATE_PROCESSED);
		goto out;
	}

	req->scst_state = ISCSI_CMD_STATE_PROCESSED;

	req->bufflen = scst_cmd_get_resp_data_len(scst_cmd);
	req->sg = scst_cmd_get_sg(scst_cmd);

	TRACE_DBG("req %p, resp_flags=%x, req->bufflen=%d, req->sg=%p", req,
		resp_flags, req->bufflen, req->sg);

	if ((req->bufflen != 0) && !(resp_flags & SCST_TSC_FLAG_STATUS)) {
		PRINT_ERROR_PR("%s", "Sending DATA without STATUS is unsupported");
		scst_set_cmd_error(scst_cmd,
			SCST_LOAD_SENSE(scst_sense_hardw_error));
		resp_flags = scst_cmd_get_tgt_resp_flags(scst_cmd);
		sBUG();
	}

	if (req->bufflen != 0) {
		/* 
		 * Check above makes sure that SCST_TSC_FLAG_STATUS is set,
		 * so status is valid here, but in future that could change.
		 * ToDo
		 */
		if (status != SAM_STAT_CHECK_CONDITION) {
			send_data_rsp(req, status,
				resp_flags & SCST_TSC_FLAG_STATUS);
		} else {
			struct iscsi_cmnd *rsp;
			struct iscsi_scsi_rsp_hdr *rsp_hdr;
			int resid;
			send_data_rsp(req, 0, 0);
			if (resp_flags & SCST_TSC_FLAG_STATUS) {
				rsp = create_status_rsp(req, status, sense,
					sense_len);
				rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;
				resid = cmnd_read_size(req) - req->bufflen;
				if (resid > 0) {
					rsp_hdr->flags |=
						ISCSI_FLG_RESIDUAL_UNDERFLOW;
					rsp_hdr->residual_count = cpu_to_be32(resid);
				} else if (resid < 0) {
					rsp_hdr->flags |=
						ISCSI_FLG_RESIDUAL_OVERFLOW;
					rsp_hdr->residual_count = cpu_to_be32(-resid);
				}
				iscsi_cmnd_init_write(rsp,
					ISCSI_INIT_WRITE_REMOVE_HASH);
			}
		}
	} else if (resp_flags & SCST_TSC_FLAG_STATUS) {
		struct iscsi_cmnd *rsp;
		struct iscsi_scsi_rsp_hdr *rsp_hdr;
		u32 resid;
		rsp = create_status_rsp(req, status, sense, sense_len);
		rsp_hdr = (struct iscsi_scsi_rsp_hdr *) &rsp->pdu.bhs;
		resid = cmnd_read_size(req);
		if (resid != 0) {
			rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
			rsp_hdr->residual_count = cpu_to_be32(resid);
		}
		iscsi_cmnd_init_write(rsp, ISCSI_INIT_WRITE_REMOVE_HASH);
	}
#ifdef EXTRACHECKS
	else
		sBUG();
#endif

	atomic_inc(&conn->conn_ref_cnt);
	smp_mb__after_atomic_inc();

	req_cmnd_release(req);

	iscsi_try_local_processing(conn);

	smp_mb__before_atomic_dec();
	atomic_dec(&conn->conn_ref_cnt);

out:
	return SCST_TGT_RES_SUCCESS;
}

static void iscsi_send_task_mgmt_resp(struct iscsi_cmnd *req, int status)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_task_mgt_hdr *req_hdr =
				(struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	struct iscsi_task_rsp_hdr *rsp_hdr;

	TRACE(TRACE_MGMT, "req %p, status %d", req, status);

	rsp = iscsi_cmnd_create_rsp_cmnd(req);
	rsp_hdr = (struct iscsi_task_rsp_hdr *)&rsp->pdu.bhs;

	rsp_hdr->opcode = ISCSI_OP_SCSI_TASK_MGT_RSP;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->itt = req_hdr->itt;
	rsp_hdr->response = status;

	if ((req_hdr->function & ISCSI_FUNCTION_MASK) ==
			ISCSI_FUNCTION_TARGET_COLD_RESET)
		rsp->should_close_conn = 1;

	iscsi_cmnd_init_write(rsp,
		ISCSI_INIT_WRITE_REMOVE_HASH | ISCSI_INIT_WRITE_WAKE);
	req_cmnd_release(req);
}

static inline int iscsi_get_mgmt_response(int status)
{
	switch(status) {
	case SCST_MGMT_STATUS_SUCCESS:
		return ISCSI_RESPONSE_FUNCTION_COMPLETE;

	case SCST_MGMT_STATUS_TASK_NOT_EXIST:
		return ISCSI_RESPONSE_UNKNOWN_TASK;

	case SCST_MGMT_STATUS_LUN_NOT_EXIST:
		return ISCSI_RESPONSE_UNKNOWN_LUN;

	case SCST_MGMT_STATUS_FN_NOT_SUPPORTED:
		return ISCSI_RESPONSE_FUNCTION_UNSUPPORTED;

	case SCST_MGMT_STATUS_REJECTED:
	case SCST_MGMT_STATUS_FAILED:
	default:
		return ISCSI_RESPONSE_FUNCTION_REJECTED;
	}
}

static void iscsi_task_mgmt_fn_done(struct scst_mgmt_cmd *scst_mcmd)
{
	struct iscsi_cmnd *req = (struct iscsi_cmnd*)
				scst_mgmt_cmd_get_tgt_priv(scst_mcmd);
	int status = iscsi_get_mgmt_response(scst_mgmt_cmd_get_status(scst_mcmd));

	TRACE(TRACE_MGMT, "scst_mcmd %p, status %d", scst_mcmd, 
		scst_mgmt_cmd_get_status(scst_mcmd));

	iscsi_send_task_mgmt_resp(req, status);

	scst_mgmt_cmd_set_tgt_priv(scst_mcmd, NULL);
}

static int iscsi_target_detect(struct scst_tgt_template *templ)
{
	/* Nothing to do */
	return 0;
}

static int iscsi_target_release(struct scst_tgt *scst_tgt)
{
	/* Nothing to do */
	return 0;
}

struct scst_tgt_template iscsi_template = {
	.name = "iscsi",
	.sg_tablesize = ISCSI_CONN_IOV_MAX,
	.threads_num = 0,
	.no_clustering = 1,
	.xmit_response_atomic = 0,
	.preprocessing_done_atomic = 1,
	.detect = iscsi_target_detect,
	.release = iscsi_target_release,
	.xmit_response = iscsi_xmit_response,
#ifndef NET_PAGE_CALLBACKS_DEFINED
	.alloc_data_buf = iscsi_alloc_data_buf,
#endif
	.preprocessing_done = iscsi_preprocessing_done,
	.pre_exec = iscsi_pre_exec,
	.task_mgmt_fn_done = iscsi_task_mgmt_fn_done,
};

static __init int iscsi_run_threads(int count, char *name, int (*fn)(void *))
{
	int res = 0;
	int i;
	struct iscsi_thread_t *thr;

	for (i = 0; i < count; i++) {
		thr = kmalloc(sizeof(*thr), GFP_KERNEL);
		if (!thr) {
			res = -ENOMEM;
			PRINT_ERROR_PR("Failed to allocate thr %d", res);
			goto out;
		}
		thr->thr = kthread_run(fn, NULL, "%s%d", name, i);
		if (IS_ERR(thr->thr)) {
			res = PTR_ERR(thr->thr);
			PRINT_ERROR_PR("kthread_create() failed: %d", res);
			kfree(thr);
			goto out;
		}
		list_add(&thr->threads_list_entry, &iscsi_threads_list);
	}

out:
	return res;
}

static void iscsi_stop_threads(void)
{
	struct iscsi_thread_t *t, *tmp;

	list_for_each_entry_safe(t, tmp, &iscsi_threads_list,
				threads_list_entry) {
		int rc = kthread_stop(t->thr);
		if (rc < 0) {
			TRACE_MGMT_DBG("kthread_stop() failed: %d", rc);
		}
		list_del(&t->threads_list_entry);
		kfree(t);
	}
}

static int __init iscsi_init(void)
{
	int err;
	int num;

	PRINT_INFO_PR("iSCSI SCST Target - version %s", ISCSI_VERSION_STRING);

#ifdef NET_PAGE_CALLBACKS_DEFINED
	err = net_set_get_put_page_callbacks(iscsi_get_page_callback,
			iscsi_put_page_callback);
	if (err != 0) {
		PRINT_INFO_PR("Unable to set page callbackes: %d", err);
		goto out;
	}
#else
	PRINT_INFO_PR("%s", "Patch put_page_callback.patch not applied on your "
		"kernel. Running in the performance degraded mode. Refer "
		"README file for details");
#endif

	BUILD_BUG_ON(MAX_DATA_SEG_LEN != (ISCSI_CONN_IOV_MAX<<PAGE_SHIFT));

	if ((ctr_major = register_chrdev(0, ctr_name, &ctr_fops)) < 0) {
		PRINT_ERROR_PR("failed to register the control device %d", ctr_major);
		err = ctr_major;
		goto out_callb;
	}

	if ((err = event_init()) < 0)
		goto out_reg;

	iscsi_cmnd_cache = kmem_cache_create("scst_iscsi_cmnd",
		sizeof(struct iscsi_cmnd), 0, 0, NULL, NULL);
	if (!iscsi_cmnd_cache) {
		err = -ENOMEM;
		goto out_event;
	}

	if (scst_register_target_template(&iscsi_template) < 0) {
		err = -ENODEV;
		goto out_kmem;
	}
	iscsi_template_registered = 1;

	if ((err = iscsi_procfs_init()) < 0)
		goto out_reg_tmpl;

	num = max(num_online_cpus(), 2);

	err = iscsi_run_threads(num, "iscsird", istrd);
	if (err != 0)
		goto out_thr;

	err = iscsi_run_threads(num, "iscsiwr", istwr);
	if (err != 0)
		goto out_thr;

out:
	return err;

out_thr:
	iscsi_procfs_exit();
	iscsi_stop_threads();

out_reg_tmpl:
	scst_unregister_target_template(&iscsi_template);

out_kmem:
	kmem_cache_destroy(iscsi_cmnd_cache);

out_event:
	event_exit();

out_reg:
	unregister_chrdev(ctr_major, ctr_name);

out_callb:
#ifdef NET_PAGE_CALLBACKS_DEFINED
	net_set_get_put_page_callbacks(NULL, NULL);
#endif	
	goto out;
}

static void __exit iscsi_exit(void)
{
	iscsi_stop_threads();

	unregister_chrdev(ctr_major, ctr_name);

	iscsi_procfs_exit();
	event_exit();

	kmem_cache_destroy(iscsi_cmnd_cache);

	scst_unregister_target_template(&iscsi_template);

#ifdef NET_PAGE_CALLBACKS_DEFINED
	net_set_get_put_page_callbacks(NULL, NULL);
#endif
}


module_init(iscsi_init);
module_exit(iscsi_exit);

MODULE_LICENSE("GPL");
