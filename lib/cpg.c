/*
 * vi: set autoindent tabstop=4 shiftwidth=4 :
 *
 * Copyright (c) 2006-2008 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Christine Caulfield (ccaulfie@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Provides a closed process group API using the corosync executive
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include <corosync/corotypes.h>
#include <corosync/cpg.h>
#include <corosync/ipc_cpg.h>
#include <corosync/mar_cpg.h>
#include <corosync/ais_util.h>

struct cpg_inst {
	int response_fd;
	int dispatch_fd;
	int finalize;
	cpg_flow_control_state_t flow_control_state;
	cpg_callbacks_t callbacks;
	void *context;
	pthread_mutex_t response_mutex;
	pthread_mutex_t dispatch_mutex;
};

static void cpg_instance_destructor (void *instance);

static struct saHandleDatabase cpg_handle_t_db = {
	.handleCount		        = 0,
	.handles			= 0,
	.mutex				= PTHREAD_MUTEX_INITIALIZER,
	.handleInstanceDestructor	= cpg_instance_destructor
};

/*
 * Clean up function for a cpg instance (cpg_initialize) handle
 */
static void cpg_instance_destructor (void *instance)
{
	struct cpg_inst *cpg_inst = instance;

	pthread_mutex_destroy (&cpg_inst->response_mutex);
	pthread_mutex_destroy (&cpg_inst->dispatch_mutex);
}


/**
 * @defgroup cpg_corosync The closed process group API
 * @ingroup corosync
 *
 * @{
 */

cs_error_t cpg_initialize (
	cpg_handle_t *handle,
	cpg_callbacks_t *callbacks)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;

	error = saHandleCreate (&cpg_handle_t_db, sizeof (struct cpg_inst), handle);
	if (error != CS_OK) {
		goto error_no_destroy;
	}

	error = saHandleInstanceGet (&cpg_handle_t_db, *handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		goto error_destroy;
	}

	error = saServiceConnect (&cpg_inst->dispatch_fd,
				     &cpg_inst->response_fd,
		CPG_SERVICE);
	if (error != CS_OK) {
		goto error_put_destroy;
	}

	memcpy (&cpg_inst->callbacks, callbacks, sizeof (cpg_callbacks_t));

	pthread_mutex_init (&cpg_inst->response_mutex, NULL);

	pthread_mutex_init (&cpg_inst->dispatch_mutex, NULL);

	(void)saHandleInstancePut (&cpg_handle_t_db, *handle);

	return (CS_OK);

error_put_destroy:
	(void)saHandleInstancePut (&cpg_handle_t_db, *handle);
error_destroy:
	(void)saHandleDestroy (&cpg_handle_t_db, *handle);
error_no_destroy:
	return (error);
}

cs_error_t cpg_finalize (
	cpg_handle_t handle)
{
	struct cpg_inst *cpg_inst;
	cs_error_t error;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	pthread_mutex_lock (&cpg_inst->response_mutex);

	/*
	 * Another thread has already started finalizing
	 */
	if (cpg_inst->finalize) {
		pthread_mutex_unlock (&cpg_inst->response_mutex);
		(void)saHandleInstancePut (&cpg_handle_t_db, handle);
		return (CS_ERR_BAD_HANDLE);
	}

	cpg_inst->finalize = 1;

	pthread_mutex_unlock (&cpg_inst->response_mutex);

	(void)saHandleDestroy (&cpg_handle_t_db, handle);

	/*
	 * Disconnect from the server
	 */
	if (cpg_inst->response_fd != -1) {
		shutdown(cpg_inst->response_fd, 0);
		close(cpg_inst->response_fd);
	}
	if (cpg_inst->dispatch_fd != -1) {
		shutdown(cpg_inst->dispatch_fd, 0);
		close(cpg_inst->dispatch_fd);
	}
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (CS_OK);
}

cs_error_t cpg_fd_get (
	cpg_handle_t handle,
	int *fd)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	*fd = cpg_inst->dispatch_fd;

	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (CS_OK);
}

cs_error_t cpg_context_get (
	cpg_handle_t handle,
	void **context)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	*context = cpg_inst->context;

	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (CS_OK);
}

cs_error_t cpg_context_set (
	cpg_handle_t handle,
	void *context)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	cpg_inst->context = context;

	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (CS_OK);
}

struct res_overlay {
	mar_res_header_t header __attribute__((aligned(8)));
	char data[512000];
};

