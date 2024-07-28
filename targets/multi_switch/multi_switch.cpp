#include <../../include/bm/bm_sim/_assert.h>
#include <../../include/bm/bm_sim/parser.h>
#include <../../include/bm/bm_sim/tables.h>
#include <../../include/bm/bm_sim/logger.h>

#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "multi_switch.h"
#include "register_access.h"

spdlog::logger *transmit_logger;
spdlog::logger *recieve_logger;
spdlog::logger *ingress_logger;
spdlog::logger *egress_logger;
spdlog::logger *ing_buf_logger;
spdlog::logger *egg_buf_logger;

namespace {

struct hash_ex_multi {
  uint32_t operator()(const char *buf, size_t s) const {
    const uint32_t p = 16777619;
    uint32_t hash = 2166136261;

    for (size_t i = 0; i < s; i++)
      hash = (hash ^ buf[i]) * p;

    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return static_cast<uint32_t>(hash);
  }
};

struct bmv2_hash_multi {
  uint64_t operator()(const char *buf, size_t s) const {
    return bm::hash::xxh64(buf, s);
  }
};

}  // namespace

// if REGISTER_HASH calls placed in the anonymous namespace, some compiler can
// give an unused variable warning
REGISTER_HASH(hash_ex_multi);
REGISTER_HASH(bmv2_hash_multi);

extern int import_primitives_multi(MultiSwitch *multi_switch);

packet_id_t MultiSwitch::packet_id = 0;

class MultiSwitch::MirroringSessions {
 public:
  bool add_session(mirror_id_t mirror_id,
                   const MirroringSessionConfig &config) {
    Lock lock(mutex);
    if (0 <= mirror_id && mirror_id <= RegisterAccess::MAX_MIRROR_SESSION_ID) {
      sessions_map[mirror_id] = config;
      return true;
    } else {
      bm::Logger::get()->error("mirror_id out of range. No session added.");
      return false;
    }
  }

  bool delete_session(mirror_id_t mirror_id) {
    Lock lock(mutex);
    if (0 <= mirror_id && mirror_id <= RegisterAccess::MAX_MIRROR_SESSION_ID) {
      return sessions_map.erase(mirror_id) == 1;
    } else {
      bm::Logger::get()->error("mirror_id out of range. No session deleted.");
      return false;
    }
  }

  bool get_session(mirror_id_t mirror_id,
                   MirroringSessionConfig *config) const {
    Lock lock(mutex);
    auto it = sessions_map.find(mirror_id);
    if (it == sessions_map.end()) return false;
    *config = it->second;
    return true;
  }

 private:
  using Mutex = std::mutex;
  using Lock = std::lock_guard<Mutex>;

  mutable std::mutex mutex;
  std::unordered_map<mirror_id_t, MirroringSessionConfig> sessions_map;
};

// Arbitrates which packets are processed by the ingress thread. Resubmit and
// recirculate packets go to a high priority queue, while normal packets go to a
// low priority queue. We assume that starvation is not going to be a problem.
// Resubmit packets are dropped if the queue is full in order to make sure the
// ingress thread cannot deadlock. We do the same for recirculate packets even
// though the same argument does not apply for them. Enqueueing normal packets
// is blocking (back pressure is applied to the interface).
class MultiSwitch::InputBuffer {
 public:

  using QueueImpl = std::deque<std::unique_ptr<Packet> >;

  enum class PacketType {
    NORMAL,
    RESUBMIT,
    RECIRCULATE,
    SENTINEL  // signal for the ingress thread to terminate
  };

  InputBuffer(size_t capacity_hi, size_t capacity_lo)
      : capacity_hi(capacity_hi), capacity_lo(capacity_lo) { }

  int push_front(PacketType packet_type, std::unique_ptr<Packet> &&item) {
    switch (packet_type) {
      case PacketType::NORMAL:
        return push_front(&queue_lo, capacity_lo, &cvar_can_push_lo,
                          std::move(item), true);
      case PacketType::RESUBMIT:
      case PacketType::RECIRCULATE:
        return push_front(&queue_hi, capacity_hi, &cvar_can_push_hi,
                          std::move(item), false);
      case PacketType::SENTINEL:
        return push_front(&queue_hi, capacity_hi, &cvar_can_push_hi,
                          std::move(item), true);
    }
    _BM_UNREACHABLE("Unreachable statement");
    return 0;
  }

