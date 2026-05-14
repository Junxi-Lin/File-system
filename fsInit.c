/**************************************************************
* Class::  CSC-415-02 Spring 2026
* Name:: Fnu Atiksha, Edson Sanchez Bernal
* Student IDs:: 923641508, 924372147
* GitHub-Name:: Edsonsb
* Group-Name:: Epsilon
* Project:: Basic File System
*
* File:: fsInit.c
*
* Description:: Initializes the file system volume, VCB,
* free-space bitmap, root directory, and shared block helpers.
*
**************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fsLow.h"
#include "mfs.h"
#include "fsInternal.h"

static VolumeControlBlock g_vcb;
static unsigned char *g_freeMap = NULL;
static int g_fileSystemReady = 0;
static int g_freeMapDirty = 0;

static uint64_t ceilDivide(uint64_t numerator, uint64_t denominator)
{
    return (numerator + denominator - 1) / denominator;
}

static void setBlockAllocated(uint64_t blockNumber, int allocated)
{
    if ((g_freeMap == NULL) || (blockNumber >= g_vcb.totalBlocks))
        return;

    uint64_t byteIndex = blockNumber / 8;
    unsigned char bitMask = (unsigned char)(1 << (blockNumber % 8));

    if (allocated)
        g_freeMap[byteIndex] |= bitMask;
    else
        g_freeMap[byteIndex] &= (unsigned char)~bitMask;
}

static int isBlockAllocated(uint64_t blockNumber)
{
    if ((g_freeMap == NULL) || (blockNumber >= g_vcb.totalBlocks))
        return 1;

    uint64_t byteIndex = blockNumber / 8;
    unsigned char bitMask = (unsigned char)(1 << (blockNumber % 8));

    return ((g_freeMap[byteIndex] & bitMask) != 0);
}

static int writeVCBToDisk(void)
{
    void *blockBuffer = calloc(1, g_vcb.blockSize);
    if (blockBuffer == NULL)
        return -1;

    memcpy(blockBuffer, &g_vcb, sizeof(g_vcb));

    if (LBAwrite(blockBuffer, 1, 0) != 1)
    {
        free(blockBuffer);
        return -1;
    }

    free(blockBuffer);
    return 0;
}

static int writeFreeMapToDisk(void)
{
    if (g_freeMap == NULL)
        return -1;

    if (LBAwrite(g_freeMap, g_vcb.freeMapBlocks, g_vcb.freeMapStart) !=
        g_vcb.freeMapBlocks)
        return -1;

    g_freeMapDirty = 0;
    return 0;
}

static int loadFreeMapFromDisk(void)
{
    uint64_t freeMapBytes = g_vcb.freeMapBlocks * g_vcb.blockSize;

    g_freeMap = malloc(freeMapBytes);
    if (g_freeMap == NULL)
        return -1;

    if (LBAread(g_freeMap, g_vcb.freeMapBlocks, g_vcb.freeMapStart) !=
        g_vcb.freeMapBlocks)
    {
        free(g_freeMap);
        g_freeMap = NULL;
        return -1;
    }

    g_freeMapDirty = 0;
    return 0;
}

int fs_allocateBlocks(uint64_t requestedBlocks, uint64_t *startBlock)
{
    uint64_t currentRun = 0;
    uint64_t runStart = 0;

    if ((requestedBlocks == 0) || (startBlock == NULL) || (g_freeMap == NULL))
        return -1;

    for (uint64_t block = 0; block < g_vcb.totalBlocks; block++)
    {
        if (!isBlockAllocated(block))
        {
            if (currentRun == 0)
                runStart = block;

            currentRun++;

            if (currentRun == requestedBlocks)
            {
                for (uint64_t mark = runStart;
                     mark < (runStart + requestedBlocks); mark++)
                {
                    setBlockAllocated(mark, 1);
                }

                g_freeMapDirty = 1;
                *startBlock = runStart;
                return 0;
            }
        }
        else
        {
            currentRun = 0;
        }
    }

    return -1;
}

int fs_freeBlocks(uint64_t startBlock, uint64_t blockCount)
{
    if ((g_freeMap == NULL) || (blockCount == 0))
        return -1;

    if ((startBlock + blockCount) > g_vcb.totalBlocks)
        return -1;

    for (uint64_t block = startBlock; block < startBlock + blockCount; block++)
    {
        setBlockAllocated(block, 0);
    }

    g_freeMapDirty = 1;
    return writeFreeMapToDisk();
}

int fs_readBlocks(void *buffer, uint64_t count, uint64_t start)
{
    if ((buffer == NULL) || (count == 0))
        return -1;

    if ((start + count) > g_vcb.totalBlocks)
        return -1;

    if (LBAread(buffer, count, start) != count)
        return -1;

    return 0;
}

int fs_writeBlocks(void *buffer, uint64_t count, uint64_t start)
{
    if ((buffer == NULL) || (count == 0))
        return -1;

    if ((start + count) > g_vcb.totalBlocks)
        return -1;

    if (LBAwrite(buffer, count, start) != count)
        return -1;

    return 0;
}

static int initializeRootDirectory(void)
{
    uint64_t directoryBytes = g_vcb.rootDirBlocks * g_vcb.blockSize;
    time_t now = time(NULL);
    DirectoryEntry *entries = calloc(1, directoryBytes);

    if (entries == NULL)
        return -1;

    for (uint64_t i = 0; i < g_vcb.rootDirEntries; i++)
    {
        entries[i].in_use = 0;
    }

    strncpy(entries[0].name, ".", MAX_NAME - 1);
    entries[0].in_use = 1;
    entries[0].is_directory = 1;
    entries[0].size = directoryBytes;
    entries[0].location = g_vcb.rootDirStart;
    entries[0].timestamp = now;

    strncpy(entries[1].name, "..", MAX_NAME - 1);
    entries[1].in_use = 1;
    entries[1].is_directory = 1;
    entries[1].size = directoryBytes;
    entries[1].location = g_vcb.rootDirStart;
    entries[1].timestamp = now;

    if (LBAwrite(entries, g_vcb.rootDirBlocks, g_vcb.rootDirStart) !=
        g_vcb.rootDirBlocks)
    {
        free(entries);
        return -1;
    }

    free(entries);
    return 0;
}

static int formatVolume(uint64_t numberOfBlocks, uint64_t blockSize)
{
    uint64_t bitsPerBlock = blockSize * 8;
    uint64_t freeMapBlocks = ceilDivide(numberOfBlocks, bitsPerBlock);
    uint64_t freeMapBytes = freeMapBlocks * blockSize;
    uint64_t rootDirBytes =
        INITIAL_ROOT_DIR_ENTRIES * sizeof(DirectoryEntry);
    uint64_t rootDirBlocks = ceilDivide(rootDirBytes, blockSize);
    uint64_t rootDirStart = 0;

    memset(&g_vcb, 0, sizeof(g_vcb));
    strncpy(g_vcb.magic, VCB_MAGIC, sizeof(g_vcb.magic) - 1);

    g_vcb.version = VCB_VERSION;
    g_vcb.totalBlocks = numberOfBlocks;
    g_vcb.blockSize = blockSize;
    g_vcb.freeMapStart = 1;
    g_vcb.freeMapBlocks = freeMapBlocks;
    g_vcb.rootDirBlocks = rootDirBlocks;

    g_freeMap = calloc(1, freeMapBytes);
    if (g_freeMap == NULL)
        return -1;

    setBlockAllocated(0, 1);

    for (uint64_t block = g_vcb.freeMapStart;
         block < (g_vcb.freeMapStart + g_vcb.freeMapBlocks); block++)
    {
        setBlockAllocated(block, 1);
    }

    if (fs_allocateBlocks(rootDirBlocks, &rootDirStart) != 0)
        return -1;

    g_vcb.rootDirStart = rootDirStart;
    g_vcb.rootDirEntries =
        (rootDirBlocks * blockSize) / sizeof(DirectoryEntry);

    if (initializeRootDirectory() != 0)
        return -1;

    if (writeFreeMapToDisk() != 0)
        return -1;

    if (writeVCBToDisk() != 0)
        return -1;

    return 0;
}

static int mountVolume(uint64_t numberOfBlocks, uint64_t blockSize)
{
    void *blockBuffer = calloc(1, blockSize);
    VolumeControlBlock *diskVCB;

    if (blockBuffer == NULL)
        return -1;

    if (LBAread(blockBuffer, 1, 0) != 1)
    {
        free(blockBuffer);
        return -1;
    }

    diskVCB = (VolumeControlBlock *)blockBuffer;

    if ((memcmp(diskVCB->magic, VCB_MAGIC, strlen(VCB_MAGIC)) != 0) ||
        (diskVCB->blockSize != blockSize) ||
        (diskVCB->totalBlocks != numberOfBlocks))
    {
        free(blockBuffer);
        return formatVolume(numberOfBlocks, blockSize);
    }

    memcpy(&g_vcb, diskVCB, sizeof(g_vcb));
    free(blockBuffer);

    return loadFreeMapFromDisk();
}

int initFileSystem(uint64_t numberOfBlocks, uint64_t blockSize)
{
    printf("Initializing File System with %llu blocks with a block size of %llu\n",
        (ull_t)numberOfBlocks, (ull_t)blockSize);

    if (g_freeMap != NULL)
    {
        free(g_freeMap);
        g_freeMap = NULL;
    }

    if (mountVolume(numberOfBlocks, blockSize) != 0)
        return -1;

    g_fileSystemReady = 1;

    printf("VCB ready: totalBlocks=%llu blockSize=%llu freeMapStart=%llu "
        "freeMapBlocks=%llu rootDirStart=%llu rootDirBlocks=%llu\n",
        (ull_t)g_vcb.totalBlocks,
        (ull_t)g_vcb.blockSize,
        (ull_t)g_vcb.freeMapStart,
        (ull_t)g_vcb.freeMapBlocks,
        (ull_t)g_vcb.rootDirStart,
        (ull_t)g_vcb.rootDirBlocks);

    return 0;
}

void exitFileSystem()
{
    if (g_fileSystemReady && g_freeMapDirty)
    {
        writeFreeMapToDisk();
    }

    free(g_freeMap);
    g_freeMap = NULL;
    g_fileSystemReady = 0;
    g_freeMapDirty = 0;

    printf("System exiting\n");
}