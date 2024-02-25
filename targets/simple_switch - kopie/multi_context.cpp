#include <../../include/bm/bm_sim/switch.h>
#include <../../include/bm/bm_sim/P4Objects.h>
#include <../../include/bm/bm_sim/_assert.h>
#include <../../include/bm/bm_sim/debugger.h>
#include <../../include/bm/bm_sim/event_logger.h>
#include <../../include/bm/bm_sim/logger.h>
#include <../../include/bm/bm_sim/options_parse.h>
#include <../../include/bm/bm_sim/packet.h>
#include <../../include/bm/bm_sim/periodic_task.h>
#include <../../include/bm/config.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include "multi_context.h"

#include "../../src/bm_sim/md5.h"

namespace bm {

MultiContexts::MultiContexts(bool enable_swap)
    : SwitchWContexts(4u, enable_swap) { }

std::unique_ptr<Packet>
MultiContexts::new_packet_ptr(port_t ingress_port,
                       packet_id_t id, int ingress_length,
                       // NOLINTNEXTLINE(whitespace/operators)
                       PacketBuffer &&buffer) {
  return new_packet_ptr(0u, ingress_port, id, ingress_length,
                        std::move(buffer));
}

Packet
MultiContexts::new_packet(port_t ingress_port, packet_id_t id, int ingress_length,
                   // NOLINTNEXTLINE(whitespace/operators)
                   PacketBuffer &&buffer) {
  return new_packet(0u, ingress_port, id, ingress_length, std::move(buffer));
}

}