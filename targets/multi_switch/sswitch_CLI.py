#!/usr/bin/env python3
# Copyright 2013-present Barefoot Networks, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

#
#
#  Antonin Bas (antonin@barefootnetworks.com)
#
#
from functools import wraps
import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
target_dir = os.path.abspath("../..")
sys.path.append(target_dir)

from tools import runtime_CLI
from runtime_CLI import *

from tools import bmpy_utils as utils

from sswitch_runtime import SimpleSwitch
from sswitch_runtime.ttypes import *

from tools.bm_runtime.standard import Standard
from tools.bm_runtime.standard.ttypes import *
try:
    from tools.bm_runtime.simple_pre import SimplePre
except:
    pass
try:
    from tools.bm_runtime.simple_pre_lag import SimplePreLAG
except:
    pass



class Context:
    def __init__(self):
        self.TABLES = {}
        self.ACTION_PROFS = {}
        self.ACTIONS = {}
        self.METER_ARRAYS = {}
        self.COUNTER_ARRAYS = {}
        self.REGISTER_ARRAYS = {}
        self.CUSTOM_CRC_CALCS = {}
        self.PARSE_VSETS = {}
        self.SUFFIX_LOOKUP_MAP = {}

    def reset_config(self):
        self.TABLES.clear()
        self.ACTION_PROFS.clear()
        self.ACTIONS.clear()
        self.METER_ARRAYS.clear()
        self.COUNTER_ARRAYS.clear()
        self.REGISTER_ARRAYS.clear()
        self.CUSTOM_CRC_CALCS.clear()
        self.PARSE_VSETS.clear()
        self.SUFFIX_LOOKUP_MAP.clear()

context_1 = Context()
context_2 = Context()
context_3 = Context()
context_4 = Context()

contexts = [context_1, context_2, context_3, context_4]

class TableMulti:
    def __init__(self, name, id_, context_id):
        self.name = name
        self.id_ = id_
        self.match_type_ = None
        self.actions = {}
        self.key = []
        self.default_action = None
        self.type_ = None
        self.support_timeout = False
        self.action_prof = None
        self.context = contexts[context_id]

        self.context.TABLES[name] = self

    def num_key_fields(self):
        return len(self.key)

    def key_str(self):
        return ",\t".join([name + "(" + MatchType.to_str(t) + ", " + str(bw) + ")" for name, t, bw in self.key])

    def table_str(self):
        ap_str = "implementation={}".format(
            "None" if not self.action_prof else self.action_prof.name)
        return "{0:30} [{1}, mk={2}]".format(self.name, ap_str, self.key_str())

    def get_action(self, action_name):
        key = ResType.action, action_name
        action = self.context.SUFFIX_LOOKUP_MAP.get(key, None)
        if action is None or action.name not in self.actions:
            return None
        return action


class ActionProfMulti:
    def __init__(self, name, id_, context_id):
        self.name = name
        self.id_ = id_
        self.with_selection = False
        self.actions = {}
        self.ref_cnt = 0
        self.context = contexts[context_id]

        self.context.ACTION_PROFS[name] = self

    def action_prof_str(self):
        return "{0:30} [{1}]".format(self.name, self.with_selection)

    def get_action(self, action_name):
        key = ResType.action, action_name
        action = self.context.SUFFIX_LOOKUP_MAP.get(key, None)
        if action is None or action.name not in self.actions:
            return None
        return action


class ActionMulti:
    def __init__(self, name, id_, context_id):
        self.name = name
        self.id_ = id_
        self.runtime_data = []
        self.context = contexts[context_id]

        self.context.ACTIONS[name] = self

    def num_params(self):
        return len(self.runtime_data)

    def runtime_data_str(self):
        return ",\t".join([name + "(" + str(bw) + ")" for name, bw in self.runtime_data])

    def action_str(self):
        return "{0:30} [{1}]".format(self.name, self.runtime_data_str())
    

class MeterArrayMulti:
    def __init__(self, name, id_, context_id):
        self.name = name
        self.id_ = id_
        self.type_ = None
        self.is_direct = None
        self.size = None
        self.binding = None
        self.rate_count = None
        self.context = contexts[context_id]

        self.context.METER_ARRAYS[name] = self

    def meter_str(self):
        return "{0:30} [{1}, {2}]".format(self.name, self.size,
                                          MeterType.to_str(self.type_))


