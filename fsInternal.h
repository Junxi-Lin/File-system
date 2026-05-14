/**************************************************************
* Class::  CSC-415-02 Spring 2026
* Name:: Ahmed Rashad, Edson Sanchez Bernal
* Student IDs:: 924143880
* GitHub-Name:: Edsonsb
* Group-Name:: Epsilon
* Project:: Basic File System
*
* File:: fsInternal.h
*
* Description::
* This header file defines the shared internal structures and
* helper function prototypes used by the file system core.
*
**************************************************************/

#ifndef FS_INTERNAL_H
#define FS_INTERNAL_H

#include "stdint.h"
#include "stddef.h"
#include "time.h"

#define VCB_MAGIC "MFSVCB1"
#define VCB_VERSION 1
#define MAX_NAME 64
#define INITIAL_ROOT_DIR_ENTRIES 50

typedef struct
{
    char magic[8];
    uint64_t version;
    uint64_t totalBlocks;
    uint64_t blockSize;
    uint64_t freeMapStart;
    uint64_t freeMapBlocks;
    uint64_t rootDirStart;
    uint64_t rootDirBlocks;
    uint64_t rootDirEntries;
} VolumeControlBlock;

typedef struct
{
    char name[MAX_NAME];
    int in_use;
    int is_directory;
    uint64_t size;
    uint64_t location;
    time_t timestamp;
} DirectoryEntry;

int fs_allocateBlocks(uint64_t requestedBlocks, uint64_t *startBlock);
int fs_freeBlocks(uint64_t startBlock, uint64_t blockCount);
int fs_readBlocks(void *buffer, uint64_t count, uint64_t start);
int fs_writeBlocks(void *buffer, uint64_t count, uint64_t start);
int fs_resolvePath(const char *path, DirectoryEntry *entryOut,
    uint64_t *parentLocationOut, uint64_t *entryIndexOut);
int fs_resolveParentPath(const char *path, uint64_t *parentLocationOut,
    char *childNameOut, size_t childNameSize);

#endif
