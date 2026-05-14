/**************************************************************
* Class::  CSC-415-02 Spring 2026
* Name:: Edson Sanchez Bernal
* Student IDs:: 924372147
* GitHub-Name:: Edsonsb
* Group-Name:: Epsilon
* Project:: Basic File System
*
* File:: fsDir.c
*
* Description::
* Directory path traversal, current working directory tracking,
* and directory API implementation for the basic file system.
*
**************************************************************/

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mfs.h"
#include "fsInternal.h"

#define PATH_BUFFER_SIZE 4096

static char g_cwdPath[PATH_BUFFER_SIZE] = "/";
static uint64_t g_cwdLocation = 0;
static uint64_t g_cwdBlocks = 0;
static int g_cwdInitialized = 0;

static uint64_t ceilDivide(uint64_t numerator, uint64_t denominator)
{
    return (numerator + denominator - 1) / denominator;
}

static int loadVCB(VolumeControlBlock *vcb)
{
    void *buffer;

    if (vcb == NULL)
    {
        return -1;
    }

    buffer = calloc(1, 512);
    if (buffer == NULL)
    {
        return -1;
    }

    if (fs_readBlocks(buffer, 1, 0) != 0)
    {
        free(buffer);
        return -1;
    }

    memcpy(vcb, buffer, sizeof(VolumeControlBlock));
    free(buffer);

    if (memcmp(vcb->magic, VCB_MAGIC, strlen(VCB_MAGIC)) != 0)
    {
        return -1;
    }

    return 0;
}

static void makeRootEntry(const VolumeControlBlock *vcb, DirectoryEntry *entry)
{
    memset(entry, 0, sizeof(DirectoryEntry));
    strncpy(entry->name, "/", MAX_NAME - 1);
    entry->in_use = 1;
    entry->is_directory = 1;
    entry->size = vcb->rootDirBlocks * vcb->blockSize;
    entry->location = vcb->rootDirStart;
}

static uint64_t blocksForEntry(const VolumeControlBlock *vcb,
    const DirectoryEntry *entry)
{
    if (entry->size == 0)
    {
        return vcb->rootDirBlocks;
    }

    return ceilDivide(entry->size, vcb->blockSize);
}