class CounterArrayMulti:
    def __init__(self, name, id_,context_id):
        self.name = name
        self.id_ = id_
        self.is_direct = None
        self.size = None
        self.binding = None
        self.context = contexts[context_id]

        self.context.COUNTER_ARRAYS[name] = self

    def counter_str(self):
        return "{0:30} [{1}]".format(self.name, self.size)


class RegisterArrayMulti:
    def __init__(self, name, id_, context_id):
        self.name = name
        self.id_ = id_
        self.width = None
        self.size = None
        self.context = contexts[context_id]

        self.context.REGISTER_ARRAYS[name] = self

    def register_str(self):
        return "{0:30} [{1}]".format(self.name, self.size)
    
class ParseVSetMulti:
    def __init__(self, name, id_, context_id):
        self.name = name
        self.id_ = id_
        self.bitwidth = None
        self.context = contexts[context_id]

        self.context.PARSE_VSETS[name] = self

    def parse_vset_str(self):
        return "{0:30} [compressed bitwidth:{1}]".format(
            self.name, self.bitwidth)
    

def load_json_str_multi(json_str, context_id, architecture_spec=None):

    def get_header_type(header_name, j_headers):
        for h in j_headers:
            if h["name"] == header_name:
                return h["header_type"]
        assert(0)

    def get_field_bitwidth(header_type, field_name, j_header_types):
        for h in j_header_types:
            if h["name"] != header_type:
                continue
            for t in h["fields"]:
                # t can have a third element (field signedness)
                f, bw = t[0], t[1]
                if f == field_name:
                    return bw
        assert(0)

    context = contexts[context_id]
    context.reset_config()

    json_ = json.loads(json_str)

    def get_json_key(key):
        return json_.get(key, [])

    for j_action in get_json_key("actions"):
        action = ActionMulti(j_action["name"], j_action["id"],context_id)
        for j_param in j_action["runtime_data"]:
            action.runtime_data += [(j_param["name"], j_param["bitwidth"])]
    
    for j_pipeline in get_json_key("pipelines"):
        if "action_profiles" in j_pipeline:  # new JSON format
            for j_aprof in j_pipeline["action_profiles"]:
                action_prof = ActionProfMulti(j_aprof["name"], j_aprof["id"], context_id)
                action_prof.with_selection = "selector" in j_aprof

        for j_table in j_pipeline["tables"]:
            table = TableMulti(j_table["name"], j_table["id"], context_id)
            table.match_type = MatchType.from_str(j_table["match_type"])
            table.type_ = TableType.from_str(j_table["type"])
            table.support_timeout = j_table["support_timeout"]
            for action in j_table["actions"]:
                table.actions[action] = context.ACTIONS[action]

            if table.type_ in {TableType.indirect, TableType.indirect_ws}:
                if "action_profile" in j_table:
                    action_prof = context.ACTION_PROFS[j_table["action_profile"]]
                else:  # for backward compatibility
                    assert("act_prof_name" in j_table)
                    action_prof = ActionProfMulti(j_table["act_prof_name"],
                                             table.id_, context_id)
                    action_prof.with_selection = "selector" in j_table
                action_prof.actions.update(table.actions)
                action_prof.ref_cnt += 1
                table.action_prof = action_prof
            
            for j_key in j_table["key"]:
                target = j_key["target"]
                match_type = MatchType.from_str(j_key["match_type"])
                if match_type == MatchType.VALID:
                    field_name = target + "_valid"
                    bitwidth = 1
                elif target[1] == "$valid$":
                    field_name = target[0] + "_valid"
                    bitwidth = 1
                else:
                    field_name = ".".join(target)
                    header_type = get_header_type(target[0],
                                                  json_["headers"])
                    bitwidth = get_field_bitwidth(header_type, target[1],
                                                  json_["header_types"])
                table.key += [(field_name, match_type, bitwidth)]

    for j_meter in get_json_key("meter_arrays"):
        meter_array = MeterArrayMulti(j_meter["name"], j_meter["id"], context_id)
        if "is_direct" in j_meter and j_meter["is_direct"]:
            meter_array.is_direct = True
            meter_array.binding = j_meter["binding"]
        else:
            meter_array.is_direct = False
            meter_array.size = j_meter["size"]
        meter_array.type_ = MeterType.from_str(j_meter["type"])
        meter_array.rate_count = j_meter["rate_count"]

    for j_counter in get_json_key("counter_arrays"):
        counter_array = CounterArrayMulti(j_counter["name"], j_counter["id"], context_id)
        counter_array.is_direct = j_counter["is_direct"]
        if counter_array.is_direct:
            counter_array.binding = j_counter["binding"]
        else:
            counter_array.size = j_counter["size"]

    for j_register in get_json_key("register_arrays"):
        register_array = RegisterArrayMulti(j_register["name"], j_register["id"], context_id)
        register_array.size = j_register["size"]
        register_array.width = j_register["bitwidth"]

    for j_calc in get_json_key("calculations"):
        calc_name = j_calc["name"]
        if j_calc["algo"] == "crc16_custom":
            context.CUSTOM_CRC_CALCS[calc_name] = 16
        elif j_calc["algo"] == "crc32_custom":
            context.CUSTOM_CRC_CALCS[calc_name] = 32

    for j_parse_vset in get_json_key("parse_vsets"):
        parse_vset = ParseVSetMulti(j_parse_vset["name"], j_parse_vset["id"], context_id)
        parse_vset.bitwidth = j_parse_vset["compressed_bitwidth"]

    if architecture_spec is not None:
        # call architecture specific json parsing code
        architecture_spec(json_)

    # Builds a dictionary mapping (object type, unique suffix) to the object
    # (Table, Action, etc...). In P4_16 the object name is the fully-qualified
    # name, which can be quite long, which is why we accept unique suffixes as
    # valid identifiers.
    # Auto-complete does not support suffixes, only the fully-qualified names,
    # but that can be changed in the future if needed.
    suffix_count = Counter()
    for res_type, res_dict in [
            (ResType.table, context.TABLES), (ResType.action_prof, context.ACTION_PROFS),
            (ResType.action, context.ACTIONS), (ResType.meter_array, context.METER_ARRAYS),
            (ResType.counter_array, context.COUNTER_ARRAYS),
            (ResType.register_array, context.REGISTER_ARRAYS),
            (ResType.parse_vset, context.PARSE_VSETS)]:
        for name, res in res_dict.items():
            suffix = None
            for s in reversed(name.split('.')):
                suffix = s if suffix is None else s + '.' + suffix
                key = (res_type, suffix)
                context.SUFFIX_LOOKUP_MAP[key] = res
                suffix_count[key] += 1
    for key, c in suffix_count.items():
        if c > 1:
            del context.SUFFIX_LOOKUP_MAP[key]
    
            
