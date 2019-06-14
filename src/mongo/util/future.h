/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

// Keeping this first to ensure it compiles by itself
#include "mongo/util/future_impl.h"

#include <boost/intrusive_ptr.hpp>
#include <type_traits>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
/**
 * Future<T> is logically a possibly-deferred StatusWith<T> (or Status when T is void).
 *
 * As is usual for rvalue-qualified methods, you may call at most one of them on a given Future.
 *
 * A future may be passed between threads, but only one thread may use it at a time.
 *
 * TODO decide if destroying a Future before extracting the result should cancel work or should
 * cancellation be explicit. For now avoid unnecessarily throwing away active Futures since the
 * behavior may change. End all Future chains with either a blocking call to get()/getNoThrow() or a
 * non-blocking call to getAsync().
 *
 * Future<void> should be the same as the generic Future<T> with the following exceptions:
 *   - Anything mentioning StatusWith<T> will use Status instead.
 *   - Anything returning references to T will just return void since there are no void references.
 *   - Anything taking a T argument will receive no arguments.
 */
template <typename T>
class MONGO_WARN_UNUSED_RESULT_CLASS Future {
    using Impl = future_details::FutureImpl<T>;
    using T_unless_void = std::conditional_t<std::is_void_v<T>, future_details::FakeVoid, T>;

public:
    static_assert(!std::is_same<T, Status>::value,
                  "Future<Status> is banned. Use Future<void> instead.");
    static_assert(!isStatusWith<T>, "Future<StatusWith<T>> is banned. Just use Future<T> instead.");
    static_assert(!future_details::isFutureLike<T>,
                  "Future of Future types is banned. Just use Future<T> instead.");
    static_assert(!std::is_reference<T>::value, "Future<T&> is banned.");
    static_assert(!std::is_const<T>::value, "Future<const T> is banned.");
    static_assert(!std::is_array<T>::value, "Future<T[]> is banned.");

    using value_type = T;

    /**
     * For non-void T: Constructs a Future in a moved-from state that can only be assigned to
     *                 or destroyed.
     *
     * For void T: Constructs a ready future for parity with Future<T>(T)
     */
    Future() = default;

    Future& operator=(Future&&) = default;
    Future(Future&&) = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    /**
     * For non-void T: This must be passed a not-OK Status.
     *
     * For void T: This behaves like the StatusWith constructor and accepts any Status.
     */
    /* implicit */ Future(Status status) : Future(Impl::makeReady(std::move(status))) {}

    // These should not be used with T=void.
    /* implicit */ Future(T_unless_void val) : Future(Impl::makeReady(std::move(val))) {
        static_assert(!std::is_void_v<T>);
    }
    /* implicit */ Future(StatusWith<T_unless_void> sw) : Future(Impl::makeReady(std::move(sw))) {
        static_assert(!std::is_void_v<T>);
    }

    /**
     * Make a ready Future<T> from a value for cases where you don't need to wait asynchronously.
     *
     * Calling this is faster than getting a Future out of a Promise, and is effectively free. It is
     * fast enough that you never need to avoid returning a Future from an API, even if the result
     * is ready 99.99% of the time.
     *
     * As an example, if you are handing out results from a batch, you can use this when for each
     * result while you have a batch, then use a Promise to return a not-ready Future when you need
     * to get another batch.
     */
    static Future<T> makeReady(T_unless_void val) {  // TODO emplace?
        return mongo::Future(Impl::makeReady(std::move(val)));
    }

    static Future<T> makeReady(Status status) {
        return mongo::Future(Impl::makeReady(std::move(status)));
    }

    static Future<T> makeReady(StatusWith<T_unless_void> val) {
        return mongo::Future(Impl::makeReady(std::move(val)));
    }

    template <typename U = T, typename = std::enable_if_t<std::is_void_v<U>>>
    static Future<void> makeReady() {  // TODO emplace?
        return mongo::Future(Impl::makeReady());
    }

    /**
     * Convert this Future to a SharedSemiFuture.
     */
    SharedSemiFuture<T> share() && noexcept {
        return std::move(_impl).share();
    }

    /**
     * If this returns true, get() is guaranteed not to block and callbacks will be immediately
     * invoked. You can't assume anything if this returns false since it may be completed
     * immediately after checking (unless you have independent knowledge that this Future can't
     * complete in the background).
     *
     * Callers must still call get() or similar, even on Future<void>, to ensure that they are
     * correctly sequenced with the completing task, and to be informed about whether the Promise
     * completed successfully.
     *
     * This is generally only useful as an optimization to avoid prep work, such as setting up
     * timeouts, that is unnecessary if the Future is ready already.
     */
    bool isReady() const {
        return _impl.isReady();
    }