  void pop_back(std::unique_ptr<Packet> *pItem) {
    Lock lock(mutex);
    cvar_can_pop.wait(
        lock, [this] { return (queue_hi.size() + queue_lo.size()) > 0; });
    // give higher priority to resubmit/recirculate queue
    if (queue_hi.size() > 0) {
      *pItem = std::move(queue_hi.back());
      queue_hi.pop_back();
      lock.unlock();
      cvar_can_push_hi.notify_one();
    } else {
      *pItem = std::move(queue_lo.back());
      queue_lo.pop_back();
      lock.unlock();
      cvar_can_push_lo.notify_one();
    }
  }

  QueueImpl queue_hi;
  QueueImpl queue_lo;

 private:
  using Mutex = std::mutex;
  using Lock = std::unique_lock<Mutex>;
  //using QueueImpl = std::deque<std::unique_ptr<Packet> >;

  int push_front(QueueImpl *queue, size_t capacity,
                 std::condition_variable *cvar,
                 std::unique_ptr<Packet> &&item, bool blocking) {
    Lock lock(mutex);
    while (queue->size() == capacity) {
      if (!blocking) return 0;
      cvar->wait(lock);
    }
    queue->push_front(std::move(item));
    lock.unlock();
    cvar_can_pop.notify_one();
    return 1;
  }

  mutable std::mutex mutex;
  mutable std::condition_variable cvar_can_push_hi;
  mutable std::condition_variable cvar_can_push_lo;
  mutable std::condition_variable cvar_can_pop;
  size_t capacity_hi;
  size_t capacity_lo;
};

MultiSwitch::MultiSwitch(bool enable_swap, port_t drop_port,
                           size_t nb_queues_per_port)
  : MultiContexts(enable_swap),
    drop_port(drop_port),
    nb_queues_per_port(nb_queues_per_port),
    output_buffer(128),
    // cannot use std::bind because of a clang bug
    // https://stackoverflow.com/questions/32030141/is-this-incorrect-use-of-stdbind-or-a-compiler-bug
    my_transmit_fn([this](port_t port_num, packet_id_t pkt_id,
                          const char *buffer, int len) {
        _BM_UNUSED(pkt_id);
        this->transmit_fn(port_num, buffer, len);
    }),
    start(clock::now()),
    mirroring_sessions(new MirroringSessions()) {
  
  for(int i = 0; i < 4; i++) {
    input_buffers.push_back(std::make_shared<InputBuffer>(1028 /* normal capacity */, 1028 /* resubmit/recirc capacity */));
    EgressThreadMapper mapper(nb_egress_threads);
    egress_buffers.push_back(std::make_shared<Queue<std::unique_ptr<Packet>>>(64));
    pres.push_back(std::make_shared<McSimplePreLAG>());
    add_component_multi<McSimplePreLAG>(i,pres.at(i));
  }

  spdlog::drop("transmit_logger");
  spdlog::drop("recieve_logger");
  spdlog::drop("ingress_logger");
  spdlog::drop("egress_logger");

  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);
  std::tm now_tm = *std::localtime(&now_time_t);
  std::stringstream ss;
  ss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
  std::string time_str = ss.str();

  std::string transmit = "../logs/transmit";
  std::string transmit_remove = "../logs/transmit.txt";
  std::remove(transmit_remove.c_str());
  auto logger_ = spdlog::rotating_logger_mt("transmit_logger", transmit,
                                          1024 * 1024 * 5, 3, true);
  transmit_logger = logger_.get();
  transmit_logger->set_level(spdlog::level::trace);
  transmit_logger->set_pattern("[%H:%M:%S.%f] %v");

  std::string receive = "../logs/receive";
  std::string receive_remove = "../logs/receive.txt";
  std::remove(receive_remove.c_str());
  logger_ = spdlog::rotating_logger_mt("recieve_logger", receive,
                                          1024 * 1024 * 5, 3, true);
  recieve_logger = logger_.get();
  recieve_logger->set_level(spdlog::level::trace);
  recieve_logger->set_pattern("[%H:%M:%S.%f] %v");

  std::string ingress = "../logs/ingress";
  std::string ingress_remove = "../logs/ingress.txt";
  std::remove(ingress_remove.c_str());
  logger_ = spdlog::rotating_logger_mt("ingress_logger", ingress,
                                          1024 * 1024 * 5, 3, true);
  ingress_logger = logger_.get();
  ingress_logger->set_level(spdlog::level::trace);
  ingress_logger->set_pattern("[%H:%M:%S.%f] %v");

  std::string egress = "../logs/egress";
  std::string egress_remove = "../logs/egress.txt";
  std::remove(egress_remove.c_str());
  logger_ = spdlog::rotating_logger_mt("egress_logger", egress,
                                          1024 * 1024 * 5, 3, true);
  egress_logger = logger_.get();
  egress_logger->set_level(spdlog::level::trace);
  egress_logger->set_pattern("[%H:%M:%S.%f] %v");

  std::string ing_buf = "../logs/ing_buf";
  std::string ing_buf_remove = "../logs/ing_buf.txt";
  std::remove(ing_buf_remove.c_str());
  logger_ = spdlog::rotating_logger_mt("ing_buf", ing_buf,
                                          1024 * 1024 * 5, 3, true);
  ing_buf_logger = logger_.get();
  ing_buf_logger->set_level(spdlog::level::trace);
  ing_buf_logger->set_pattern("[%H:%M:%S.%f] %v");

  std::string egg_buf = "../logs/egg_buf";
  std::string egg_buf_remove = "../logs/egg_buf.txt";
  std::remove(egg_buf_remove.c_str());
  logger_ = spdlog::rotating_logger_mt("egg_buf", egg_buf,
                                          1024 * 1024 * 5, 3, true);
  egg_buf_logger = logger_.get();
  egg_buf_logger->set_level(spdlog::level::trace);
  egg_buf_logger->set_pattern("[%H:%M:%S.%f] %v");

  set_multi_switch(true);
 

  add_required_field("standard_metadata", "ingress_port");
  add_required_field("standard_metadata", "packet_length");
  add_required_field("standard_metadata", "instance_type");
  add_required_field("standard_metadata", "egress_spec");
  add_required_field("standard_metadata", "egress_port");

  force_arith_header("standard_metadata");
  force_arith_header("queueing_metadata");
  force_arith_header("intrinsic_metadata");

  import_primitives_multi(this);
}

