// Don't even ask what is this
#define nsel_CONFIG_SELECT_EXPECTED 1

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>

#include "asio.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "glog/logging.h"
#include "liburing.h"
#include "nonstd/expected.hpp"

using nonstd::expected;
using nonstd::make_unexpected;

using asio::add_service;
using asio::execution_context;
using asio::use_service;
using asio::detail::buffer_sequence_adapter;
using asio::detail::execution_context_service_base;
using asio::detail::io_object_impl;
using asio::detail::op_queue;
using asio::detail::scheduler;
using asio::detail::scheduler_operation;
using asio::detail::scheduler_task;
using asio::detail::scoped_ptr;

namespace {

// Declarations

class Ring {
    io_uring ring_;

public:
    using SqeRef = std::reference_wrapper<io_uring_sqe>;
    using CqeRef = std::reference_wrapper<io_uring_cqe>;

    static_assert(sizeof(SqeRef) == sizeof(io_uring_sqe*));
    static_assert(sizeof(CqeRef) == sizeof(io_uring_cqe*));

    explicit Ring(unsigned entries, io_uring_params& params);
    Ring(Ring&& rhs) noexcept;

    std::optional<SqeRef> getSqe() noexcept;
    expected<unsigned, std::error_code> submit() noexcept;
    expected<CqeRef, std::error_code> wait() noexcept;

    ~Ring();
};

class IoUringOperationBase : public scheduler_operation {
protected:
    size_t res_ = 0;
    std::error_code ec_ = {};

public:
    using scheduler_operation::func_type;
    using PrepFunc = void (*)(scheduler_operation*, io_uring_sqe&);

    explicit IoUringOperationBase(func_type completion, PrepFunc prep);

    void prepare(io_uring_sqe& sqe) { prep_(this, sqe); }
    void setRes(size_t res) { res_ = res; }
    void setErrorCode(std::error_code ec) { ec_ = ec; }

private:
    PrepFunc prep_;
};

template <typename Handler, typename MutableBufferSequence>
class IoUringReadAtOp : public IoUringOperationBase {
    int fd_;
    off_t offset_;
    Handler handler_;
    MutableBufferSequence buffers_;
    buffer_sequence_adapter<asio::mutable_buffer, MutableBufferSequence> bufs_;

public:
    ASIO_DEFINE_HANDLER_PTR(IoUringReadAtOp);

    template <typename H, typename M>
    explicit IoUringReadAtOp(int fd, off_t offset, H handler, M buffers)
        : IoUringOperationBase(onComplete, onPrepare),
          fd_(fd),
          offset_(offset),
          handler_(std::move(handler)),
          buffers_(buffers),
          bufs_(buffers) {}

private:
    static void onComplete(void*, scheduler_operation* base,
                           const std::error_code&, size_t) {
        auto op = static_cast<IoUringReadAtOp*>(base);

        // Chris, forgive me for what I am about to do
        op->handler_(op->ec_, op->res_);

        auto p = IoUringReadAtOp::ptr{std::addressof(op->handler_), op, op};
        p.reset();
    }

    static void onPrepare(scheduler_operation* base, io_uring_sqe& sqe) {
        auto op = static_cast<IoUringReadAtOp*>(base);
        ::io_uring_prep_readv(&sqe, op->fd_, op->bufs_.buffers(),
                              op->bufs_.count(), op->offset_);
    }
};

class MyIoUringService
    : public execution_context_service_base<MyIoUringService>,
      public scheduler_task {
    std::optional<Ring> ring_;
    std::optional<std::reference_wrapper<scheduler>> sched_;

public:
    using SchedOpQueue = op_queue<scheduler_operation>;

    // You can store descriptor here, but I don't want
    class implementation_type {};

    explicit MyIoUringService(execution_context&, Ring);
    explicit MyIoUringService(execution_context&);

    void construct(implementation_type&);
    void destroy(implementation_type&);

    void run(long usec, SchedOpQueue&) override;

    void interrupt() override;
    void shutdown() override;

    void startOp(IoUringOperationBase&);
};

class MyIoContext : public execution_context {
    scheduler& sched_;
    MyIoUringService& ringSvc_;

    MyIoUringService& initRingSvc(Ring ring);
    static scheduler_task* getTask(execution_context& svc);
    scheduler& initSched(int concurrencyHint);

public:
    class MyExecutor {};
    using executor_type = MyExecutor;

    friend class MyExecutor;

    explicit MyIoContext(Ring ring,
                         int concurrencyHint = ASIO_CONCURRENCY_HINT_DEFAULT);