def handle_bad_input(f):
    @wraps(f)
    @runtime_CLI.handle_bad_input
    def handle(*args, **kwargs):
        try:
            return f(*args, **kwargs)
        except UIn_MatchKeyError as e:
            print("Invalid match key:", e)
        except UIn_RuntimeDataError as e:
            print("Invalid runtime data:", e)
        except UIn_Error as e:
            print("Error:", e)
        except InvalidTableOperation as e:
            error = TableOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid table operation ({})".format(error))
        except InvalidCounterOperation as e:
            error = CounterOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid counter operation ({})".format(error))
        except InvalidMeterOperation as e:
            error = MeterOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid meter operation ({})".format(error))
        except InvalidRegisterOperation as e:
            error = RegisterOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid register operation ({})".format(error))
        except InvalidLearnOperation as e:
            error = LearnOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid learn operation ({})".format(error))
        except InvalidSwapOperation as e:
            error = SwapOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid swap operation ({})".format(error))
        except InvalidDevMgrOperation as e:
            error = DevMgrErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid device manager operation ({})".format(error))
        except InvalidCrcOperation as e:
            error = CrcErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid crc operation ({})".format(error))
        except InvalidToeplitzHashOperation as e:
            error = ToeplitzHashErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid Toeplitz hash operation ({})".format(error))
        except InvalidParseVSetOperation as e:
            error = ParseVSetOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid parser value set operation ({})".format(error))
        except InvalidMirroringOperation as e:
            error = MirroringOperationErrorCode._VALUES_TO_NAMES[e.code]
            print("Invalid mirroring operation (%s)" % error)
    return handle

