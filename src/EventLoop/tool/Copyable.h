#ifndef EVENTLOOP_TOOL_COPYABLE_H
#define EVENTLOOP_TOOL_COPYABLE_H

class NonCopyable {
protected:
    constexpr NonCopyable() = default;

//    ~NonCopyable() = default;

    NonCopyable(const NonCopyable &) = delete;

    NonCopyable &operator=(const NonCopyable &) = delete;
};

class Copyable {
protected:
    constexpr Copyable() = default;
    //  ~Copyable() = default;
};

#endif //EVENTLOOP_TOOL_COPYABLE_H
