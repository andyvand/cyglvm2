/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

/* Locking functions for LVM
 * The main purpose of this part of the library is to serialise LVM
 * management operations (across a cluster if necessary)
 */


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <search.h>
#include <errno.h>

#include "metadata.h"
#include "activate.h"
#include "log.h"
#include "clvm.h"
#include "cmgr.h"


/* This gets stuck at the start of memory we allocate so we
   can sanity-check it at deallocation time */
#define LVM_SIGNATURE 0x434C564D

#define MAX_CLUSTER_MEMBER_NAME_LEN 255

/* NOTE: the LVMD uses the socket FD as the client ID, this means
   that any client that calls fork() will inherit the context of
   it's parent. */
static int clvmd_sock = -1;

/* Open connection to the Cluster Manager daemon */
static int open_local_sock(void)
{
    int local_socket;
    struct sockaddr_un sockaddr;

    /* Open local socket */
    local_socket = socket(PF_UNIX, SOCK_STREAM, 0);
    if (local_socket < 0)
    {
	perror("Can't create local socket");
	return -1;
    }

    fcntl(local_socket, F_SETFD, !FD_CLOEXEC);

    strcpy(sockaddr.sun_path, CLVMD_SOCKNAME);
    sockaddr.sun_family = AF_UNIX;
    if (connect(local_socket, (struct sockaddr *)&sockaddr, sizeof(sockaddr)))
    {
	int saved_errno = errno;

	close(local_socket);

	errno = saved_errno;
        return -1;
    }
    return local_socket;
}

/* Send a request and return the status */
static int send_request(char *inbuf, int inlen, char **retbuf)
{
    char outbuf[PIPE_BUF];
    struct clvm_header *outheader = (struct clvm_header *)outbuf;
    int len;
    int off;
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(clvmd_sock, &fds);

    /* Send it to CLVMD */
    if (write(clvmd_sock, inbuf, inlen) != inlen)
    {
	perror("Error writing to CLVMD");
	return -1;
    }

    /* Get the response */
    if ( (len = read(clvmd_sock, outbuf, sizeof(struct clvm_header))) < 0)
    {
	perror("Error reading CLVMD");
	return -1;
    }
    if (len == 0)
    {
	fprintf(stderr, "EOF reading CLVMD");
	errno = ENOTCONN;
	return -1;
    }

    /* Allocate buffer */
    *retbuf = malloc(len + outheader->arglen);
    if (!*retbuf)
    {
	errno = ENOMEM;
	return -1;
    }

    /* Copy the header */
    memcpy(*retbuf, outbuf, len);
    outheader = (struct clvm_header *)*retbuf;

    /* Read the returned values */
    off = 1; /* we've already read the first byte */

    while (off < outheader->arglen && len > 0)
    {
	len = read(clvmd_sock, outheader->args+off, PIPE_BUF);
	if (len > 0)
	    off += len;
    }

    /* Was it an error ? */
    if (outheader->status < 0)
    {
	errno = -outheader->status;
	return -2;
    }
    return 0;
}

/* Build the structure header and parse-out wildcard node names */
static void build_header(struct clvm_header *head, int cmd, char *node, void *data, int len)
{
    head->cmd      = cmd;
    head->status   = 0;
    head->flags    = 0;
    head->clientid = 0;
    head->arglen   = len;
    if (node)
    {
	/* Allow a couple of special node names:
	 "*" for all nodes,
	 "." for the local node only
	*/
	if (strcmp(node, "*") == 0)
	{
	    head->node[0] = '\0';
	}
	else if (strcmp(node, ".") == 0)
	{
	    head->node[0] = '\0';
	    head->flags = CLVMD_FLAG_LOCAL;
	}
	else
	{
	    strcpy(head->node, node);
	}
    }
    else
    {
	head->node[0] = '\0';
    }
}

/* Send a message to a(or all) node(s) in the cluster */
int cluster_write(char cmd, char *node, void *data, int len)
{
    char  outbuf[sizeof(struct clvm_header)+len+strlen(node)+1];
    char *retbuf = NULL;
    int   status;
    struct clvm_header *head = (struct clvm_header *)outbuf;

    if (clvmd_sock == -1)
	clvmd_sock = open_local_sock();
    if (clvmd_sock == -1)
	return -1;

    build_header(head, cmd, node, data, len);
    memcpy(head->node+strlen(head->node)+1, data, len);

    status = send_request(outbuf, sizeof(struct clvm_header)+strlen(head->node)+len, &retbuf);
    if (retbuf) free(retbuf);

    return status;
}



/* API: Send a message to a(or all) node(s) in the cluster
   and wait for replies */
