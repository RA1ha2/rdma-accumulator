#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define PORT 12345
#define ARRAY_SIZE 100  // 接收100个值

typedef struct {
    int sock_fd;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint64_t *array;    // 接收数组
} client_ctx_t;

void *handle_client(void *arg) {
    client_ctx_t *c = (client_ctx_t *)arg;
    int client_fd = c->sock_fd;

    // 先发送本地 QP 信息
    uint32_t qpn_local = c->qp->qp_num;
    uint64_t array_addr = (uint64_t)c->array;
    uint32_t rkey_local = c->mr->rkey;
    union ibv_gid local_gid;
    ibv_query_gid(c->ctx, 1, 1, &local_gid);
    send(client_fd, &qpn_local, 4, 0);
    send(client_fd, &array_addr, 8, 0);
    send(client_fd, &rkey_local, 4, 0);
    send(client_fd, &local_gid, sizeof(local_gid), 0);
    printf("Sent local QP info\n");

    // 接收客户端 QP 信息
    uint32_t remote_qpn;
    uint64_t remote_addr;
    uint32_t remote_rkey;
    union ibv_gid remote_gid;
    recv(client_fd, &remote_qpn, 4, 0);
    recv(client_fd, &remote_addr, 8, 0);
    recv(client_fd, &remote_rkey, 4, 0);
    recv(client_fd, &remote_gid, sizeof(remote_gid), 0);
    printf("Received remote QPN=%u\n", remote_qpn);

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
    ibv_modify_qp(c->qp, &rtr_attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);

    // QP -> RTS
    struct ibv_qp_attr rts_attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 0x12,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = 0,
        .max_rd_atomic = 1
    };
    ibv_modify_qp(c->qp, &rts_attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);

    printf("Client ready, waiting for %d values...\n", ARRAY_SIZE);

    // 轮询数组，直到所有槽位非零（客户端会按顺序写入）
    int received = 0;
    uint64_t total = 0;
    while (received < ARRAY_SIZE) {
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (c->array[i] != 0) {
                total += c->array[i];
                printf("Received %lu at index %d, total=%lu\n", c->array[i], i, total);
                c->array[i] = 0;  // 清零以便后续检测（可选）
                received++;
            }
        }
        usleep(1000);
    }
    printf("All values received. Total sum = %lu\n", total);

    close(client_fd);
    return NULL;
}

int main() {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list || !dev_list[0]) {
        fprintf(stderr, "No RDMA device\n");
        return 1;
    }
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx) { perror("ibv_open_device"); return 1; }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("ibv_alloc_pd"); ibv_close_device(ctx); return 1; }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT) };
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);
    printf("Server listening on port %d\n", PORT);

    while (1) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) continue;

        uint64_t *array = (uint64_t *)calloc(ARRAY_SIZE, sizeof(uint64_t));
        struct ibv_mr *mr = ibv_reg_mr(pd, array, ARRAY_SIZE * sizeof(uint64_t),
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!mr) { perror("ibv_reg_mr"); close(client_fd); free(array); continue; }

        struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
        struct ibv_qp_init_attr qp_init = {
            .send_cq = cq, .recv_cq = cq,
            .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
            .qp_type = IBV_QPT_RC
        };
        struct ibv_qp *qp = ibv_create_qp(pd, &qp_init);
        struct ibv_qp_attr init_attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = 1,
            .qp_access_flags = IBV_ACCESS_REMOTE_WRITE
        };
        ibv_modify_qp(qp, &init_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

        client_ctx_t *c = (client_ctx_t *)malloc(sizeof(client_ctx_t));
        c->sock_fd = client_fd;
        c->ctx = ctx;
        c->pd = pd;
        c->cq = cq;
        c->qp = qp;
        c->mr = mr;
        c->array = array;

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, c);
        pthread_detach(tid);
    }

    close(listen_fd);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return 0;
}