class SimpleSwitchAPI(runtime_CLI.RuntimeAPI):
    @staticmethod
    def get_thrift_services():
        return [("simple_switch", SimpleSwitch.Client)]

    def __init__(self, pre_type, standard_client, mc_client, sswitch_client):
        runtime_CLI.RuntimeAPI.__init__(self, pre_type,
                                        standard_client, mc_client)
        self.sswitch_client = sswitch_client

    @handle_bad_input
    def do_set_queue_depth(self, line):
        "Set depth of one / all egress queue(s): set_queue_depth <nb_pkts> [<egress_port> [<priority>]]"
        args = line.split()
        self.at_least_n_args(args, 1)
        depth = self.parse_int(args[0], "nb_pkts")
        if len(args) > 2:
            port = self.parse_int(args[1], "egress_port")
            priority = self.parse_int(args[2], "priority")
            self.sswitch_client.set_egress_priority_queue_depth(port, priority, depth)
        elif len(args) == 2:
            port = self.parse_int(args[1], "egress_port")
            self.sswitch_client.set_egress_queue_depth(port, depth)
        else:
            self.sswitch_client.set_all_egress_queue_depths(depth)

    @handle_bad_input
    def do_set_queue_rate(self, line):
        "Set rate of one / all egress queue(s): set_queue_rate <rate_pps> [<egress_port> [<priority>]]"
        args = line.split()
        self.at_least_n_args(args, 1)
        rate = self.parse_int(args[0], "rate_pps")
        if len(args) > 2:
            port = self.parse_int(args[1], "egress_port")
            priority = self.parse_int(args[2], "priority")
            self.sswitch_client.set_egress_priority_queue_rate(port, priority, rate)
        elif len(args) == 2:
            port = self.parse_int(args[1], "egress_port")
            self.sswitch_client.set_egress_queue_rate(port, rate)
        else:
            self.sswitch_client.set_all_egress_queue_rates(rate)

    @handle_bad_input
    def do_mirroring_add(self, line):
        "Add mirroring session to unicast port: mirroring_add <mirror_id> <egress_port>"
        args = line.split()
        self.exactly_n_args(args, 2)
        mirror_id = self.parse_int(args[0], "mirror_id")
        egress_port = self.parse_int(args[1], "egress_port")
        config = MirroringSessionConfig(port=egress_port)
        self.sswitch_client.mirroring_session_add(mirror_id, config)

    @handle_bad_input
    def do_mirroring_add_mc(self, line):
        "Add mirroring session to multicast group: mirroring_add_mc <mirror_id> <mgrp>"
        args = line.split()
        self.exactly_n_args(args, 2)
        mirror_id = self.parse_int(args[0], "mirror_id")
        mgrp = self.parse_int(args[1], "mgrp")
        config = MirroringSessionConfig(mgid=mgrp)
        self.sswitch_client.mirroring_session_add(mirror_id, config)

    @handle_bad_input
    def do_mirroring_delete(self, line):
        "Delete mirroring session: mirroring_delete <mirror_id>"
        args = line.split()
        self.exactly_n_args(args, 1)
        mirror_id = self.parse_int(args[0], "mirror_id")
        self.sswitch_client.mirroring_session_delete(mirror_id)

    @handle_bad_input
    def do_mirroring_get(self, line):
        "Display mirroring session: mirroring_get <mirror_id>"
        args = line.split()
        self.exactly_n_args(args, 1)
        mirror_id = self.parse_int(args[0], "mirror_id")
        config = self.sswitch_client.mirroring_session_get(mirror_id)
        print(config)

    @handle_bad_input
    def do_get_time_elapsed(self, line):
        "Get time elapsed (in microseconds) since the switch started: get_time_elapsed"
        print(self.sswitch_client.get_time_elapsed_us())

    @handle_bad_input
    def do_get_time_since_epoch(self, line):
        "Get time elapsed (in microseconds) since the switch clock's epoch: get_time_since_epoch"
        print(self.sswitch_client.get_time_since_epoch_us())

    @handle_bad_input
    def do_show_tables_multi(self, line):
        "List tables defined in the P4 program: show_tables"
        self.exactly_n_args(line.split(), 1)
        args = line.split()
        context_id = int(args[0]) - 1
        context = contexts[context_id]
        for table_name in sorted(context.TABLES):
            print(context.TABLES[table_name].table_str())

    @handle_bad_input
    def do_show_actions_multi(self, line):
        "List actions defined in the P4 program: show_actions"
        self.exactly_n_args(line.split(), 1)
        args = line.split()
        context_id = int(args[0])
        context = contexts[context_id]
        self.exactly_n_args(line.split(), 1)
        for action_name in sorted(context.ACTIONS):
            print(context.ACTIONS[action_name].action_str())

    @handle_bad_input
    def do_table_add_multi(self, line):
        args = line.split()
        self.at_least_n_args(args, 4)
    
        "Add entry to a match table: table_add <table name> <action name> <match fields> => <action parameters> [priority]"

        table_name, action_name = args[1], args[2]
        context_id = int(args[0]) - 1
        table = self.get_res_multi("table", table_name, ResType.table, context_id)
        action = table.get_action(action_name)

        if action is None:
            raise UIn_Error(
                "Table %s has no action %s" % (table_name, action_name)
            )

        if table.match_type in {MatchType.TERNARY, MatchType.RANGE}:
            try:
                priority = int(args.pop(-1))
            except:
                raise UIn_Error(
                    "Table is ternary, but could not extract a valid priority from args"
                )
        else:
            priority = 0

        for idx, input_ in enumerate(args[3:]):
            if input_ == "=>":
                break
        idx += 3
        match_key = args[3:idx]
        action_params = args[idx + 1:]
        if len(match_key) != table.num_key_fields():
            raise UIn_Error(
                "Table %s needs %d key fields" % (
                    table_name, table.num_key_fields())
            )

        runtime_data = self.parse_runtime_data(action, action_params)

        match_key = parse_match_key(table, match_key)

        print("Adding entry to", MatchType.to_str(
            table.match_type), "match table", table_name)

        # disable, maybe a verbose CLI option?
        self.print_table_add(match_key, action_name, runtime_data)

        entry_handle = self.client.bm_mt_add_entry(
            context_id, table.name, match_key, action.name, runtime_data,
            BmAddEntryOptions(priority=priority)
        )

        print("Entry has been added with handle", entry_handle)

    @handle_bad_input
    def do_table_dump_multi(self, line):
        "Display entries in a match-table: table_dump <table name>"
        args = line.split()
        self.exactly_n_args(args, 2)
        context_id = int(args[0]) - 1
        table_name = args[1]
        print(table_name)
        table = self.get_res_multi("table", table_name, ResType.table, context_id)
        entries = self.client.bm_mt_get_entries(context_id, table.name)

        print("==========")
        print("TABLE ENTRIES")

        for e in entries:
            print("**********")
            self.dump_one_entry(table, e)

        if table.type_ == TableType.indirect or\
           table.type_ == TableType.indirect_ws:
            assert(table.action_prof is not None)
            self._dump_act_prof(table.action_prof)

        # default entry
        default_entry = self.client.bm_mt_get_default_entry(context_id, table.name)
        print("==========")
        print("Dumping default entry")
        self.dump_action_entry(default_entry)

        print("==========")

    @staticmethod
    def get_thrift_services_multi(pre_type):
        services = [("standard", Standard.Client)]

        if pre_type == PreType.SimplePre:
            services += [("simple_pre", SimplePre.Client)]
        elif pre_type == PreType.SimplePreLAG:
            services += [("simple_pre_lag", SimplePreLAG.Client)]
        else:
            services += [(None, None)]

        return services
    
    def thrift_connect_multi(thrift_ip, thrift_port, services):
        return utils.thrift_connect(thrift_ip, thrift_port, services)
    
    def load_json_config_multi(standard_client=None, json_path=None, architecture_spec=None):
        for context_id in range(4):
            load_json_str_multi(utils.get_json_config_context(
                context_id,standard_client, json_path), context_id, architecture_spec)

    def get_res_multi(self, type_name, name, res_type, context_id):
        key = res_type, name
        context = contexts[context_id]
        if key not in context.SUFFIX_LOOKUP_MAP:
            raise UIn_ResourceError(type_name, name)
        return context.SUFFIX_LOOKUP_MAP[key]

def main():
    args = runtime_CLI.get_parser().parse_args()

    args.pre = runtime_CLI.PreType.SimplePreLAG


    services = SimpleSwitchAPI.get_thrift_services_multi(args.pre)
    services.extend(SimpleSwitchAPI.get_thrift_services())
    
    
    standard_client, mc_client, sswitch_client = SimpleSwitchAPI.thrift_connect_multi(
        args.thrift_ip, args.thrift_port, services
    )

    runtime_CLI.load_json_config(standard_client, args.json)

    SimpleSwitchAPI.load_json_config_multi(standard_client, args.json)

    SimpleSwitchAPI(args.pre, standard_client, mc_client, sswitch_client).cmdloop()

if __name__ == '__main__':
    main()
