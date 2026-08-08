#pragma once
#include <vector>
#include <string>

class Buffer {
private:
    static constexpr size_t kPrependSize = 8; // 头部预留空间，prepend 无需 O(n) 位移

    std::vector<char> buf_;
    size_t readIndex_;
    size_t writeIndex_;

public:
    enum ReadResult { kSuccess, kError, kClosed };

    Buffer();
    ~Buffer();

    ReadResult readFd(int fd);
    ReadResult writeFd(int fd);
    std::string retrieveAll();
    std::string retrieve(size_t len);
    size_t readableBytes();
    size_t writeableBytes();
    size_t prependableBytes() const { return readIndex_; }
    void prepend(const char* data, size_t len);
    void append(const char* data, size_t len);
    const char* peek() const;
    void shrinkIfLarge();
};