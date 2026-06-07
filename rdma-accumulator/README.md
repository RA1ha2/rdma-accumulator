# RDMA 分布式累加器（RDMA Distributed Accumulator）

本项目基于 RDMA 单边写（RDMA Write） 实现了一个轻量级的分布式累加器原型。客户端通过 RDMA 直接将 1 到 100 的整数写入服务端预注册的内存数组，服务端轮询数组并累加，最终输出总和 5050。整个过程绕过服务端 CPU，展示了 RDMA 在高性能分布式计算中的核心优势。

## ✨ 功能特点

- 单边 RDMA Write：数据直接从客户端内存传输到服务端内存，服务端 CPU 零参与。
- 可靠数据传输：使用 RC（Reliable Connection）QP，保证顺序和可靠性。
- 数组轮询接收：服务端通过轮询内存数组感知数据到达，避免复杂的通知机制。
- 独立缓冲区：客户端为每个数值分配独立内存，防止数据覆盖。
- TCP 带外握手：通过普通 TCP 连接交换 QP 信息（QPN、rkey、地址、GID）。

## 🛠 环境要求

- 两台支持 Soft‑RoCE 的 Ubuntu 22.04 虚拟机（或真实 RDMA 网卡，如 Mellanox ConnectX 系列）。
- 已安装 RDMA 核心库和开发包：

  sudo apt update
  sudo apt install -y libibverbs-dev rdma-core
确保 Soft‑RoCE 设备已创建（如 rxe0）且状态为 ACTIVE：


sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev <你的网卡名>   # 例如 enp0s3
rdma link show   # 应显示 rxe0 ACTIVE
📥 编译
将仓库克隆到两台机器上，进入目录执行：


make
编译成功后生成两个可执行文件：

server – 服务端程序

client – 客户端程序

🚀 运行
服务端（假设 IP 为 192.168.3.139）：


./server
输出示例：


Server listening on port 12345
Sent local QP info
Received remote QPN=17
Client ready, waiting for 100 values...
Received 1 at index 0, total=1
...
Received 100 at index 99, total=5050
All values received. Total sum = 5050
客户端（IP 为 192.168.3.134，替换为服务端 IP）：


./client 192.168.3.139
输出示例：


TCP connected
Received remote QPN=18, ring_addr=0x55e9a817b820
Sent local QP info
Sent 10 values
Sent 20 values
...
Sent 100 values
All 100 values sent.
📁 代码结构

.
├── server_array.c              # 服务端：数组轮询，累加 1..100
├── client_independent_buffer.c # 客户端：独立缓冲区，发送 1..100
├── Makefile                    # 编译脚本
└── README.md                   # 本文件
🧠 核心原理解析
1. 控制平面（TCP 带外连接）
创建普通 TCP socket，交换双方的 QP 号、内存地址、rkey、GID。

服务端先监听，客户端主动连接；双方发送/接收顺序需严格配对。

2. 数据平面（RDMA Write）
客户端为每个整数值分配独立的内存缓冲区（send_bufs[i]），注册为 MR。

构造 ibv_send_wr，设置 opcode = IBV_WR_RDMA_WRITE，目标地址为 remote_addr + i*8（服务端数组的第 i 个槽位）。

调用 ibv_post_send 提交请求，网卡直接 DMA 数据到服务端内存。

3. 同步机制
客户端：轮询 CQ 等待每个 WR 完成，保证发送顺序。

服务端：轮询内存数组，检测每个槽位是否非零（表示已写入），累加后清零。

4. QP 状态机
必须严格按照 RESET → INIT → RTR → RTS 顺序转换：

INIT：配置端口、允许远程写。

RTR：填入对端 QP 号和 GID，启用全局路由（is_global=1）。

RTS：设置超时和重传参数，进入可工作状态。

5. GID 与全局路由
Soft‑RoCE 使用 GID（IPv6 格式）代替 LID。

ibv_query_gid(ctx, 1, 1, &gid) 获取索引 1 的 GID（通常为 IPv4 映射地址）。

修改 QP 到 RTR 时，ah_attr.grh 中填入对端 GID 和本地 GID 索引。

📈 性能与扩展
当前实现传输 100 个 8 字节整数，总数据量仅 800 字节，主要用于验证正确性。

可通过修改 N（#define N 100）发送更大数据块，实测 RDMA 带宽可接近网络线速。


RDMA Aware Programming User Manual (NVIDIA)

linux-rdma/rdma-core 源码

Soft‑RoCE 配置：RDMA over Converged Ethernet (RoCE) - Ubuntu Wiki

📝 许可
本项目采用 MIT 许可证。详见 LICENSE 文件。