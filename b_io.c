/**************************************************************
* Class::  CSC-415-02 Spring 2026
* Name:: Fnu Atiksha, Edson Sanchez Bernal, Junxi Lin
* Student IDs:: 923641508, 924372147, 923696927
* GitHub-Name:: Edsonsb
* Group-Name:: Epsilon
* Project:: Basic File System
*
* File:: b_io.c
*
* Description::
* This file implements basic file operations
* (open, read, write, seek, and close) for the custom file system.
* Uses block operations from the file system core to read  data,
* and directory functions to resolve paths and update file entries.
*
**************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "b_io.h"
#include "mfs.h"
#include "fsInternal.h"

#define MAXFCBS 20

typedef struct b_fcb
{
    int used;
    int flags;
    off_t offset;

    DirectoryEntry entry;       /* local copy of file entry */
    uint64_t parentLocation;
    uint64_t entryIndex;
} b_fcb;

static b_fcb fcbArray[MAXFCBS];
static int startup = 0;


static uint64_t ceilDivide(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    return (numerator + denominator - 1) / denominator;
}

static int loadVCB(VolumeControlBlock *vcb)
{
    unsigned char buffer[512];

    if (vcb == NULL)
    {
        return -1;
    }

    memset(buffer, 0, sizeof(buffer));

    if (fs_readBlocks(buffer, 1, 0) != 0)
    {
        return -1;
    }

    memcpy(vcb, buffer, sizeof(VolumeControlBlock));

    if (memcmp(vcb->magic, VCB_MAGIC, strlen(VCB_MAGIC)) != 0)
    {
        return -1;
    }

    return 0;
}

static void makeRootDirectoryEntry(const VolumeControlBlock *vcb, DirectoryEntry *rootEntry)
{
    if ((vcb == NULL) || (rootEntry == NULL))
    {
        return;
    }

    memset(rootEntry, 0, sizeof(DirectoryEntry));
    strncpy(rootEntry->name, "/", MAX_NAME - 1);
    rootEntry->name[MAX_NAME - 1] = '\0';
    rootEntry->in_use = 1;
    rootEntry->is_directory = 1;
    rootEntry->size = vcb->rootDirBlocks * vcb->blockSize;
    rootEntry->location = vcb->rootDirStart;
    rootEntry->timestamp = time(NULL);
}

static int getDirectoryBlocks(const VolumeControlBlock *vcb,const DirectoryEntry *dirEntry, uint64_t *blockCountOut)
{
    uint64_t blocks;

    if ((vcb == NULL) || (dirEntry == NULL) || (blockCountOut == NULL))
    {
        return -1;
    }

    if (dirEntry->is_directory == 0)
    {
        return -1;
    }

    blocks = ceilDivide(dirEntry->size, vcb->blockSize);
    if (blocks == 0)
    {
        blocks = vcb->rootDirBlocks;
    }

    *blockCountOut = blocks;
    return 0;
}

static int loadDirectoryByEntry(const DirectoryEntry *dirEntry,
 const VolumeControlBlock *vcb, DirectoryEntry **entriesOut, uint64_t *entryCountOut, uint64_t *blockCountOut)
{
    DirectoryEntry *entries;
    uint64_t blocks;
    uint64_t bytes;

    if ((dirEntry == NULL) || (vcb == NULL) || (entriesOut == NULL))
    {
        return -1;
    }

    if ((dirEntry->in_use == 0) || (dirEntry->is_directory == 0))
    {
        return -1;
    }

    if (getDirectoryBlocks(vcb, dirEntry, &blocks) != 0)
    {
        return -1;
    }

    bytes = blocks * vcb->blockSize;

    entries = (DirectoryEntry *)calloc(1, bytes);
    if (entries == NULL)
    {
        return -1;
    }

    if (fs_readBlocks(entries, blocks, dirEntry->location) != 0)
    {
        free(entries);
        return -1;
    }

    *entriesOut = entries;

    if (entryCountOut != NULL)
    {
        *entryCountOut = bytes / sizeof(DirectoryEntry);
    }

    if (blockCountOut != NULL)
    {
        *blockCountOut = blocks;
    }

    return 0;
}

static int saveDirectoryByEntry(const DirectoryEntry *dirEntry,const VolumeControlBlock *vcb, DirectoryEntry *entries)
{
    uint64_t blocks;

    if ((dirEntry == NULL) || (vcb == NULL) || (entries == NULL))
    {
        return -1;
    }

    if (getDirectoryBlocks(vcb, dirEntry, &blocks) != 0)
    {
        return -1;
    }

    if (fs_writeBlocks(entries, blocks, dirEntry->location) != 0)
    {
        return -1;
    }

    return 0;
}

