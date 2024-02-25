#!/bin/sh

cd ../../

./simple_switch_CLI

#table_add_multi 1 ipv4_match to_port_action 192.168.1.6/32 => 30

#table_add_multi 2 ipv4_match to_port_action 192.168.1.4/32 => 10

#table_add_multi 4 ipv4_lpm ipv4_forward 192.168.1.2/32 => 00:11:22:33:44:55 0

#table_add_multi 4 ipv4_lpm ipv4_forward 192.168.1.4/32 => 00:11:22:33:44:77 10

#p = Ether()/IP(dst="192.168.1.6")/UDP()
#sendp(p, iface="veth0")

#p = Ether()/IP(dst="192.168.1.4")/UDP()
#sendp(p, iface="veth2")

#p = Ether()/IP(dst="192.168.1.2")/UDP()
#sendp(p, iface="veth4")

#p = Ether()/IP(dst="192.18.1.4")/UDP()
#sendp(p, iface="veth4")

#sudo tcpdump -n -i veth0

#sudo tcpdump -n -i veth2

#sudo tcpdump -n -i veth4