cs_error_t cpg_dispatch (
	cpg_handle_t handle,
	cs_dispatch_flags_t dispatch_types)
{
	struct pollfd ufds;
	int timeout = -1;
	cs_error_t error;
	int cont = 1; /* always continue do loop except when set to 0 */
	int dispatch_avail;
	struct cpg_inst *cpg_inst;
	struct res_lib_cpg_flowcontrol_callback *res_cpg_flowcontrol_callback;
	struct res_lib_cpg_confchg_callback *res_cpg_confchg_callback;
	struct res_lib_cpg_deliver_callback *res_cpg_deliver_callback;
	struct res_lib_cpg_groups_get_callback *res_lib_cpg_groups_get_callback;
	cpg_callbacks_t callbacks;
	struct res_overlay dispatch_data;
	int ignore_dispatch = 0;
	struct cpg_address member_list[CPG_MEMBERS_MAX];
	struct cpg_address left_list[CPG_MEMBERS_MAX];
	struct cpg_address joined_list[CPG_MEMBERS_MAX];
	struct cpg_name group_name;
	mar_cpg_address_t *left_list_start;
	mar_cpg_address_t *joined_list_start;
	unsigned int i;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	/*
	 * Timeout instantly for SA_DISPATCH_ONE or SA_DISPATCH_ALL and
	 * wait indefinately for SA_DISPATCH_BLOCKING
	 */
	if (dispatch_types == CS_DISPATCH_ALL) {
		timeout = 0;
	}

	do {
		ufds.fd = cpg_inst->dispatch_fd;
		ufds.events = POLLIN;
		ufds.revents = 0;

		error = saPollRetry (&ufds, 1, timeout);
		if (error != CS_OK) {
			goto error_nounlock;
		}

		pthread_mutex_lock (&cpg_inst->dispatch_mutex);

		/*
		 * Regather poll data in case ufds has changed since taking lock
		 */
		error = saPollRetry (&ufds, 1, timeout);
		if (error != CS_OK) {
			goto error_nounlock;
		}

		/*
		 * Handle has been finalized in another thread
		 */
		if (cpg_inst->finalize == 1) {
			error = CS_OK;
			pthread_mutex_unlock (&cpg_inst->dispatch_mutex);
			goto error_unlock;
		}

		dispatch_avail = ufds.revents & POLLIN;
		if (dispatch_avail == 0 && dispatch_types == CS_DISPATCH_ALL) {
			pthread_mutex_unlock (&cpg_inst->dispatch_mutex);
			break; /* exit do while cont is 1 loop */
		} else
		if (dispatch_avail == 0) {
			pthread_mutex_unlock (&cpg_inst->dispatch_mutex);
			continue; /* next poll */
		}

		if (ufds.revents & POLLIN) {
			/*
			 * Queue empty, read response from socket
			 */
			error = saRecvRetry (cpg_inst->dispatch_fd, &dispatch_data.header,
				sizeof (mar_res_header_t));
			if (error != CS_OK) {
				goto error_unlock;
			}
			if (dispatch_data.header.size > sizeof (mar_res_header_t)) {
				error = saRecvRetry (cpg_inst->dispatch_fd, &dispatch_data.data,
					dispatch_data.header.size - sizeof (mar_res_header_t));

				if (error != CS_OK) {
					goto error_unlock;
				}
			}
		} else {
			pthread_mutex_unlock (&cpg_inst->dispatch_mutex);
			continue;
		}

		/*
		 * Make copy of callbacks, message data, unlock instance, and call callback
		 * A risk of this dispatch method is that the callback routines may
		 * operate at the same time that cpgFinalize has been called.
		*/
		memcpy (&callbacks, &cpg_inst->callbacks, sizeof (cpg_callbacks_t));

		pthread_mutex_unlock (&cpg_inst->dispatch_mutex);
		/*
		 * Dispatch incoming message
		 */
		switch (dispatch_data.header.id) {
		case MESSAGE_RES_CPG_DELIVER_CALLBACK:
			res_cpg_deliver_callback = (struct res_lib_cpg_deliver_callback *)&dispatch_data;

			cpg_inst->flow_control_state = res_cpg_deliver_callback->flow_control_state;
			marshall_from_mar_cpg_name_t (
				&group_name,
				&res_cpg_deliver_callback->group_name);

			callbacks.cpg_deliver_fn (handle,
				&group_name,
				res_cpg_deliver_callback->nodeid,
				res_cpg_deliver_callback->pid,
				&res_cpg_deliver_callback->message,
				res_cpg_deliver_callback->msglen);
			break;

		case MESSAGE_RES_CPG_CONFCHG_CALLBACK:
			res_cpg_confchg_callback = (struct res_lib_cpg_confchg_callback *)&dispatch_data;

			for (i = 0; i < res_cpg_confchg_callback->member_list_entries; i++) {
				marshall_from_mar_cpg_address_t (&member_list[i],
					&res_cpg_confchg_callback->member_list[i]);
			}
			left_list_start = res_cpg_confchg_callback->member_list +
				res_cpg_confchg_callback->member_list_entries;
			for (i = 0; i < res_cpg_confchg_callback->left_list_entries; i++) {
				marshall_from_mar_cpg_address_t (&left_list[i],
					&left_list_start[i]);
			}
			joined_list_start = res_cpg_confchg_callback->member_list +
				res_cpg_confchg_callback->member_list_entries +
				res_cpg_confchg_callback->left_list_entries;
			for (i = 0; i < res_cpg_confchg_callback->joined_list_entries; i++) {
				marshall_from_mar_cpg_address_t (&joined_list[i],
					&joined_list_start[i]);
			}
			marshall_from_mar_cpg_name_t (
				&group_name,
				&res_cpg_confchg_callback->group_name);

			callbacks.cpg_confchg_fn (handle,
				&group_name,
				member_list,
				res_cpg_confchg_callback->member_list_entries,
				left_list,
				res_cpg_confchg_callback->left_list_entries,
				joined_list,
				res_cpg_confchg_callback->joined_list_entries);
			break;

		case MESSAGE_RES_CPG_GROUPS_CALLBACK:
			res_lib_cpg_groups_get_callback = (struct res_lib_cpg_groups_get_callback *)&dispatch_data;
			marshall_from_mar_cpg_name_t (
				&group_name,
				&res_lib_cpg_groups_get_callback->group_name);
			for (i = 0; i < res_lib_cpg_groups_get_callback->num_members; i++) {
				marshall_from_mar_cpg_address_t (&member_list[i],
					&res_lib_cpg_groups_get_callback->member_list[i]);
			}

			callbacks.cpg_groups_get_fn(handle,
						    res_lib_cpg_groups_get_callback->group_num,
						    res_lib_cpg_groups_get_callback->total_groups,
						    &group_name,
						    member_list,
						    res_lib_cpg_groups_get_callback->num_members);

			break;

		case MESSAGE_RES_CPG_FLOWCONTROL_CALLBACK:
			res_cpg_flowcontrol_callback = (struct res_lib_cpg_flowcontrol_callback *)&dispatch_data;
			cpg_inst->flow_control_state = res_cpg_flowcontrol_callback->flow_control_state;
			break;

		default:
			error = CS_ERR_LIBRARY;
			goto error_nounlock;
			break;
		}

		/*
		 * Determine if more messages should be processed
		 * */
		switch (dispatch_types) {
		case CS_DISPATCH_ONE:
			if (ignore_dispatch) {
				ignore_dispatch = 0;
			} else {
				cont = 0;
			}
			break;
		case CS_DISPATCH_ALL:
			if (ignore_dispatch) {
				ignore_dispatch = 0;
			}
			break;
		case CS_DISPATCH_BLOCKING:
			break;
		}
	} while (cont);

error_unlock:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);
error_nounlock:
	return (error);
}

