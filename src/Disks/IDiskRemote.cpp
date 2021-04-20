#include <Disks/IDiskRemote.h>

#include "Disks/DiskFactory.h"
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromS3.h>
#include <IO/WriteHelpers.h>
#include <Poco/File.h>
#include <Common/checkStackSize.h>
#include <Common/createHardLink.h>
#include <Common/quoteString.h>
#include <common/logger_useful.h>
#include <boost/algorithm/string.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT;
    extern const int INCORRECT_DISK_INDEX;
    extern const int FILE_ALREADY_EXISTS;
    extern const int CANNOT_DELETE_DIRECTORY;
}


/// Load metadata by path or create empty if `create` flag is set.
IDiskRemote::Metadata::Metadata(
        const String & remote_fs_root_path_,
        const String & disk_path_,
        const String & metadata_file_path_,
        bool create)
    : remote_fs_root_path(remote_fs_root_path_)
    , disk_path(disk_path_)
    , metadata_file_path(metadata_file_path_)
    , total_size(0), remote_fs_objects(0), ref_count(0)
{
    if (create)
        return;

    try
    {
        ReadBufferFromFile buf(disk_path + metadata_file_path, 1024); /* reasonable buffer size for small file */

        UInt32 version;
        readIntText(version, buf);
        assertChar('\n', buf);

        if (version < VERSION_ABSOLUTE_PATHS || version > VERSION_READ_ONLY_FLAG)
            throw Exception(
                ErrorCodes::UNKNOWN_FORMAT,
                "Unknown metadata file version. Path: {}. Version: {}. Maximum expected version: {}",
                disk_path + metadata_file_path, std::to_string(version), std::to_string(VERSION_READ_ONLY_FLAG));

        UInt32 remote_fs_objects_count;
        readIntText(remote_fs_objects_count, buf);
        assertChar('\t', buf);
        readIntText(total_size, buf);
        assertChar('\n', buf);
        remote_fs_objects.resize(remote_fs_objects_count);

        for (size_t i = 0; i < remote_fs_objects_count; ++i)
        {
            String remote_fs_object_path;
            size_t remote_fs_object_size;
            readIntText(remote_fs_object_size, buf);
            assertChar('\t', buf);
            readEscapedString(remote_fs_object_path, buf);
            if (version == VERSION_ABSOLUTE_PATHS)
            {
                if (!boost::algorithm::starts_with(remote_fs_object_path, remote_fs_root_path))
                    throw Exception(
                        ErrorCodes::UNKNOWN_FORMAT,
                        "Path in metadata does not correspond S3 root path. Path: {}, root path: {}, disk path: {}",
                        remote_fs_object_path, remote_fs_root_path, disk_path_);

                remote_fs_object_path = remote_fs_object_path.substr(remote_fs_root_path.size());
            }
            assertChar('\n', buf);
            remote_fs_objects[i] = {remote_fs_object_path, remote_fs_object_size};
        }

        readIntText(ref_count, buf);
        assertChar('\n', buf);

        if (version >= VERSION_READ_ONLY_FLAG)
        {
            readBoolText(read_only, buf);
            assertChar('\n', buf);
        }
    }
    catch (Exception & e)
    {
        if (e.code() == ErrorCodes::UNKNOWN_FORMAT)
            throw;

        throw Exception("Failed to read metadata file", e, ErrorCodes::UNKNOWN_FORMAT);
    }
}

void IDiskRemote::Metadata::addObject(const String & path, size_t size)
{
    total_size += size;
    remote_fs_objects.emplace_back(path, size);
}

/// Fsync metadata file if 'sync' flag is set.
void IDiskRemote::Metadata::save(bool sync)
{
    WriteBufferFromFile buf(disk_path + metadata_file_path, 1024);

    writeIntText(VERSION_RELATIVE_PATHS, buf);
    writeChar('\n', buf);

    writeIntText(remote_fs_objects.size(), buf);
    writeChar('\t', buf);
    writeIntText(total_size, buf);
    writeChar('\n', buf);

    for (const auto & [remote_fs_object_path, remote_fs_object_size] : remote_fs_objects)
    {
        writeIntText(remote_fs_object_size, buf);
        writeChar('\t', buf);
        writeEscapedString(remote_fs_object_path, buf);
        writeChar('\n', buf);
    }

    writeIntText(ref_count, buf);
    writeChar('\n', buf);

    writeBoolText(read_only, buf);
    writeChar('\n', buf);

    buf.finalize();
    if (sync)
        buf.sync();
}


