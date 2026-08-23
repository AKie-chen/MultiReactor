#include "Buffer.h"
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <algorithm>
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
        iov[0].iov_base = buf_.data() + writeIndex_; // writable==0 时也安全：data()+size 是一端后指针，无 operator[] UB
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
            } else if (errno == EINTR) {
                continue;
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
        } else if (errno == EINTR) {
            continue; // 被信号打断，重试
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
        this->shrinkIfLarge(); // 仅完全消费后收缩；否则 resize 会截断未读数据
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
    makeSpace(len);
    char* dest = buf_.data() + writeIndex_;
    memcpy(dest, data, len);
    writeIndex_ += len;
}

const char* Buffer::peek() const
{
    return buf_.data() + readIndex_;
}

void Buffer::shrinkIfLarge()
{
    if (buf_.size() > kPrependSize) {
        buf_.resize(kPrependSize); // 仅空缓冲区可安全调用（调用方保证）
    }
    // 超过高水位才真正归还内存，避免 keep-alive 连接每请求一次 shrink_to_fit 的分配抖动
    if (buf_.capacity() > 2 * 1024 * 1024) {
        buf_.shrink_to_fit();
    }
}

void Buffer::makeSpace(size_t len)
{
    if (writeableBytes() + prependableBytes() < len + kPrependSize) {
        // 可用空间不够：扩容。need 含 writeIndex_，保证不截断未读数据；
        // 指数增长避免每次 append 都 realloc + 拷贝可读段（O(n^2)）
        size_t need = writeIndex_ + len + kPrependSize;
        size_t cap  = buf_.size();
        while (cap < need) cap *= 2;
        buf_.resize(cap);
    } else {
        // 空间足够：把可读数据腾到头部，回收 readIndex_ 之前被消费过的空间
        size_t readable = readableBytes();
        std::copy(buf_.data() + readIndex_, buf_.data() + writeIndex_, buf_.data() + kPrependSize);
        readIndex_  = kPrependSize;
        writeIndex_ = readIndex_ + readable;
    }
}