    /**
     * Returns when the future isReady().
     *
     * Throws if the interruptible passed is interrupted (explicitly or via deadline).
     */
    void wait(Interruptible* interruptible = Interruptible::notInterruptible()) const {
        return _impl.wait(interruptible);
    }

    /**
     * Returns Status::OK() when the future isReady().
     *
     * Returns a non-okay status if the interruptible is interrupted.
     */
    Status waitNoThrow(Interruptible* interruptible = Interruptible::notInterruptible()) const
        noexcept {
        return _impl.waitNoThrow(interruptible);
    }

    /**
     * Gets the value out of this Future, blocking until it is ready.
     *
     * get() methods throw on error, while getNoThrow() returns a !OK status.
     *
     * These methods can be called multiple times, except for the rvalue overloads.
     *
     * Note: It is impossible to differentiate interruptible interruption from an error propagating
     * down the future chain with these methods.  If you need to distinguish the two cases, call
     * wait() first.
     */
    T get(Interruptible* interruptible = Interruptible::notInterruptible()) && {
        return std::move(_impl).get(interruptible);
    }

    future_details::AddRefUnlessVoid<T> get(
        Interruptible* interruptible = Interruptible::notInterruptible()) & {
        return _impl.get(interruptible);
    }
    future_details::AddRefUnlessVoid<const T> get(
        Interruptible* interruptible = Interruptible::notInterruptible()) const& {
        return _impl.get(interruptible);
    }
    StatusOrStatusWith<T> getNoThrow(
        Interruptible* interruptible = Interruptible::notInterruptible()) &&
        noexcept {
        return std::move(_impl).getNoThrow(interruptible);
    }
    StatusOrStatusWith<T> getNoThrow(
        Interruptible* interruptible = Interruptible::notInterruptible()) const& noexcept {
        return _impl.getNoThrow(interruptible);
    }

    /**
     * This ends the Future continuation chain by calling a callback on completion. Use this to
     * escape back into a callback-based API.
     *
     * For now, the callback must not fail, since there is nowhere to propagate the error to.
     * TODO decide how to handle func throwing.
     */
    template <typename Func>
        void getAsync(Func&& func) && noexcept {
        std::move(_impl).getAsync(std::forward<Func>(func));
    }

    //
    // The remaining methods are all continuation based and take a callback and return a Future.
    // Each method has a comment indicating the supported signatures for that callback, and a
    // description of when the callback is invoked and how the impacts the returned Future. It may
    // be helpful to think of Future continuation chains as a pipeline of stages that take input
    // from earlier stages and produce output for later stages.
    //
    // Be aware that the callback may be invoked inline at the call-site or at the producer when
    // setting the value. Therefore, you should avoid doing blocking work inside of a callback.
    // Additionally, avoid acquiring any locks or mutexes that the caller already holds, otherwise
    // you risk a deadlock. If either of these concerns apply to your callback, it should schedule
    // itself on an executor, rather than doing work in the callback.
    // TODO make this easier to do by having executor APIs return Futures.
    //
    // Error handling in callbacks: all exceptions thrown propagate to the returned Future
    // automatically. Callbacks that return Status or StatusWith<T> behave as-if they were wrapped
    // in something that called uassertStatusOK() on the return value. There is no way to
    // distinguish between a function throwing or returning a !OK status.
    //
    // Callbacks that return Future<T> are automatically unwrapped and connected to the returned
    // Future<T>, rather than producing a Future<Future<T>>.
    //

