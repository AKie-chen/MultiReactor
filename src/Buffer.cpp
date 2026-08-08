#include "Buffer.h"
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <sys/uio.h>

Buffer::Buffer()
    : readIndex_(kPrependSize), writeIndex_(kPrependSize)
{
    buf_.resize(kPrependSize);
}

Buffer::~Buffer() {}

Buffer::ReadResult Buffer::readFd(int fd)
{
    while (true) {
        char extrabuf[65536];
        size_t writable = writeableBytes();

        iovec iov[2];
        iov[0].iov_base = &buf_[writeIndex_];
        iov[0].iov_len  = writable;
        iov[1].iov_base = extrabuf;
        iov[1].iov_len  = sizeof(extrabuf);

        ssize_t bytes_read = readv(fd, iov, 2);
        if (bytes_read > 0) {
            if (static_cast<size_t>(bytes_read) <= writable) {
                writeIndex_ += bytes_read;
            } else {
                writeIndex_ = buf_.size();
                append(extrabuf, bytes_read - writable);
            }
            continue;
        } else if (bytes_read == 0) {
            return kClosed;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return kSuccess;
            } else {
                return kError;
            }
        }
    }
}

Buffer::ReadResult Buffer::writeFd(int fd)
{
    size_t remaining = writeIndex_ - readIndex_;
    while (remaining > 0) {
        ssize_t n = send(fd, buf_.data() + readIndex_, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            readIndex_ += n;
            remaining -= n;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return kSuccess;
        } else {
            return kError;
        }
    }
    return kSuccess;
}

std::string Buffer::retrieveAll()
{
    std::string result(buf_.data() + readIndex_, writeIndex_ - readIndex_);
    readIndex_  = kPrependSize;
    writeIndex_ = kPrependSize;
    buf_.resize(kPrependSize);
    this->shrinkIfLarge();
    return result;
}

std::string Buffer::retrieve(size_t len)
{
    if (len > readableBytes()) {
        len = readableBytes();
    }
    if (len == 0) {
        return "";
    }
    std::string result(buf_.data() + readIndex_, len);
    readIndex_ += len;
    if (readIndex_ == writeIndex_) {
        readIndex_  = kPrependSize;
        writeIndex_ = kPrependSize;
    }
    return result;
}

size_t Buffer::readableBytes()
{
    return writeIndex_ - readIndex_;
}

size_t Buffer::writeableBytes()
{
    return buf_.size() - writeIndex_;
}

void Buffer::prepend(const char* data, size_t len)
{
    // 利用头部预留空间，O(1) prepend（大多数情况）
    if (len <= readIndex_) {
        readIndex_ -= len;
        memcpy(&buf_[readIndex_], data, len);
    } else {
        // 预留空间不足：腾挪数据，扩大预留区
        size_t readable = readableBytes();
        size_t newPrepend = len + kPrependSize;
        std::vector<char> newBuf(newPrepend + readable);
        memcpy(newBuf.data() + newPrepend,
               buf_.data() + readIndex_, readable);
        memcpy(newBuf.data() + newPrepend - len, data, len);
        readIndex_  = newPrepend - len;
        writeIndex_ = newPrepend + readable;
        buf_.swap(newBuf);
    }
}

void Buffer::append(const char* data, size_t len)
{
    if (len > (buf_.size() - writeIndex_)) {
        // 指数扩容：避免频繁 realloc，每次至少翻倍
        size_t need = writeIndex_ + len;
        size_t cap  = buf_.size();
        while (cap < need) cap *= 2;
        buf_.resize(cap);
    }
    char* dest = &buf_[writeIndex_];
    memcpy(dest, data, len);
    writeIndex_ += len;
}

const char* Buffer::peek() const
{
    return buf_.data() + readIndex_;
}

void Buffer::shrinkIfLarge()
{
    if (buf_.capacity() > 65536) {
        buf_.resize(kPrependSize);
        buf_.shrink_to_fit();
    } else {
        buf_.resize(kPrependSize);
    }
}