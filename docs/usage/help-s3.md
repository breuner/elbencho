## elbencho --help-s3

> **_NOTE:_**  This page has been auto-generated from built-in help text  of the `elbencho` executable.

<pre><code>
S3 object storage testing. (The options here are intentionally similar to
"--help-multi" to enable multi-protocol storage testing.)

Usage: ./elbencho [OPTIONS] BUCKET [MORE_BUCKETS]

S3 Service Arguments:
  --s3endpoints arg     Comma-separated list of S3 endpoints. (Format: 
                        [http(s)://]hostname[:port])
  --s3key arg           S3 access key. (This can also be set via the 
                        AWS_ACCESS_KEY_ID env variable.)
  --s3secret arg        S3 access secret. (This can also be set via the 
                        AWS_SECRET_ACCESS_KEY env variable.)

Basic Options:
  -d [ --mkdirs ]       Create buckets. (Already existing buckets are not 
                        treated as error.)
  -w [ --write ]        Write/upload objects.
  -r [ --read ]         Read/download objects.
  --stat                Read object status attributes (size etc).
  -F [ --delfiles ]     Delete objects.
  -D [ --deldirs ]      Delete buckets.
  -t [ --threads ] arg  Number of I/O worker threads. (Default: 1)
  -n [ --dirs ] arg     Number of directories per I/O worker thread. 
                        Directories are slash-separated object key prefixes. 
                        This can be 0 to disable creation of any subdirs. 
                        (Default: 1)
  -N [ --files ] arg    Number of objects per thread per directory. (Default: 
                        1) Example: "-t2 -n3 -N4" will use 2x3x4=24 objects.
  -s [ --size ] arg     Object size. (Default: 0)
  -b [ --block ] arg    The part block size for uploads and the ranged read 
                        size for downloads. Multipart upload will automatically
                        be used if object size is larger than part block size. 
                        Each thread needs to keep one block in RAM (or multiple
                        blocks if "--iodepth" is used), so be careful with 
                        large block sizes. (Default: 1M)

Frequently Used Options:
  --s3fastget           Send downloaded objects directly to /dev/null instead 
                        of a memory buffer. This option is incompatible with 
                        any buffer post-processing options like data 
                        verification or GPU data transfer.
  --treefile arg        The path to a treefile containing a list of object 
                        names to use for shared upload or download if the 
                        object size exceeds "--sharesize".
  --lat                 Show minimum, average and maximum latency for PUT/GET 
                        operations and for complete upload/download in case of 
                        chunked transfers.

Miscellaneous Options:
  --sharesize arg       In custom tree mode or when object keys are given 
                        directly as arguments, this defines the object size as 
                        of which multiple threads are used to upload/download 
                        an object. (Default: 0, which means 32 x blocksize)
  --s3region arg        S3 region.
  --zones arg           Comma-separated list of NUMA zones to bind this process
                        to. If multiple zones are given, then worker threads 
                        are bound round-robin to the zones. (Hint: See 'lscpu' 
                        for available NUMA zones.)

Examples:
  Create bucket "mybucket":
    $ elbencho --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \
        -d mybucket

  Test 2 threads, each creating 3 directories with 4 10MiB objects inside:
    $ elbencho --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \
        -w -t 2 -n 3 -N 4 -s 10m -b 5m mybucket

  Delete objects and bucket created by example above:
    $ elbencho --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \
        -D -F -t 2 -n 3 -N 4 mybucket

  Shared upload of 4 1GiB objects via 8 threads in 16MiB blocks:
    $ elbencho --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \
        -w -t 8 -s 1g -b 16m mybucket/myobject{1..4}
</code></pre>