static int loadDirectory(const DirectoryEntry *dirEntry,
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

    entries = calloc(1, bytes);
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

static int saveDirectory(const DirectoryEntry *dirEntry,
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

static int findEntryInDirectory(DirectoryEntry *entries, uint64_t entryCount,
    const char *name)
{
    uint64_t i;

    if ((entries == NULL) || (name == NULL))
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

static int findFreeEntry(DirectoryEntry *entries, uint64_t entryCount)
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

static int ensureCwdInitialized(void)
{
    VolumeControlBlock vcb;

    if (g_cwdInitialized)
    {
        return 0;
    }

    if (loadVCB(&vcb) != 0)
    {
        return -1;
    }

    g_cwdLocation = vcb.rootDirStart;
    g_cwdBlocks = vcb.rootDirBlocks;
    strncpy(g_cwdPath, "/", sizeof(g_cwdPath) - 1);
    g_cwdPath[sizeof(g_cwdPath) - 1] = '\0';
    g_cwdInitialized = 1;

    return 0;
}

static int resolvePathInternal(const char *path, DirectoryEntry *entryOut,
    uint64_t *parentLocationOut, uint64_t *entryIndexOut)
{
    VolumeControlBlock vcb;
    DirectoryEntry current;
    DirectoryEntry parent;
    char *pathCopy;
    char *token;
    char *savePtr = NULL;
    int result = -1;
    int hasParent = 0;
    uint64_t parentLocation = 0;
    uint64_t entryIndex = 0;

    if ((path == NULL) || (path[0] == '\0') ||
        (ensureCwdInitialized() != 0) || (loadVCB(&vcb) != 0))
    {
        return -1;
    }

    if (path[0] == '/')
    {
        makeRootEntry(&vcb, &current);
    }
    else
    {
        makeRootEntry(&vcb, &current);
        current.location = g_cwdLocation;
        current.size = g_cwdBlocks * vcb.blockSize;
    }

    pathCopy = malloc(strlen(path) + 1);
    if (pathCopy == NULL)
    {
        return -1;
    }
    strcpy(pathCopy, path);

    token = strtok_r(pathCopy, "/", &savePtr);
    if (token == NULL)
    {
        if (entryOut != NULL)
        {
            *entryOut = current;
        }
        if (parentLocationOut != NULL)
        {
            *parentLocationOut = current.location;
        }
        if (entryIndexOut != NULL)
        {
            *entryIndexOut = 0;
        }
        free(pathCopy);
        return 0;
    }

    while (token != NULL)
    {
        DirectoryEntry *entries = NULL;
        uint64_t entryCount = 0;
        int index = 0;

        if (strlen(token) >= MAX_NAME)
        {
            result = -1;
            break;
        }

        if (strcmp(token, ".") == 0)
        {
            parent = current;
            parentLocation = current.location;
            entryIndex = 0;
            hasParent = 1;
        }
        else
        {
            parent = current;
            parentLocation = parent.location;

            if (loadDirectory(&parent, &vcb, &entries, &entryCount, NULL) != 0)
            {
                result = -1;
                break;
            }

            index = findEntryInDirectory(entries, entryCount, token);
            if (index < 0)
            {
                free(entries);
                result = -1;
                break;
            }

            current = entries[index];
            entryIndex = (uint64_t)index;
            hasParent = 1;
            free(entries);
        }

        token = strtok_r(NULL, "/", &savePtr);
        if ((token != NULL) && (current.is_directory == 0))
        {
            result = -1;
            break;
        }
    }

    if (token == NULL)
    {
        if (entryOut != NULL)
        {
            *entryOut = current;
        }
        if (parentLocationOut != NULL)
        {
            *parentLocationOut = hasParent ? parentLocation : current.location;
        }
        if (entryIndexOut != NULL)
        {
            *entryIndexOut = hasParent ? entryIndex : 0;
        }
        result = 0;
    }

    free(pathCopy);
    return result;
}

int fs_resolvePath(const char *path, DirectoryEntry *entryOut,
    uint64_t *parentLocationOut, uint64_t *entryIndexOut)
{
    return resolvePathInternal(path, entryOut, parentLocationOut,
        entryIndexOut);
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

    copy = malloc(strlen(path) + 1);
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
        *parentPathOut = malloc(2);
        if (*parentPathOut == NULL)
        {
            free(copy);
            return -1;
        }
        strcpy(*parentPathOut, ".");
        strncpy(childNameOut, copy, childNameSize - 1);
        childNameOut[childNameSize - 1] = '\0';
    }
    else if (lastSlash == copy)
    {
        *parentPathOut = malloc(2);
        if (*parentPathOut == NULL)
        {
            free(copy);
            return -1;
        }
        strcpy(*parentPathOut, "/");
        strncpy(childNameOut, lastSlash + 1, childNameSize - 1);
        childNameOut[childNameSize - 1] = '\0';
    }
    else
    {
        *lastSlash = '\0';
        *parentPathOut = malloc(strlen(copy) + 1);
        if (*parentPathOut == NULL)
        {
            free(copy);
            return -1;
        }
        strcpy(*parentPathOut, copy);
        strncpy(childNameOut, lastSlash + 1, childNameSize - 1);
        childNameOut[childNameSize - 1] = '\0';
    }

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

static int resolveParentInternal(const char *path, DirectoryEntry *parentOut,
    uint64_t *parentLocationOut, char *childNameOut, size_t childNameSize)
{
    char *parentPath = NULL;
    DirectoryEntry parent;

    if (splitParentAndName(path, &parentPath, childNameOut, childNameSize) != 0)
    {
        return -1;
    }

    if ((fs_resolvePath(parentPath, &parent, NULL, NULL) != 0) ||
        (parent.is_directory == 0))
    {
        free(parentPath);
        return -1;
    }

    if (parentOut != NULL)
    {
        *parentOut = parent;
    }

    if (parentLocationOut != NULL)
    {
        *parentLocationOut = parent.location;
    }

    free(parentPath);
    return 0;
}

int fs_resolveParentPath(const char *path, uint64_t *parentLocationOut,
    char *childNameOut, size_t childNameSize)
{
    return resolveParentInternal(path, NULL, parentLocationOut,
        childNameOut, childNameSize);
}

static int canonicalizePath(const char *path, char *outPath, size_t outSize)
{
    char *copy;
    char *token;
    char *savePtr = NULL;
    char components[128][MAX_NAME];
    size_t componentCount = 0;
    size_t used = 1;

    if ((path == NULL) || (outPath == NULL) || (outSize < 2) ||
        (ensureCwdInitialized() != 0))
    {
        return -1;
    }

    if (path[0] != '/')
    {
        copy = malloc(strlen(g_cwdPath) + 1);
        if (copy == NULL)
        {
            return -1;
        }
        strcpy(copy, g_cwdPath);

        token = strtok_r(copy, "/", &savePtr);
        while (token != NULL)
        {
            if (componentCount >= 128)
            {
                free(copy);
                return -1;
            }
            strncpy(components[componentCount], token, MAX_NAME - 1);
            components[componentCount][MAX_NAME - 1] = '\0';
            componentCount++;
            token = strtok_r(NULL, "/", &savePtr);
        }
        free(copy);
    }

    copy = malloc(strlen(path) + 1);
    if (copy == NULL)
    {
        return -1;
    }
    strcpy(copy, path);

    savePtr = NULL;
    token = strtok_r(copy, "/", &savePtr);
    while (token != NULL)
    {
        if (strcmp(token, ".") == 0)
        {
            token = strtok_r(NULL, "/", &savePtr);
            continue;
        }

        if (strcmp(token, "..") == 0)
        {
            if (componentCount > 0)
            {
                componentCount--;
            }
            token = strtok_r(NULL, "/", &savePtr);
            continue;
        }

        if ((strlen(token) >= MAX_NAME) || (componentCount >= 128))
        {
            free(copy);
            return -1;
        }

        strncpy(components[componentCount], token, MAX_NAME - 1);
        components[componentCount][MAX_NAME - 1] = '\0';
        componentCount++;
        token = strtok_r(NULL, "/", &savePtr);
    }
    free(copy);

    strcpy(outPath, "/");
    if (componentCount == 0)
    {
        return 0;
    }

    for (size_t i = 0; i < componentCount; i++)
    {
        size_t nameLen = strlen(components[i]);
        size_t extra = nameLen + ((i == 0) ? 0 : 1);

        if ((used + extra) >= outSize)
        {
            return -1;
        }

        if (i != 0)
        {
            strcat(outPath, "/");
            used++;
        }
        strcat(outPath, components[i]);
        used += nameLen;
    }

    return 0;
}

int fs_mkdir(const char *pathname, mode_t mode)
{
    VolumeControlBlock vcb;
    DirectoryEntry parent;
    DirectoryEntry *parentEntries = NULL;
    DirectoryEntry *newEntries = NULL;
    char childName[MAX_NAME];
    uint64_t parentEntryCount = 0;
    uint64_t newDirStart = 0;
    uint64_t directoryBytes;
    int slot;
    time_t now;

    (void)mode;

    if ((loadVCB(&vcb) != 0) ||
        (resolveParentInternal(pathname, &parent, NULL, childName,
            sizeof(childName)) != 0))
    {
        return -1;
    }

    if (loadDirectory(&parent, &vcb, &parentEntries,
        &parentEntryCount, NULL) != 0)
    {
        return -1;
    }

    if (findEntryInDirectory(parentEntries, parentEntryCount, childName) >= 0)
    {
        free(parentEntries);
        return -1;
    }

    slot = findFreeEntry(parentEntries, parentEntryCount);
    if (slot < 0)
    {
        free(parentEntries);
        return -1;
    }

    directoryBytes = vcb.rootDirBlocks * vcb.blockSize;
    newEntries = calloc(1, directoryBytes);
    if (newEntries == NULL)
    {
        free(parentEntries);
        return -1;
    }

    if (fs_allocateBlocks(vcb.rootDirBlocks, &newDirStart) != 0)
    {
        free(newEntries);
        free(parentEntries);
        return -1;
    }

    now = time(NULL);
    strncpy(newEntries[0].name, ".", MAX_NAME - 1);
    newEntries[0].in_use = 1;
    newEntries[0].is_directory = 1;
    newEntries[0].size = directoryBytes;
    newEntries[0].location = newDirStart;
    newEntries[0].timestamp = now;

    strncpy(newEntries[1].name, "..", MAX_NAME - 1);
    newEntries[1].in_use = 1;
    newEntries[1].is_directory = 1;
    newEntries[1].size = parent.size;
    newEntries[1].location = parent.location;
    newEntries[1].timestamp = now;

    if (fs_writeBlocks(newEntries, vcb.rootDirBlocks, newDirStart) != 0)
    {
        fs_freeBlocks(newDirStart, vcb.rootDirBlocks);
        free(newEntries);
        free(parentEntries);
        return -1;
    }

    memset(&parentEntries[slot], 0, sizeof(DirectoryEntry));
    strncpy(parentEntries[slot].name, childName, MAX_NAME - 1);
    parentEntries[slot].in_use = 1;
    parentEntries[slot].is_directory = 1;
    parentEntries[slot].size = directoryBytes;
    parentEntries[slot].location = newDirStart;
    parentEntries[slot].timestamp = now;

    if (saveDirectory(&parent, &vcb, parentEntries) != 0)
    {
        fs_freeBlocks(newDirStart, vcb.rootDirBlocks);
        free(newEntries);
        free(parentEntries);
        return -1;
    }

    free(newEntries);
    free(parentEntries);
    return 0;
}

int fs_rmdir(const char *pathname)
{
    VolumeControlBlock vcb;
    DirectoryEntry parent;
    DirectoryEntry target;
    DirectoryEntry *parentEntries = NULL;
    DirectoryEntry *targetEntries = NULL;
    char childName[MAX_NAME];
    uint64_t parentEntryCount = 0;
    uint64_t targetEntryCount = 0;
    uint64_t targetBlocks = 0;
    int targetIndex;

    if ((loadVCB(&vcb) != 0) ||
        (resolveParentInternal(pathname, &parent, NULL, childName,
            sizeof(childName)) != 0))
    {
        return -1;
    }

    if (loadDirectory(&parent, &vcb, &parentEntries,
        &parentEntryCount, NULL) != 0)
    {
        return -1;
    }

    targetIndex = findEntryInDirectory(parentEntries, parentEntryCount,
        childName);
    if (targetIndex < 0)
    {
        free(parentEntries);
        return -1;
    }

    target = parentEntries[targetIndex];
    if ((target.is_directory == 0) || (target.location == vcb.rootDirStart))
    {
        free(parentEntries);
        return -1;
    }

    if (loadDirectory(&target, &vcb, &targetEntries, &targetEntryCount,
        &targetBlocks) != 0)
    {
        free(parentEntries);
        return -1;
    }

    for (uint64_t i = 2; i < targetEntryCount; i++)
    {
        if (targetEntries[i].in_use)
        {
            free(targetEntries);
            free(parentEntries);
            return -1;
        }
    }

    memset(&parentEntries[targetIndex], 0, sizeof(DirectoryEntry));
    if (saveDirectory(&parent, &vcb, parentEntries) != 0)
    {
        free(targetEntries);
        free(parentEntries);
        return -1;
    }

    fs_freeBlocks(target.location, targetBlocks);

    free(targetEntries);
    free(parentEntries);
    return 0;
}

fdDir *fs_opendir(const char *pathname)
{
    VolumeControlBlock vcb;
    DirectoryEntry dirEntry;
    DirectoryEntry *entries = NULL;
    fdDir *dirp;
    uint64_t entryCount = 0;
    uint64_t blockCount = 0;

    if ((loadVCB(&vcb) != 0) ||
        (fs_resolvePath(pathname, &dirEntry, NULL, NULL) != 0) ||
        (dirEntry.is_directory == 0))
    {
        return NULL;
    }

    if (loadDirectory(&dirEntry, &vcb, &entries, &entryCount,
        &blockCount) != 0)
    {
        return NULL;
    }

    dirp = calloc(1, sizeof(fdDir));
    if (dirp == NULL)
    {
        free(entries);
        return NULL;
    }

    dirp->di = calloc(1, sizeof(struct fs_diriteminfo));
    if (dirp->di == NULL)
    {
        free(dirp);
        free(entries);
        return NULL;
    }

    dirp->d_reclen = sizeof(struct fs_diriteminfo);
    dirp->dirEntryPosition = 0;
    dirp->entryCount = entryCount;
    dirp->directoryLocation = dirEntry.location;
    dirp->directoryBlocks = blockCount;
    dirp->directory = entries;

    return dirp;
}

struct fs_diriteminfo *fs_readdir(fdDir *dirp)
{
    DirectoryEntry *entries;
    DirectoryEntry *entry;

    if ((dirp == NULL) || (dirp->directory == NULL) || (dirp->di == NULL))
    {
        return NULL;
    }

    entries = (DirectoryEntry *)dirp->directory;
    while (dirp->dirEntryPosition < dirp->entryCount)
    {
        entry = &entries[dirp->dirEntryPosition];
        dirp->dirEntryPosition++;

        if (entry->in_use == 0)
        {
            continue;
        }

        memset(dirp->di, 0, sizeof(struct fs_diriteminfo));
        dirp->di->d_reclen = sizeof(struct fs_diriteminfo);
        dirp->di->fileType = entry->is_directory ? FT_DIRECTORY : FT_REGFILE;
        strncpy(dirp->di->d_name, entry->name,
            sizeof(dirp->di->d_name) - 1);

        return dirp->di;
    }

    return NULL;
}

int fs_closedir(fdDir *dirp)
{
    if (dirp == NULL)
    {
        return -1;
    }

    free(dirp->directory);
    free(dirp->di);
    free(dirp);
    return 0;
}

char *fs_getcwd(char *pathname, size_t size)
{
    if ((pathname == NULL) || (ensureCwdInitialized() != 0))
    {
        return NULL;
    }

    if (strlen(g_cwdPath) + 1 > size)
    {
        return NULL;
    }

    strcpy(pathname, g_cwdPath);
    return pathname;
}

int fs_setcwd(char *pathname)
{
    VolumeControlBlock vcb;
    DirectoryEntry dirEntry;
    char newPath[PATH_BUFFER_SIZE];

    if ((pathname == NULL) || (loadVCB(&vcb) != 0) ||
        (fs_resolvePath(pathname, &dirEntry, NULL, NULL) != 0) ||
        (dirEntry.is_directory == 0) ||
        (canonicalizePath(pathname, newPath, sizeof(newPath)) != 0))
    {
        return -1;
    }

    strncpy(g_cwdPath, newPath, sizeof(g_cwdPath) - 1);
    g_cwdPath[sizeof(g_cwdPath) - 1] = '\0';
    g_cwdLocation = dirEntry.location;
    g_cwdBlocks = blocksForEntry(&vcb, &dirEntry);

    return 0;
}

int fs_isDir(char *pathname)
{
    DirectoryEntry entry;

    if ((pathname == NULL) ||
        (fs_resolvePath(pathname, &entry, NULL, NULL) != 0))
    {
        return 0;
    }

    return entry.is_directory ? 1 : 0;
}
