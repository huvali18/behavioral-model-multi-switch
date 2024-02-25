#!/bin/sh

#set up virtual ethernet adapters

#veth0-veth1

sudo ip link add name veth0 type veth peer name veth1
sudo ip link set dev veth0 address 00:11:22:33:44:55
sudo ip link set dev veth1 address 00:11:22:33:44:66
sudo ip addr add 192.168.1.1/24 dev veth0
sudo ip addr add 192.168.1.2/24 dev veth1
sudo ip link set dev veth0 up
sudo ip link set dev veth1 up
sudo ip link set veth0 mtu 9500
sudo ip link set veth1 mtu 9500
sudo sysctl net.ipv6.conf.veth0.disable_ipv6=1
sudo sysctl net.ipv6.conf.veth1.disable_ipv6=1


#veth2-veth3

sudo ip link add name veth2 type veth peer name veth3
sudo ip link set dev veth2 address 00:11:22:33:44:77
sudo ip link set dev veth3 address 00:11:22:33:44:88
sudo ip addr add 192.168.1.3/24 dev veth2
sudo ip addr add 192.168.1.4/24 dev veth3
sudo ip link set dev veth2 up
sudo ip link set dev veth3 up
sudo ip link set veth2 mtu 9500
sudo ip link set veth3 mtu 9500
sudo sysctl net.ipv6.conf.veth2.disable_ipv6=1
sudo sysctl net.ipv6.conf.veth3.disable_ipv6=1

#veth4-veth5

sudo ip link add name veth4 type veth peer name veth5
sudo ip link set dev veth4 address 00:11:22:33:44:99
sudo ip link set dev veth5 address 00:11:22:33:44:00
sudo ip addr add 192.168.1.5/24 dev veth4
sudo ip addr add 192.168.1.6/24 dev veth5
sudo ip link set dev veth4 up
sudo ip link set dev veth5 up
sudo ip link set veth4 mtu 9500
sudo ip link set veth5 mtu 9500
sudo sysctl net.ipv6.conf.veth4.disable_ipv6=1
sudo sysctl net.ipv6.conf.veth5.disable_ipv6=1

cd ../../
./multi_switch -i 0@veth1 -i 10@veth3 -i 30@veth5 --log-file switch_log.txt test/standard/pipe1.json test/standard/pipe2.json test/standard/pipe3.json test/standard/pipe4.json
