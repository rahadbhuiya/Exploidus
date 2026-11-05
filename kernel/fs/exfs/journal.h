#pragma once
#include "exfs.h"

/*  Journal Operations  */
bool exfs_journal_stage(exfs_volume_t *vol, uint64_t block_idx, const void *data);
bool exfs_meta_write_block(exfs_volume_t *vol, uint64_t block_idx, const void *data);
bool exfs_txn_read_block(exfs_volume_t *vol, uint64_t block_idx, void *out);
void exfs_journal_begin(exfs_volume_t *vol);
bool exfs_journal_commit(exfs_volume_t *vol);
void exfs_journal_replay(exfs_volume_t *vol);

/*  Block I/O Primitives  */
bool exfs_read_block(exfs_volume_t *vol, uint64_t block_idx, void *out);
bool exfs_write_block(exfs_volume_t *vol, uint64_t block_idx, const void *data);
uint64_t exfs_alloc_block(exfs_volume_t *vol);
void exfs_free_block(exfs_volume_t *vol, uint64_t blk);
bool exfs_read_inode(exfs_volume_t *vol, uint64_t inode_num, exfs_inode_t *out);
bool exfs_write_inode(exfs_volume_t *vol, uint64_t inode_num, const exfs_inode_t *in);
