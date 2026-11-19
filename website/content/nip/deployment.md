---
title: "NIP 安装部署指南"
date: 2026-05-01
tags: NIP,部署,安装,CentOS,内核模块
summary: "NIP 部署步骤：内核版本检查、nip.conf 网卡/IP 配置、授权码加载、nip_ctl init、safer_manager + cpd_web 启动、systemd 服务化。"
slug: deployment
ord: 30
---
1. 查看 uname -a 是不是满足 3.10.0-1160.119.1.el7.x86_64 ，不满足的话，需要yum update kernel升级到centos 7.9 最后一版

2. 将我们的压缩包复制到 /home/下  tar zxvf nway.3.4.0_af.tgz,
确保我们的目录类似以下：

```
 ll /home/nway/
total 61824
-rw-r--r-- 1 root root      448 May 16 17:16 guard.ini
-rw-r--r-- 1 root root   366990 May 16 17:16 human.wav
-rwxr-xr-x 1 root root      686 May 16 17:16 install_service.sh
-rw-r--r-- 1 root root 13618918 May 16 17:16 libnway_auth_lib.so
drwxr-xr-x 2 root root       22 May 18 07:28 logs
-rw-r--r-- 1 root root     3341 May 16 17:16 mylog.log
-rw-r--r-- 1 root root     6066 May 18 07:21 nip.conf
-rwxr-xr-x 1 root root  4309880 May 16 17:16 nip_cpd
-rwxr-xr-x 1 root root  8069776 May 16 17:16 nip_ctl
-rwxr-xr-x 1 root root  8069776 May 16 17:16 nip_ctl.20260430
-rwxr-xr-x 1 root root  8041104 May 16 17:16 nip_ctl.af
-rwxr-xr-x 1 root root    14472 May 16 17:16 nip_logd
-rwxr-xr-x 1 root root    19520 May 16 17:16 nip_pcapd
-rwxr-xr-x 1 root root  5669376 May 16 17:16 nip_recdd
-rwxr-xr-x 1 root root  3879424 May 16 17:16 nip_recdd.20260430
-rwxr-xr-x 1 root root  2590504 May 16 17:16 nip_recdd.af
-rw------- 1 root root      201 May 16 22:28 nohup.out
drwxr-xr-x 2 root root      235 May 16 17:52 nway
-rw-r--r-- 1 root root      376 May 16 17:16 nway_guard.service
-rwxr-xr-x 1 root root  5656760 May 16 17:16 nway_machine_id
-rw-r--r-- 1 root root   320108 May 16 17:16 output.wav
-rw-r--r-- 1 root root   364110 May 16 17:16 record.wav
-rwxr-xr-x 1 root root  2240696 May 16 17:16 safer_guard
drwxr-xr-x 3 root root      129 May 17 19:48 safer_manager
-rwxr-xr-x 1 root root       54 May 16 17:16 start_pcap.sh
-rwxr-xr-x 1 root root       40 May 16 17:16 stop_pcap.sh
```


3. 通过 ip a 查看网卡和ip以及预先了解对接的公网ip,  在我们的 nip.conf下 [general]下
```
[general]
local_ip=本机的公网或内网ip,可以多个
interface = eth0 这里是指要进行处理的网卡名
```

1. 修改好后保存，然后获取机器码，在 ./nway_machine_id,由于之前使用了一个外网探测本机外网地址的external.org之类的地址，如果不通会响应得慢，但这个不影响。

2. 将授权码写入：/home/nway/nway/license.txt中

