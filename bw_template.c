/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <stdlib.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include <infiniband/verbs.h>
#include <errno.h>
#define WC_BATCH (10)
#define BUFFER_SIZE 1024
#define WARMUPS 10

enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

static int page_size;


struct rdma_holder{
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    void* remote_address;
    uint32_t remote_key;
};

struct rdma_holder srdma;
clock_t begin;

struct pingpong_context {
    struct ibv_context		*context;
    struct ibv_comp_channel	*channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    struct ibv_qp_cap           cap; /*FOR MAX INLINE*/
    void			*buf;
    int				size;
    int				rx_depth;
    int				routs;
    struct ibv_port_attr	portinfo;
};

struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

enum ibv_mtu pp_mtu_to_enum(int mtu)
{
    switch (mtu) {
    case 256:  return IBV_MTU_256;
    case 512:  return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default:   return -1;
    }
}

uint16_t pp_get_local_lid(struct ibv_context *context, int port)
{
    struct ibv_port_attr attr;

    if (ibv_query_port(context, port, &attr))
        return 0;

    return attr.lid;
}

int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    uint32_t v32;
    int i;

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
    }
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                          enum ibv_mtu mtu, int sl,
                          struct pingpong_dest *dest, int sgid_idx)
{
    struct ibv_qp_attr attr = {
            .qp_state		= IBV_QPS_RTR,
            .path_mtu		= mtu,
            .dest_qp_num		= dest->qpn,
            .rq_psn			= dest->psn,
            .max_dest_rd_atomic	= 1,
            .min_rnr_timer		= 12,
            .ah_attr		= {
                    .is_global	= 0,
                    .dlid		= dest->lid,
                    .sl		= sl,
                    .src_path_bits	= 0,
                    .port_num	= port
            }
    };

    if (dest->gid.global.interface_id) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = dest->gid;
        attr.ah_attr.grh.sgid_index = sgid_idx;
    }
    if (ibv_modify_qp(ctx->qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_AV                 |
            IBV_QP_PATH_MTU           |
            IBV_QP_DEST_QPN           |
            IBV_QP_RQ_PSN             |
            IBV_QP_MAX_DEST_RD_ATOMIC |
            IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state	    = IBV_QPS_RTS;
    attr.timeout	    = 14;
    attr.retry_cnt	    = 7;
    attr.rnr_retry	    = 7;
    attr.sq_psn	    = my_psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(ctx->qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_TIMEOUT            |
            IBV_QP_RETRY_CNT          |
            IBV_QP_RNR_RETRY          |
            IBV_QP_SQ_PSN             |
            IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
                                                 const struct pingpong_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        return NULL;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    /* ---- client read server fields ---- */
    if (read(sockfd, &srdma, sizeof (struct rdma_holder)) != sizeof (struct rdma_holder)) {
        perror("client read");
        fprintf(stderr, "Couldn't read remote address\n");
        goto out;
    }
    write(sockfd, "done", sizeof "done");

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;
    /* -- */
    sscanf(srdma.msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    out:
    close(sockfd);
    return rem_dest;
}

static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                                 int ib_port, enum ibv_mtu mtu,
                                                 int port, int sl,
                                                 const struct pingpong_dest *my_dest,
                                                 int sgid_idx)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_flags    = AI_PASSIVE,
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", port);
        return NULL;
    }

    listen(sockfd, 1); /*LISTEN*/
    connfd = accept(sockfd, NULL, 0); /*ACCEPT*/
    close(sockfd);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return NULL;
    }
    /* server read */
    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, sgid_idx)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }


    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    
    /* get memory reg of ctx , wrap msg, and transfer its remote addr and rkey for access.. */
    struct ibv_mr ctMR = *(ctx->mr);
    struct rdma_holder transfer;
    strncpy(transfer.msg, msg, sizeof(msg));
    transfer.remote_address = ctx->buf; /*the start point of mem area from buf*/
    transfer.remote_key = (uint32_t)ctMR.rkey;
    
    if (write(connfd, &transfer, sizeof (struct rdma_holder)) != sizeof (struct rdma_holder)) {
        fprintf(stderr, "Couldn't send local address\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    read(connfd, msg, sizeof msg);

    out:
    close(connfd);
    return rem_dest;
}

#include <sys/param.h>

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int tx_depth, int port,
                                            int use_event, int is_server)
{
    struct pingpong_context *ctx;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size     = size;
    ctx->rx_depth = rx_depth;
    ctx->routs    = rx_depth;

    ctx->buf = malloc(roundup(size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }

    memset(ctx->buf, 0x7b + is_server, size);

    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        return NULL;
    }

    if (use_event) {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            fprintf(stderr, "Couldn't create completion channel\n");
            return NULL;
        }
    } else
        ctx->channel = NULL;

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return NULL;
    }
    /* Memory Registration - generate rkey, remote_address */
    /* add flag for rdma -- > IBV_ACCESS_REMOTE_WRITE */
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    /* Creating Completion Queue */
    ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
            ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

    {
        struct ibv_qp_init_attr attr = {
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .cap     = {
                        .max_send_wr  = tx_depth,
                        .max_recv_wr  = rx_depth,
                        .max_send_sge = 1,
                        .max_recv_sge = 1
                },
                .qp_type = IBV_QPT_RC
        };
    
    /* Creating Queue Pair - {Send Queue, Recive Queue} */
        ctx->qp = ibv_create_qp(ctx->pd, &attr);
        ctx->cap = attr.cap; /* NEED THIS FOR MAX INLINE -> IN CAP */
        if (!ctx->qp)  {
            fprintf(stderr, "Couldn't create QP\n");
            return NULL;
        }
    }

    {
        struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = port,
                .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                IBV_ACCESS_REMOTE_WRITE
        };

        if (ibv_modify_qp(ctx->qp, &attr,
                IBV_QP_STATE              |
                IBV_QP_PKEY_INDEX         |
                IBV_QP_PORT               |
                IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            return NULL;
        }
    }

    return ctx;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ctx->channel) {
        if (ibv_destroy_comp_channel(ctx->channel)) {
            fprintf(stderr, "Couldn't destroy completion channel\n");
            return 1;
        }
    }

    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }

    free(ctx->buf);
    free(ctx);

    return 0;
}

