/*
 * Copyright (c) 2015 Roc authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_netio/udp_sender_port.h"
#include "roc_address/socket_addr_to_str.h"
#include "roc_core/helpers.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_netio/sendto.h"

namespace roc {
namespace netio {

namespace {

const core::nanoseconds_t PacketLogInterval = 20 * core::Second;

} // namespace

UdpSenderPort::UdpSenderPort(const UdpSenderConfig& config,
                             uv_loop_t& event_loop,
                             core::IAllocator& allocator)
    : BasicPort(allocator)
    , config_(config)
    , close_handler_(NULL)
    , close_handler_arg_(NULL)
    , loop_(event_loop)
    , write_sem_initialized_(false)
    , handle_initialized_(false)
    , pending_packets_(0)
    , sent_packets_(0)
    , sent_packets_blk_(0)
    , stopped_(true)
    , closed_(false)
    , fd_()
    , rate_limiter_(PacketLogInterval) {
}

UdpSenderPort::~UdpSenderPort() {
    if (handle_initialized_ || write_sem_initialized_) {
        roc_panic("udp sender: sender was not fully closed before calling destructor");
    }

    if (pending_packets_) {
        roc_panic("udp sender: packets weren't fully sent before calling destructor");
    }
}

const address::SocketAddr& UdpSenderPort::address() const {
    return config_.bind_address;
}

bool UdpSenderPort::open() {
    if (int err = uv_async_init(&loop_, &write_sem_, write_sem_cb_)) {
        roc_log(LogError, "udp sender: uv_async_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return false;
    }

    write_sem_.data = this;
    write_sem_initialized_ = true;

    if (int err = uv_udp_init(&loop_, &handle_)) {
        roc_log(LogError, "udp sender: uv_udp_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return false;
    }

    handle_.data = this;
    handle_initialized_ = true;

    int bind_err = UV_EINVAL;
    if (address_.family() == address::Family_IPv6) {
        bind_err = uv_udp_bind(&handle_, config_.bind_address.saddr(), UV_UDP_IPV6ONLY);
    }
    if (bind_err == UV_EINVAL || bind_err == UV_ENOTSUP) {
        bind_err = uv_udp_bind(&handle_, config_.bind_address.saddr(), 0);
    }
    if (bind_err != 0) {
        roc_log(LogError, "udp sender: uv_udp_bind(): [%s] %s", uv_err_name(bind_err),
                uv_strerror(bind_err));
        return false;
    }

    if (config_.broadcast_enabled) {
        roc_log(LogDebug, "udp sender: setting broadcast flag for port %s",
                address::socket_addr_to_str(config_.bind_address).c_str());

        if (int err = uv_udp_set_broadcast(&handle_, 1)) {
            roc_log(LogError, "udp sender: uv_udp_set_broadcast(): [%s] %s",
                    uv_err_name(err), uv_strerror(err));
            return false;
        }
    }

    int addrlen = (int)config_.bind_address.slen();
    if (int err = uv_udp_getsockname(&handle_, config_.bind_address.saddr(), &addrlen)) {
        roc_log(LogError, "udp sender: uv_udp_getsockname(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return false;
    }

    if (addrlen != (int)config_.bind_address.slen()) {
        roc_log(LogError,
                "udp sender: uv_udp_getsockname(): unexpected len: got=%lu expected=%lu",
                (unsigned long)addrlen, (unsigned long)config_.bind_address.slen());
        return false;
    }

    const int fd_err = uv_fileno((uv_handle_t*)&handle_, &fd_);
    if (fd_err != 0) {
        roc_panic("udp sender: uv_fileno(): [%s] %s", uv_err_name(fd_err),
                  uv_strerror(fd_err));
    }

    roc_log(LogInfo, "udp sender: opened port %s",
            address::socket_addr_to_str(config_.bind_address).c_str());

    stopped_ = false;

    return true;
}

bool UdpSenderPort::async_close(ICloseHandler& handler, void* handler_arg) {
    if (close_handler_) {
        roc_panic("udp sender: can't call async_close() twice");
    }

    close_handler_ = &handler;
    close_handler_arg_ = handler_arg;

    stopped_ = true;

    if (fully_closed_()) {
        return false;
    }

    if (pending_packets_ == 0) {
        start_closing_();
    }

    return true;
}

void UdpSenderPort::write(const packet::PacketPtr& pp) {
    if (!pp) {
        roc_panic("udp sender: unexpected null packet");
    }

    if (!pp->udp()) {
        roc_panic("udp sender: unexpected non-udp packet");
    }

    if (!pp->data()) {
        roc_panic("udp sender: unexpected packet w/o data");
    }

    if (stopped_) {
        roc_panic("udp sender: attempt to use stopped sender");
    }

    write_(pp);

    report_stats_();
}

void UdpSenderPort::write_(const packet::PacketPtr& pp) {
    const bool had_pending = (++pending_packets_ > 1);

    if (!had_pending) {
        if (try_nonblocking_send_(pp)) {
            --pending_packets_;
            return;
        }
    }

    queue_.push_back(*pp);

    if (int err = uv_async_send(&write_sem_)) {
        roc_panic("udp sender: uv_async_send(): [%s] %s", uv_err_name(err),
                  uv_strerror(err));
    }
}

void UdpSenderPort::close_cb_(uv_handle_t* handle) {
    roc_panic_if_not(handle);

    UdpSenderPort& self = *(UdpSenderPort*)handle->data;

    if (handle == (uv_handle_t*)&self.handle_) {
        self.handle_initialized_ = false;
    } else {
        self.write_sem_initialized_ = false;
    }

    if (self.handle_initialized_ || self.write_sem_initialized_) {
        return;
    }

    roc_log(LogInfo, "udp sender: closed port %s",
            address::socket_addr_to_str(self.config_.bind_address).c_str());

    roc_panic_if_not(self.close_handler_);

    self.closed_ = true;
    self.close_handler_->handle_closed(self, self.close_handler_arg_);
}

void UdpSenderPort::write_sem_cb_(uv_async_t* handle) {
    roc_panic_if_not(handle);

    UdpSenderPort& self = *(UdpSenderPort*)handle->data;

    // Using try_pop_front_exclusive() makes this method lock-free and wait-free.
    // try_pop_front_exclusive() may return NULL if the queue is not empty, but
    // push_back() is currently in progress. In this case we can exit the loop
    // before processing all packets, but write() always calls uv_async_send()
    // after push_back(), so we'll wake up soon and process the rest packets.
    while (packet::PacketPtr pp = self.queue_.try_pop_front_exclusive()) {
        packet::UDP& udp = *pp->udp();

        const int packet_num = ++self.sent_packets_;
        ++self.sent_packets_blk_;

        roc_log(
            LogTrace, "udp sender: sending packet: num=%d src=%s dst=%s sz=%ld",
            packet_num, address::socket_addr_to_str(self.config_.bind_address).c_str(),
            address::socket_addr_to_str(udp.dst_addr).c_str(), (long)pp->data().size());

        uv_buf_t buf;
        buf.base = (char*)pp->data().data();
        buf.len = pp->data().size();

        udp.request.data = &self;

        if (int err = uv_udp_send(&udp.request, &self.handle_, &buf, 1,
                                  udp.dst_addr.saddr(), send_cb_)) {
            roc_log(LogError, "udp sender: uv_udp_send(): [%s] %s", uv_err_name(err),
                    uv_strerror(err));
            continue;
        }

        // will be decremented in send_cb_()
        pp->incref();
    }
}

void UdpSenderPort::send_cb_(uv_udp_send_t* req, int status) {
    roc_panic_if_not(req);

    UdpSenderPort& self = *(UdpSenderPort*)req->data;

    packet::PacketPtr pp =
        packet::Packet::container_of(ROC_CONTAINER_OF(req, packet::UDP, request));

    // one reference for incref() called from write_sem_cb_()
    // one reference for the shared pointer above
    roc_panic_if(pp->getref() < 2);

    // decrement reference counter incremented in write_sem_cb_()
    pp->decref();

    if (status < 0) {
        roc_log(LogError,
                "udp sender:"
                " can't send packet: src=%s dst=%s sz=%ld: [%s] %s",
                address::socket_addr_to_str(self.config_.bind_address).c_str(),
                address::socket_addr_to_str(pp->udp()->dst_addr).c_str(),
                (long)pp->data().size(), uv_err_name(status), uv_strerror(status));
    }

    const int pending_packets = --self.pending_packets_;

    if (pending_packets == 0 && self.stopped_) {
        self.start_closing_();
    }
}

bool UdpSenderPort::fully_closed_() const {
    if (!handle_initialized_ && !write_sem_initialized_) {
        return true;
    }

    if (closed_) {
        return true;
    }

    return false;
}

void UdpSenderPort::start_closing_() {
    if (fully_closed_()) {
        return;
    }

    if (handle_initialized_ && !uv_is_closing((uv_handle_t*)&handle_)) {
        roc_log(LogInfo, "udp sender: closing port %s",
                address::socket_addr_to_str(config_.bind_address).c_str());

        uv_close((uv_handle_t*)&handle_, close_cb_);
    }

    if (write_sem_initialized_ && !uv_is_closing((uv_handle_t*)&write_sem_)) {
        uv_close((uv_handle_t*)&write_sem_, close_cb_);
    }
}

bool UdpSenderPort::try_nonblocking_send_(const packet::PacketPtr& pp) {
    if (!config_.non_blocking_enabled) {
        return false;
    }

    const packet::UDP& udp = *pp->udp();
    const bool success =
        sendto_nb(fd_, pp->data().data(), pp->data().size(), udp.dst_addr);

    if (success) {
        const int packet_num = ++sent_packets_;
        roc_log(
            LogTrace, "udp sender: sent packet non-blocking: num=%d src=%s dst=%s sz=%ld",
            packet_num, address::socket_addr_to_str(config_.bind_address).c_str(),
            address::socket_addr_to_str(udp.dst_addr).c_str(), (long)pp->data().size());
    }

    return success;
}

void UdpSenderPort::report_stats_() {
    if (!rate_limiter_.allow()) {
        return;
    }

    const int sent_packets = sent_packets_;
    const int sent_packets_nb = (sent_packets - sent_packets_blk_);

    const double nb_ratio =
        sent_packets_nb != 0 ? (double)sent_packets_ / sent_packets_nb : 0.;

    roc_log(LogDebug, "udp sender: total=%u nb=%u nb_ratio=%.5f", sent_packets,
            sent_packets_nb, nb_ratio);
}

} // namespace netio
} // namespace roc
