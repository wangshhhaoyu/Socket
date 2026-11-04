/*
 * thread_compat.h
 * ---------------------------------------------------------------------------
 * 轻量线程启动封装。
 *
 * 采用标准 C++11 <thread>, 避免直接使用 Win32 的 _beginthreadex 样板代码;
 * 仅封装"启动一个后台线程"这一最小能力, 不引入线程池等复杂框架。
 * ---------------------------------------------------------------------------
 */
#ifndef CHAT_THREAD_COMPAT_H
#define CHAT_THREAD_COMPAT_H

#include <thread>
#include <utility>

namespace chat {

/*
 * 启动一个后台线程, 线程体由 shared_ptr 持有参数, 保证参数对象生命周期安全。
 * 返回的 std::thread 对象被 detach, 进程结束时线程自然结束。
 */
template <typename Fn, typename... Args>
inline void thread_start(Fn &&fn, Args &&...args) {
    std::thread t(std::forward<Fn>(fn), std::forward<Args>(args)...);
    t.detach();
}

} // namespace chat

#endif /* CHAT_THREAD_COMPAT_H */