int cluster_request(char cmd, char *node, void *data, int len,
		    lvm_response_t **response, int *num)
{
    char  outbuf[sizeof(struct clvm_header)+len+strlen(node)+1];
    int  *outptr;
    char *inptr;
    char *retbuf = NULL;
    int   status;
    int   i;
    int   num_responses=0;
    struct clvm_header *head = (struct clvm_header *)outbuf;
    lvm_response_t *rarray;

    *num = 0;

    if (clvmd_sock == -1)
	clvmd_sock = open_local_sock();
    if (clvmd_sock == -1)
	return -1;

    build_header(head, cmd, node, data, len);
    memcpy(head->node+strlen(head->node)+1, data, len);

    status = send_request(outbuf, sizeof(struct clvm_header)+strlen(head->node)+len, &retbuf);
    if (status == 0 || status == -2)
    {
	/* Count the number of responses we got */
	head = (struct clvm_header *)retbuf;
	inptr = head->args;
	while (inptr[0])
	{
	    num_responses++;
	    inptr += strlen(inptr)+1;
	    inptr += sizeof(int);
	    inptr += strlen(inptr)+1;
	}

	/* Allocate response array. With an extra pair of INTs on the front to sanity
	   check the pointer when we are given it back to free */
	outptr = malloc(sizeof(lvm_response_t) * num_responses + sizeof(int)*2);
	if (!outptr)
	{
	    if (retbuf) free(retbuf);
	    errno = ENOMEM;
	    return -1;
	}

	*response = (lvm_response_t *)(outptr+2);
	outptr[0] = LVM_SIGNATURE;
	outptr[1] = num_responses;
	rarray = *response;

	/* Unpack the response into an lvm_response_t array */
	inptr = head->args;
	i = 0;
	while (inptr[0])
	{
	    strcpy(rarray[i].node, inptr);
	    inptr += strlen(inptr)+1;

	    rarray[i].status = *(int *)inptr;
	    inptr += sizeof(int);

	    rarray[i].response = malloc(strlen(inptr)+1);
	    if (rarray[i].response == NULL)
	    {
		/* Free up everything else and return error */
		int j;
		for (j=0; j<i; j++)
		    free(rarray[i].response);
		free(outptr);
		errno = ENOMEM;
		return -1;
	    }

	    strcpy(rarray[i].response, inptr);
	    rarray[i].len = strlen(inptr);
	    inptr += strlen(inptr)+1;
	    i++;
	}
	*num = num_responses;
	*response = rarray;
    }

    if (retbuf) free(retbuf);
    return status;
}

/* API: Free reply array */
int cluster_free_request(lvm_response_t *response)
{
    int *ptr = (int *)response-2;
    int i;
    int num;

    /* Check it's ours to free */
    if (response == NULL || *ptr != LVM_SIGNATURE)
    {
	errno = EINVAL;
	return -1;
    }

    num = ptr[1];
    for (i = 0; i<num; i++)
    {
	free(response[i].response);
    }
    free(ptr);

    return 0;
}


/* These are a "higher-level" API providing black-box lock/unlock
   functions for cluster LVM...maybe */


/* Set by lock(), used by unlock() */
static int num_responses;
static lvm_response_t *response;

int lock_for_cluster(char scope, char *name, int suspend)
{
    int  status;
    int  i;
    char *args;
    int  len;
    int  cmd;

    /* Validate scope */
    if (scope != 'V' && scope != 'L' && scope != 'G')
    {
	errno = EINVAL;
	return -1;
    }

    /* Allow for NULL in name field */
    if (name)
    {
	len = strlen(name)+2;
	args = alloca(len);
	strcpy(args+1, name);
    }
    else
    {
	len = 2;
	args = alloca(len);
	args[1] = '\0';
    }
    args[0] = scope;

    cmd = (suspend)?CLVMD_CMD_LOCK_SUSPEND:CLVMD_CMD_LOCK;

    status = cluster_request(cmd,
			     "", args, len,
			     &response, &num_responses);

    /* If any nodes were down then display them and return an error */
    for (i=0; i<num_responses; i++)
    {
	if (response[i].status == -EHOSTDOWN)
	{
	    log_verbose("clvmd not running on node %s\n", response[i].node);
	    status = -1;
	}
    }

    /* If there was an error then free the memory now as the caller won't
       want to do the unlock */
    if (status)
    {
	int saved_errno = errno;
	cluster_free_request(response);
	num_responses = 0;
	errno = saved_errno;
    }
    return status;
}

