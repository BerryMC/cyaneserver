# 协议 340（1.12.2）实现与验证状态

依据优先级：**本机 jar 行为观测（oracle） > 反编译源码 > minecraft.wiki**。
wiki.vg 已并入 minecraft.wiki，页面标注 `[vg]` 来源，其"当前协议"跟随最新版本，不含 340 细节——因此 1.12.2 的事实一律以 jar 为准。

## 已验证事实（探针实测原版服务端）

| 事实 | 值 |
|---|---|
| 握手 SB 0x00 | `protocol VarInt | host String | port u16 | next VarInt`（next=1 status / 2 login） |
| 状态 SB 0x00 / CB 0x00 | 请求为空；响应为 `JSON String` |
| Ping SB 0x01 / Pong CB 0x01 | `int64`，原样回显 |
| 状态响应 JSON 键序 | `description → players → version`（可选 `favicon`） |
| 登录 SB 0x00 LoginStart | `name String(≤16)` |
| 登录 CB 0x01 EncryptionRequest | `serverId String(空) | 公钥 VarInt+bytes | verifyToken VarInt+bytes(4)`；公钥为 **1024 位 RSA 的 X.509 SPKI DER，162 字节** |
| 登录 CB 0x03 SetCompression | `threshold VarInt`（默认 256），**先于 LoginSuccess 发出** |
| 登录 CB 0x02 LoginSuccess | `uuid String | name String` |
| offline 模式 | 跳过 EncryptionRequest：`SetCompression → LoginSuccess → JoinGame(0x23)`，无加密 |
| online 模式（M1b） | `EncryptionRequest → EncryptionResponse → SetCompression → LoginSuccess`，AES-128-CFB8 加密 |
| 位置编码 | `x(26) << 38 | y(12) << 26 | z(26)`（见 R-008） |

以上由 `tools/probe_status.py` / `tools/probe_login.py` 打原版 1.12.2 服务端观测，`docs/reference-log.md` 对应记录。

## 逐字节对照

状态响应已与原版逐字节一致（`tests/test_status.cpp` 的黄金向量，来自真实抓包）：

```
00 71 {"description":{"text":"..."},"players":{"max":N,"online":M},"version":{"name":"1.12.2","protocol":340}}
```

## 包覆盖状态

| 状态 | 方向 | 已实现 | 验证 |
|---|---|---|---|
| Handshake | SB | 0x00 | 探针 |
| Status | SB/CB | 0x00/0x01 全部 | 逐字节对照 |
| Login | SB | 0x00 | 探针 |
| Login | CB | 0x00 Disconnect | 探针 |
| Login | CB | 0x01/0x02/0x03 | M1b 完成 |
| Play | — | 未开始 | M2 |

## 编码决策

- VarInt/VarLong：7 位一组小端，负数按 32/64 位补码展开（5/10 字节上限）。
- String：**字节数**为长度的 VarInt 前缀，UTF-8 校验，长度上限 32767 字符（协议值）× 4 字节。
- 压缩帧：`长度 | 未压缩长度(0=未压缩) | 数据`；低于阈值直发。压缩用 zlib 封装格式（`compress2/uncompress`），zlib-ng compat 构建提供。
- 加密（M1b，已实现）：AES-128-CFB8，IV = 密钥前 16 字节；RSA-1024 密钥生成与解密（OAEP）；SHA-1 用于 serverId 签名与离线 UUID 派生；`SessionService` 接口 + `MojangSessionService`（HTTPS GET）；密文读写路径已接入 `Connection`。
