// ============================================
// 备注：加密操作 C API
//
// 备注：封裝用户空间密码学操作（read_passphrase, derive_passphrase, add_key 等）。
// 备注：与 Rust 的 key.rs 配合使用：
// 备注：  Rust 端：Passphrase 输入、ChaCha20 派生、keyctl 管理
// 备注：  C 端：read_passphrase() 从终端读取口令（关闭 echo）
//
// 备注：密码学流程：
// 备注：  1. read_passphrase() — 读取用户口令
// 备注：  2. derive_passphrase() — scrypt + ChaCha20 密钥派生
// 备注：  3. bch2_add_key() — 添加到内核 keyring
// ============================================
#ifndef _CRYPTO_H
#define _CRYPTO_H

#include "tools-util.h"

struct bch_sb;
struct bch_sb_field_crypt;
struct bch_key;
struct bch_encrypted_key;

char *read_passphrase(const char *);

struct bch_key derive_passphrase(struct bch_sb_field_crypt *, const char *);
bool bch2_sb_is_encrypted(struct bch_sb *);
bool bch2_passphrase_check(struct bch_sb *, const char *,
			   struct bch_key *, struct bch_encrypted_key *);
bool bch2_add_key(struct bch_sb *, const char *, const char *, const char *);
void bch_sb_crypt_init(struct bch_sb *sb, struct bch_sb_field_crypt *,
		       const char *);

void bch_crypt_update_passphrase(struct bch_sb *sb, struct bch_sb_field_crypt *crypt,
			struct bch_key *key, const char *new_passphrase);

#endif /* _CRYPTO_H */
