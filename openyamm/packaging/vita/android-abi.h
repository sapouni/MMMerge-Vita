#pragma once

#include <stdint.h>

// Android armeabi-v7a layouts. Do not substitute VitaSDK's host-facing structures.
typedef struct AndroidTimespec
{
    int32_t seconds;
    int32_t nanoseconds;
} AndroidTimespec;

typedef struct AndroidStat
{
    uint64_t device;
    uint8_t padding0[4];
    uint32_t legacyInode;
    uint32_t mode;
    uint32_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t specialDevice;
    uint8_t padding1[4];
    int64_t size;
    uint32_t blockSize;
    uint64_t blocks;
    AndroidTimespec accessTime;
    AndroidTimespec modifyTime;
    AndroidTimespec changeTime;
    uint64_t inode;
} AndroidStat;

typedef struct AndroidDirent
{
    uint64_t inode;
    int64_t offset;
    uint16_t recordLength;
    uint8_t type;
    char name[256];
} AndroidDirent;

typedef struct AndroidThreadAttributes
{
    uint32_t flags;
    uint32_t stackBase;
    uint32_t stackSize;
    uint32_t guardSize;
    int32_t policy;
    int32_t priority;
} AndroidThreadAttributes;