int
MultiSwitch::receive_(port_t port_num, const char *buffer, int len) {
  // we limit the packet buffer to original size + 512 bytes, which means we
  // cannot add more than 512 bytes of header data to the packet, which should
  // be more than enough
 

  auto packet = new_packet_ptr(port_num, packet_id++, len,
                               bm::PacketBuffer(len + 512, buffer, len));

  recieve_logger->trace("{}", packet->get_signature());

  BMELOG(packet_in, *packet);

  PHV *phv = packet->get_phv();
  // many current P4 programs assume this
  // it is also part of the original P4 spec
  phv->reset_metadata();
  RegisterAccess::clear_all(packet.get());

  // setting standard metadata

  phv->get_field("standard_metadata.ingress_port").set(port_num);
  // using packet register 0 to store length, this register will be updated for
  // each add_header / remove_header primitive call
  packet->set_register(RegisterAccess::PACKET_LENGTH_REG_IDX, len);
  phv->get_field("standard_metadata.packet_length").set(len);
  Field &f_instance_type = phv->get_field("standard_metadata.instance_type");
  f_instance_type.set(PKT_INSTANCE_TYPE_NORMAL);

  if (phv->has_field("intrinsic_metadata.ingress_global_timestamp")) {
    phv->get_field("intrinsic_metadata.ingress_global_timestamp")
        .set(get_ts().count());
  }
  
  if(port_num < 8) {
      input_buffers.at(0)->push_front(
      InputBuffer::PacketType::NORMAL, std::move(packet));
      ingress_logger->trace("Pipe 1: {}", input_buffers.at(0)->queue_lo.size());
  }
  else if (port_num < 16) {
      input_buffers.at(1)->push_front(
      InputBuffer::PacketType::NORMAL, std::move(packet));
      ingress_logger->trace("Pipe 2: {}", input_buffers.at(1)->queue_lo.size());
  }
  else if (port_num < 24) {
      input_buffers.at(2)->push_front(
      InputBuffer::PacketType::NORMAL, std::move(packet));
      ingress_logger->trace("Pipe 3: {}", input_buffers.at(2)->queue_lo.size());
  }
  else if (port_num < 32) {
      input_buffers.at(3)->push_front(
      InputBuffer::PacketType::NORMAL, std::move(packet));
      ingress_logger->trace("Pipe 4: {}", input_buffers.at(3)->queue_lo.size());
  }
  
  return 0;
}

