#pragma once
void test_ufs(void);
int ufs_write_diag(const void *buf);
int ufs_read_lba(uint32_t lba, void *buf);
int ufs_write_lba(uint32_t lba, const void *buf);
int ufs_capacity(uint32_t *lastlba);
