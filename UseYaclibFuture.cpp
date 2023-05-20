#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <iostream>

#include "asio.hpp"
#include "yaclib/async/contract.hpp"
#include "yaclib/fwd.hpp"

namespace {

class UseYaclibFuture {};
constexpr UseYaclibFuture useYaclibFuture{};

struct LikeErrorCode : asio::error_code {
    using asio::error_code::error_code;

    LikeErrorCode(yaclib::StopTag /*tag*/)
        : std::error_code{std::make_error_code(std::errc::operation_canceled)} {
    }

    LikeErrorCode(const asio::error_code &ec)
        : asio::error_code(ec.value(), ec.category()) {}

    LikeErrorCode(LikeErrorCode &&) noexcept = default;
    LikeErrorCode(const LikeErrorCode &) noexcept = default;
    LikeErrorCode &operator=(LikeErrorCode &&) noexcept = default;
    LikeErrorCode &operator=(const LikeErrorCode &) noexcept = default;
};

template <typename T>
class ContractFactory {
public:
    using ValueType = T;
    using ErrorType = LikeErrorCode;

    using FutureType = yaclib::Future<ValueType, ErrorType>;
    using PromiseType = yaclib::Promise<ValueType, ErrorType>;

    FutureType getFuture() noexcept { return std::move(future_); }

protected:
    void createContract() {
        auto [f, p] = yaclib::MakeContract<ValueType, ErrorType>();
        future_ = std::move(f);
        promise_ = std::move(p);
    }

    FutureType future_{};
    PromiseType promise_{};
};

class FutureHandler0 : public ContractFactory<void> {
public:
    void operator()() { std::move(this->promise_).Set(); }
};

class FutureHandlerError0 : public ContractFactory<void> {
public:
    void operator()(const asio::error_code &ec) {
        if (ec)
            std::move(this->promise_).Set(LikeErrorCode{ec});
        else
            std::move(this->promise_).Set();
    }
};

class FutureHandlerException0 : public ContractFactory<void> {
public:
    void operator()(const std::exception_ptr &ex) {
        if (ex)
            std::move(this->promise_).Set(ex);
        else
            std::move(this->promise_).Set();
    }
};

template <typename T>
class FutureHandler1 : public ContractFactory<T> {
public:
    template <typename Arg>
    void operator()(Arg &&arg) {
        std::move(this->promise_).Set(std::forward<Arg>(arg));
    }
};

template <typename T>
class FutureHandlerError1 : public ContractFactory<T> {
public:
    template <typename Arg>
    void operator()(const asio::error_code &ec, Arg &&arg) {
        if (ec)
            std::move(this->promise_).Set(LikeErrorCode{ec});
        else
            std::move(this->promise_).Set(std::forward<Arg>(arg));
    }
};

template <typename T>
class FutureHandlerException1 : public ContractFactory<T> {
public:
    template <typename Arg>
    void operator()(const std::exception_ptr &ex, Arg &&arg) {
        if (ex)
            std::move(this->promise_).Set(ex);
        else
            std::move(this->promise_).Set(std::forward<Arg>(arg));
    }
};

template <typename T>
class FutureHandlerN : public ContractFactory<T> {
public:
    template <typename... Args>
    void operator()(Args &&...args) {
        std::move(this->promise_).Set(std::forward<Args>(args)...);
    }
};

template <typename T>
class FutureHandlerErrorN : public ContractFactory<T> {
public:
    template <typename... Args>
    void operator()(const asio::error_code &ec, Args &&...args) {
        if (ec)
            std::move(this->promise_).Set(LikeErrorCode{ec});
        else
            std::move(this->promise_).Set(std::forward_as_tuple<Args>(args)...);
    }
};

template <typename T>
class FutureHandlerExceptionN : public ContractFactory<T> {
public:
    template <typename... Args>
    void operator()(const std::exception_ptr &ex, Args &&...args) {
        if (ex)
            std::move(this->promise_).Set(ex);
        else
            std::move(this->promise_).Set(std::forward_as_tuple<Args>(args)...);
    }
};

template <typename>
class FutureHandlerSelector;

template <>
class FutureHandlerSelector<void()> : public FutureHandler0 {};

template <>
class FutureHandlerSelector<void(asio::error_code)>
    : public FutureHandlerError0 {};

template <>
class FutureHandlerSelector<void(std::exception_ptr)>
    : public FutureHandlerException0 {};

template <typename Arg>
class FutureHandlerSelector<void(Arg)> : public FutureHandler1<Arg> {};

template <typename Arg>
class FutureHandlerSelector<void(asio::error_code, Arg)>
    : public FutureHandlerError1<Arg> {};

template <typename Arg>
class FutureHandlerSelector<void(std::exception_ptr, Arg)>
    : public FutureHandlerException1<Arg> {};

template <typename... Arg>
class FutureHandlerSelector<void(Arg...)>
    : public FutureHandlerN<std::tuple<Arg...>> {};

template <typename... Arg>
class FutureHandlerSelector<void(asio::error_code, Arg...)>
    : public FutureHandlerErrorN<std::tuple<Arg...>> {};

template <typename... Arg>
class FutureHandlerSelector<void(std::exception_ptr, Arg...)>
    : public FutureHandlerExceptionN<std::tuple<Arg...>> {};

template <typename Signature>
class FutureHandler : public FutureHandlerSelector<Signature> {
public:
    explicit FutureHandler(UseYaclibFuture) { this->createContract(); }
};

template <typename Signature>
class FutureHandlerAsyncResult {
public:
    using completion_handler_type = FutureHandler<Signature>;
    using return_type = typename completion_handler_type::FutureType;