IDiskRemote::Metadata IDiskRemote::readMeta(const String & path) const
{
    return Metadata(remote_fs_root_path, metadata_path, path);
}


IDiskRemote::Metadata IDiskRemote::createMeta(const String & path) const
{
    return Metadata(remote_fs_root_path, metadata_path, path, true);
}


void IDiskRemote::removeMeta(const String & path, bool keep_in_remote_fs)
{
    LOG_DEBUG(log, "Remove file by path: {}", backQuote(metadata_path + path));

    Poco::File file(metadata_path + path);

    if (!file.isFile())
        throw Exception(ErrorCodes::CANNOT_DELETE_DIRECTORY, "Path '{}' is a directory", path);

    try
    {
        auto metadata = readMeta(path);

        /// If there is no references - delete content from S3.
        if (metadata.ref_count == 0)
        {
            file.remove();

            if (!keep_in_remote_fs)
                removeFromRemoteFS(metadata);
        }
        else /// In other case decrement number of references, save metadata and delete file.
        {
            --metadata.ref_count;
            metadata.save();
            file.remove();
        }
    }
    catch (const Exception & e)
    {
        /// If it's impossible to read meta - just remove it from FS.
        if (e.code() == ErrorCodes::UNKNOWN_FORMAT)
        {
            LOG_WARNING(
                log,
                "Metadata file {} can't be read by reason: {}. Removing it forcibly.",
                backQuote(path),
                e.nested() ? e.nested()->message() : e.message());

            file.remove();
        }
        else
            throw;
    }
}


void IDiskRemote::removeMetaRecursive(const String & path, bool keep_in_remote_fs)
{
    checkStackSize(); /// This is needed to prevent stack overflow in case of cyclic symlinks.

    Poco::File file(metadata_path + path);
    if (file.isFile())
    {
        removeMeta(path, keep_in_remote_fs);
    }
    else
    {
        for (auto it{iterateDirectory(path)}; it->isValid(); it->next())
            removeMetaRecursive(it->path(), keep_in_remote_fs);
        file.remove();
    }
}


DiskPtr DiskRemoteReservation::getDisk(size_t i) const
{
    if (i != 0)
        throw Exception("Can't use i != 0 with single disk reservation", ErrorCodes::INCORRECT_DISK_INDEX);
    return disk;
}


void DiskRemoteReservation::update(UInt64 new_size)
{
    std::lock_guard lock(disk->reservation_mutex);
    disk->reserved_bytes -= size;
    size = new_size;
    disk->reserved_bytes += size;
}


