#pragma once
#include "exfs.h"
#include "journal.h"

/*  Inode & Block Allocation Internals  */
uint64_t exfs_resolve_block(exfs_volume_t *vol, exfs_inode_t *in,
                           uint64_t file_block, bool alloc,
                           bool *inode_dirty);

void exfs_compute_whole_file_hash(exfs_volume_t *vol,
                                  exfs_inode_t *in,
                                  uint8_t out_hash[32]);

void exfs_append_prov(exfs_volume_t *vol, uint64_t inode_num,
                      exfs_inode_t *in, uint8_t op, uint64_t user_id);