cs_error_t cpg_join (
    cpg_handle_t handle,
    struct cpg_name *group)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;
	struct iovec iov[2];
	struct req_lib_cpg_join req_lib_cpg_join;
	struct res_lib_cpg_join res_lib_cpg_join;
	struct req_lib_cpg_trackstart req_lib_cpg_trackstart;
	struct res_lib_cpg_trackstart res_lib_cpg_trackstart;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	pthread_mutex_lock (&cpg_inst->response_mutex);

	/* Automatically add a tracker */
	req_lib_cpg_trackstart.header.size = sizeof (struct req_lib_cpg_trackstart);
	req_lib_cpg_trackstart.header.id = MESSAGE_REQ_CPG_TRACKSTART;
	marshall_to_mar_cpg_name_t (&req_lib_cpg_trackstart.group_name,
		group);

	iov[0].iov_base = (char *)&req_lib_cpg_trackstart;
	iov[0].iov_len = sizeof (struct req_lib_cpg_trackstart);

	error = saSendMsgReceiveReply (cpg_inst->dispatch_fd, iov, 1,
		&res_lib_cpg_trackstart, sizeof (struct res_lib_cpg_trackstart));

	if (error != CS_OK) {
		pthread_mutex_unlock (&cpg_inst->response_mutex);
		goto error_exit;
	}

	/* Now join */
	req_lib_cpg_join.header.size = sizeof (struct req_lib_cpg_join);
	req_lib_cpg_join.header.id = MESSAGE_REQ_CPG_JOIN;
	req_lib_cpg_join.pid = getpid();
	marshall_to_mar_cpg_name_t (&req_lib_cpg_join.group_name,
		group);

	iov[0].iov_base = (char *)&req_lib_cpg_join;
	iov[0].iov_len = sizeof (struct req_lib_cpg_join);

	error = saSendMsgReceiveReply (cpg_inst->response_fd, iov, 1,
		&res_lib_cpg_join, sizeof (struct res_lib_cpg_join));

	pthread_mutex_unlock (&cpg_inst->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

	error = res_lib_cpg_join.header.error;

error_exit:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}

