# 更新签名身份的持久备份

本目录只保存 **加密私钥备份** 和公开的身份摘要，不参与程序打包。仓库是公开仓库，明文私钥和恢复口令绝不能提交。

- 生效版本：V2.0.4。公钥 SPKI SHA-256 为 `1b54f808072ac93c97d98d007a8e8c71d41a17834a0c5d810c614e9d75a05ae7`。
- `serialctl-update-v2.0.4.encrypted.pem` 使用 PKCS#8、AES-256-CBC、PBKDF2-HMAC-SHA256 和 600000 次迭代；恢复口令为独立生成的高熵随机值。
- `identity.json` 记录密文摘要和公钥身份。恢复后必须与客户端内置公钥匹配。
- 拥有者另行私密保存恢复 ZIP，其中包含本加密备份、恢复口令文件与说明。请在电脑和密码管理器／离线介质至少各保留一份；工作区文件不算外部备份。
- 恢复口令不放在仓库、GitHub Release、应用 ZIP、更新服务器或 CI 日志中。当前集成无 GitHub Secrets 权限（HTTP 403），不能声称已托管到密钥保险库。

更换工作区后，从拥有者保存的私密恢复 ZIP 取出口令文件，再恢复同一把密钥：

```sh
python tools/recover_update_key.py \
  --password-file /private/backup/recovery-password.txt \
  --output /workspace/.serialctl-signing/serialctl-update-v2.0.4-rsa2048.pem
```

恢复工具不生成新密钥，不覆盖已有文件，不输出私钥或口令；校验身份后才以私密权限写入仓库外的目标。也可省略 `--password-file`，在不回显的提示中输入恢复口令。

后续发布继续复用这把密钥。找不到本地私钥时先恢复，不自行轮换。若私钥和口令都丢失，加密备份无法恢复；若密钥泄露或确需轮换，须明确安排客户端信任迁移。

V2.0.0 的旧身份私钥未能找回，因此 V2.0.4 必须手动部署一次。此后完整 ZIP 与内嵌签名 update.json 的双文件格式保持，支持跨版本升级，不依赖中间版本或历史安装包。