void
MultiSwitch::start_and_return_() {
  check_queueing_metadata();
  for (size_t i = 0; i < nb_ingress_threads; i++) {
    threads_.push_back(std::thread(&MultiSwitch::ingress_thread, this, i));
  }
  for (size_t i = 0; i < nb_egress_threads; i++) {
    threads_.push_back(std::thread(&MultiSwitch::egress_thread, this, i));
  }

  threads_.push_back(std::thread(&MultiSwitch::transmit_thread, this));
}

void
MultiSwitch::swap_notify_() {
  bm::Logger::get()->debug(
      "simple_switch target has been notified of a config swap");
  check_queueing_metadata();
}

MultiSwitch::~MultiSwitch() {
  for (size_t i = 0; i < nb_ingress_threads; i++) {
    input_buffers.at(i)->push_front(
      InputBuffer::PacketType::SENTINEL, nullptr);
  }
  for (size_t i = 0; i < nb_egress_threads; i++) {
    egress_buffers.at(i)->push_front(nullptr);
  }
  output_buffer.push_front(nullptr);
  for (auto& thread_ : threads_) {
    thread_.join();
  }
}

void
MultiSwitch::reset_target_state_() {
  bm::Logger::get()->debug("Resetting simple_switch target-specific state");
  get_component<McSimplePreLAG>()->reset_state();
}

bool
MultiSwitch::mirroring_add_session(mirror_id_t mirror_id,
                                    const MirroringSessionConfig &config) {
  return mirroring_sessions->add_session(mirror_id, config);
}

bool
MultiSwitch::mirroring_delete_session(mirror_id_t mirror_id) {
  return mirroring_sessions->delete_session(mirror_id);
}

bool
MultiSwitch::mirroring_get_session(mirror_id_t mirror_id,
                                    MirroringSessionConfig *config) const {
  return mirroring_sessions->get_session(mirror_id, config);
}

int
MultiSwitch::set_egress_priority_queue_depth(size_t port, size_t priority,
                                              const size_t depth_pkts) {
  return -1;
}

int
MultiSwitch::set_egress_queue_depth(size_t port, const size_t depth_pkts) {
  return -1;
}

int
MultiSwitch::set_all_egress_queue_depths(const size_t depth_pkts) {
  return -1;
}

int
MultiSwitch::set_egress_priority_queue_rate(size_t port, size_t priority,
                                             const uint64_t rate_pps) {
  return -1;
}

int
MultiSwitch::set_egress_queue_rate(size_t port, const uint64_t rate_pps) {
  return -1;
}

int
MultiSwitch::set_all_egress_queue_rates(const uint64_t rate_pps) {
  return -1;
}

uint64_t
MultiSwitch::get_time_elapsed_us() const {
  return get_ts().count();
}

uint64_t
MultiSwitch::get_time_since_epoch_us() const {
  auto tp = clock::now();
  return duration_cast<ts_res>(tp.time_since_epoch()).count();
}

void
MultiSwitch::set_transmit_fn(TransmitFn fn) {
  my_transmit_fn = std::move(fn);
}

void
MultiSwitch::transmit_thread() {
  while (1) {
    std::unique_ptr<Packet> packet;
    output_buffer.pop_back(&packet);
    if (packet == nullptr) break;
    transmit_logger->trace("{}", packet->get_signature());
    BMELOG(packet_out, *packet);
    BMLOG_DEBUG_PKT(*packet, "Transmitting packet of size {} out of port {}",
                    packet->get_data_size(), packet->get_egress_port());
    my_transmit_fn(packet->get_egress_port(), packet->get_packet_id(),
                   packet->data(), packet->get_data_size());
  }
}

ts_res
MultiSwitch::get_ts() const {
  return duration_cast<ts_res>(clock::now() - start);
}