    expected<size_t, std::error_code> run();
    executor_type get_executor() { return executor_type{}; }
};

class MyIoObject {
    using IoObjectImpl =
        io_object_impl<MyIoUringService, MyIoContext::executor_type>;

    IoObjectImpl impl_;

public:
    template <typename ExecutionContext>
    explicit MyIoObject(ExecutionContext&);

    template <typename MutableBuffers, typename ReadToken>
    auto asyncReadAt(int fd, off_t offset, MutableBuffers buffers,
                     ReadToken token) {
        auto initiation = AsyncReadInitiation{this};
        return asio::async_initiate<ReadToken, void(std::error_code, size_t)>(
            std::move(initiation), token, fd, offset, buffers);
    }

private:
    class AsyncReadInitiation;
    friend class AsyncReadInitiation;

    class AsyncReadInitiation {
        MyIoObject* obj_;

    public:
        explicit AsyncReadInitiation(MyIoObject* obj) : obj_(obj) {}

        template <typename MutableBuffers, typename Handler>
        void operator()(Handler handler, int fd, off_t offset,
                        MutableBuffers buffers) {
            using opType = IoUringReadAtOp<Handler, MutableBuffers>;

            auto op = typename opType::ptr{std::addressof(handler),
                                           opType::ptr::allocate(handler), 0};
            op.p = new (op.v) opType{fd, offset, handler, buffers};

            obj_->impl_.get_service().startOp(*op.p);
            op.v = op.p = 0;
        }
    };
};

// Definitions

Ring::Ring(unsigned entries, io_uring_params& params) {
    const auto res = ::io_uring_queue_init_params(entries, &ring_, &params);
    if (res < 0) {
        throw std::system_error{-res, std::system_category(),
                                "::io_uring_queue_init_params"};
    }
}

Ring::Ring(Ring&& rhs) noexcept : ring_(rhs.ring_) { rhs.ring_.ring_fd = -1; }

std::optional<Ring::SqeRef> Ring::getSqe() noexcept {
    const auto sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe) return std::nullopt;
    return *sqe;
}

std::error_category& ringSubmitErrorCategory() {
    static class RingSubmitErrorCategory : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "IoUring submit error category";
        }

        std::string message(int condition) const override {
            return std::strerror(condition);
        }
    } cat;

    return cat;
}

expected<unsigned, std::error_code> Ring::submit() noexcept {
    const auto res = ::io_uring_submit(&ring_);
    if (res < 0) {
        return make_unexpected(
            std::error_code{-res, ringSubmitErrorCategory()});
    }

    return res;
}

std::error_category& ringWaitErrorCategory() {
    static class RingWaitErrorCategory : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "IoUring wait error category";
        }
        std::string message(int condition) const override {
            return std::strerror(condition);
        }
    } cat;

    return cat;
}

expected<Ring::CqeRef, std::error_code> Ring::wait() noexcept {
    io_uring_cqe* cqe = nullptr;
    const auto res = ::io_uring_wait_cqe(&ring_, &cqe);
    if (res < 0) {
        return make_unexpected(std::error_code{-res, ringWaitErrorCategory()});
    }

    CHECK_EQ(res, 0) << "::io_uring_wait_cqe return value is greater than 0: "
                     << res << ". This wasn't in docs";
    return *cqe;
}

Ring::~Ring() {
    if (ring_.ring_fd > 0) ::io_uring_queue_exit(&ring_);
}

IoUringOperationBase::IoUringOperationBase(
    IoUringOperationBase::func_type completion,
    IoUringOperationBase::PrepFunc prep)
    : scheduler_operation(completion), prep_(prep) {}

MyIoUringService::MyIoUringService(execution_context& ctx, Ring ring)
    : execution_context_service_base<MyIoUringService>(ctx),
      ring_(std::move(ring)),
      sched_(use_service<scheduler>(ctx)) {}

MyIoUringService::MyIoUringService(execution_context& ctx)
    : execution_context_service_base<MyIoUringService>(ctx),
      ring_(std::nullopt),
      sched_(std::nullopt) {
    CHECK(false) << "Invalid ctr";
}

std::error_category& ringCompletionErrorCategory() {
    static class RingCompletionErrorCategory : public std::error_category {
    public:
        const char* name() const noexcept {
            return "IoUring completion error category";
        }
        std::string message(int condition) const {
            return std::strerror(condition);
        }
    } cat;

    return cat;
}

