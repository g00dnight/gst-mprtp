#!/bin/bash
programname=$0

NSSND="ns_snd"
NSRCV="ns_rcv"
SENDER="sender"
RECEIVER="receiver"
LOGSDIR="logs"
REPORTSDIR="reports"
SCRIPTSDIR="scripts2"
TEMPDIR=$SCRIPTSDIR"/temp"
EVALDIR=$SCRIPTSDIR"/evals"
CONFDIR=$SCRIPTSDIR"/configs"
RUNDIR=$SCRIPTSDIR"/runs"

rm -R -f $LOGSDIR/*

#setup defaults
DURATION=120
OWD_SND=100
OWD_RCV=100

  rm $TEMPDIR/peer1/*
  rm $TEMPDIR/peer2/*
  rm $TEMPDIR/peer3/*

while [[ $# -gt 1 ]]
do
key="$1"
case $key in
    -s|--owdsnd)
    OWD_SND="$2"
    shift # past argument
    ;;
    -r|--owdrcv)
    OWD_RCV="$2"
    shift # past argument
    ;;
    *)
            # unknown option
    ;;
esac
shift # past argument or value
done



  sudo ip netns exec ns_mid tc qdisc change dev veth2 root handle 1: netem delay "$OWD_SND"ms
  sudo ip netns exec ns_mid tc qdisc change dev veth1 root handle 1: netem delay "$OWD_RCV"ms

  PEER1_SND="$TEMPDIR/sender_1.sh"
  echo "ntrt -c$CONFDIR/ntrt_snd_meas.ini -m$CONFDIR/ntrt_snd_rmcat3.cmds -t$DURATION &" > $PEER1_SND
  echo -n "./$SENDER" >> $PEER1_SND
  ./$CONFDIR/peer1params.sh >> $PEER1_SND
  chmod 777 $PEER1_SND

  PEER2_SND="$TEMPDIR/sender_2.sh"
  echo -n "./$SENDER" > $PEER2_SND
  ./$CONFDIR/peer2params_ns_rcv.sh >> $PEER2_SND
  chmod 777 $PEER2_SND

  PEER1_RCV="$TEMPDIR/receiver_1.sh"
  echo "ntrt -c$CONFDIR/ntrt_rcv_meas.ini -m$CONFDIR/ntrt_rcv_rmcat3.cmds -t$DURATION &" > $PEER1_RCV
  echo -n "./$RECEIVER" >> $PEER1_RCV
  ./$CONFDIR/peer1params.sh >> $PEER1_RCV
  echo -n "--save_received_yuvfile=0 " >> $PEER1_RCV 
  chmod 777 $PEER1_RCV

  PEER2_RCV="$TEMPDIR/receiver_2.sh"
  echo -n "./$RECEIVER" > $PEER2_RCV
  ./$CONFDIR/peer2params_ns_rcv.sh >> $PEER2_RCV
  chmod 777 $PEER2_RCV

  #start receiver and sender
  sudo ip netns exec $NSRCV ./$PEER1_RCV 2> $LOGSDIR"/"receiver.log &
  sudo ip netns exec $NSSND ./$PEER2_RCV 2> $LOGSDIR"/"receiver_2.log &
  sleep 2
  sudo ip netns exec $NSSND ./$PEER1_SND 2> $LOGSDIR"/"sender.log &
  sudo ip netns exec $NSRCV ./$PEER2_SND 2> $LOGSDIR"/"sender_2.log &

cleanup()
# example cleanup function
{
  pkill receiver
  pkill sender
  pkill ntrt
  ps -ef | grep 'main.sh' | grep -v grep | awk '{print $2}' | xargs kill
}
 
control_c()
# run if user hits control-c
{
  echo -en "\n*** Program is terminated ***\n"
  cleanup
  exit $?
}

trap control_c SIGINT

sleep $DURATION

cleanup