void
MultiSwitch::enqueue(port_t egress_port, std::unique_ptr<Packet> &&packet) {
    packet->set_egress_port(egress_port);
    PHV *phv = packet->get_phv();

    if (with_queueing_metadata) {
      phv->get_field("queueing_metadata.enq_timestamp").set(get_ts().count());
      phv->get_field("queueing_metadata.enq_qdepth")
          .set(egress_buffers.at(0)->size());
    }

    size_t priority = phv->has_field(SSWITCH_PRIORITY_QUEUEING_SRC) ?
        phv->get_field(SSWITCH_PRIORITY_QUEUEING_SRC).get<size_t>() : 0u;
    if (priority >= nb_queues_per_port) {
      bm::Logger::get()->error("Priority out of range, dropping packet");
      return;
    }

    uint64_t signature = packet->get_signature();

    if(egress_port < 8) {
        egress_buffers.at(0)->push_front(std::move(packet));
        egress_logger->trace("Pipe 1: {}", egress_buffers.at(0)->size());
    }
    else if (egress_port < 16) {
        egress_buffers.at(1)->push_front(std::move(packet));
        egress_logger->trace("Pipe 2: {}", egress_buffers.at(1)->size());
    }
    else if (egress_port < 24) {
        egress_buffers.at(2)->push_front(std::move(packet));
        egress_logger->trace("Pipe 3: {}", egress_buffers.at(2)->size());
    }
    else if (egress_port < 32) {
        egress_buffers.at(3)->push_front(std::move(packet));
        egress_logger->trace("Pipe 4: {}", egress_buffers.at(3)->size());
    }

    ing_buf_logger->trace("{}", signature);
}

// used for ingress cloning, resubmit
void
MultiSwitch::copy_field_list_and_set_type(
    const std::unique_ptr<Packet> &packet,
    const std::unique_ptr<Packet> &packet_copy,
    PktInstanceType copy_type, p4object_id_t field_list_id) {
  PHV *phv_copy = packet_copy->get_phv();
  phv_copy->reset_metadata();
  FieldList *field_list = this->get_field_list(field_list_id);
  field_list->copy_fields_between_phvs(phv_copy, packet->get_phv());
  phv_copy->get_field("standard_metadata.instance_type").set(copy_type);
}

void
MultiSwitch::check_queueing_metadata() {
  // TODO(antonin): add qid in required fields
  bool enq_timestamp_e = field_exists("queueing_metadata", "enq_timestamp");
  bool enq_qdepth_e = field_exists("queueing_metadata", "enq_qdepth");
  bool deq_timedelta_e = field_exists("queueing_metadata", "deq_timedelta");
  bool deq_qdepth_e = field_exists("queueing_metadata", "deq_qdepth");
  if (enq_timestamp_e || enq_qdepth_e || deq_timedelta_e || deq_qdepth_e) {
    if (enq_timestamp_e && enq_qdepth_e && deq_timedelta_e && deq_qdepth_e) {
      with_queueing_metadata = true;
      return;
    } else {
      bm::Logger::get()->warn(
          "Your JSON input defines some but not all queueing metadata fields");
    }
  }
  with_queueing_metadata = false;
}

void
MultiSwitch::multicast(Packet *packet, unsigned int mgid) {
  auto *phv = packet->get_phv();
  auto &f_rid = phv->get_field("intrinsic_metadata.egress_rid");
  const auto pre_out = pres.at(0)->replicate({mgid});
  auto packet_size =
      packet->get_register(RegisterAccess::PACKET_LENGTH_REG_IDX);
  for (const auto &out : pre_out) {
    auto egress_port = out.egress_port;
    BMLOG_DEBUG_PKT(*packet, "Replicating packet on port {}", egress_port);
    f_rid.set(out.rid);
    std::unique_ptr<Packet> packet_copy = packet->clone_with_phv_ptr();
    RegisterAccess::clear_all(packet_copy.get());
    packet_copy->set_register(RegisterAccess::PACKET_LENGTH_REG_IDX,
                              packet_size);
    enqueue(egress_port, std::move(packet_copy));
  }
}

