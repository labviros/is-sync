is-sync
===
This repository contains a service that syncs resources from different entities. All entities must publish their Timestamp and also be capable to set a delay at one sampling period. This service will only works if all computer nodes are synced. For that, we recommend using a [NTP](http://www.ntp.org/), which is a protocol that synchronize the clocks of computers over a network. The command below install the necessary dependencies to use NTP.

```shell
$ sudo apt-get install ntp ntpdate
```

After installing the ntp configuration file (*/etc/ntp.conf*) must be modified. Comment all lines that starts with **pool** and add **server IP_ADDRESS** at the top. This ip address belongs to a machine that will be used as time reference. To accelerate the sync process, we also provide on this repository a [script](https://github.com/labviros/is-sync/blob/master/ntp-sync) that forces computer time adjustment and monitor (see below) offset from source. An offset less than 1 millisecond is acceptable to run all applications.

```shell
     remote         refid    st t when poll reach   delay   offset  jitter
==========================================================================
 edge          LOCAL(0)       6 u   19   64    1    0.212   -0.175   0.000
 ```