    explicit FutureHandlerAsyncResult(completion_handler_type &handler)
        : return_(handler.getFuture()) {}

    return_type get() noexcept { return std::move(return_); }

private:
    return_type return_;
};

}  // namespace

namespace asio {

template <typename Result, typename... Args>
class async_result<UseYaclibFuture, Result(Args...)>
    : public FutureHandlerAsyncResult<Result(Args...)> {
public:
    using BaseType = FutureHandlerAsyncResult<Result(Args...)>;
    using BaseType::BaseType;
};

}  // namespace asio

int main() try {
    auto ctx = asio::io_context{};
    auto fd = ::open("/dev/zero", O_RDWR);
    if (fd < 0) {
        std::cerr << "::open /dev/zero: " << std::strerror(errno) << "\n";
        return EXIT_FAILURE;
    }

    auto file = asio::random_access_file{ctx, fd};
    auto timer = asio::steady_timer{ctx, std::chrono::seconds{5}};
    auto arr = std::array<std::byte, 4096>{};
    std::fill(arr.begin(), arr.end(), std::byte{0xFF});

    auto buf = asio::mutable_buffer{arr.data(), arr.size()};

    // clang-format off
    auto f = file.async_read_some_at(0, buf, useYaclibFuture)
        .ThenInline([&](size_t res) {
            if (res != arr.size()) {
                std::cerr << "Read invalid count: " << res << "\n";
                throw std::runtime_error{"Error #1"};
            }

            if (!std::all_of(arr.begin(), arr.end(),
                [](auto b) { return b == std::byte{0x00}; })) {
                std::cerr << "Must be filled with zeroes\n";
                throw std::runtime_error{"Error #2"};
            }

            std::fill(arr.begin(), arr.end(), std::byte{0xFF});
            return file.async_write_some_at(0, buf, useYaclibFuture);
        }).ThenInline([&](size_t res) {
            if (res != arr.size()) {
                std::cerr << "Written invalid count: " << res << "\n";
                throw std::runtime_error{"Error #3"};
            }

            return timer.async_wait(useYaclibFuture);
        }).ThenInline([&] {
            return file.async_read_some_at(0, buf, useYaclibFuture);
        });
    // clang-format on

    ctx.run();

    auto res = std::move(f).Get();
    switch (res.State()) {
        case yaclib::ResultState::Value: {
            std::cout << "Got value\n";
            auto v = std::move(res).Value();
            std::cout << "Value: " << v << "\n";

            if (v != arr.size()) {
                std::cerr << "Invalid read bytes: " << v << "\n";
                return EXIT_FAILURE;
            }

            // clang-format off
            if (!std::all_of(arr.begin(), arr.end(),
                [](auto b) { return b == std::byte{0x00}; })) {
                std::cerr << "Must be filled with zeroes\n";
                return EXIT_FAILURE;
            }
            // clang-format on

            return EXIT_SUCCESS;
        }
        case yaclib::ResultState::Empty:
            std::cerr << "Got empty future\n";
            return EXIT_FAILURE;
        case yaclib::ResultState::Error: {
            std::cerr << "Got Error\n";
            auto ec = std::move(res).Error();
            std::cerr << "Message: " << ec.message() << "\n";
            return ec.value();
        }
        case yaclib::ResultState::Exception:
            std::cerr << "Got exception\n";
            std::rethrow_exception(std::move(res).Exception());
        default:
            return EXIT_FAILURE;
    }

} catch (const std::system_error &ex) {
    std::cerr << "Caught system exception: " << ex.what() << "\n";
    std::cerr << "Message: " << ex.code().message() << "\n";
    return ex.code().value();
} catch (const std::exception &ex) {
    std::cerr << "Caught exceptoin:  " << ex.what() << "\n";
    return EXIT_FAILURE;
}