void
MultiSwitch::ingress_thread(size_t worker_id) {
  PHV *phv;

  while (1) {
    std::unique_ptr<Packet> packet;

    input_buffers.at(worker_id)->pop_back(&packet);
    
    if (packet == nullptr) break;

    // TODO(antonin): only update these if swapping actually happened?
    Parser *parser = this->get_parser(worker_id, "parser");
    Pipeline *ingress_mau = this->get_pipeline(worker_id, "ingress")

    phv = packet->get_phv();

    port_t ingress_port = packet->get_ingress_port();
    (void) ingress_port;
    BMLOG_DEBUG_PKT(*packet, "Processing packet received on port {}",
                    ingress_port);

    auto ingress_packet_size =
        packet->get_register(RegisterAccess::PACKET_LENGTH_REG_IDX);


    /* This looks like it comes out of the blue. However this is needed for
       ingress cloning. The parser updates the buffer state (pops the parsed
       headers) to make the deparser's job easier (the same buffer is
       re-used). But for ingress cloning, the original packet is needed. This
       kind of looks hacky though. Maybe a better solution would be to have the
       parser leave the buffer unchanged, and move the pop logic to the
       deparser. TODO? */

    const Packet::buffer_state_t packet_in_state = packet->save_buffer_state();

    parser->parse(packet.get());

    if (phv->has_field("standard_metadata.parser_error")) {
      phv->get_field("standard_metadata.parser_error").set(
          packet->get_error_code().get());
    }

    
    if (phv->has_field("standard_metadata.checksum_error")) {
      phv->get_field("standard_metadata.checksum_error").set(
           packet->get_checksum_error() ? 1 : 0);
    }

    ingress_mau->apply(packet.get());

    packet->reset_exit();

    Field &f_egress_spec = phv->get_field("standard_metadata.egress_spec");
    port_t egress_spec = f_egress_spec.get_uint();

    auto clone_mirror_session_id =
        RegisterAccess::get_clone_mirror_session_id(packet.get());
    auto clone_field_list = RegisterAccess::get_clone_field_list(packet.get());

    int learn_id = RegisterAccess::get_lf_field_list(packet.get());
    unsigned int mgid = 0u;

    // detect mcast support, if this is true we assume that other fields needed
    // for mcast are also defined
    if (phv->has_field("intrinsic_metadata.mcast_grp")) {
      Field &f_mgid = phv->get_field("intrinsic_metadata.mcast_grp");
      mgid = f_mgid.get_uint();
    }

    // INGRESS CLONING
    if (clone_mirror_session_id) {
      BMLOG_DEBUG_PKT(*packet, "Cloning packet at ingress");
      RegisterAccess::set_clone_mirror_session_id(packet.get(), 0);
      RegisterAccess::set_clone_field_list(packet.get(), 0);
      MirroringSessionConfig config;
      // Extract the part of clone_mirror_session_id that contains the
      // actual session id.
      clone_mirror_session_id &= RegisterAccess::MIRROR_SESSION_ID_MASK;
      bool is_session_configured = mirroring_get_session(
          static_cast<mirror_id_t>(clone_mirror_session_id), &config);
      if (is_session_configured) {
        const Packet::buffer_state_t packet_out_state =
            packet->save_buffer_state();
        packet->restore_buffer_state(packet_in_state);
        p4object_id_t field_list_id = clone_field_list;
        std::unique_ptr<Packet> packet_copy = packet->clone_no_phv_ptr();
        RegisterAccess::clear_all(packet_copy.get());
        packet_copy->set_register(RegisterAccess::PACKET_LENGTH_REG_IDX,
                                  ingress_packet_size);
        // We need to parse again.
        // The alternative would be to pay the (huge) price of PHV copy for
        // every ingress packet.
        // Since parsers can branch on the ingress port, we need to preserve it
        // to ensure re-parsing gives the same result as the original parse.
        // TODO(https://github.com/p4lang/behavioral-model/issues/795): other
        // standard metadata should be preserved as well.
        packet_copy->get_phv()
            ->get_field("standard_metadata.ingress_port")
            .set(ingress_port);
        parser->parse(packet_copy.get());
        copy_field_list_and_set_type(packet, packet_copy,
                                     PKT_INSTANCE_TYPE_INGRESS_CLONE,
                                     field_list_id);
        if (config.mgid_valid) {
          BMLOG_DEBUG_PKT(*packet, "Cloning packet to MGID {}", config.mgid);
          multicast(packet_copy.get(), config.mgid);
        }
        if (config.egress_port_valid) {
          BMLOG_DEBUG_PKT(*packet, "Cloning packet to egress port {}",
                          config.egress_port);
          enqueue(config.egress_port, std::move(packet_copy));
        }
        packet->restore_buffer_state(packet_out_state);
      }
    }

    // LEARNING
    if (learn_id > 0) {
      get_learn_engine()->learn(learn_id, *packet.get());
    }

    // RESUBMIT
    auto resubmit_flag = RegisterAccess::get_resubmit_flag(packet.get());
    if (resubmit_flag) {
      BMLOG_DEBUG_PKT(*packet, "Resubmitting packet");
      // get the packet ready for being parsed again at the beginning of
      // ingress
      packet->restore_buffer_state(packet_in_state);
      p4object_id_t field_list_id = resubmit_flag;
      RegisterAccess::set_resubmit_flag(packet.get(), 0);
      // TODO(antonin): a copy is not needed here, but I don't yet have an
      // optimized way of doing this
      std::unique_ptr<Packet> packet_copy = packet->clone_no_phv_ptr();
      PHV *phv_copy = packet_copy->get_phv();
      copy_field_list_and_set_type(packet, packet_copy,
                                   PKT_INSTANCE_TYPE_RESUBMIT,
                                   field_list_id);
      RegisterAccess::clear_all(packet_copy.get());
      packet_copy->set_register(RegisterAccess::PACKET_LENGTH_REG_IDX,
                                ingress_packet_size);
      phv_copy->get_field("standard_metadata.packet_length")
          .set(ingress_packet_size);

      input_buffers.at(worker_id)->push_front(
          InputBuffer::PacketType::RESUBMIT, std::move(packet_copy));
     
      
      continue;
    }

    // MULTICAST
    if (mgid != 0) {
      BMLOG_DEBUG_PKT(*packet, "Multicast requested for packet");
      auto &f_instance_type = phv->get_field("standard_metadata.instance_type");
      f_instance_type.set(PKT_INSTANCE_TYPE_REPLICATION);
      multicast(packet.get(), mgid);
      // when doing multicast, we discard the original packet
      continue;
    }

    port_t egress_port = egress_spec;
    BMLOG_DEBUG_PKT(*packet, "Egress port is {}", egress_port);

    if (egress_port == drop_port) {  // drop packet
      BMLOG_DEBUG_PKT(*packet, "Dropping packet at the end of ingress");
      continue;
    }
    auto &f_instance_type = phv->get_field("standard_metadata.instance_type");
    f_instance_type.set(PKT_INSTANCE_TYPE_NORMAL);

    enqueue(egress_port, std::move(packet));
  }
}