/* POST Recive */
static int pp_post_recv(struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t) ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };
    struct ibv_recv_wr wr = {
            .wr_id	    = PINGPONG_RECV_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .next       = NULL
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
            break;

    return i;
}


/*POST Send RDMA IMMEDIATE - different opcode */
/* only by the client */
static int post_send_rdma_ime(struct pingpong_context *ctx){
    int flags = IBV_SEND_SIGNALED;
   if(ctx->size <= ctx->cap.max_inline_data){
        flags= IBV_SEND_SIGNALED | IBV_SEND_INLINE;    
    }
    /* INLINE CASE - need to add the flag IBV_SEND_INLINE */
    if(ctx->size <= ctx->cap.max_inline_data){
        flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
        
    }
    
    struct ibv_sge list = {
            .addr	= (uint64_t)ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };
    
    struct ibv_send_wr *bad_wr, wr = {
                .wr_id	    = PINGPONG_SEND_WRID,
                .sg_list    = &list,
                .num_sge    = 1,
                .opcode     = IBV_WR_RDMA_WRITE_WITH_IMM,
                .send_flags = flags,
                .next       = NULL,
                .wr.rdma.rkey= srdma.remote_key,
                .wr.rdma.remote_addr=(uint64_t)srdma.remote_address               
    };
    
    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

/*POST Send */
/*  server 
    client - by RDMA*/
static int pp_post_send(struct pingpong_context *ctx, char* servername)
{
    int flags = IBV_SEND_SIGNALED;
    
    /* INLINE CASE - need to add the flag IBV_SEND_INLINE */
     if(ctx->size <= ctx->cap.max_inline_data){
        flags= IBV_SEND_SIGNALED | IBV_SEND_INLINE;    
    }
    
    /* S\G */
    struct ibv_sge list = {
            .addr	= (uint64_t)ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };
    
    /* IF SERVER */
    if (!servername){
    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = PINGPONG_SEND_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = flags,
            .next       = NULL
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
    }
    
    /* IF CLIENT */
    else{
        struct ibv_send_wr *bad_wr, wr = {
                .wr_id	    = PINGPONG_SEND_WRID,
                .sg_list    = &list,
                .num_sge    = 1,
                .opcode     = IBV_WR_RDMA_WRITE,
                .send_flags = flags,
                .next       = NULL,
                .wr.rdma.rkey= srdma.remote_key,
                .wr.rdma.remote_addr=(uint64_t)srdma.remote_address               
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
    }
}


int wait_for_last_to_be_recived(struct pingpong_context *ctx){
    int rcns = 0;
    while (rcns == 0 ){
        /* Work Request */
        struct ibv_wc wc[WC_BATCH];
        int ne, i;
        
        /* DO POLLING, have you done yet ? .. */
        do {
            ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);
        
        for (i = 0; i < ne; ++i) {
             /* Make sure each Work Completion really got success */
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                return 1;
            }
            
            /* # change this shit to one if */
            if ( (int) wc[i].wr_id == PINGPONG_RECV_WRID){
                if (--ctx->routs <= 10) {
                        ctx->routs += pp_post_recv(ctx, ctx->rx_depth - ctx->routs);
                        if (ctx->routs < ctx->rx_depth) {
                            fprintf(stderr,
                                    "Couldn't post receive (%d)\n",
                                    ctx->routs);
                            return 1;
                        }
                    }
                rcns++;
            }
        } 
    }
    return 0;
}



int pp_wait_completions(struct pingpong_context *ctx, int iters)
{
    int rcnt = 0, scnt = 0; /*num of receives num of sent*/
    while (rcnt + scnt < iters) {
        /*Creating Work Completion */
        struct ibv_wc wc[WC_BATCH];
        int ne, i;
    
        /* DO POLLING, have you done yet ? .. */
        do {
            ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);
        
        
        for (i = 0; i < ne; ++i) {
            
            /* Make sure each Work Completion really got success */
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                return 1;
            }
            
            /* Switch CASE on each Work Completion - to see if it's post send\recive\send rdma and update*/
            switch ((int) wc[i].wr_id) {
    
            case PINGPONG_SEND_WRID:
                ++scnt;
                break;

            case PINGPONG_RECV_WRID:
                if (--ctx->routs <= 10) {
                    ctx->routs += pp_post_recv(ctx, ctx->rx_depth - ctx->routs);
                    if (ctx->routs < ctx->rx_depth) {
                        fprintf(stderr,
                                "Couldn't post receive (%d)\n",
                                ctx->routs);
                        return 1;
                    }
                }
                ++rcnt;
                break;
                
           
            default:
                fprintf(stderr, "Completion for unknown wr_id %d\n",
                        (int) wc[i].wr_id);
                return 1;
            }
        }

    }
    return 0;
}