    /**
     * Callbacks passed to then() are only called if the input Future completes successfully.
     * Otherwise the error propagates automatically, bypassing the callback.
     */
    template <typename Func>
        auto then(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).then(std::forward<Func>(func)));
    }

    /**
     * Callbacks passed to onCompletion() are called if the input Future completes with or without
     * an error.
     *
     * The callback can either produce a replacement value (which must be a T), return a replacement
     * Future<T> (such as by retrying), or return/throw a replacement error.
     */
    template <typename Func>
        auto onCompletion(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).onCompletion(std::forward<Func>(func)));
    }

    /**
     * Callbacks passed to onError() are only called if the input Future completes with an error.
     * Otherwise, the successful result propagates automatically, bypassing the callback.
     *
     * The callback can either produce a replacement value (which must be a T), return a replacement
     * Future<T> (such as by retrying), or return/throw a replacement error.
     *
     * Note that this will only catch errors produced by earlier stages; it is not registering a
     * general error handler for the entire chain.
     */
    template <typename Func>
        Future<T> onError(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).onError(std::forward<Func>(func)));
    }

    /**
     * Same as the other two onErrors but only calls the callback if the code matches the template
     * parameter. Otherwise lets the error propagate unchanged.
     */
    template <ErrorCodes::Error code, typename Func>
        Future<T> onError(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).template onError<code>(std::forward<Func>(func)));
    }

    /**
     * Similar to the first two onErrors, but only calls the callback if the category matches
     * the template parameter. Otherwise lets the error propagate unchanged.
     */
    template <ErrorCategory category, typename Func>
        Future<T> onErrorCategory(Func&& func) && noexcept {
        return mongo::Future(
            std::move(_impl).template onErrorCategory<category>(std::forward<Func>(func)));
    }

    //
    // The tap/tapError/tapAll family of functions take callbacks to observe the flow through a
    // future chain without affecting the propagating result, except possibly if they throw. If the
    // naming seems odd, you can think of it like a "wire tap" in that it allows you to observe a
    // conversation between two parties (the promise-producer and future-consumer) without adding
    // messages of your own. This is why all callbacks are required to return void.
    //
    // TODO decide what to do if callback throws:
    //  - transition the future chain to failure
    //  - ignore
    //  - fatal (current impl)
    //

    /**
     * Callback is called if the input completes successfully.
     *
     * This can be used to inform some outside system of the result.
     */
    template <typename Func>
        Future<T> tap(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).tap(std::forward<Func>(func)));
    }

    /**
     * Callback is called if the input completes with an error.
     *
     * This can be used to log.
     */
    template <typename Func>
        Future<T> tapError(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).tapError(std::forward<Func>(func)));
    }

    /**
     * Callback is called when the input completes, regardless of success or failure.
     *
     * This can be used for cleanup. Some other libraries name the equivalent method finally to
     * match the common semantic from other languages.
     *
     * Warning: If func takes a StatusWith<T>, it requires copying the value on success. If that is
     * too expensive, it can be avoided by either providing a function object with separate
     * Status/const T& overloads, or by using a generic lambda if you don't need to consult the
     * value for your cleanup.
     */
    template <typename Func>
        Future<T> tapAll(Func&& func) && noexcept {
        return mongo::Future(std::move(_impl).tapAll(std::forward<Func>(func)));
    }

    /**
     * Ignores the return value of a future, transforming it down into a Future<void>.
     *
     * This only ignores values, not errors.  Those remain propogated until an onError handler.
     *
     * Equivalent to then([](auto&&){});
     */
    Future<void> ignoreValue() && noexcept {
        return mongo::Future(std::move(_impl).ignoreValue());
    }

private:
    template <typename>
    friend class Future;
    template <typename>
    friend class future_details::FutureImpl;
    friend class Promise<T>;

    explicit Future(future_details::SharedStateHolder<T_unless_void>&& impl)
        : _impl(std::move(impl)) {}

    explicit Future(Impl&& impl) : _impl(std::move(impl)) {}
    operator Impl &&() && {
        return std::move(_impl);
    }

    template <typename U>
    auto propagateResultTo(U&& arg) && {
        return std::move(_impl).propagateResultTo(std::forward<U>(arg));
    }

    Impl _impl;
};

// Deduction Guides
template <typename T>
Future(T)->Future<T>;
template <typename T>
Future(StatusWith<T>)->Future<T>;

/**
 * This class represents the producer side of a Future.
 *
 * This is a single-shot class: you may either set a value or error at most once. If no value or
 * error has been set at the time this Promise is destroyed, a error will be set with
 * ErrorCode::BrokenPromise. This should generally be considered a programmer error, and should not
 * be relied upon. We may make it debug-fatal in the future.
 *
 * Only one thread can use a given Promise at a time, but another thread may be using the associated
 * Future object.
 *
 * If the result is ready when producing the Future, it is more efficient to use
 * makeReadyFutureWith() or Future<T>::makeReady() than to use a Promise<T>.
 *
 * A default constructed `Promise` is in a null state.  Null `Promises` can only be assigned over
 * and destroyed. It is a programmer error to call any methods on a null `Promise`.  Any methods
 * that complete a `Promise` leave it in the null state.
 */
template <typename T>
class Promise {
    using SharedStateT = future_details::SharedState<T>;

public:
    using value_type = T;

