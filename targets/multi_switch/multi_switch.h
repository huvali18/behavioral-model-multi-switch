
#ifndef MULTI_SWITCH_MULTI_SWITCH_H_
#define MUTLI_SWITCH_MUTLI_SWITCH_H_

#include <../../include/bm/bm_sim/queue.h>
#include <../../include/bm/bm_sim/queueing.h>
#include <../../include/bm/bm_sim/packet.h>
#include <../../include/bm/bm_sim/switch.h>
#include <../../include/bm/bm_sim/event_logger.h>
#include <../../include/bm/bm_sim/simple_pre_lag.h>
#include "queue_multi.cpp"
#include "multi_context.h"

#include <memory>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>

// TODO(antonin)
// experimental support for priority queueing
// to enable it, uncomment this flag
// you can also choose the field from which the priority value will be read, as
// well as the number of priority queues per port
// PRIORITY 0 IS THE LOWEST PRIORITY
#define SSWITCH_PRIORITY_QUEUEING_SRC "intrinsic_metadata.priority"

using ts_res = std::chrono::microseconds;
using std::chrono::duration_cast;
using ticks = std::chrono::nanoseconds;

using bm::MultiContexts;
using bm::Queue;
using bm::Packet;
using bm::PHV;
using bm::Parser;
using bm::Deparser;
using bm::Pipeline;
using bm::McSimplePreLAG;
using bm::Field;
using bm::FieldList;
using bm::packet_id_t;
using bm::p4object_id_t;

class MultiSwitch : public MultiContexts {
 public:
  using mirror_id_t = int;

  using TransmitFn = std::function<void(port_t, packet_id_t,
                                        const char *, int)>;

  struct MirroringSessionConfig {
    port_t egress_port;
    bool egress_port_valid;
    unsigned int mgid;
    bool mgid_valid;
  };

  static constexpr port_t default_drop_port = 511;
  static constexpr size_t default_nb_queues_per_port = 1;

 private:
  using clock = std::chrono::high_resolution_clock;

 public:
  // by default, swapping is off
  explicit MultiSwitch(bool enable_swap = false,
                        port_t drop_port = default_drop_port,
                        size_t nb_queues_per_port = default_nb_queues_per_port);

  ~MultiSwitch();

  int receive_(port_t port_num, const char *buffer, int len) override;

  void start_and_return_() override;

  void reset_target_state_() override;

  void swap_notify_() override;

  bool mirroring_add_session(mirror_id_t mirror_id,
                             const MirroringSessionConfig &config);

  bool mirroring_delete_session(mirror_id_t mirror_id);

  bool mirroring_get_session(mirror_id_t mirror_id,
                             MirroringSessionConfig *config) const;

  int set_egress_priority_queue_depth(size_t port, size_t priority,
                                      const size_t depth_pkts);
  int set_egress_queue_depth(size_t port, const size_t depth_pkts);
  int set_all_egress_queue_depths(const size_t depth_pkts);

  int set_egress_priority_queue_rate(size_t port, size_t priority,
                                     const uint64_t rate_pps);
  int set_egress_queue_rate(size_t port, const uint64_t rate_pps);
  int set_all_egress_queue_rates(const uint64_t rate_pps);

  // returns the number of microseconds elapsed since the switch started
  uint64_t get_time_elapsed_us() const;

  // returns the number of microseconds elasped since the clock's epoch
  uint64_t get_time_since_epoch_us() const;

  // returns the packet id of most recently received packet. Not thread-safe.
  static packet_id_t get_packet_id() {
    return packet_id - 1;
  }

  void set_transmit_fn(TransmitFn fn);

  port_t get_drop_port() const {
    return drop_port;
  }

  MultiSwitch(const MultiSwitch &) = delete;
  MultiSwitch &operator =(const MultiSwitch &) = delete;
  MultiSwitch(MultiSwitch &&) = delete;
  MultiSwitch &&operator =(MultiSwitch &&) = delete;

 private:
  static constexpr int nb_egress_threads = 4u;
  static constexpr size_t nb_ingress_threads = 4u;
  static packet_id_t packet_id;

  class MirroringSessions;

  class InputBuffer;

  enum PktInstanceType {
    PKT_INSTANCE_TYPE_NORMAL,
    PKT_INSTANCE_TYPE_INGRESS_CLONE,
    PKT_INSTANCE_TYPE_EGRESS_CLONE,
    PKT_INSTANCE_TYPE_COALESCED,
    PKT_INSTANCE_TYPE_RECIRC,
    PKT_INSTANCE_TYPE_REPLICATION,
    PKT_INSTANCE_TYPE_RESUBMIT,
  };

  struct EgressThreadMapper {
    explicit EgressThreadMapper(size_t nb_threads)
        : nb_threads(nb_threads) { }

    size_t operator()(size_t egress_port) const {
      return egress_port % nb_threads;
    }

    size_t nb_threads;
  };

 private:
  void ingress_thread(size_t worker_id);
  void egress_thread(size_t worker_id);
  void transmit_thread();

  ts_res get_ts() const;

  // TODO(antonin): switch to pass by value?
  void enqueue(port_t egress_port, std::unique_ptr<Packet> &&packet);

  void copy_field_list_and_set_type(
      const std::unique_ptr<Packet> &packet,
      const std::unique_ptr<Packet> &packet_copy,
      PktInstanceType copy_type, p4object_id_t field_list_id);

  void check_queueing_metadata();

  void multicast(Packet *packet, unsigned int mgid);

 private:
  port_t drop_port;
  std::vector<std::thread> threads_;
  std::vector<std::shared_ptr<InputBuffer>> input_buffers;
  std::vector<std::shared_ptr<Queue<std::unique_ptr<Packet>>>> egress_buffers;
  std::vector<std::shared_ptr<McSimplePreLAG>> pres;
  // for these queues, the write operation is non-blocking and we drop the
  // packet if the queue is full
  size_t nb_queues_per_port;
  Queue<std::unique_ptr<Packet>> output_buffer;
  TransmitFn my_transmit_fn;
  clock::time_point start;
  bool with_queueing_metadata{false};
  std::unique_ptr<MirroringSessions> mirroring_sessions;
};

#endif