int unlock_for_cluster(char scope, char *name, int suspend)
{
    int status;
    int i;
    int len;
    int failed;
    int cmd;
    int num_unlock_responses;
    char *args;
    lvm_response_t *unlock_response;

    /* We failed - this should not have been called */
    if (num_responses == 0)
	return 0;


    /* Validate scope */
    if (scope != 'V' && scope != 'L' && scope != 'G')
    {
	errno = EINVAL;
	return -1;
    }

    /* Allow for NULL in name field */
    if (name)
    {
	len = strlen(name)+2;
	args = alloca(len);
	strcpy(args+1, name);
    }
    else
    {
	len = 2;
	args = alloca(len);
	args[1] = '\0';
    }
    args[0] = scope;

    cmd = (suspend)?CLVMD_CMD_UNLOCK_RESUME:CLVMD_CMD_UNLOCK;

    /* See if it failed anywhere */
    failed = 0;
    for (i=0; i<num_responses; i++)
    {
	if (response[i].status != 0)
	    failed++;
    }

    /* If it failed on any nodes then we only unlock on
       the nodes that succeeded */
    if (failed)
    {
	for (i=0; i<num_responses; i++)
	{
	    /* Unlock the ones that succeeded */
	    if (response[i].status == 0)
	    {
		status = cluster_request(cmd,
					 response[i].node,
					 args, len,
					 &unlock_response, &num_unlock_responses);
		if (status)
		{
		    log_verbose("cluster command to node %s failed: %s\n",
				    response[i].node, strerror(errno));
		}
		else if (unlock_response[0].status != 0)
		{
		    log_verbose("unlock on node %s failed: %s\n",
				response[i].node, strerror(unlock_response[0].status));
		}
		cluster_free_request(unlock_response);
	    }
	    else
	    {
		log_verbose("command on node %s failed: '%s' - will be left locked\n",
			    response[i].node, strerror(response[i].status));
	    }
	}
    }
    else
    {
	/* All OK, we can do a full cluster unlock */
	status = cluster_request(cmd,
				 "",
				 args, len,
				 &unlock_response, &num_unlock_responses);
	if (status)
	{
	    log_verbose("cluster command failed: %s\n",
			strerror(errno));
	}
	else
	{
	    for (i=0; i<num_unlock_responses; i++)
	    {
		if (unlock_response[i].status != 0)
		{
		    log_verbose("unlock on node %s failed: %s\n",
				response[i].node, strerror(unlock_response[0].status));
		}
	    }
	}
	cluster_free_request(unlock_response);
    }
    cluster_free_request(response);

    return 0;
}


static int clustered = 0;
static int suspended = 0;

int lock_lvm(int suspend)
{
    int status;

    suspended = suspend;
    status = lock_for_cluster('G', NULL, suspend);
    if (status == -1)
    {
	/* ENOENT means clvmd is not running - assume we are not clustered */
	/* TODO: May need some way of forcing this to fail in the future */
	if (errno == ENOENT)
	{
	    clustered = 0;
	    /* TODO: Local Lock
	       suspend*/
	    return 0;
	}
	clustered = 1;
	return -1;
    }
    clustered = 1;
    return status;
}


int unlock_lvm()
{
    if (!clustered)
    {
	/* TODO: Local unlock
	   resume all */
	return 0;
    }
    else
    {
	return unlock_for_cluster('G', NULL, suspended);
    }
}



int lock_vg(struct volume_group *vg, int suspend)
{
    int status;

    suspended = suspend;
    status = lock_for_cluster('V', vg->name, suspend);
    if (status == -1)
    {
	/* ENOENT means clvmd is not running - assume we are not clustered */
	/* TODO: May need some way of forcing this to fail in the future */
	if (errno == ENOENT)
	{
	    clustered = 0;
	    /* TODO: Local Lock */
	    suspend_lvs_in_vg(vg, 1);
	    return 0;
	}
	clustered = 1;
	return -1;
    }
    clustered = 1;
    return status;
}


int unlock_vg(struct volume_group *vg)
{
    if (!clustered)
    {
	activate_lvs_in_vg(vg);
	/* TODO Local Unlock */
	return 0;
    }
    else
    {
	return unlock_for_cluster('V', vg->name, suspended);
    }
}


int lock_lv(struct logical_volume *lv, int suspend)
{
    int status;

    suspended = suspend;
    status = lock_for_cluster('L', lv->name, suspend);
    if (status == -1)
    {
	/* ENOENT means clvmd is not running - assume we are not clustered */
	/* TODO: May need some way of forcing this to fail in the future */
	if (errno == ENOENT)
	{
	    /* TODO: Local lock */
	    clustered = 0;
	    lv_suspend(lv, 1);
	    return 0;
	}
	clustered = 1;
	return -1;
    }
    clustered = 1;
    return status;
}


int unlock_lv(struct logical_volume *lv)
{
    if (!clustered)
    {
	lv_reactivate(lv);
	/* TODO Local Unlock */
	return 0;
    }
    else
    {
	return unlock_for_cluster('L', lv->name, suspended);
    }
}