static void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
    printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
    printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
    printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
    printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
    printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
    printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
    printf("  -l, --sl=<sl>          service level value\n");
    printf("  -e, --events           sleep on CQ events (default poll)\n");
    printf("  -g, --gid-idx=<gid index> local port gid index\n");
}

int main(int argc, char *argv[])
{
    
    /*----------------------------------------------------
     * |We chose 1000 iters such that in iteration num1000 the average time
     * |will change in less than 1% so we observed that this is the case for iters to be about 1000
     * |we sent 10 msgs for warmingup (and that for each msg_size = {1,2,4,8,....1024=1KB}
     * |we tested the code for different amount of warmups, few warmups caused less stability.
     * |MAX INLINE =  we can't determine the actual value, BUT -->
     * |if the send size is less than the cap.max_inline_data reported by the qp,
     * |we had to use IBV_SEND_INLINE flag. Therefore, we used cap.max_inline_data
     * -------------------------------------------------*/
    
    
    struct ibv_device      **dev_list;
    struct ibv_device       *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest     my_dest;
    struct pingpong_dest    *rem_dest;
    char                    *ib_devname = NULL;
    char                    *servername;
    int                      port = 24333;
    int                      ib_port = 1;
    enum ibv_mtu             mtu = IBV_MTU_2048;
    int                      rx_depth = 100;
    int                      tx_depth = 100;
    int                      iters = 1000;
    int                      use_event = 0;
    int                      size = 1024;
    int                      sl = 0;
    int                      gidx = -1;
    char                     gid[33];

    srand48(getpid() * time(NULL));

    while (1) {
        int c;

        static struct option long_options[] = {
                { .name = "port",     .has_arg = 1, .val = 'p' },
                { .name = "ib-dev",   .has_arg = 1, .val = 'd' },
                { .name = "ib-port",  .has_arg = 1, .val = 'i' },
                { .name = "size",     .has_arg = 1, .val = 's' },
                { .name = "mtu",      .has_arg = 1, .val = 'm' },
                { .name = "rx-depth", .has_arg = 1, .val = 'r' },
                { .name = "iters",    .has_arg = 1, .val = 'n' },
                { .name = "sl",       .has_arg = 1, .val = 'l' },
                { .name = "events",   .has_arg = 0, .val = 'e' },
                { .name = "gid-idx",  .has_arg = 1, .val = 'g' },
                { 0 }
        };

        c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:eg:", long_options, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);
            if (port < 0 || port > 65535) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 'd':
            ib_devname = strdup(optarg);
            break;

        case 'i':
            ib_port = strtol(optarg, NULL, 0);
            if (ib_port < 0) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 's':
            size = strtol(optarg, NULL, 0);
            break;

        case 'm':
            mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
            if (mtu < 0) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 'r':
            rx_depth = strtol(optarg, NULL, 0);
            break;

        case 'n':
            iters = strtol(optarg, NULL, 0);
            break;

        case 'l':
            sl = strtol(optarg, NULL, 0);
            break;

        case 'e':
            ++use_event;
            break;

        case 'g':
            gidx = strtol(optarg, NULL, 0);
            break;

        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind == argc - 1)
        servername = strdup(argv[optind]);
    else if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!ib_devname) {
        ib_dev = *dev_list;
        if (!ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                break;
        ib_dev = dev_list[i];
        if (!ib_dev) {
            fprintf(stderr, "IB device %s not found\n", ib_devname);
            return 1;
        }
    }

    ctx = pp_init_ctx(ib_dev, size, rx_depth, tx_depth, ib_port, use_event, !servername);
    if (!ctx)
        return 1;

    ctx->routs = pp_post_recv(ctx, ctx->rx_depth);
    if (ctx->routs < ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", ctx->routs);
        return 1;
    }

    if (use_event)
        if (ibv_req_notify_cq(ctx->cq, 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            return 1;
        }


    if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_dest.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }

    if (gidx >= 0) {
        if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
            fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
            return 1;
        }
    } else
        memset(&my_dest.gid, 0, sizeof my_dest.gid);

    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
    /*printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           my_dest.lid, my_dest.qpn, my_dest.psn, gid);*/

    /* IF CLIENT */
    if (servername)
        rem_dest = pp_client_exch_dest(servername, port, &my_dest);
    else
        /* IF SERVER */
        rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx);

    if (!rem_dest)
        return 1;

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
    /*printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);*/
    
    /* IF CLIENT --> connect ctx */
    if (servername)
        if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx))
            return 1;
    
    /* ------- Client Loop ------- */
    if (servername) {
        int i;
        int msg_size;
        for(msg_size = 1; msg_size<=BUFFER_SIZE; msg_size=msg_size<<1){

            memset(ctx->buf,'a',msg_size);
            ctx->size = msg_size;
            /*-----------------------------------------------------------*/
            for (i = 0; i < iters+WARMUPS-1; i++) {
                
                /* if finish with WARMUPS --> start clocking ! */
                if(i==WARMUPS){
                    begin = clock();
                }
                
                /* client send RDMA */
                if(pp_post_send(ctx,servername)){
                    fprintf(stderr, "Client ouldn't post send RDMA\n");
                    return 1;
                }
                
                /* wait for completion */
                pp_wait_completions(ctx,1);
            }
            /*-----------------------------------------------------------*/
            
            /* client send msg #X in size=msg_size to server by RDMA IMMEDIATE */
            if(post_send_rdma_ime(ctx)){
                fprintf(stderr, "Client ouldn't post send RDMA IMMEDIATE\n");
                return 1;
            }
            
            wait_for_last_to_be_recived(ctx);
            int bytes = msg_size*iters;
            float time_passed = (double)(clock() -begin)  / (double)(CLOCKS_PER_SEC);
            printf("%d\t%f\tBps\n",msg_size,bytes/ (double)time_passed);
        }
        
        printf("Client Done.\n");
        
    } else {
        
        int i;
        /* ------- Server Loop ------- */
        for(i=1; i<=BUFFER_SIZE; i=i<<1){
            
            /* server wait to get the X msg in size=i, that was send by IMMEDIATE */
            wait_for_last_to_be_recived(ctx);
            /* server replies after all X msgs have arrived */
            if (pp_post_send(ctx,servername)) {
            fprintf(stderr, "Server couldn't post send\n");
            return 1;
            }
            
            /* wait for completion */
            pp_wait_completions(ctx,1);
        }
        
        printf("Server Done.\n");
    }

    ibv_free_device_list(dev_list);
    free(rem_dest);
    return 0;
}
