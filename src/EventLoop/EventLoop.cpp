// Use of this source code is governed by a BSD-style license// that can be found in the License file.// Author: adugeek#include "EventLoop.h"#include <sys/timerfd.h>ChannelPtr EventLoop::null_channel_ptr = nullptr;ChannelPtr &EventLoop::add_channel(int fd, bool is_socket, bool is_nonblock, ssize_t lifetime,                                   ChannelCallback io_event_cb) {    if (init_status_ == INIT) {        init();    }    if (init_status_ != SUCCESS || unlawful_fd(fd)) {        return null_channel_ptr;    }    ChannelPtr ptr(new Channel(fd, is_socket, is_nonblock, channel_event_map_));    if (ptr == nullptr) {        return null_channel_ptr;    }    ptr->set_event_cb(io_event_cb);    reg_event_.events = EPOLLIN;    reg_event_.data.u32 = ptr->id();    if (epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &reg_event_) < 0) {        return null_channel_ptr;    }    ChannelId id = ptr->id();    channel_map_[id] = std::move(ptr);    if (lifetime > 0) {        add_channel_lifetime(id, static_cast<size_t>(lifetime));    }    return channel_map_[id];}ChannelPtr &EventLoop::add_connecting_channel(int fd, ssize_t connect_time, ssize_t lifetime,                                              ChannelCallback io_event_cb) {    if (init_status_ == INIT) {        init();    }    if (init_status_ != SUCCESS || unlawful_fd(fd)) {        return null_channel_ptr;    }    ChannelPtr ptr(new Channel(fd, true, true, channel_event_map_));    if (ptr == nullptr) {        return null_channel_ptr;    }    ptr->set_event_cb(io_event_cb);    ptr->is_connected_ = false;    reg_event_.events = EPOLLOUT;    reg_event_.data.u32 = ptr->id();    if (epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &reg_event_) < 0) {        return null_channel_ptr;    }    ChannelId id = ptr->id();    channel_map_[id] = std::move(ptr);    if (connect_time > 0) {        add_task_on_channel(false, id, static_cast<size_t>(connect_time), nullptr,                            [this, id](EventLoopPtr &, ChannelPtr &channel_ptr, void *, bool *again) {                                if (!channel_ptr->is_connected_) {                                    channel_event_map_[id] |= EVENT_CONNECT_TIMEOVER;                                }                                *again = false;                            });    }    if (lifetime > 0) {        add_channel_lifetime(id, static_cast<size_t>(lifetime));    }    return channel_map_[id];}int EventLoop::create_timer_fd() noexcept {    struct itimerspec value;    value.it_value.tv_sec = 1;    value.it_value.tv_nsec = 0;    value.it_interval.tv_sec = 1;    value.it_interval.tv_nsec = 0;    timer_ = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);    return timerfd_settime(timer_, 0, &value, nullptr) < 0 ? -1 : 0;}int EventLoop::add_timer_channel() {    ChannelPtr timer_channel(new Channel(timer_, false, false, channel_event_map_));    if (timer_channel == nullptr) {        return -1;    }    timer_channel->event_cb_ = [this](EventLoop *loop, const ChannelPtr &channel_ptr, ChannelEvent event) {        uint64_t times;        if (event & EVENT_IN) {            if (read(channel_ptr->fd(), &times, sizeof(uint64_t)) != sizeof(uint64_t)) {                loop->stop();            }            task_wheel_.tick();            return;        }        loop->stop();    };    reg_event_.data.u32 = timer_channel->id();    reg_event_.events = EPOLLIN;    if (epoll_ctl(epoll_, EPOLL_CTL_ADD, timer_channel->fd(), &reg_event_) < 0) {        return -1;    }    ChannelId id = timer_channel->id();    channel_map_[id] = std::move(timer_channel);    return 0;}void EventLoop::handle_cb() noexcept {    auto iterator = channel_event_map_.begin();    while (iterator != channel_event_map_.end()) {        const ChannelId channel_id = iterator->first;        ChannelPtr &channel_ptr = channel_map_[channel_id];        if (channel_ptr == nullptr) {            channel_lives_map_.erase(channel_id);            channel_map_.erase(channel_id);            channel_event_map_.erase(iterator++);            continue;        }        ChannelEvent &channel_event = iterator->second;        if (channel_event == 0) {            channel_event_map_.erase(iterator++);            continue;        }        const uint32_t io_event = channel_event & 0x0000ffff;        if (io_event != 0) {            channel_ptr->event_cb_(this, channel_ptr, io_event);            channel_event ^= io_event;        }        if (channel_event == 0) {            channel_event_map_.erase(iterator++);            continue;        }        if (channel_event & TODO_ERASE) {            channel_lives_map_.erase(channel_id);            channel_map_.erase(channel_id);            channel_event_map_.erase(iterator++);            continue;        }        reg_event_.data.u32 = channel_id;        reg_event_.events = 0;        if (channel_event & TODO_REGO) {            reg_event_.events = (EPOLLIN | EPOLLOUT);            channel_event ^= TODO_REGO;        }        if (channel_event & TODO_OUTPUT) {            if (!channel_ptr->is_connected_) {                int error = 0;                socklen_t sz = sizeof(int);                int code = getsockopt(channel_ptr->fd(), SOL_SOCKET, SO_ERROR, (void *) &error, &sz);                if (code < 0 || error != 0) {                    reg_event_.events = 0;                    channel_event |= EVENT_CONNECT_ERR;                }                else {                    channel_ptr->is_connected_ = true;                    reg_event_.events = (EPOLLIN | EPOLLOUT);                }            }            if (channel_ptr->is_connected_) {                write_begin_label:                ssize_t write_res = channel_ptr->write_buffer_.write_some(channel_ptr->fd());                if (write_res < 0) {                    if (EINTR == errno) {                        goto write_begin_label;                    }                    else if (EAGAIN == errno) {                        // nothing                        // 如果 channel 之前is_connected==false ，因为reg_event_.events 在前面赋值过了，所以此处不需要改变 reg_event_.events                        // 如果 channel 之前is_connected==true ,  则（EPOLLIN | EPOLLOUT） 事件必然已经注册过，reg_event_.events 没必要赋值导致重新注册                    }                    else {                        reg_event_.events = 0;                        channel_event |= EVENT_SEND_ERR;                    }                }                else if (!channel_ptr->write_buffer_.empty()) {                    //nothing                    // 如果 channel 之前is_connected==false 因为reg_event_.events 在前面赋值过了，所以此处不需要改变 reg_event_.events                    // 如果 channel 之前is_connected==true ,则 （EPOLLIN | EPOLLOUT） 事件必然已经注册过，reg_event_.events 没必要赋值导致重新注册                }                else if (channel_ptr->write_buffer_.empty()) {                    reg_event_.events = EPOLLIN;                }            }            channel_event ^= TODO_OUTPUT;        }        if (channel_event & TODO_SHUTDOWN) {            if (channel_ptr->write_buffer_.empty()) {                ::shutdown(channel_ptr->fd(), SHUT_RDWR);                channel_event ^= TODO_SHUTDOWN;            }        }        if (reg_event_.events != 0 && epoll_ctl(epoll_, EPOLL_CTL_MOD, channel_ptr->fd(), &reg_event_) < 0) {            channel_event |= EVENT_EPOLL_ERR;        }        channel_event == 0 ? channel_event_map_.erase(iterator++) : ++iterator;    }}void EventLoop::loop() noexcept {    epoll_event events[100];    while (!quit_) {        begin_wait_label:        int res = epoll_wait(epoll_, events, 100, -1);        if (res < 0) {            if (errno == EINTR) {                goto begin_wait_label;            }            quit_ = true;            break;        }        for (int i = 0; i < res; ++i) {            const ChannelId id = events[i].data.u32;            uint32_t epoll_event = events[i].events;            ChannelEvent &channel_event = channel_event_map_[id];            if (epoll_event & EPOLLHUP) {                channel_event |= EVENT_HUP;            }            if (epoll_event & EPOLLIN) {                channel_event |= EVENT_IN;            }            if (epoll_event & EPOLLOUT) {                channel_event |= TODO_OUTPUT;            }        }        handle_cb();    }}EventLoop::~EventLoop() noexcept {    if (context_deleter != nullptr) {        context_deleter(context.ptr);    }    channel_lives_map_.clear();    channel_event_map_.clear();    channel_map_.clear();    close(epoll_);}