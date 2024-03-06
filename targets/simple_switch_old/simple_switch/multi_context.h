
#ifndef MULTI_SWITCH_MULTI_CONTEXT_H_
#define MUTLI_SWITCH_MUTLI_CONTEXT_H_


#include <condition_variable>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/thread/shared_mutex.hpp>
#include "../../include/bm/bm_sim/switch.h"
#include "../../include/bm/bm_sim/action_profile.h"
#include "../../include/bm/bm_sim/context.h"
#include "../../include/bm/bm_sim/dev_mgr.h"
#include "../../include/bm/bm_sim/device_id.h"
#include "../../include/bm/bm_sim/learning.h"
#include "../../include/bm/bm_sim/lookup_structures.h"
#include "../../include/bm/bm_sim/phv_source.h"
#include "../../include/bm/bm_sim/queue.h"
#include "../../include/bm/bm_sim/runtime_interface.h"
#include "../../include/bm/bm_sim/target_parser.h"

namespace bm {

class MultiContexts : public SwitchWContexts {
 public:
  //! See SwitchWContexts::SwitchWContexts()
  explicit MultiContexts(bool enable_swap = false);

  // to avoid C++ name hiding
  using SwitchWContexts::field_exists;
  //! Checks that the given field was defined in the input JSON used to
  //! configure the switch
  bool field_exists(const std::string &header_name,
                    const std::string &field_name) const {
    return field_exists(0, header_name, field_name);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::new_packet_ptr;
  //! Convenience wrapper around SwitchWContexts::new_packet_ptr() for a single
  //! context switch.
  std::unique_ptr<Packet> new_packet_ptr(port_t ingress_port,
                                         packet_id_t id, int ingress_length,
                                         // cpplint false positive
                                         // NOLINTNEXTLINE(whitespace/operators)
                                         PacketBuffer &&buffer);

  // to avoid C++ name hiding
  using SwitchWContexts::new_packet;
  //! Convenience wrapper around SwitchWContexts::new_packet() for a single
  //! context switch.
  Packet new_packet(port_t ingress_port, packet_id_t id, int ingress_length,
                    // cpplint false positive
                    // NOLINTNEXTLINE(whitespace/operators)
                    PacketBuffer &&buffer);

  //! Return a raw, non-owning pointer to Pipeline \p name. This pointer will be
  //! invalidated if a configuration swap is performed by the target. See
  //! switch.h documentation for details. Return a nullptr if there is no
  //! pipeline with this name.
  Pipeline *get_pipeline(size_t cxt_id, const std::string &name) {
    return get_context(cxt_id)->get_pipeline(name);
  }

  //! Return a raw, non-owning pointer to Parser \p name. This pointer will be
  //! invalidated if a configuration swap is performed by the target. See
  //! switch.h documentation for details. Return a nullptr if there is no parser
  //! with this name.
  Parser *get_parser(size_t cxt_id, const std::string &name) {
    return get_context(cxt_id)->get_parser(name);
  }

  //! Return a raw, non-owning pointer to Deparser \p name. This pointer will be
  //! invalidated if a configuration swap is performed by the target. See
  //! switch.h documentation for details. Return a nullptr if there is no
  //! deparser with this name.
  Deparser *get_deparser(size_t cxt_id, const std::string &name) {
    return get_context(cxt_id)->get_deparser(name);
  }

  //! Return a raw, non-owning pointer to the FieldList with id \p
  //! field_list_id. This pointer will be invalidated if a configuration swap is
  //! performed by the target. See switch.h documentation for details.
  FieldList *get_field_list(const p4object_id_t field_list_id) {
    return get_context(0)->get_field_list(field_list_id);
  }

  // Added for testing, other "object types" can be added if needed
  p4object_id_t get_table_id(const std::string &name) {
    return get_context(0)->get_table_id(name);
  }

  p4object_id_t get_action_id(const std::string &table_name,
                              const std::string &action_name) {
    return get_context(0)->get_action_id(table_name, action_name);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_learn_engine;
  //! Obtain a pointer to the LearnEngine for this Switch instance
  LearnEngineIface *get_learn_engine() {
    return get_learn_engine(0);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_ageing_monitor;
  AgeingMonitorIface *get_ageing_monitor() {
    return get_ageing_monitor(0);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_config_options;
  ConfigOptionMap get_config_options() const {
    return get_config_options(0);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_error_codes;
  //! Return a copy of the error codes map (a bi-directional map between an
  //! error code's integral value and its name / description) for the switch.
  ErrorCodeMap get_error_codes() const {
    return get_error_codes(0);
  }

  //! Add a component to this Switch. Each Switch maintains a map `T` ->
  //! `shared_ptr<T>`, which maps a type (using `typeid`) to a shared pointer to
  //! an object of the same type. The pointer can be retrieved at a later time
  //! by using get_component().
  template<typename T>
  bool add_component(std::shared_ptr<T> ptr) {
    return add_cxt_component<T>(0, std::move(ptr));
  }


  template<typename T>
  bool add_component_multi(size_t cxt_id, std::shared_ptr<T> ptr) {
    return add_cxt_component<T>(cxt_id, std::move(ptr));
  }



  //! Retrieve the shared pointer to an object of type `T` previously added to
  //! the Switch using add_component().
  template<typename T>
  std::shared_ptr<T> get_component() {
    return get_cxt_component<T>(0);
  }
};

}

#endif