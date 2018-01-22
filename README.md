is-sync
===
This repository contains a service that syncs resources from different entities. 
All entities must publish their Timestamp and also be capable to set a delay at one sampling period.
This service will only work if the clock of all computer nodes are synced. For that we recommend using [chrony](https://chrony.tuxfamily.org/) - a versatile implementation of the Network Time Protocol (NTP).

- Install chrony on all computer nodes:
```shell
$ sudo apt install chrony
```

- Choose one node to be the reference (i.e 10.60.0.3)

- On the **reference node** add **allow 0/0** in /etc/chrony/chrony.conf to allow any user to connect to the server:
```diff
+ allow 0/0
```

- On **other nodes** edit /etc/chrony/chrony.conf and remove all lines that start with *pool* and add one with our reference node:
```diff
- pool 2.debian.pool.ntp.org offline iburst
+ pool 10.60.0.3 iburst
```

- Restart chrony on every node:
```shell
$ sudo service chrony restart
```

- Check the synchronism by running:
```shell
$ sudo chronyc sources

210 Number of sources = 1
MS Name/IP address         Stratum Poll Reach LastRx Last sample
===============================================================================
^* 10.60.0.3                     3   6    37    12  +8468ns[ +361us] +/-   14ms
 ```