#!/bin/sh

cd ../../

./simple_switch_CLI

#table_add_multi 1 ipv4_match to_port_action 192.168.1.6/24 => 30

#table_add_multi 2 ipv4_match to_port_action 192.168.1.4/24 => 10

#table_add_multi 3 ipv4_match to_port_action 192.168.1.2/24 => 1

#table_add_multi 4 ipv4_match to_port_action 192.168.1.6/24 => 30

#p = Ether()/IP(dst="192.168.1.6")/UDP()
#sendp(p, iface="veth0")

#p = Ether()/IP(dst="192.168.1.4")/UDP()
#sendp(p, iface="veth2")

#p = Ether()/IP(dst="192.168.1.6")/UDP()
#sendp(p, iface="veth4")

#p = Ether()/IP(dst="192.168.1.1")/UDP()
#sendp(p, iface="veth0")