    /**
     * Creates a null `Promise`.
     */
    Promise() = default;

    ~Promise() {
        breakPromiseIfNeeded();
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;


    /**
     * Breaks this `Promise`, if not fulfilled and not in a null state.
     */
    Promise& operator=(Promise&& p) noexcept {
        breakPromiseIfNeeded();
        _sharedState = std::move(p._sharedState);
        return *this;
    }

    Promise(Promise&&) = default;

    /**
     * Sets a value or error into this Promise by calling func, which must take no arguments and
     * return one of T, StatusWith<T> (or Status when T is void), or Future<T>. All errors, whether
     * returned or thrown, will be correctly propagated.
     *
     * If the function returns a Future<T>, this Promise's Future will complete when the returned
     * Future<T> completes, as-if it was passed to Promise::setFrom().
     *
     * If any work is needed to produce the result, prefer doing something like:
     *     promise.setWith([&]{ return makeResult(); });
     * over code like:
     *     promise.emplaceValue(makeResult());
     * because this method will correctly propagate errors thrown from makeResult(), rather than
     * ErrorCodes::BrokenPromise.
     */
    template <typename Func>
    void setWith(Func&& func) noexcept {
        setFrom(Future<void>::makeReady().then(std::forward<Func>(func)));
    }

    /**
     * Sets the value into this Promise when the passed-in Future completes, which may have already
     * happened. If it hasn't, it is still safe to destroy this Promise since it is no longer
     * involved.
     */
    void setFrom(Future<T>&& future) noexcept {
        setImpl([&](boost::intrusive_ptr<future_details::SharedState<T>>&& sharedState) {
            std::move(future).propagateResultTo(sharedState.get());
        });
    }

    template <typename... Args>
    void emplaceValue(Args&&... args) noexcept {
        setImpl([&](boost::intrusive_ptr<SharedStateT>&& sharedState) {
            sharedState->emplaceValue(std::forward<Args>(args)...);
        });
    }

    void setError(Status status) noexcept {
        invariant(!status.isOK());
        setImpl([&](boost::intrusive_ptr<SharedStateT>&& sharedState) {
            sharedState->setError(std::move(status));
        });
    }

    // TODO rename to not XXXWith and handle void
    void setFromStatusWith(StatusWith<T> sw) noexcept {
        setImpl([&](boost::intrusive_ptr<SharedStateT>&& sharedState) {
            sharedState->setFromStatusWith(std::move(sw));
        });
    }

    static auto makePromiseFutureImpl() {
        struct PromiseAndFuture {
            Promise<T> promise = Promise(make_intrusive<SharedStateT>());
            Future<T> future = promise.getFuture();
        };
        return PromiseAndFuture();
    }

private:
    explicit Promise(boost::intrusive_ptr<SharedStateT>&& sharedState)
        : _sharedState(std::move(sharedState)) {}

    // This is not public because we found it frequently was involved in races.  The
    // `makePromiseFuture<T>` API avoids those races entirely.
    Future<T> getFuture() noexcept {
        using namespace future_details;
        _sharedState->threadUnsafeIncRefCountTo(2);
        return Future<T>(SharedStateHolder<VoidToFakeVoid<T>>(
            boost::intrusive_ptr<SharedState<T>>(_sharedState.get(), /*add ref*/ false)));
    }

    friend class Future<void>;

    template <typename Func>
    void setImpl(Func&& doSet) noexcept {
        invariant(_sharedState);
        // We keep `sharedState` as a stack local, to preserve ownership of the resource,
        // in case the code in `doSet` unblocks a thread which winds up causing
        // `~Promise` to be invoked.
        auto sharedState = std::move(_sharedState);
        doSet(std::move(sharedState));
        // Note: `this` is potentially dead, at this point.
    }

    // The current promise will be broken, if not already fulfilled.
    void breakPromiseIfNeeded() {
        if (MONGO_unlikely(_sharedState)) {
            _sharedState->setError({ErrorCodes::BrokenPromise, "broken promise"});
        }
    }

    boost::intrusive_ptr<SharedStateT> _sharedState;
};

/**
 * SharedSemiFuture<T> is logically a possibly-deferred StatusWith<T> (or Status when T is void).
 *
 * All methods that are present do the same as on a Future<T> so see it for documentation.
 *
 * Unlike Future<T> it only supports blocking operation, not chained continuations. This is intended
 * to protect the promise-completer's execution context from needing to perform arbitrary
 * operations requested by other subsystem's continuations.
 * TODO Support continuation chaining when supplied with an executor to run them on.
 *
 * A SharedSemiFuture may be passed between threads, but only one thread may use it at a time.
 */
template <typename T>
class MONGO_WARN_UNUSED_RESULT_CLASS SharedSemiFuture {
    using Impl = future_details::SharedStateHolder<T>;
    using T_unless_void = std::conditional_t<std::is_void_v<T>, future_details::FakeVoid, T>;

public:
    static_assert(!std::is_same<T, Status>::value,
                  "SharedSemiFuture<Status> is banned. Use SharedSemiFuture<void> instead.");
    static_assert(
        !isStatusWith<T>,
        "SharedSemiFuture<StatusWith<T>> is banned. Just use SharedSemiFuture<T> instead.");
    static_assert(
        !future_details::isFutureLike<T>,
        "SharedSemiFuture of Future types is banned. Just use SharedSemiFuture<T> instead.");
    static_assert(!std::is_reference<T>::value, "SharedSemiFuture<T&> is banned.");
    static_assert(!std::is_const<T>::value, "SharedSemiFuture<const T> is banned.");
    static_assert(!std::is_array<T>::value, "SharedSemiFuture<T[]> is banned.");

