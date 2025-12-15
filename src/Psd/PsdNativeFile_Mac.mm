//
//  PsdNativeFile_Mac.mm
//  Contributed to psd_sdk
//
//  Created by Oluseyi Sonaiya on 3/29/20.
//  Copyright © 2020 Oluseyi Sonaiya. All rights reserved.
//
// psd_sdk Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)
//
// FIXED VERSION: Removed circular block references that caused crashes

#include <wchar.h>
#include <codecvt>
#include <locale>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "PsdPch.h"
#include "PsdNativeFile_Mac.h"

#include "PsdAllocator.h"
#include "PsdPlatform.h"
#include "PsdMemoryUtil.h"
#include "PsdLog.h"
#include "Psdinttypes.h"


PSD_NAMESPACE_BEGIN

// FIXED: Simplified operation structures - no longer store blocks
struct DispatchReadOperation
{
    void* dataReadBuffer;
    uint32_t length;
    uint64_t offset;
    dispatch_semaphore_t semaphore;
    bool success;
};

struct DispatchWriteOperation
{
    dispatch_data_t dataToWrite;
    size_t bytesWritten;
    uint32_t length;
    uint64_t offset;
    dispatch_semaphore_t semaphore;
    bool success;
};


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
NativeFile::NativeFile(Allocator* allocator)
    : File(allocator)
    , m_fileDescriptor(0)
{
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoOpenRead(const wchar_t* filename)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>,wchar_t> convert;
    std::string s = convert.to_bytes(filename);
    char const *cs = s.c_str();
    m_fileDescriptor = open(cs, O_RDONLY);
    if (m_fileDescriptor == -1)
    {
        PSD_ERROR("NativeFile", "Cannot obtain handle for file \"%ls\".", filename);
        return false;
    }

    return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoOpenWrite(const wchar_t* filename)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>,wchar_t> convert;
    std::string s = convert.to_bytes(filename);
    char const *cs = s.c_str();
    m_fileDescriptor = open(cs, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP);
    if (m_fileDescriptor == -1)
    {
        PSD_ERROR("NativeFile", "Cannot obtain handle for file \"%ls\".", filename);
        return false;
    }

    return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoClose(void)
{
    if (m_fileDescriptor == -1)
        return false;

    const int success = close(m_fileDescriptor);
    if  (success == -1)
    {
        PSD_ERROR("NativeFile", "Cannot close handle.");
        return false;
    }

    m_fileDescriptor = -1;
    return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
File::ReadOperation NativeFile::DoRead(void* buffer, uint32_t count, uint64_t position)
{
    DispatchReadOperation *operation = memoryUtil::Allocate<DispatchReadOperation>(m_allocator);
    operation->dataReadBuffer = buffer;
    operation->length = count;
    operation->offset = position;
    operation->semaphore = nullptr;  // Will be created in DoWaitForRead
    operation->success = false;

    return static_cast<File::ReadOperation>(operation);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoWaitForRead(File::ReadOperation& operation)
{
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    DispatchReadOperation *op = static_cast<DispatchReadOperation *>(operation);
    lseek(m_fileDescriptor, op->offset, SEEK_SET);
    op->semaphore = dispatch_semaphore_create(0);
    op->success = false;

    // FIXED: Create block inline without circular reference
    // Capture only what we need, not the whole operation
    void* targetBuffer = op->dataReadBuffer;
    uint32_t expectedLength = op->length;
    dispatch_semaphore_t sem = op->semaphore;
    __block bool* successPtr = &op->success;

    dispatch_read(m_fileDescriptor, op->length, queue, ^(dispatch_data_t data, int error) {
        if (error == 0 && data != NULL) {
            dispatch_data_apply(data, ^bool(dispatch_data_t  _Nonnull region, size_t offset, const void * _Nonnull buffer, size_t size) {
                memcpy(targetBuffer, buffer, size);
                return true;
            });

            size_t bytesRead = dispatch_data_get_size(data);
            if (bytesRead >= expectedLength) {
                *successPtr = true;
            } else {
                PSD_ERROR("NativeFile", "Read only %zu of %u bytes", bytesRead, expectedLength);
            }
        } else {
            PSD_ERROR("NativeFile", "Dispatch read error: %d", error);
        }
        dispatch_semaphore_signal(sem);
    });

    dispatch_semaphore_wait(op->semaphore, DISPATCH_TIME_FOREVER);

    bool result = op->success;
    dispatch_release(op->semaphore);

    if (!result) {
        PSD_ERROR("NativeFile", "Failed to wait for previous asynchronous read operation.");
        return false;
    }

    return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
File::WriteOperation NativeFile::DoWrite(const void* buffer, uint32_t count, uint64_t position)
{
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    DispatchWriteOperation *operation = memoryUtil::Allocate<DispatchWriteOperation>(m_allocator);
    operation->length = count;
    operation->offset = position;
    operation->bytesWritten = 0;
    operation->success = false;
    operation->dataToWrite = dispatch_data_create(buffer, count, queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    operation->semaphore = nullptr;  // Will be created in DoWaitForWrite

    return static_cast<File::WriteOperation>(operation);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoWaitForWrite(File::WriteOperation& operation)
{
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    DispatchWriteOperation *op = static_cast<DispatchWriteOperation *>(operation);
    lseek(m_fileDescriptor, op->offset, SEEK_SET);
    op->semaphore = dispatch_semaphore_create(0);

    // FIXED: Create block inline without circular reference
    uint32_t expectedLength = op->length;
    dispatch_semaphore_t sem = op->semaphore;
    __block size_t* bytesWrittenPtr = &op->bytesWritten;
    __block bool* successPtr = &op->success;

    dispatch_write(m_fileDescriptor, op->dataToWrite, queue, ^(dispatch_data_t d, int error) {
        if (error == 0) {
            *bytesWrittenPtr = expectedLength;
            *successPtr = true;
        } else {
            PSD_ERROR("NativeFile", "Dispatch write error: %d", error);
        }
        dispatch_semaphore_signal(sem);
    });

    dispatch_semaphore_wait(op->semaphore, DISPATCH_TIME_FOREVER);

    bool result = op->success;
    dispatch_release(op->semaphore);
    dispatch_release(op->dataToWrite);

    if (!result || op->bytesWritten < op->length) {
        PSD_ERROR("NativeFile", "Failed to wait for previous asynchronous write operation.");
        return false;
    }

    return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
uint64_t NativeFile::DoGetSize(void) const
{
    if (m_fileDescriptor == -1)
        return 0;

    struct stat fileStat;
    if (fstat(m_fileDescriptor, &fileStat) == 0) {
        return static_cast<uint64_t>(fileStat.st_size);
    }

    return 0;
}

PSD_NAMESPACE_END
