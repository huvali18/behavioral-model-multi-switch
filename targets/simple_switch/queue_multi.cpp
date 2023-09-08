#include <algorithm>  // for std::max
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <tuple>  // for std::forward_as_tuple
#include <unordered_map>
#include <utility>  // for std::piecewise_construct
#include <vector>
#include <../../bm/bm_sim/queueing.h>

namespace bm {
    using MutexType = std::mutex;
    using LockType = std::unique_lock<MutexType>;

    template <typename T, typename FMap>
    class QueueingLogicPriRLMulti {
        using MutexType = std::mutex;
        using LockType = std::unique_lock<MutexType>;

        public:
        //! See QueueingLogic::QueueingLogicRL() for an introduction. The difference
        //! here is that each logical queue can receive several priority queues (as
        //! determined by \p nb_priorities, which is set to `2` by default). Each of
        //! these priority queues will initially be able to hold \p capacity
        //! elements. The capacity of each priority queue can be changed later by
        //! using set_capacity(size_t queue_id, size_t priority, size_t c).
        QueueingLogicPriRLMulti(size_t capacity,
                            FMap map_to_worker, size_t nb_priorities = 2)
            :   capacity(capacity),
                map_to_worker(std::move(map_to_worker)),
                nb_priorities(nb_priorities) { }

        //! If priority queue \p priority of logical queue \p queue_id is full, the
        //! function will return `0` immediately. Otherwise, \p item will be copied to
        //! the queue and the function will return `1`. If \p queue_id or \p priority
        //! are incorrect, an exception of type std::out_of_range will be thrown (same
        //! if the FMap object provided to the constructor does not behave correctly).
        int push_front(size_t queue_id, size_t priority, const T &item) {
            LockType lock(mutex);
            auto &q_info = get_queue(queue_id);
            auto &w_info = workers_info;
            auto &q_info_pri = q_info.at(priority);
            if (q_info_pri.size >= q_info_pri.capacity) return 0;
            q_info_pri.last_sent = get_next_tp(q_info_pri);
            w_info.queues[priority].emplace(
                item, queue_id, q_info_pri.last_sent, w_info.wrapping_counter++);
            q_info_pri.size++;
            q_info.size++;
            w_info.size++;
            w_info.q_not_empty.notify_one();
            return 1;
        }

        int push_front(size_t queue_id, const T &item) {
            return push_front(queue_id, 0, item);
        }

        //! Same as push_front(size_t queue_id, size_t priority, const T &item), but
        //! \p item is moved instead of copied.
        int push_front(size_t queue_id, size_t priority, T &&item) {
            LockType lock(mutex);
            auto &q_info = get_queue(queue_id);
            auto &w_info = workers_info;
            auto &q_info_pri = q_info.at(priority);
            if (q_info_pri.size >= q_info_pri.capacity) return 0;
            q_info_pri.last_sent = get_next_tp(q_info_pri);
            w_info.queues[priority].emplace(
                std::move(item),
                queue_id,
                q_info_pri.last_sent,
                w_info.wrapping_counter++);
            q_info_pri.size++;
            q_info.size++;
            w_info.size++;
            w_info.q_not_empty.notify_one();
            return 1;
        }

        int push_front(size_t queue_id, T &&item) {
            return push_front(queue_id, 0, std::move(item));
        }

        //! Retrieves an element for the worker thread indentified by \p worker_id and
        //! moves it to \p pItem. The id of the logical queue which contained this
        //! element is copied to \p queue_id and the priority value of the served
        //! queue is copied to \p priority.
        //! Elements are retrieved according to the priority queue they are in
        //! (highest priorities, i.e. lowest priority values, are served first). Once
        //! a given priority queue reaches its maximum rate, the next queue is served.
        //! If no elements are available (either the queues are empty or they have
        //! exceeded their rate already), the function will block.
        void pop_back(size_t *queue_id, size_t *priority,
                        T *pItem) {
            LockType lock(mutex);
            auto &w_info = workers_info;
            MyQ *queue = nullptr;
            size_t pri;
            while (true) {
            if (w_info.size == 0) {
                w_info.q_not_empty.wait(lock);
            } else {
                auto now = clock::now();
                auto next = clock::time_point::max();
                for (pri = 0; pri < nb_priorities; pri++) {
                auto &q = w_info.queues[pri];
                if (q.size() == 0) continue;
                if (q.top().send <= now) {
                    queue = &q;
                    break;
                }
                next = std::min(next, q.top().send);
                }
                if (queue) break;
                w_info.q_not_empty.wait_until(lock, next);
            }
            }
            *queue_id = queue->top().queue_id;
            *priority = pri;
            // TODO(antonin): improve / document this
            // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
            *pItem = std::move(const_cast<QE &>(queue->top()).e);
            queue->pop();
            auto &q_info = get_queue_or_throw(*queue_id);
            auto &q_info_pri = q_info.at(*priority);
            q_info_pri.size--;
            q_info.size--;
            w_info.size--;
        }