    using value_type = T;

    SharedSemiFuture() = default;

    /*implicit*/ SharedSemiFuture(const Future<T>& fut) = delete;
    /*implicit*/ SharedSemiFuture(Future<T>&& fut) : SharedSemiFuture(std::move(fut).share()) {}
    /*implicit*/ SharedSemiFuture(Status error) : _shared(Impl::makeReady(std::move(error))) {}


    // These should not be used with T=void.
    /*implicit*/ SharedSemiFuture(T_unless_void val) : _shared(Impl::makeReady(std::move(val))) {
        static_assert(!std::is_void_v<T>);
    }
    /*implicit*/ SharedSemiFuture(StatusWith<T_unless_void> sw)
        : _shared(Impl::makeReady(std::move(sw))) {
        static_assert(!std::is_void_v<T>);
    }

    bool isReady() const {
        return _shared.isReady();
    }

    void wait(Interruptible* interruptible = Interruptible::notInterruptible()) const {
        _shared.wait(interruptible);
    }

    Status waitNoThrow(Interruptible* interruptible = Interruptible::notInterruptible()) const
        noexcept {
        return _shared.waitNoThrow(interruptible);
    }

    future_details::AddRefUnlessVoid<const T> get(
        Interruptible* interruptible = Interruptible::notInterruptible()) const& {
        return _shared.get(interruptible);
    }

    StatusOrStatusWith<T> getNoThrow(
        Interruptible* interruptible = Interruptible::notInterruptible()) const& noexcept {
        return _shared.getNoThrow(interruptible);
    }

private:
    template <typename>
    friend class SharedPromise;
    template <typename>
    friend class future_details::FutureImpl;
    friend class SharedSemiFuture<void>;

    explicit SharedSemiFuture(boost::intrusive_ptr<future_details::SharedState<T>> ptr)
        : _shared(std::move(ptr)) {}
    explicit SharedSemiFuture(future_details::SharedStateHolder<T>&& holder)
        : _shared(std::move(holder)) {}

    future_details::SharedStateHolder<T> _shared;
};

// Deduction Guides
template <typename T>
SharedSemiFuture(T)->SharedSemiFuture<T>;
template <typename T>
SharedSemiFuture(StatusWith<T>)->SharedSemiFuture<T>;

/**
 * This class represents the producer of SharedSemiFutures.
 *
 * This is a single-shot class: you may either set a value or error at most once. However you may
 * extract as many futures as you want and they will all be completed at the same time. Any number
 * of threads can extract a future at the same time. It is also safe to extract a future
 * concurrently with completing the promise. If you extract a future after the promise has been
 * completed, a ready future will be returned. You must still ensure that all calls to getFuture()
 * complete prior to destroying the Promise.
 *
 * If no value or error has been set at the time this Promise is destroyed, an error will be set
 * with ErrorCode::BrokenPromise. This should generally be considered a programmer error, and should
 * not be relied upon. We may make it debug-fatal in the future.
 *
 * Unless otherwise specified, all methods behave the same as on Promise<T>.
 */
template <typename T>
class SharedPromise {
public:
    using value_type = T;

    /**
     * Creates a `SharedPromise` ready for use.
     */
    SharedPromise() = default;

