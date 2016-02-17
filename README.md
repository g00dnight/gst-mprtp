# gst-mprtp

**What is it?**
  
Multipath RTP (MPRTP) extends RTP header in order to 
be used for splitting a consequent media stream amongst 
several subflow. Thus it is used for transmitting 
a coherent media stream on different path parallely. 

**The Latest Version**

Details of the latest version can be found at 
https://github.com/multipath-rtp/gst-mprtp.

**Installation**

gst-mprtp is a gstreamer plugin. In order to use it
you need gstreamer, gst-plugins-base and gst-plugins-good.
For pipelining details see tests/server and client 
applications.
  
If you have the requirements then simply give the
sudo make install command in bash.
  
**Tests**

For testing the plugin with server and client you need tc
and you need to run the following scripts:
  
1. Run gst-mprtp/setup_enviroment.sh shell script
This will create a simulation enviroment with two 
namespaces and setup 3 virtual interfaces on each 
of it along with 3 routes.
  
  
2. Enters into ns0 and ns1 bash by using 
"sudo ip netns exec [ns0|ns1] bash" command
 
3.1. Run gst-mprtp/tests/setup_ns0.sh in ns0 
3.2. Run gst-mprtp/tests/setup_ns1.sh in ns1
  
4. Run gst-mprtp/tests/run_test.sh --profile=X
For profile options run gst-mprtp/tests/server --info

**Contacts**

Balázs Krieth, Varun Singh, Jörg Ott
     
**Acknowledgements** 
  
Special thanks to Jesus Llorente Santos for writing 
the original test scripts. 
  
