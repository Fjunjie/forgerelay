# 部署示例（M5 交付，需求 §16）

- `forgerelayd.service`：systemd 单元示例（低权限用户 + 沙箱化 + 优雅停止）。
- 初始化（首次部署）：

```bash
sudo useradd --system --home /var/lib/forgerelay forgerelay
sudo mkdir -p /etc/forgerelay /var/lib/forgerelay /var/log/forgerelay
sudo cp config/forgerelay.example.toml /etc/forgerelay/server.toml
sudo chown -R forgerelay:forgerelay /var/lib/forgerelay /var/log/forgerelay
sudo chmod 750 /var/lib/forgerelay /var/log/forgerelay   # SEC-03 最小权限
sudo chmod 640 /etc/forgerelay/server.toml

# 数据库初始化工具：创建首个 Admin 用户并打印一次性令牌（FR-AUTH-03）
sudo -u forgerelay forgerelayd --config /etc/forgerelay/server.toml --create-admin admin
sudo systemctl enable --now forgerelayd
```

- TLS：生成证书/私钥后填入 `[tls]`（SEC-01/02；私钥权限 0600）。