    ~SharedPromise() {
        if (MONGO_unlikely(!haveCompleted())) {
            _sharedState->setError({ErrorCodes::BrokenPromise, "broken promise"});
        }
    }

    SharedPromise(const SharedPromise&) = delete;
    SharedPromise(SharedPromise&&) = delete;
    SharedPromise& operator=(const SharedPromise&) = delete;
    SharedPromise& operator=(SharedPromise&& p) noexcept = delete;

    /**
     * Returns a future associated with this promise. All returned futures will be completed when
     * the promise is completed.
     */
    SharedSemiFuture<T> getFuture() const {
        return SharedSemiFuture<T>(_sharedState);
    }

    template <typename Func>
    void setWith(Func&& func) noexcept {
        invariant(!haveCompleted());
        setFrom(Future<void>::makeReady().then(std::forward<Func>(func)));
    }

    void setFrom(Future<T>&& future) noexcept {
        invariant(!haveCompleted());
        std::move(future).propagateResultTo(_sharedState.get());
    }

    template <typename... Args>
    void emplaceValue(Args&&... args) noexcept {
        invariant(!haveCompleted());
        _sharedState->emplaceValue(std::forward<Args>(args)...);
    }

    void setError(Status status) noexcept {
        invariant(!status.isOK());
        invariant(!haveCompleted());
        _sharedState->setError(std::move(status));
    }

    // TODO rename to not XXXWith and handle void
    void setFromStatusWith(StatusWith<T> sw) noexcept {
        invariant(!haveCompleted());
        _sharedState->setFromStatusWith(std::move(sw));
    }

private:
    friend class Future<void>;

    bool haveCompleted() const noexcept {
        // This can be relaxed because it is only called from the Promise thread which is also the
        // only thread that will transition this from returning false to true. Additionally it isn't
        // used to establish synchronization with any other thread.
        return _sharedState->state.load(std::memory_order_relaxed) ==
            future_details::SSBState::kFinished;
    }

    const boost::intrusive_ptr<future_details::SharedState<T>> _sharedState =
        make_intrusive<future_details::SharedState<T>>();
};

/**
 * Makes a ready Future with the return value of a nullary function. This has the same semantics as
 * Promise::setWith, and has the same reasons to prefer it over Future<T>::makeReady(). Also, it
 * deduces the T, so it is easier to use.
 */
template <typename Func>
auto makeReadyFutureWith(Func&& func) {
    return Future<void>::makeReady().then(std::forward<Func>(func));
}

/**
 * Returns a bound Promise and Future in a struct with friendly names (promise and future) that also
 * works well with C++17 structured bindings.
 */
template <typename T>
inline auto makePromiseFuture() {
    return Promise<T>::makePromiseFutureImpl();
}

/**
 * This metafunction allows APIs that take callbacks and return Future to avoid doing their own type
 * calculus. This results in the base value_type that would result from passing Func to a
 * Future<T>::then(), with the same normalizing of T/StatusWith<T>/Future<T> returns. This is
 * primarily useful for implementations of executors rather than their users.
 *
 * This returns the unwrapped T rather than Future<T> so it will be easy to create a Promise<T>.
 *
 * Examples:
 *
 * FutureContinuationResult<std::function<void()>> == void
 * FutureContinuationResult<std::function<Status()>> == void
 * FutureContinuationResult<std::function<Future<void>()>> == void
 *
 * FutureContinuationResult<std::function<int()>> == int
 * FutureContinuationResult<std::function<StatusWith<int>()>> == int
 * FutureContinuationResult<std::function<Future<int>()>> == int
 *
 * FutureContinuationResult<std::function<int(bool)>, bool> == int
 *
 * FutureContinuationResult<std::function<int(bool)>, NotBool> SFINAE-safe substitution failure.
 */
template <typename Func, typename... Args>
using FutureContinuationResult = typename future_details::FutureContinuationResultImpl<
    std::invoke_result_t<Func, Args&&...>>::type;

//
// Implementations of methods that couldn't be defined in the class due to ordering requirements.
//

template <typename T>
    inline SharedSemiFuture<future_details::FakeVoidToVoid<T>>
    future_details::FutureImpl<T>::share() && noexcept {
    using Out = SharedSemiFuture<FakeVoidToVoid<T>>;
    if (_immediate)
        return Out(SharedStateHolder<FakeVoidToVoid<T>>::makeReady(std::move(*_immediate)));
    return Out(SharedStateHolder<FakeVoidToVoid<T>>(std::move(_shared)));
}

}  // namespace mongo
