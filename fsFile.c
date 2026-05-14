/**************************************************************
* Class::  CSC-415-02 Spring 2026
* Name:: Junxi Lin
* Student IDs:: 923696927
* GitHub-Name:: Edsonsb
* Group-Name:: Epsilon
* Project:: Basic File System
*
* File:: fsFile.c
*
* Description::
* This file contains helper functions for file-related operations.
* It  supports checking whether a name is a file, getting file
* statistics, and deleting a file from the root directory.
*
**************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mfs.h"
#include "fsInternal.h"

static uint64_t ceilDivide(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    return (numerator + denominator - 1) / denominator;
}

/* Reads the volume control block from block 0.*/

static int loadVCB(VolumeControlBlock *vcb)
{
    unsigned char buffer[5120];

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

static uint64_t blocksForEntry(const VolumeControlBlock *vcb,
    const DirectoryEntry *entry)
{
    uint64_t blocks;

    if ((vcb == NULL) || (entry == NULL))
    {
        return 0;
    }

    blocks = ceilDivide(entry->size, vcb->blockSize);
    if ((blocks == 0) && (entry->is_directory != 0))
    {
        blocks = vcb->rootDirBlocks;
    }

    return blocks;
}

static int loadDirectoryByEntry(const DirectoryEntry *dirEntry,
    const VolumeControlBlock *vcb, DirectoryEntry **entriesOut,
    uint64_t *entryCountOut, uint64_t *blockCountOut)
{
    DirectoryEntry *entries;
    uint64_t blocks;
    uint64_t bytes;

    if ((dirEntry == NULL) || (vcb == NULL) || (entriesOut == NULL) ||
        (dirEntry->in_use == 0) || (dirEntry->is_directory == 0))
    {
        return -1;
    }

    blocks = blocksForEntry(vcb, dirEntry);
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

static int saveDirectoryByEntry(const DirectoryEntry *dirEntry,
    const VolumeControlBlock *vcb, DirectoryEntry *entries)
{
    uint64_t blocks;

    if ((dirEntry == NULL) || (vcb == NULL) || (entries == NULL))
    {
        return -1;
    }

    blocks = blocksForEntry(vcb, dirEntry);
    return fs_writeBlocks(entries, blocks, dirEntry->location);
}

static int findEntryInDirectory(const DirectoryEntry *entries,
    uint64_t entryCount, const char *name)
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

static int findFreeEntryInDirectory(const DirectoryEntry *entries,
    uint64_t entryCount)
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

static int splitParentAndName(const char *path, char **parentPathOut,
    char *childNameOut, size_t childNameSize)
{
    char *copy;
    char *lastSlash;
    size_t len;

    if ((path == NULL) || (path[0] == '\0') || (parentPathOut == NULL) ||
        (childNameOut == NULL) || (childNameSize == 0))
    {
        return -1;
    }

    copy = (char *)malloc(strlen(path) + 1);
    if (copy == NULL)
    {
        return -1;
    }
    strcpy(copy, path);

    len = strlen(copy);
    while ((len > 1) && (copy[len - 1] == '/'))
    {
        copy[len - 1] = '\0';
        len--;
    }

    if (strcmp(copy, "/") == 0)
    {
        free(copy);
        return -1;
    }

    lastSlash = strrchr(copy, '/');
    if (lastSlash == NULL)
    {
        *parentPathOut = (char *)malloc(2);
        if (*parentPathOut == NULL)
        {
            free(copy);
            return -1;
        }
        strcpy(*parentPathOut, ".");
        strncpy(childNameOut, copy, childNameSize - 1);
    }
    else if (lastSlash == copy)
    {
        *parentPathOut = (char *)malloc(2);
        if (*parentPathOut == NULL)
        {
            free(copy);
            return -1;
        }
        strcpy(*parentPathOut, "/");
        strncpy(childNameOut, lastSlash + 1, childNameSize - 1);
    }
    else
    {
        *lastSlash = '\0';
        *parentPathOut = (char *)malloc(strlen(copy) + 1);
        if (*parentPathOut == NULL)
        {
            free(copy);
            return -1;
        }
        strcpy(*parentPathOut, copy);
        strncpy(childNameOut, lastSlash + 1, childNameSize - 1);
    }

    childNameOut[childNameSize - 1] = '\0';
    free(copy);

    if ((childNameOut[0] == '\0') ||
        (strcmp(childNameOut, ".") == 0) ||
        (strcmp(childNameOut, "..") == 0) ||
        (strlen(childNameOut) >= MAX_NAME))
    {
        free(*parentPathOut);
        *parentPathOut = NULL;
        return -1;
    }

    return 0;
}

static int getParentEntryForPath(const char *path, DirectoryEntry *parentOut,
    char *childNameOut, size_t childNameSize)
{
    char *parentPath = NULL;
    int result;

    if ((parentOut == NULL) || (childNameOut == NULL))
    {
        return -1;
    }

    result = splitParentAndName(path, &parentPath, childNameOut,
        childNameSize);
    if (result != 0)
    {
        return -1;
    }

    result = fs_resolvePath(parentPath, parentOut, NULL, NULL);
    free(parentPath);

    if ((result != 0) || (parentOut->is_directory == 0))
    {
        return -1;
    }

    return 0;
}

static int isDirectoryDescendant(const DirectoryEntry *possibleDescendant,
    uint64_t ancestorLocation, const VolumeControlBlock *vcb)
{
    DirectoryEntry current;

    if ((possibleDescendant == NULL) || (vcb == NULL) ||
        (possibleDescendant->is_directory == 0))
    {
        return 0;
    }

    current = *possibleDescendant;
    while (current.location != vcb->rootDirStart)
    {
        DirectoryEntry *entries = NULL;
        uint64_t entryCount = 0;
        int parentIndex;

        if (current.location == ancestorLocation)
        {
            return 1;
        }

        if (loadDirectoryByEntry(&current, vcb, &entries, &entryCount,
            NULL) != 0)
        {
            return 1;
        }

        parentIndex = findEntryInDirectory(entries, entryCount, "..");
        if (parentIndex < 0)
        {
            free(entries);
            return 1;
        }

        current = entries[parentIndex];
        free(entries);
    }

    return current.location == ancestorLocation;
}

static int updateMovedDirectoryParent(const DirectoryEntry *movedDir,
    const DirectoryEntry *newParent, const VolumeControlBlock *vcb)
{
    DirectoryEntry *entries = NULL;
    uint64_t entryCount = 0;
    int parentIndex;
    int result;

    if ((movedDir == NULL) || (newParent == NULL) || (vcb == NULL) ||
        (movedDir->is_directory == 0))
    {
        return -1;
    }

    if (loadDirectoryByEntry(movedDir, vcb, &entries, &entryCount, NULL) != 0)
    {
        return -1;
    }

    parentIndex = findEntryInDirectory(entries, entryCount, "..");
    if (parentIndex < 0)
    {
        free(entries);
        return -1;
    }

    entries[parentIndex] = *newParent;
    strncpy(entries[parentIndex].name, "..", MAX_NAME - 1);
    entries[parentIndex].name[MAX_NAME - 1] = '\0';
    result = saveDirectoryByEntry(movedDir, vcb, entries);

    free(entries);
    return result;
}


int fs_isFile(char *filename)
{
    DirectoryEntry entry;

    if ((filename == NULL) ||
        (fs_resolvePath(filename, &entry, NULL, NULL) != 0))
    {
        return 0;
    }

    return ((entry.in_use == 1) && (entry.is_directory == 0)) ? 1 : 0;
}

/* Fills in the fs_stat structure for a file system path. */

int fs_stat(const char *path, struct fs_stat *buf)
{
    DirectoryEntry entry;
    VolumeControlBlock vcb;

    if ((path == NULL) || (buf == NULL))
    {
        return -1;
    }

    if ((loadVCB(&vcb) != 0) ||
        (fs_resolvePath(path, &entry, NULL, NULL) != 0) ||
        (entry.in_use == 0))
    {
        return -1;
    }

    memset(buf, 0, sizeof(struct fs_stat));

    buf->st_size = (off_t)entry.size;
    buf->st_blksize = (blksize_t)vcb.blockSize;

    if (entry.size == 0)
    {
        buf->st_blocks = 0;
    }
    else
    {
        buf->st_blocks = (blkcnt_t)
            ((entry.size + vcb.blockSize - 1) / vcb.blockSize);
    }

    buf->st_accesstime = entry.timestamp;
    buf->st_modtime = entry.timestamp;
    buf->st_createtime = entry.timestamp;

    return 0;
}

/* Deletes a regular file from any resolved directory path. */


int fs_delete(char *filename)
{
    DirectoryEntry *entries = NULL;
    DirectoryEntry fileEntry;
    DirectoryEntry parentEntry;
    VolumeControlBlock vcb;
    uint64_t entryIndex = 0;
    uint64_t entryCount = 0;
    uint64_t blocksUsed;
    char childName[MAX_NAME];

    if ((filename == NULL) || (filename[0] == '\0'))
    {
        return -1;
    }

    if ((loadVCB(&vcb) != 0) ||
        (fs_resolvePath(filename, &fileEntry, NULL, &entryIndex) != 0) ||
        (getParentEntryForPath(filename, &parentEntry, childName,
            sizeof(childName)) != 0))
    {
        return -1;
    }

    if ((fileEntry.in_use == 0) || (fileEntry.is_directory != 0))
    {
        return -1;
    }

    if (loadDirectoryByEntry(&parentEntry, &vcb, &entries, &entryCount,
        NULL) != 0)
    {
        return -1;
    }

    if ((entryIndex >= entryCount) ||
        (strcmp(entries[entryIndex].name, childName) != 0))
    {
        free(entries);
        return -1;
    }

    if (fileEntry.size > 0)
    {
        blocksUsed = ceilDivide(fileEntry.size, vcb.blockSize);

        if (fs_freeBlocks(fileEntry.location, blocksUsed) != 0)
        {
            free(entries);
            return -1;
        }
    }

    memset(&entries[entryIndex], 0, sizeof(DirectoryEntry));

    if (saveDirectoryByEntry(&parentEntry, &vcb, entries) != 0)
    {
        free(entries);
        return -1;
    }

    free(entries);
    return 0;
}

int fs_move(char *source, char *destination)
{
    VolumeControlBlock vcb;
    DirectoryEntry sourceEntry;
    DirectoryEntry sourceParent;
    DirectoryEntry destParent;
    DirectoryEntry destExisting;
    DirectoryEntry *sourceEntries = NULL;
    DirectoryEntry *destEntries = NULL;
    uint64_t sourceIndex = 0;
    uint64_t sourceEntryCount = 0;
    uint64_t destEntryCount = 0;
    char sourceName[MAX_NAME];
    char destName[MAX_NAME];
    int destSlot;
    int sameParent;
    int destinationExists;

    if ((source == NULL) || (destination == NULL) ||
        (source[0] == '\0') || (destination[0] == '\0'))
    {
        return -1;
    }

    if ((loadVCB(&vcb) != 0) ||
        (fs_resolvePath(source, &sourceEntry, NULL, &sourceIndex) != 0) ||
        (getParentEntryForPath(source, &sourceParent, sourceName,
            sizeof(sourceName)) != 0))
    {
        return -1;
    }

    if ((sourceEntry.location == vcb.rootDirStart) ||
        (strcmp(sourceName, ".") == 0) || (strcmp(sourceName, "..") == 0))
    {
        return -1;
    }

    destinationExists = (fs_resolvePath(destination, &destExisting,
        NULL, NULL) == 0);
    if (destinationExists && (destExisting.is_directory != 0))
    {
        destParent = destExisting;
        strncpy(destName, sourceName, sizeof(destName) - 1);
        destName[sizeof(destName) - 1] = '\0';
    }
    else if (destinationExists)
    {
        return -1;
    }
    else if (getParentEntryForPath(destination, &destParent, destName,
        sizeof(destName)) != 0)
    {
        return -1;
    }

    if ((sourceEntry.is_directory != 0) &&
        isDirectoryDescendant(&destParent, sourceEntry.location, &vcb))
    {
        return -1;
    }

    sameParent = (sourceParent.location == destParent.location);

    if (loadDirectoryByEntry(&sourceParent, &vcb, &sourceEntries,
        &sourceEntryCount, NULL) != 0)
    {
        return -1;
    }

    if ((sourceIndex >= sourceEntryCount) ||
        (sourceEntries[sourceIndex].in_use == 0))
    {
        free(sourceEntries);
        return -1;
    }

    if (sameParent)
    {
        int duplicateIndex = findEntryInDirectory(sourceEntries,
            sourceEntryCount, destName);

        if ((duplicateIndex >= 0) && ((uint64_t)duplicateIndex != sourceIndex))
        {
            free(sourceEntries);
            return -1;
        }

        strncpy(sourceEntries[sourceIndex].name, destName, MAX_NAME - 1);
        sourceEntries[sourceIndex].name[MAX_NAME - 1] = '\0';
        sourceEntries[sourceIndex].timestamp = time(NULL);

        if (saveDirectoryByEntry(&sourceParent, &vcb, sourceEntries) != 0)
        {
            free(sourceEntries);
            return -1;
        }

        free(sourceEntries);
        return 0;
    }

    if (loadDirectoryByEntry(&destParent, &vcb, &destEntries,
        &destEntryCount, NULL) != 0)
    {
        free(sourceEntries);
        return -1;
    }

    if (findEntryInDirectory(destEntries, destEntryCount, destName) >= 0)
    {
        free(destEntries);
        free(sourceEntries);
        return -1;
    }

    destSlot = findFreeEntryInDirectory(destEntries, destEntryCount);
    if (destSlot < 0)
    {
        free(destEntries);
        free(sourceEntries);
        return -1;
    }

    strncpy(sourceEntry.name, destName, MAX_NAME - 1);
    sourceEntry.name[MAX_NAME - 1] = '\0';
    sourceEntry.timestamp = time(NULL);
    destEntries[destSlot] = sourceEntry;

    if ((sourceEntry.is_directory != 0) &&
        (updateMovedDirectoryParent(&sourceEntry, &destParent, &vcb) != 0))
    {
        free(destEntries);
        free(sourceEntries);
        return -1;
    }

    if (saveDirectoryByEntry(&destParent, &vcb, destEntries) != 0)
    {
        free(destEntries);
        free(sourceEntries);
        return -1;
    }

    memset(&sourceEntries[sourceIndex], 0, sizeof(DirectoryEntry));
    if (saveDirectoryByEntry(&sourceParent, &vcb, sourceEntries) != 0)
    {
        free(destEntries);
        free(sourceEntries);
        return -1;
    }

    free(destEntries);
    free(sourceEntries);
    return 0;
}