DiskRemoteReservation::~DiskRemoteReservation()
{
    try
    {
        std::lock_guard lock(disk->reservation_mutex);
        if (disk->reserved_bytes < size)
        {
            disk->reserved_bytes = 0;
            LOG_ERROR(disk->log, "Unbalanced reservations size for disk '{}'.", disk->getName());
        }
        else
        {
            disk->reserved_bytes -= size;
        }

        if (disk->reservation_count == 0)
            LOG_ERROR(disk->log, "Unbalanced reservation count for disk '{}'.", disk->getName());
        else
            --disk->reservation_count;
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}


IDiskRemote::IDiskRemote(
    const String & disk_name_,
    const String & remote_fs_root_path_,
    const String & metadata_path_,
    const String & log_name_,
    std::unique_ptr<Executor> executor_)
    : IDisk(std::move(executor_))
    , disk_name(disk_name_)
    , remote_fs_root_path(remote_fs_root_path_)
    , metadata_path(metadata_path_)
    , log(&Poco::Logger::get(log_name_))
{
}


bool IDiskRemote::exists(const String & path) const
{
    return Poco::File(metadata_path + path).exists();
}


bool IDiskRemote::isFile(const String & path) const
{
    return Poco::File(metadata_path + path).isFile();
}


void IDiskRemote::createFile(const String & path)
{
    /// Create empty metadata file.
    auto metadata = createMeta(path);
    metadata.save();
}


size_t IDiskRemote::getFileSize(const String & path) const
{
    auto metadata = readMeta(path);
    return metadata.total_size;
}


void IDiskRemote::moveFile(const String & from_path, const String & to_path)
{
    if (exists(to_path))
        throw Exception("File already exists: " + to_path, ErrorCodes::FILE_ALREADY_EXISTS);

    Poco::File(metadata_path + from_path).renameTo(metadata_path + to_path);
}


void IDiskRemote::replaceFile(const String & from_path, const String & to_path)
{
    if (exists(to_path))
    {
        const String tmp_path = to_path + ".old";
        moveFile(to_path, tmp_path);
        moveFile(from_path, to_path);
        removeFile(tmp_path);
    }
    else
        moveFile(from_path, to_path);
}


void IDiskRemote::removeSharedFile(const String & path, bool keep_in_remote_fs)
{
    removeMeta(path, keep_in_remote_fs);
}


void IDiskRemote::removeSharedRecursive(const String & path, bool keep_in_remote_fs)
{
    removeMetaRecursive(path, keep_in_remote_fs);
}


void IDiskRemote::removeFileIfExists(const String & path)
{
    if (Poco::File(metadata_path + path).exists())
        removeMeta(path, /* keep_in_remote_fs */ false);
}


void IDiskRemote::removeRecursive(const String & path)
{
    checkStackSize(); /// This is needed to prevent stack overflow in case of cyclic symlinks.

    Poco::File file(metadata_path + path);
    if (file.isFile())
    {
        removeFile(path);
    }
    else
    {
        for (auto it{iterateDirectory(path)}; it->isValid(); it->next())
            removeRecursive(it->path());
        file.remove();
    }
}


void IDiskRemote::setReadOnly(const String & path)
{
    /// We should store read only flag inside metadata file (instead of using FS flag),
    /// because we modify metadata file when create hard-links from it.
    auto metadata = readMeta(path);
    metadata.read_only = true;
    metadata.save();
}


bool IDiskRemote::isDirectory(const String & path) const
{
    return Poco::File(metadata_path + path).isDirectory();
}


void IDiskRemote::createDirectory(const String & path)
{
    Poco::File(metadata_path + path).createDirectory();
}


void IDiskRemote::createDirectories(const String & path)
{
    Poco::File(metadata_path + path).createDirectories();
}


void IDiskRemote::clearDirectory(const String & path)
{
    for (auto it{iterateDirectory(path)}; it->isValid(); it->next())
        if (isFile(it->path()))
            removeFile(it->path());
}


void IDiskRemote::removeDirectory(const String & path)
{
    Poco::File(metadata_path + path).remove();
}


DiskDirectoryIteratorPtr IDiskRemote::iterateDirectory(const String & path)
{
    return std::make_unique<RemoteDiskDirectoryIterator>(metadata_path + path, path);
}


void IDiskRemote::listFiles(const String & path, std::vector<String> & file_names)
{
    for (auto it = iterateDirectory(path); it->isValid(); it->next())
        file_names.push_back(it->name());
}


void IDiskRemote::setLastModified(const String & path, const Poco::Timestamp & timestamp)
{
    Poco::File(metadata_path + path).setLastModified(timestamp);
}


Poco::Timestamp IDiskRemote::getLastModified(const String & path)
{
    return Poco::File(metadata_path + path).getLastModified();
}


void IDiskRemote::createHardLink(const String & src_path, const String & dst_path)
{
    /// Increment number of references.
    auto src = readMeta(src_path);
    ++src.ref_count;
    src.save();

    /// Create FS hardlink to metadata file.
    DB::createHardLink(metadata_path + src_path, metadata_path + dst_path);
}


ReservationPtr IDiskRemote::reserve(UInt64 bytes)
{
    if (!tryReserve(bytes))
        return {};

    return std::make_unique<DiskRemoteReservation>(std::static_pointer_cast<IDiskRemote>(shared_from_this()), bytes);
}


bool IDiskRemote::tryReserve(UInt64 bytes)
{
    std::lock_guard lock(reservation_mutex);
    if (bytes == 0)
    {
        LOG_DEBUG(log, "Reserving 0 bytes on s3 disk {}", backQuote(disk_name));
        ++reservation_count;
        return true;
    }

    auto available_space = getAvailableSpace();
    UInt64 unreserved_space = available_space - std::min(available_space, reserved_bytes);
    if (unreserved_space >= bytes)
    {
        LOG_DEBUG(log, "Reserving {} on disk {}, having unreserved {}.",
            ReadableSize(bytes), backQuote(disk_name), ReadableSize(unreserved_space));
        ++reservation_count;
        reserved_bytes += bytes;
        return true;
    }
    return false;
}

}
