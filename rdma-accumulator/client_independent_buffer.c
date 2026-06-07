#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define SERVER_PORT 12345
#define N 100

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list || !dev_list[0]) {
        fprintf(stderr, "No RDMA device\n");
        return 1;
    }
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx) {
        perror("ibv_open_device");
        return 1;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        return 1;
    }

    uint64_t *send_bufs = (uint64_t *)malloc(N * sizeof(uint64_t));
    for (int i = 0; i < N; i++) send_bufs[i] = i + 1;

    struct ibv_mr *mr = ibv_reg_mr(pd, send_bufs, N * sizeof(uint64_t), IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        free(send_bufs);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq");
        ibv_dereg_mr(mr);
        free(send_bufs);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq, .recv_cq = cq,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init);
    if (!qp) {
        perror("ibv_create_qp");
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(send_bufs);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_qp_attr init_attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE
    };
    if (ibv_modify_qp(qp, &init_attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "modify_qp to INIT failed\n");
        return 1;
    }

    union ibv_gid local_gid;
    if (ibv_query_gid(ctx, 1, 1, &local_gid)) {
        fprintf(stderr, "Failed to query GID\n");
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(SERVER_PORT) };
    inet_pton(AF_INET, server_ip, &addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("TCP connected\n");

    // 先接收服务端 QP 信息
    uint32_t remote_qpn;
    uint64_t remote_addr;
    uint32_t remote_rkey;
    union ibv_gid remote_gid;
    if (recv(sock, &remote_qpn, 4, 0) != 4 ||
        recv(sock, &remote_addr, 8, 0) != 8 ||
        recv(sock, &remote_rkey, 4, 0) != 4 ||
        recv(sock, &remote_gid, sizeof(remote_gid), 0) != sizeof(remote_gid)) {
        fprintf(stderr, "Failed to receive remote QP info\n");
        return 1;
    }
    printf("Received remote QPN=%u, ring_addr=%lx\n", remote_qpn, remote_addr);

    // 发送本地 QP 信息
    uint32_t qpn_local = qp->qp_num;
    uint64_t addr_local = (uint64_t)send_bufs;
    uint32_t rkey_local = mr->rkey;
    send(sock, &qpn_local, 4, 0);
    send(sock, &addr_local, 8, 0);
    send(sock, &rkey_local, 4, 0);
    send(sock, &local_gid, sizeof(local_gid), 0);
    printf("Sent local QP info\n");

    // QP -> RTR
    struct ibv_qp_attr rtr_attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = remote_qpn,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 0x12,
        .ah_attr = {
            .is_global = 1,
            .port_num = 1,
            .grh = { .dgid = remote_gid, .sgid_index = 1, .hop_limit = 1 }
        }
    };
    if (ibv_modify_qp(qp, &rtr_attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "modify_qp to RTR failed\n");
        return 1;
    }

    // QP -> RTS
    struct ibv_qp_attr rts_attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 0x12,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = 0,
        .max_rd_atomic = 1
    };
    ibv_modify_qp(qp, &rts_attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);

    // 循环发送：每个值写入服务端数组的不同偏移
    struct ibv_send_wr *bad_wr;
    for (int i = 0; i < N; i++) {
        uint64_t *ptr = &send_bufs[i];
        struct ibv_sge sge = { .addr = (uint64_t)ptr, .length = 8, .lkey = mr->lkey };
        struct ibv_send_wr wr = {
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma = { .remote_addr = remote_addr + i*8, .rkey = remote_rkey }
        };
        if (ibv_post_send(qp, &wr, &bad_wr)) {
            perror("ibv_post_send");
            break;
        }
        struct ibv_wc wc;
        while (ibv_poll_cq(cq, 1, &wc) == 0);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "RDMA write failed at index %d, status=%d\n", i, wc.status);
            break;
        }
        if ((i+1) % 10 == 0) printf("Sent %d values\n", i+1);
    }
    printf("All %d values sent.\n", N);

    close(sock);
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    free(send_bufs);
    return 0;
}