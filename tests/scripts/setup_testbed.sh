#!/bin/sh

#MpRTP networking, thanks to Jesús Llorente Santos for the help

PATH1_VETH0_S="veth0"
PATH1_VETH0_R="veth1"

NS_SND="ns_snd"
NS_RCV="ns_rcv"

#Remove existing namespace
sudo ip netns del $NS_SND
sudo ip netns del $NS_RCV

echo "Configure network subflows"
#Remove existing veth pairs
sudo ip link del $PATH1_VETH0_S

#Create veth pairs
sudo ip link add $PATH1_VETH0_S type veth peer name $PATH1_VETH0_R

#Bring up
sudo ip link set dev $PATH1_VETH0_S up
sudo ip link set dev $PATH1_VETH0_R up


#Create the specific namespaces
sudo ip netns add $NS_SND
sudo ip netns add $NS_RCV

#Move the interfaces to the namespace
sudo ip link set $PATH1_VETH0_S netns $NS_SND
sudo ip link set $PATH1_VETH0_R netns $NS_RCV

#Configure the loopback interface in namespace
sudo ip netns exec $NS_SND ip address add 127.0.0.1/8 dev lo
sudo ip netns exec $NS_SND ip link set dev lo up
sudo ip netns exec $NS_RCV ip address add 127.0.0.1/8 dev lo
sudo ip netns exec $NS_RCV ip link set dev lo up

#Bring up interface in namespace
sudo ip netns exec $NS_SND ip link set dev $PATH1_VETH0_S up
sudo ip netns exec $NS_SND ip address add 10.0.0.1/24 dev $PATH1_VETH0_S
sudo ip netns exec $NS_RCV ip link set dev $PATH1_VETH0_R up
sudo ip netns exec $NS_RCV ip address add 10.0.0.2/24 dev $PATH1_VETH0_R

sudo ip netns exec $NS_SND "./scripts/setup_ns_snd.sh"
sudo ip netns exec $NS_RCV "./scripts/setup_ns_rcv.sh"