void
MultiSwitch::egress_thread(size_t worker_id) {
  PHV *phv;

  while (1) {
    std::unique_ptr<Packet> packet;
    size_t port;
    size_t priority;

    egress_buffers.at(worker_id)->pop_back(&packet);
    
    if (packet == nullptr) break;

    Deparser *deparser = this->get_deparser(worker_id,"deparser");
    Pipeline *egress_mau = this->get_pipeline(worker_id,"egress");

    phv = packet->get_phv();

    if (phv->has_field("intrinsic_metadata.egress_global_timestamp")) {
      phv->get_field("intrinsic_metadata.egress_global_timestamp")
          .set(get_ts().count());
    }

    if (with_queueing_metadata) {
      auto enq_timestamp =
          phv->get_field("queueing_metadata.enq_timestamp").get<ts_res::rep>();
      phv->get_field("queueing_metadata.deq_timedelta").set(
          get_ts().count() - enq_timestamp);
      phv->get_field("queueing_metadata.deq_qdepth").set(
          egress_buffers.at(0)->size());
      if (phv->has_field("queueing_metadata.qid")) {
        auto &qid_f = phv->get_field("queueing_metadata.qid");
        qid_f.set(nb_queues_per_port - 1 - priority);
      }
    }

    phv->get_field("standard_metadata.egress_port").set(port);

    Field &f_egress_spec = phv->get_field("standard_metadata.egress_spec");
    // When egress_spec == drop_port the packet will be dropped, thus
    // here we initialize egress_spec to a value different from drop_port.
    f_egress_spec.set(drop_port + 1);

    phv->get_field("standard_metadata.packet_length").set(
        packet->get_register(RegisterAccess::PACKET_LENGTH_REG_IDX));

    egress_mau->apply(packet.get());

    auto clone_mirror_session_id =
        RegisterAccess::get_clone_mirror_session_id(packet.get());
    auto clone_field_list = RegisterAccess::get_clone_field_list(packet.get());

    // EGRESS CLONING
    if (clone_mirror_session_id) {
      BMLOG_DEBUG_PKT(*packet, "Cloning packet at egress");
      RegisterAccess::set_clone_mirror_session_id(packet.get(), 0);
      RegisterAccess::set_clone_field_list(packet.get(), 0);
      MirroringSessionConfig config;
      // Extract the part of clone_mirror_session_id that contains the
      // actual session id.
      clone_mirror_session_id &= RegisterAccess::MIRROR_SESSION_ID_MASK;
      bool is_session_configured = mirroring_get_session(
          static_cast<mirror_id_t>(clone_mirror_session_id), &config);
      if (is_session_configured) {
        p4object_id_t field_list_id = clone_field_list;
        std::unique_ptr<Packet> packet_copy =
            packet->clone_with_phv_reset_metadata_ptr();
        PHV *phv_copy = packet_copy->get_phv();
        FieldList *field_list = this->get_field_list(field_list_id);
        field_list->copy_fields_between_phvs(phv_copy, phv);
        phv_copy->get_field("standard_metadata.instance_type")
            .set(PKT_INSTANCE_TYPE_EGRESS_CLONE);
        auto packet_size =
            packet->get_register(RegisterAccess::PACKET_LENGTH_REG_IDX);
        RegisterAccess::clear_all(packet_copy.get());
        packet_copy->set_register(RegisterAccess::PACKET_LENGTH_REG_IDX,
                                  packet_size);
        if (config.mgid_valid) {
          BMLOG_DEBUG_PKT(*packet, "Cloning packet to MGID {}", config.mgid);
          multicast(packet_copy.get(), config.mgid);
        }
        if (config.egress_port_valid) {
          BMLOG_DEBUG_PKT(*packet, "Cloning packet to egress port {}",
                          config.egress_port);
          enqueue(config.egress_port, std::move(packet_copy));
        }
      }
    }

    // TODO(antonin): should not be done like this in egress pipeline
    port_t egress_spec = f_egress_spec.get_uint();
    if (egress_spec == drop_port) {  // drop packet
      BMLOG_DEBUG_PKT(*packet, "Dropping packet at the end of egress");
      continue;
    }

    deparser->deparse(packet.get());

    // RECIRCULATE
    auto recirculate_flag = RegisterAccess::get_recirculate_flag(packet.get());
    if (recirculate_flag) {
      BMLOG_DEBUG_PKT(*packet, "Recirculating packet");
      p4object_id_t field_list_id = recirculate_flag;
      RegisterAccess::set_recirculate_flag(packet.get(), 0);
      FieldList *field_list = this->get_field_list(field_list_id);
      // TODO(antonin): just like for resubmit, there is no need for a copy
      // here, but it is more convenient for this first prototype
      std::unique_ptr<Packet> packet_copy = packet->clone_no_phv_ptr();
      PHV *phv_copy = packet_copy->get_phv();
      phv_copy->reset_metadata();
      field_list->copy_fields_between_phvs(phv_copy, phv);
      phv_copy->get_field("standard_metadata.instance_type")
          .set(PKT_INSTANCE_TYPE_RECIRC);
      size_t packet_size = packet_copy->get_data_size();
      RegisterAccess::clear_all(packet_copy.get());
      packet_copy->set_register(RegisterAccess::PACKET_LENGTH_REG_IDX,
                                packet_size);
      phv_copy->get_field("standard_metadata.packet_length").set(packet_size);
      // TODO(antonin): really it may be better to create a new packet here or
      // to fold this functionality into the Packet class?
      packet_copy->set_ingress_length(packet_size);

      packet_copy->set_ingress_port(egress_spec);
      

      input_buffers.at(worker_id)->push_front(
          InputBuffer::PacketType::RECIRCULATE, std::move(packet_copy));
      
      continue;
    }

    uint64_t signature = packet->get_signature();
    output_buffer.push_front(std::move(packet));
    egg_buf_logger->trace("{}", signature);
  }
}