void MyIoUringService::run(long usec, MyIoUringService::SchedOpQueue& ops) {
    CHECK_EQ(usec, -1) << "Poll, *_until and *_for not implemented";

    const auto& cqe = [&]() -> decltype(auto) {
        auto res = ring_->wait();
        if (!res.has_value()) {
            const auto ec = res.get_unexpected();
            CHECK(false) << "Couldn't get completion: " << ec.value().value()
                         << " (" << ec.value().message() << ")";
        }

        return std::move(res).value().get();
    }();

    auto op = static_cast<IoUringOperationBase*>(::io_uring_cqe_get_data(&cqe));
    const auto res = cqe.res;
    if (res < 0) {
        op->setErrorCode(std::error_code{-res, ringCompletionErrorCategory()});
    } else {
        op->setRes(res);
    }

    ops.push(op);
}

void MyIoUringService::interrupt() { CHECK(false) << "Not implemented"; }

void MyIoUringService::shutdown() {}

void MyIoUringService::startOp(IoUringOperationBase& op) {
    auto& sqe = [&]() -> decltype(auto) {
        auto res = ring_->getSqe();
        CHECK(res.has_value()) << "Error in sqe acquisition";

        return std::move(res).value().get();
    }();

    op.prepare(sqe);
    ::io_uring_sqe_set_data(&sqe, &op);

    // For the sake of example we would do submit on every operation
    // See the asio sources if you interested how it's implemented in there
    const auto submitted = [&] {
        const auto res = ring_->submit();
        if (!res.has_value()) {
            const auto ec = res.get_unexpected();
            CHECK(false) << "Couldn't submit operation: "
                         << ec.value().message() << " (" << ec.value().value()
                         << ")";
        }

        return res.value();
    }();

    CHECK(submitted == 1) << "Unexpected submit count";

    sched_->get().work_started();
}

void MyIoUringService::construct(MyIoUringService::implementation_type&
                                 /* impl */) {
    // CHECK(false) << "Not implemented";
}

void MyIoUringService::destroy(MyIoUringService::implementation_type&
                               /* impl */) {
    // CHECK(false) << "Not implemented";
}

template <typename ExecutionContext>
MyIoObject::MyIoObject(ExecutionContext& ctx) : impl_(0, 0, ctx) {}

MyIoUringService& MyIoContext::initRingSvc(Ring ring) {
    auto svc = scoped_ptr{new MyIoUringService{*this, std::move(ring)}};
    add_service(*this, svc.get());
    return *svc.release();
}

scheduler_task* MyIoContext::getTask(execution_context& svc) {
    return &static_cast<MyIoContext&>(svc).ringSvc_;
}

scheduler& MyIoContext::initSched(int concurrencyHint) {
    auto sched =
        scoped_ptr{new scheduler{*this, concurrencyHint, false, getTask}};
    add_service(*this, sched.get());
    return *sched.release();
}

MyIoContext::MyIoContext(Ring ring, int concurrencyHint)
    : sched_(initSched(concurrencyHint)),
      ringSvc_(initRingSvc(std::move(ring))) {
    sched_.init_task();
}

expected<size_t, std::error_code> MyIoContext::run() {
    auto ec = std::error_code{};
    const auto res = sched_.run(ec);
    if (ec) {
        return make_unexpected(ec);
    }

    return res;
}

}  // namespace

int main(int /* argc */, char** argv) try {
    google::InitGoogleLogging(argv[0]);

    auto params = io_uring_params{};
    auto ring = Ring{32, params};

    auto ctx = MyIoContext{std::move(ring)};
    auto obj = MyIoObject{ctx};

    const auto fd = ::open("/dev/zero", O_RDONLY);
    if (fd < 0) {
        std::cerr << "::open: " << errno << "\n";
        return EXIT_FAILURE;
    }

    auto arr = std::array<std::byte, 4096>{};
    auto buf = asio::mutable_buffer{arr.data(), arr.size()};

    std::fill(arr.begin(), arr.end(), std::byte{0xFF});

    auto f = obj.asyncReadAt(fd, 0, buf, asio::use_future);

    ctx.run();

    const auto res = std::move(f).get();
    CHECK_EQ(res, arr.size()) << "Invalid read: " << res;

    CHECK(std::all_of(arr.begin(), arr.end(),
                      [](std::byte b) { return b == std::byte{0x00}; }));

} catch (const std::system_error& ex) {
    std::cerr << "Caught system exception: " << ex.what() << "\n";
    std::cerr << ex.code().message() << "\n";
    return ex.code().value();

} catch (const std::exception& ex) {
    std::cerr << "Caught exception: " << ex.what() << "\n";
    return EXIT_FAILURE;
}