3. 测试是否有效 ./nip_ctl init,如果可以看到正常加载那么就成功了，当然如果本机不允许访问external.org的话，可能会稍慢。检测是不是加载成功有两种 lsmod | grep nip 后需要看到 nip_kmod 257810 2(这里2）代表有2个用户态。 另外一种是直接ps aux | grep nip查看 nip_recdd和nip_logd.


7. 修改/home/nway/safer_manager/config.ini 将其中的 [vos] 下的 api_url = xxx改为本机的vos的web的http端口，不能用https
8. 启动以上服务，那么部署成功。

9. 如何做成服务？ 运行 /home/nway/install_service.sh,我们会有个默认的safer_guard服务，会自动装载模块和safer_manager，当然这个为了不影响性能，大概会启动几分钟。

10. 如何查看是不是在正常处理解析？

a. 首先查看/var/log/nip_kernel.log这里是加载内核模块日志, dmesg 会输出一些。当然，这个部分作为排查内核不能加载的问题。

b. 如何加载模块，正常的一定要先调用一下卸载：如
```
./nip_ctl stop
NIP Control Tool nip_quick_3.0.5
Attempting to stop capture daemon...
Stopping systemd service...
Killing residual process nip_recdd (PID: 26972)...
Killing residual process nip_logd (PID: 26929)...
Unloading kernel module...
Module unloaded successfully.
Module unloaded
```


然后
```
./nip_ctl init
NIP Control Tool nip_quick_3.0.5
Parsed SIP port ranges: 23061,23063
Parsed RTP port ranges: 10000-50000
rmmod: ERROR: Module nip_kmod is not currently loaded
Authorization successful. License expiration: 1780200000000 ms
Send expire time: 1780200000000
Expiration time successfully set in kernel module.
Module loaded from /tmp/nip_kmod.ko
Parsed SIP port ranges: 23061,23063
Parsed RTP port ranges: 10000-50000
Applying SIP port ranges: 23061,23063
Applied 2 SIP port ranges.
Applying RTP port ranges: 10000-50000
Sending RTP port range 10000-50000 to kernel, result: 0
Applied 1 RTP port ranges.
Loaded 5 lines from ./nway/custom_white_ip.txt
Loaded 0 lines from ./nway/ip_black.txt
Loaded 5483 lines from ./nway/region_white_ip.txt
Loaded 0 lines from ./nway/src_black.txt
Loaded 1 lines from ./nway/dst_black.txt
Loaded 66 lines from ./nway/ua_black.txt
Loaded 1 lines from ./nway/ua_white.txt
Attempting to start nip_logd...
Attempting to start nip_recdd with command: nohup /home/nway/nip_recdd >/dev/null 2>&1 &
nip_pcapd started
Apply success (init+config: ./nip.conf).
```


以上模块就加载成功了。最主要是ps aux | grep nip_recdd 要有这个进程

c. 如何输出日志
查看是不是在处理包的日志

如：`tailf logs/recd.log`
```
18:03:50.940 INFO  nip_recdd.c:3440: release call: 0155b108d63616c6c0db15a45@150.242.80.170, from: 651203, to: 120617391517171
18:03:51.059 INFO  nip_recdd.c:4461: [SIP-INVITE] A-leg identified: 4692934f-0a4d-499a-ad86-502f341c6eb0 (To: 10.0.0.3)
18:03:51.095 INFO  nip_recdd.c:4461: [SIP-INVITE] A-leg identified: 8aaf499f-c730-43b6-bae1-d1a90f28ae2b (To: 10.0.0.3)
18:03:51.385 INFO  nip_recdd.c:3440: release call: b3c5be95-1bf1-4f37-807a-b9ef9ebdc11a, from: 641088, to: 108813762148513
18:03:51.404 INFO  nip_recdd.c:4461: [SIP-INVITE] A-leg identified: 3e22e2ee-f21a-4d09-b4d1-2e38febfa81c (To: 10.0.0.3)
18:03:51.796 INFO  nip_recdd.c:4461: [SIP-INVITE] A-leg identified: 913f471f963616c6c0db15a4f@150.242.80.170 (To: 10.0.0.3)
```

d. 查看如何和vos间交互
主要是`/home/nway/safer_manager/safer_manager` 进程要启动

然后查看日志：
`tailf safer_manager/logs/sm.log`
```
2026/05/26 18:05:19 [INFO] handler.go:326 [CPD] call=5c1feb29-1ba7-49d2-9c4e-aac780d4bca4 type=正在通话中 sim=0.9292 from=641088 to=x, to_ip=, to_port=0, cpd_duration=7826,pkt count=337, counter since startup:18608 times
2026/05/26 18:05:19 [INFO] handler.go:368 [VOS] 正在通话中 detected. Disconnecting VOSID: 4053367
2026/05/26 18:05:19 [INFO] handler.go:258 [MUTE] call=9fa50e56-9884-4b22-ae87-9d58f0994b62 from= to= duration=0s msg=Continuous silence detected after answer
2026/05/26 18:05:19 [INFO] handler.go:326 [CPD] call=9fa50e56-9884-4b22-ae87-9d58f0994b62 type=mute_event sim=0.0000 from= to=, to_ip=, to_port=0, cpd_duration=0,pkt count=0, counter since startup:18608 times
2026/05/26 18:05:19 [INFO] handler.go:326 [CPD] call=321258aec63616c6c0db15eaa@150.242.80.170 type=正在通话中 sim=0.9210 from=651203 to= xxx, to_ip=, to_port=0, cpd_duration=9933,pkt count=361, counter since startup:18608 times
2026/05/26 18:05:19 [INFO] handler.go:368 [VOS] 正在通话中 detected. Disconnecting VOSID: 4053355
2026/05/26 18:05:19 [INFO] handler.go:326 [CPD] call=15f7b1c2-fe3f-491e-b279-09fc1c6d6ace type=停机 sim=0.9262 from=641088 to=      , to_ip=, to_port=0, cpd_duration=4795,pkt count=199, counter since startup:18608 times
2026/05/26 18:05:19 [INFO] handler.go:368 [VOS] 停机 detected. Disconnecting VOSID: 4053397
2026/05/26 18:05:19 [INFO] handler.go:326 [CPD] call=f5cc97ca-af17-4bbb-af7d-741b0b4a2667 type=正在通话中 sim=0.9266 from=641088 to=         , to_ip=, to_port=0, cpd_duration=16667,pkt count=800, counter since startup:18608 times
2026/05/26 18:05:19 [INFO] handler.go:368 [VOS] 正在通话中 detected. Disconnecting VOSID: 4053280
2026/05/26 18:05:20 [INFO] vos_manager.go:174 [VOS-Report] DisconnectCurrentCall:url:http://127.0.0.1:4175/external/server/DisconnectCurrentCall, body: {"callIds":[4053367,4053355,4053397,4053280]}
```