        //! Same as
        //! pop_back(size_t worker_id, size_t *queue_id, size_t *priority, T *pItem),
        //! but the priority of the popped element is discarded.
        void pop_back(size_t *queue_id, T *pItem) {
            size_t priority;
            return pop_back(queue_id, &priority, pItem);
        }

        //! @copydoc QueueingLogic::size
        //! The occupancies of all the priority queues for this logical queue are
        //! added.
        size_t size(size_t queue_id) const {
            LockType lock(mutex);
            auto it = queues_info.find(queue_id);
            if (it == queues_info.end()) return 0;
            auto &q_info = it->second;
            return q_info.size;
        }

        //! Get the occupancy of priority queue \p priority for logical queue with id
        //! \p queue_id.
        size_t size(size_t queue_id, size_t priority) const {
            LockType lock(mutex);
            auto it = queues_info.find(queue_id);
            if (it == queues_info.end()) return 0;
            auto &q_info = it->second;
            auto &q_info_pri = q_info.at(priority);
            return q_info_pri.size;
        }

        //! Set the capacity of all the priority queues for logical queue \p queue_id
        //! to \p c elements.
        void set_capacity(size_t queue_id, size_t c) {
            LockType lock(mutex);
            for_each_q(queue_id, SetCapacityFn(c));
        }

        //! Set the capacity of priority queue \p priority for logical queue \p
        //! queue_id to \p c elements.
        void set_capacity(size_t queue_id, size_t priority, size_t c) {
            LockType lock(mutex);
            for_one_q(queue_id, priority, SetCapacityFn(c));
        }

        //! Set the capacity of all the priority queues of all logical queues to \p c
        //! elements.
        void set_capacity_for_all(size_t c) {
            LockType lock(mutex);
            for (auto &p : queues_info) for_each_q(p.first, SetCapacityFn(c));
            capacity = c;
        }

        //! Set the maximum rate of all the priority queues for logical queue \p
        //! queue_id to \p pps. \p pps is expressed in "number of elements per
        //! second". Until this function is called, there will be no rate limit for
        //! the queue. The same behavior (no rate limit) can be achieved by calling
        //! this method with a rate of 0.
        void set_rate(size_t queue_id, uint64_t pps) {
            LockType lock(mutex);
            for_each_q(queue_id, SetRateFn(pps));
        }

        //! Same as set_rate(size_t queue_id, uint64_t pps) but only applies to the
        //! given priority queue.
        void set_rate(size_t queue_id, size_t priority, uint64_t pps) {
            LockType lock(mutex);
            for_one_q(queue_id, priority, SetRateFn(pps));
        }

        //! Set the rate of all the priority queues of all logical queues to \p pps.
        void set_rate_for_all(uint64_t pps) {
            LockType lock(mutex);
            for (auto &p : queues_info) for_each_q(p.first, SetRateFn(pps));
            queue_rate_pps = pps;
        }

        //! Deleted copy constructor
        QueueingLogicPriRLMulti(const QueueingLogicPriRLMulti &) = delete;
        //! Deleted copy assignment operator
        QueueingLogicPriRLMulti &operator =(const QueueingLogicPriRLMulti &) = delete;

        //! Deleted move constructor
        QueueingLogicPriRLMulti(QueueingLogicPriRLMulti &&) = delete;
        //! Deleted move assignment operator
        QueueingLogicPriRLMulti &&operator =(QueueingLogicPriRLMulti &&) = delete;