static int findEntryInDirectory(const DirectoryEntry *entries, uint64_t entryCount, const char *name)
{
    uint64_t i;

    if ((entries == NULL) || (name == NULL) || (name[0] == '\0'))
    {
        return -1;
    }

    for (i = 0; i < entryCount; i++)
    {
        if ((entries[i].in_use == 1) &&
            (strcmp(entries[i].name, name) == 0))
        {
            return (int)i;
        }
    }

    return -1;
}

static int findFreeEntryInDirectory(const DirectoryEntry *entries, uint64_t entryCount)
{
    uint64_t i;

    if (entries == NULL)
    {
        return -1;
    }

    for (i = 2; i < entryCount; i++)
    {
        if (entries[i].in_use == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static int buildParentEntryFromLocation(uint64_t parentLocation,const char *fullPath, DirectoryEntry *parentEntryOut)
{
    VolumeControlBlock vcb;

    if ((fullPath == NULL) || (parentEntryOut == NULL))
    {
        return -1;
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    if (parentLocation == vcb.rootDirStart)
    {
        makeRootDirectoryEntry(&vcb, parentEntryOut);
        return 0;
    }

    {
        char *copy = (char *)malloc(strlen(fullPath) + 1);
        char *lastSlash;

        if (copy == NULL)
        {
            return -1;
        }

        strcpy(copy, fullPath);
        lastSlash = strrchr(copy, '/');

        if (lastSlash == NULL)
        {
            free(copy);
            return -1;
        }

        if (lastSlash == copy)
        {
            makeRootDirectoryEntry(&vcb, parentEntryOut);
            free(copy);
            return 0;
        }

        *lastSlash = '\0';

        if (fs_resolvePath(copy, parentEntryOut, NULL, NULL) != 0)
        {
            free(copy);
            return -1;
        }

        free(copy);
    }

    return 0;
}

static int updateDirectoryEntryOnDisk(const DirectoryEntry *updatedEntry,
 uint64_t parentLocation, uint64_t entryIndex, const char *fullPath)
{
    VolumeControlBlock vcb;
    DirectoryEntry parentEntry;
    DirectoryEntry *parentEntries = NULL;
    uint64_t entryCount = 0;
    int result = -1;

    if ((updatedEntry == NULL) || (fullPath == NULL))
    {
        return -1;
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    if (buildParentEntryFromLocation(parentLocation, fullPath,
                                     &parentEntry) != 0)
    {
        return -1;
    }

    if (loadDirectoryByEntry(&parentEntry, &vcb, &parentEntries,
                             &entryCount, NULL) != 0)
    {
        return -1;
    }

    if (entryIndex >= entryCount)
    {
        free(parentEntries);
        return -1;
    }

    parentEntries[entryIndex] = *updatedEntry;

    if (saveDirectoryByEntry(&parentEntry, &vcb, parentEntries) == 0)
    {
        result = 0;
    }

    free(parentEntries);
    return result;
}

static int flagsAllowRead(int flags)
{
    int mode = flags & O_ACCMODE;
    return (mode == O_RDONLY) || (mode == O_RDWR);
}

static int flagsAllowWrite(int flags)
{
    int mode = flags & O_ACCMODE;
    return (mode == O_WRONLY) || (mode == O_RDWR);
}

static uint64_t blocksForFileSize(uint64_t size, uint64_t blockSize)
{
    if ((size == 0) || (blockSize == 0))
    {
        return 0;
    }

    return (size + blockSize - 1) / blockSize;
}


/* Makesure the file has enough contiguous blocks for newSize.*/

static int ensureFileCapacity(b_fcb *fcb, uint64_t newSize, const char *fullPath)
{
    VolumeControlBlock vcb;
    uint64_t oldBlocks;
    uint64_t newBlocks;

    if ((fcb == NULL) || (fullPath == NULL))
    {
        return -1;
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    oldBlocks = blocksForFileSize(fcb->entry.size, vcb.blockSize);
    newBlocks = blocksForFileSize(newSize, vcb.blockSize);

    if (newBlocks <= oldBlocks)
    {
        return 0;
    }

    if (newBlocks == 0)
    {
        return 0;
    }

    {
        uint64_t newStart = 0;
        char *tempBuffer = NULL;
        uint64_t totalBytes = newBlocks * vcb.blockSize;

        tempBuffer = (char *)calloc(1, totalBytes);
        if (tempBuffer == NULL)
        {
            return -1;
        }

        if (oldBlocks > 0)
        {
            if (fs_readBlocks(tempBuffer, oldBlocks, fcb->entry.location) != 0)
            {
                free(tempBuffer);
                return -1;
            }
        }

        if (fs_allocateBlocks(newBlocks, &newStart) != 0)
        {
            free(tempBuffer);
            return -1;
        }

        if (fs_writeBlocks(tempBuffer, newBlocks, newStart) != 0)
        {
            fs_freeBlocks(newStart, newBlocks);
            free(tempBuffer);
            return -1;
        }

        if (oldBlocks > 0)
        {
            if (fs_freeBlocks(fcb->entry.location, oldBlocks) != 0)
            {

            }
        }

        fcb->entry.location = newStart;

        if (updateDirectoryEntryOnDisk(&fcb->entry, fcb->parentLocation, fcb->entryIndex, fullPath) != 0)
        {
            free(tempBuffer);
            return -1;
        }

        free(tempBuffer);
    }

    return 0;
}


static void b_init(void)
{
    int i;

    for (i = 0; i < MAXFCBS; i++)
    {
        memset(&fcbArray[i], 0, sizeof(b_fcb));
    }

    startup = 1;
}

static b_io_fd b_getFCB(void)
{
    int i;

    for (i = 0; i < MAXFCBS; i++)
    {
        if (fcbArray[i].used == 0)
        {
            return i;
        }
    }

    return -1;
}

b_io_fd b_open(char *filename, int flags)
{
    b_io_fd fd;
    DirectoryEntry entry;
    uint64_t parentLocation = 0;
    uint64_t entryIndex = 0;

    if (startup == 0)
    {
        b_init();
    }

    if ((filename == NULL) || (filename[0] == '\0'))
    {
        return -1;
    }

    fd = b_getFCB();
    if (fd < 0)
    {
        return -1;
    }

    /*for the file already exists */
    if (fs_resolvePath(filename, &entry, &parentLocation, &entryIndex) == 0)
    {
        if (entry.is_directory != 0)
        {
            return -1;
        }

        fcbArray[fd].used = 1;
        fcbArray[fd].flags = flags;
        fcbArray[fd].offset = 0;
        fcbArray[fd].entry = entry;
        fcbArray[fd].parentLocation = parentLocation;
        fcbArray[fd].entryIndex = entryIndex;

        if ((flags & O_TRUNC) && flagsAllowWrite(flags))
        {
            VolumeControlBlock vcb;
            uint64_t oldBlocks;

            if (loadVCB(&vcb) != 0)
            {
                memset(&fcbArray[fd], 0, sizeof(b_fcb));
                return -1;
            }

            oldBlocks = blocksForFileSize(fcbArray[fd].entry.size, vcb.blockSize);

            if (oldBlocks > 0)
            {
                if (fs_freeBlocks(fcbArray[fd].entry.location, oldBlocks) != 0)
                {
                    memset(&fcbArray[fd], 0, sizeof(b_fcb));
                    return -1;
                }
            }

            fcbArray[fd].entry.size = 0;
            fcbArray[fd].entry.location = 0;
            fcbArray[fd].entry.timestamp = time(NULL);

            if (updateDirectoryEntryOnDisk(&fcbArray[fd].entry,
                fcbArray[fd].parentLocation, fcbArray[fd].entryIndex,  filename) != 0)
            {
                memset(&fcbArray[fd], 0, sizeof(b_fcb));
                return -1;
            }
        }

        if (flags & O_APPEND)
        {
            fcbArray[fd].offset = (off_t)fcbArray[fd].entry.size;
        }

        return fd;
    }

    /* the file does not exist */
    if ((flags & O_CREAT) == 0)
    {
        return -1;
    }

    {
        VolumeControlBlock vcb;
        uint64_t newParentLocation = 0;
        char childName[MAX_NAME];
        DirectoryEntry parentEntry;
        DirectoryEntry *parentEntries = NULL;
        uint64_t parentEntryCount = 0;
        int freeSlot;
        time_t now;

        if (loadVCB(&vcb) != 0)
        {
            return -1;
        }

        if (fs_resolveParentPath(filename, &newParentLocation, childName, sizeof(childName)) != 0)
        {
            return -1;
        }

        if (buildParentEntryFromLocation(newParentLocation, filename,  &parentEntry) != 0)
        {
            return -1;
        }

        if (loadDirectoryByEntry(&parentEntry, &vcb, &parentEntries, &parentEntryCount, NULL) != 0)
        {
            return -1;
        }

        if (findEntryInDirectory(parentEntries, parentEntryCount, childName) >= 0)
        {
            free(parentEntries);
            return -1;
        }

        freeSlot = findFreeEntryInDirectory(parentEntries, parentEntryCount);
        if (freeSlot < 0)
        {
            free(parentEntries);
            return -1;
        }

        memset(&parentEntries[freeSlot], 0, sizeof(DirectoryEntry));
        strncpy(parentEntries[freeSlot].name, childName, MAX_NAME - 1);
        parentEntries[freeSlot].name[MAX_NAME - 1] = '\0';
        parentEntries[freeSlot].in_use = 1;
        parentEntries[freeSlot].is_directory = 0;
        parentEntries[freeSlot].size = 0;
        parentEntries[freeSlot].location = 0;
        now = time(NULL);
        parentEntries[freeSlot].timestamp = now;

        if (saveDirectoryByEntry(&parentEntry, &vcb, parentEntries) != 0)
        {
            free(parentEntries);
            return -1;
        }

        fcbArray[fd].used = 1;
        fcbArray[fd].flags = flags;
        fcbArray[fd].offset = 0;
        fcbArray[fd].entry = parentEntries[freeSlot];
        fcbArray[fd].parentLocation = newParentLocation;
        fcbArray[fd].entryIndex = (uint64_t)freeSlot;

        if (flags & O_APPEND)
        {
            fcbArray[fd].offset = 0;
        }

        free(parentEntries);
    }

    return fd;
}

int b_seek(b_io_fd fd, off_t offset, int whence)
{
    off_t newOffset;

    if (startup == 0)
    {
        b_init();
    }

    if ((fd < 0) || (fd >= MAXFCBS))
    {
        return -1;
    }

    if (fcbArray[fd].used == 0)
    {
        return -1;
    }

    switch (whence)
    {
        case SEEK_SET:
            newOffset = offset;
            break;

        case SEEK_CUR:
            newOffset = fcbArray[fd].offset + offset;
            break;

        case SEEK_END:
            newOffset = (off_t)fcbArray[fd].entry.size + offset;
            break;

        default:
            return -1;
    }

    if (newOffset < 0)
    {
        return -1;
    }

    /* Keep implementation easy and dont allow seek beyond EOF. */
    if ((uint64_t)newOffset > fcbArray[fd].entry.size)
    {
        return -1;
    }

    fcbArray[fd].offset = newOffset;
    return (int)newOffset;
}

int b_write(b_io_fd fd, char *buffer, int count)
{
    VolumeControlBlock vcb;
    b_fcb *fcb;
    uint64_t newEnd;
    uint64_t newSize;
    uint64_t totalBlocks;
    char *fileBuffer = NULL;
    int result = -1;

    if (startup == 0)
    {
        b_init();
    }

    if ((fd < 0) || (fd >= MAXFCBS))
    {
        return -1;
    }

    if ((buffer == NULL) || (count < 0))
    {
        return -1;
    }

    fcb = &fcbArray[fd];

    if (fcb->used == 0)
    {
        return -1;
    }

    if (!flagsAllowWrite(fcb->flags))
    {
        return -1;
    }

    if (count == 0)
    {
        return 0;
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    if (fcb->flags & O_APPEND)
    {
        fcb->offset = (off_t)fcb->entry.size;
    }

    newEnd = (uint64_t)fcb->offset + (uint64_t)count;
    newSize = (newEnd > fcb->entry.size) ? newEnd : fcb->entry.size;

    if (ensureFileCapacity(fcb, newSize, fcb->entry.name) != 0)
    {
        /*
         * entry.name only has file name, not full path.
         * So can rewrite fcn                           */
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    totalBlocks = blocksForFileSize(newSize, vcb.blockSize);

    if (totalBlocks == 0)
    {
        return 0;
    }

    fileBuffer = (char *)calloc(1, totalBlocks * vcb.blockSize);
    if (fileBuffer == NULL)
    {
        return -1;
    }

    if ((fcb->entry.size > 0) && (fcb->entry.location != 0))
    {
        uint64_t oldBlocks = blocksForFileSize(fcb->entry.size, vcb.blockSize);

        if (oldBlocks > 0)
        {
            if (fs_readBlocks(fileBuffer, oldBlocks, fcb->entry.location) != 0)
            {
                free(fileBuffer);
                return -1;
            }
        }
    }

    memcpy(fileBuffer + fcb->offset, buffer, count);

    if (fcb->entry.location == 0)
    {
        uint64_t newStart = 0;

        if (fs_allocateBlocks(totalBlocks, &newStart) != 0)
        {
            free(fileBuffer);
            return -1;
        }

        fcb->entry.location = newStart;
    }
    else
    {
        uint64_t currentBlocks = blocksForFileSize(fcb->entry.size, vcb.blockSize);

        if (totalBlocks > currentBlocks)
        {
            uint64_t newStart = 0;
            char *expandedBuffer = NULL;

            expandedBuffer = (char *)calloc(1, totalBlocks * vcb.blockSize);
            if (expandedBuffer == NULL)
            {
                free(fileBuffer);
                return -1;
            }

            memcpy(expandedBuffer, fileBuffer, totalBlocks * vcb.blockSize);

            if (fs_allocateBlocks(totalBlocks, &newStart) != 0)
            {
                free(expandedBuffer);
                free(fileBuffer);
                return -1;
            }

            if (fs_writeBlocks(expandedBuffer, totalBlocks, newStart) != 0)
            {
                fs_freeBlocks(newStart, totalBlocks);
                free(expandedBuffer);
                free(fileBuffer);
                return -1;
            }

            if (currentBlocks > 0)
            {
                fs_freeBlocks(fcb->entry.location, currentBlocks);
            }

            fcb->entry.location = newStart;
            free(expandedBuffer);
            free(fileBuffer);

            fcb->entry.size = newSize;
            fcb->entry.timestamp = time(NULL);
            fcb->offset += count;


            if (updateDirectoryEntryOnDisk(&fcb->entry, fcb->parentLocation, fcb->entryIndex, fcb->entry.name) != 0)
            {
                return count;
            }

            return count;
        }
    }

    if (fs_writeBlocks(fileBuffer, totalBlocks, fcb->entry.location) != 0)
    {
        free(fileBuffer);
        return -1;
    }

    fcb->entry.size = newSize;
    fcb->entry.timestamp = time(NULL);
    fcb->offset += count;

    result = count;

    if (updateDirectoryEntryOnDisk(&fcb->entry, fcb->parentLocation, fcb->entryIndex, fcb->entry.name) != 0)
    {

    }

    free(fileBuffer);
    return result;
}

int b_read(b_io_fd fd, char *buffer, int count)
{
    VolumeControlBlock vcb;
    b_fcb *fcb;
    uint64_t bytesAvailable;
    uint64_t bytesToRead;
    uint64_t bytesRead = 0;
    char *blockBuffer = NULL;

    if (startup == 0)
    {
        b_init();
    }

    if ((fd < 0) || (fd >= MAXFCBS))
    {
        return -1;
    }

    if ((buffer == NULL) || (count < 0))
    {
        return -1;
    }

    fcb = &fcbArray[fd];

    if (fcb->used == 0)
    {
        return -1;
    }

    if (!flagsAllowRead(fcb->flags))
    {
        return -1;
    }

    if (count == 0)
    {
        return 0;
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    if ((uint64_t)fcb->offset >= fcb->entry.size)
    {
        return 0;
    }

    bytesAvailable = fcb->entry.size - (uint64_t)fcb->offset;
    bytesToRead = ((uint64_t)count < bytesAvailable) ? (uint64_t)count : bytesAvailable;

    blockBuffer = (char *)malloc(vcb.blockSize);
    if (blockBuffer == NULL)
    {
        return -1;
    }

    while (bytesRead < bytesToRead)
    {
        uint64_t fileOffset = (uint64_t)fcb->offset;
        uint64_t fileBlockIndex = fileOffset / vcb.blockSize;
        uint64_t blockOffset = fileOffset % vcb.blockSize;
        uint64_t diskBlock = fcb->entry.location + fileBlockIndex;
        uint64_t remaining = bytesToRead - bytesRead;
        uint64_t chunkSize = vcb.blockSize - blockOffset;

        if (chunkSize > remaining)
        {
            chunkSize = remaining;
        }

        if (fs_readBlocks(blockBuffer, 1, diskBlock) != 0)
        {
            free(blockBuffer);
            return -1;
        }

        memcpy(buffer + bytesRead, blockBuffer + blockOffset, chunkSize);

        bytesRead += chunkSize;
        fcb->offset += (off_t)chunkSize;
    }

    fcb->entry.timestamp = time(NULL);

    updateDirectoryEntryOnDisk(&fcb->entry, fcb->parentLocation, fcb->entryIndex, fcb->entry.name);

    free(blockBuffer);
    return (int)bytesRead;
}

int b_close(b_io_fd fd)
{
    if (startup == 0)
    {
        b_init();
    }

    if ((fd < 0) || (fd >= MAXFCBS))
    {
        return -1;
    }

    if (fcbArray[fd].used == 0)
    {
        return -1;
    }

    memset(&fcbArray[fd], 0, sizeof(b_fcb));
    return 0;
}
