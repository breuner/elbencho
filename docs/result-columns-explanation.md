# Elbencho result columns: First done, last done, tail phase

## tl;dr
The "first done" column represents the aggregate result from the beginning of the test phase until the first/fastest thread finished its fair share of the work. It could be called the "hero number". The "last done" column shows the aggregate result from the beginning until the last/slowest thread finished its fair share of the work. It is more representative for typical real-world applications that assign a fair share of a dataset to each thread or client and won't continue until the last/slowest thread finished writing or reading its fair share.

## Result example
```
[user@node001 ~]$ elbencho --hosts 192.168.0.[1,3,5] -w --direct -b 1m -s 16g -t 96 "/mnt/storage/bigfiles/file[1-256]"

OPERATION   RESULT TYPE         FIRST DONE   LAST DONE
=========== ================    ==========   =========
WRITE       Elapsed time     :    2m3.241s    2m5.755s
            IOPS             :       33395       33352
            Throughput MiB/s :       33395       33352
            Total MiB        :     4115703     4194304
```

## Basic definitions
The "first done" column represents the aggregate result from the beginning of the test phase until the last point in time when all threads were still active, so until the first/fastest thread finished its fair share of the work. "last done" is what benchmarking tools would typically call the end result, because it's the aggregate result from the beginning until the last/slowest thread finished its fair share of the work.

"Fair share" means that each thread gets the same amount of work assigned during the preparation of a benchmarking phase, so that each thread can processes its work package completely independent of the other threads to ensure that communication between the threads or between multiple clients does not become a limiting factor. For example, if a 1GB file should be written with two threads then each thread will get exactly a half GB to write.

## Tail phase
In a perfect world, "first done" and "last done" would be the same, meaning that the system was perfectly fair for all threads on all clients. The reality is that there is a lot in the I/O path that leads to not perfectly fair processing of requests - at the drive level, the cpu level, the network level, the driver level and so on. Thus, a difference between the two values is normal, but ideally the difference in the throughput results is not big.

The time between "first done" and "last done" is called the "tail phase", because in a graph it's typically the phase of decreasing throughput, given that more and more threads finish their work and the remainder of threads typically cannot push the same performance anymore that was achieved when all threads were still active.

## Which result should be used?
If you're interested in the throughput that the storage system can achieve while all threads are still active (this is also often referred to as "hero number") then "first done" is the right value to look at. 

If you care about a result that is closer to typical real-world application experience (e.g. an application that needs to read a fair share of an input dataset into each client and will not continue until the last bit of that input dataset has been read) then the "last done" column is the one to look at.

## Additional notes

### Test duration and dataset size
It generally is a good idea to try to come up with test cases that run for a reaonsable amount of time (e.g. minutes instead of only a few seconds) to make sure that the result is really accurate and also represents throughput numbers that the system can deliver sustained. The size of the dataset is also relevant to ensure that the results are not biased by caches.

This might require deeper knowledge about the storage system, such as the size of caches on client and server side and whether the system respects the POSIX O_DIRECT (`elbencho --direct`) flag to bypass the caching for reads and writes.

These details might not always be available and long test phases might not always be feasible.

### Limited accuracy of "first done" for distributed tests
When using the distributed mode (`elbencho --hosts ...`) then the "first done" result has a precision of 500ms by default. (This is configurable via the `--svcupint` option.) That means for very short tests (e.g. less than 5sec runtime) it has low accuracy and should only be taken with a big grain of salt. 

The reason for the limited precision is that the "first done" result requires knowing the result from each client at the exact point in time when the first/fastest one finished its fair share. This is technically not possible in a distributed system without a small inaccuracy.

This is only a something to consider for distributed tests and only for the "first done" result. For single host tests, the inaccuracy is so small that it's neglectible. The "last done" result is also always accurate, because it can easily be calculated from the results on each individual client and does not require knowing the status of each client at a certain point in time while the test was running.