        private:
        using ticks = std::chrono::nanoseconds;
        // clock choice? switch to steady if observing re-ordering
        // in my Linux VM, it seems that both clocks behave the same (can sometimes
        // stop increasing for a bit but do not go backwards).
        // using clock = std::chrono::steady_clock;
        using clock = std::chrono::high_resolution_clock;

        static constexpr ticks rate_to_ticks(uint64_t pps) {
            using std::chrono::duration;
            using std::chrono::duration_cast;
            return (pps == 0) ?
                ticks(0) : duration_cast<ticks>(duration<double>(1. / pps));
        }

        struct QE {
            QE(T e, size_t queue_id, const clock::time_point &send, size_t id)
                : e(std::move(e)), queue_id(queue_id), send(send), id(id) { }

            T e;
            size_t queue_id;
            clock::time_point send;
            size_t id;
        };

        struct QEComp {
            bool operator()(const QE &lhs, const QE &rhs) const {
            return (lhs.send == rhs.send) ? lhs.id > rhs.id : lhs.send > rhs.send;
            }
        };

        using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;

        struct QueueInfoPri {
            QueueInfoPri(size_t capacity, uint64_t queue_rate_pps)
                : capacity(capacity),
                queue_rate_pps(queue_rate_pps),
                pkt_delay_ticks(rate_to_ticks(queue_rate_pps)),
                last_sent(clock::now()) { }

            size_t size{0};
            size_t capacity;
            uint64_t queue_rate_pps;
            ticks pkt_delay_ticks;
            clock::time_point last_sent;
        };

        struct QueueInfo : public std::vector<QueueInfoPri> {
            QueueInfo(size_t capacity, uint64_t queue_rate_pps, size_t nb_priorities)
                : std::vector<QueueInfoPri>(
                    nb_priorities, QueueInfoPri(capacity, queue_rate_pps)) { }

            size_t size{0};
        };

        struct WorkerInfo {
            mutable std::condition_variable q_not_empty{};
            size_t size{0};
            std::array<MyQ, 32> queues;
            size_t wrapping_counter{0};
        };

        QueueInfo &get_queue(size_t queue_id) {
            auto it = queues_info.find(queue_id);
            if (it != queues_info.end()) return it->second;
            auto p = queues_info.emplace(
                queue_id, QueueInfo(capacity, queue_rate_pps, nb_priorities));
            return p.first->second;
        }

        const QueueInfo &get_queue_or_throw(size_t queue_id) const {
            return queues_info.at(queue_id);
        }

        QueueInfo &get_queue_or_throw(size_t queue_id) {
            return queues_info.at(queue_id);
        }

        clock::time_point get_next_tp(const QueueInfoPri &q_info_pri) {
            return std::max(clock::now(),
                            q_info_pri.last_sent + q_info_pri.pkt_delay_ticks);
        }

        template <typename Function>
        Function for_each_q(size_t queue_id, Function fn) {
            auto &q_info = get_queue(queue_id);
            for (auto &q_info_pri : q_info) fn(q_info_pri);
            return fn;
        }

        template <typename Function>
        Function for_one_q(size_t queue_id, size_t priority, Function fn) {
            auto &q_info = get_queue(queue_id);
            auto &q_info_pri = q_info.at(priority);
            fn(q_info_pri);
            return fn;
        }

        struct SetCapacityFn {
            explicit SetCapacityFn(size_t c)
                : c(c) { }

            void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
            info.capacity = c;
            }

            size_t c;
        };

        struct SetRateFn {
            explicit SetRateFn(uint64_t pps)
                : pps(pps) {
            using std::chrono::duration;
            using std::chrono::duration_cast;
            pkt_delay_ticks = rate_to_ticks(pps);
            }

            void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
            info.queue_rate_pps = pps;
            info.pkt_delay_ticks = pkt_delay_ticks;
            }

            uint64_t pps;
            ticks pkt_delay_ticks;
        };

        mutable MutexType mutex;
        size_t capacity;  // default capacity
        uint64_t queue_rate_pps{0};  // default rate
        std::unordered_map<size_t, QueueInfo> queues_info{};
        WorkerInfo workers_info;
        std::vector<MyQ> queues{};
        FMap map_to_worker;
        size_t nb_priorities;
        };
}