cs_error_t cpg_leave (
    cpg_handle_t handle,
    struct cpg_name *group)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;
	struct iovec iov[2];
	struct req_lib_cpg_leave req_lib_cpg_leave;
	struct res_lib_cpg_leave res_lib_cpg_leave;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cpg_leave.header.size = sizeof (struct req_lib_cpg_leave);
	req_lib_cpg_leave.header.id = MESSAGE_REQ_CPG_LEAVE;
	req_lib_cpg_leave.pid = getpid();
	marshall_to_mar_cpg_name_t (&req_lib_cpg_leave.group_name,
		group);

	iov[0].iov_base = (char *)&req_lib_cpg_leave;
	iov[0].iov_len = sizeof (struct req_lib_cpg_leave);

	pthread_mutex_lock (&cpg_inst->response_mutex);

	error = saSendMsgReceiveReply (cpg_inst->response_fd, iov, 1,
		&res_lib_cpg_leave, sizeof (struct res_lib_cpg_leave));

	pthread_mutex_unlock (&cpg_inst->response_mutex);
	if (error != CS_OK) {
		goto error_exit;
	}

	error = res_lib_cpg_leave.header.error;

error_exit:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}

cs_error_t cpg_mcast_joined (
	cpg_handle_t handle,
	cpg_guarantee_t guarantee,
	struct iovec *iovec,
	int iov_len)
{
	int i;
	cs_error_t error;
	struct cpg_inst *cpg_inst;
	struct iovec iov[64];
	struct req_lib_cpg_mcast req_lib_cpg_mcast;
	struct res_lib_cpg_mcast res_lib_cpg_mcast;
	int msg_len = 0;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	for (i = 0; i < iov_len; i++ ) {
		msg_len += iovec[i].iov_len;
	}

	req_lib_cpg_mcast.header.size = sizeof (struct req_lib_cpg_mcast) +
		msg_len;

	req_lib_cpg_mcast.header.id = MESSAGE_REQ_CPG_MCAST;
	req_lib_cpg_mcast.guarantee = guarantee;
	req_lib_cpg_mcast.msglen = msg_len;

	iov[0].iov_base = (char *)&req_lib_cpg_mcast;
	iov[0].iov_len = sizeof (struct req_lib_cpg_mcast);
	memcpy (&iov[1], iovec, iov_len * sizeof (struct iovec));

	pthread_mutex_lock (&cpg_inst->response_mutex);

	error = saSendMsgReceiveReply (cpg_inst->response_fd, iov, iov_len + 1,
		&res_lib_cpg_mcast, sizeof (res_lib_cpg_mcast));

	pthread_mutex_unlock (&cpg_inst->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

/*	Only update the flow control state when the return value is OK.
 *	Otherwise the flow control state is not guaranteed to be valid in the
 *	return message.
 *	Also, don't set to ENABLED if the return value is TRY_AGAIN as this can lead
 *	to Flow Control State sync issues between AIS LIB and EXEC.
 */
	if (res_lib_cpg_mcast.header.error == CS_OK) {
		cpg_inst->flow_control_state = res_lib_cpg_mcast.flow_control_state;
	}
	error = res_lib_cpg_mcast.header.error;

error_exit:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}

cs_error_t cpg_membership_get (
	cpg_handle_t handle,
	struct cpg_name *group_name,
	struct cpg_address *member_list,
	int *member_list_entries)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;
	struct iovec iov;
	struct req_lib_cpg_membership req_lib_cpg_membership_get;
	struct res_lib_cpg_confchg_callback res_lib_cpg_membership_get;
	unsigned int i;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cpg_membership_get.header.size = sizeof (mar_req_header_t);
	req_lib_cpg_membership_get.header.id = MESSAGE_REQ_CPG_MEMBERSHIP;
	marshall_to_mar_cpg_name_t (&req_lib_cpg_membership_get.group_name,
		group_name);

	iov.iov_base = (char *)&req_lib_cpg_membership_get;
	iov.iov_len = sizeof (mar_req_header_t);

	pthread_mutex_lock (&cpg_inst->response_mutex);

	error = saSendMsgReceiveReply (cpg_inst->response_fd, &iov, 1,
		&res_lib_cpg_membership_get, sizeof (mar_res_header_t));

	pthread_mutex_unlock (&cpg_inst->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

	error = res_lib_cpg_membership_get.header.error;

	/*
	 * Copy results to caller
	 */
	*member_list_entries = res_lib_cpg_membership_get.member_list_entries;
	if (member_list) {
		for (i = 0; i < res_lib_cpg_membership_get.member_list_entries; i++) {
			marshall_from_mar_cpg_address_t (&member_list[i],
				&res_lib_cpg_membership_get.member_list[i]);
		}
	}

error_exit:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}

cs_error_t cpg_local_get (
	cpg_handle_t handle,
	unsigned int *local_nodeid)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;
	struct iovec iov;
	struct req_lib_cpg_local_get req_lib_cpg_local_get;
	struct res_lib_cpg_local_get res_lib_cpg_local_get;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cpg_local_get.header.size = sizeof (mar_req_header_t);
	req_lib_cpg_local_get.header.id = MESSAGE_REQ_CPG_LOCAL_GET;

	iov.iov_base = &req_lib_cpg_local_get;
	iov.iov_len = sizeof (struct req_lib_cpg_local_get);

	pthread_mutex_lock (&cpg_inst->response_mutex);

	error = saSendMsgReceiveReply (cpg_inst->response_fd, &iov, 1,
		&res_lib_cpg_local_get, sizeof (res_lib_cpg_local_get));

	pthread_mutex_unlock (&cpg_inst->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

	error = res_lib_cpg_local_get.header.error;

	*local_nodeid = res_lib_cpg_local_get.local_nodeid;

error_exit:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}

cs_error_t cpg_groups_get (
	cpg_handle_t handle,
	unsigned int *num_groups)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;
	struct iovec iov;
	struct req_lib_cpg_groups_get req_lib_cpg_groups_get;
	struct res_lib_cpg_groups_get res_lib_cpg_groups_get;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cpg_groups_get.header.size = sizeof (mar_req_header_t);
	req_lib_cpg_groups_get.header.id = MESSAGE_REQ_CPG_GROUPS_GET;

	iov.iov_base = &req_lib_cpg_groups_get;
	iov.iov_len = sizeof (struct req_lib_cpg_groups_get);

	pthread_mutex_lock (&cpg_inst->response_mutex);

	error = saSendMsgReceiveReply (cpg_inst->response_fd, &iov, 1,
		&res_lib_cpg_groups_get, sizeof (res_lib_cpg_groups_get));

	pthread_mutex_unlock (&cpg_inst->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

	*num_groups = res_lib_cpg_groups_get.num_groups;
	error = res_lib_cpg_groups_get.header.error;

	/* Real output is delivered via a callback */
error_exit:
	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}

cs_error_t cpg_flow_control_state_get (
	cpg_handle_t handle,
	cpg_flow_control_state_t *flow_control_state)
{
	cs_error_t error;
	struct cpg_inst *cpg_inst;

	error = saHandleInstanceGet (&cpg_handle_t_db, handle, (void *)&cpg_inst);
	if (error != CS_OK) {
		return (error);
	}

	*flow_control_state = cpg_inst->flow_control_state;

	(void)saHandleInstancePut (&cpg_handle_t_db, handle);

	return (error);
}
/** @} */
