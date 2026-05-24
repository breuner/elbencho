#!/usr/bin/env python3

import sys

# Try importing the required AWS SDK modules
try:
    import boto3
    from botocore.exceptions import ClientError
    from botocore.config import Config
except ImportError:
    print("Error: Required AWS SDK modules ('boto3' or 'botocore') are not installed.", file=sys.stderr)
    print("\nPlease install boto3 to use this script. For example:", file=sys.stderr)
    print("  Ubuntu/Debian:  sudo apt install python3-boto3", file=sys.stderr)
    print("  RHEL & friends: sudo dnf install python3-boto3", file=sys.stderr)
    print("  Virtual Env:    pip install boto3\n", file=sys.stderr)
    sys.exit(1)

import argparse
import time
from datetime import datetime, timezone, timedelta
from concurrent.futures import ProcessPoolExecutor, as_completed

def get_args():
    parser = argparse.ArgumentParser(
        description="Multi-threaded cleanup of unfinished S3 multi-part uploads that are older than a given age. "
            "Credentials can be provided via env vars AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY."
    )
    parser.add_argument("--bucket", required=True, help="S3 bucket name.")
    parser.add_argument("--minutes", type=int, required=True,
        help="Delete unfinished uploads older than this number of minutes. (Can be '0'.)")
    parser.add_argument("--endpoint-url", type=str, default=None, help="Custom S3 endpoint URL.")
    parser.add_argument("--workers", type=int, default=4, help="Number of worker threads.")
    parser.add_argument("--info", action="store_true", help="Info mode: Scan and list matching uploads with part counts, but do not delete.")
    return parser.parse_args()

def count_parts(s3_client, bucket, key, upload_id):
    """Helper to get the number of uploaded parts for a specific multi-part upload."""
    try:
        paginator = s3_client.get_paginator('list_parts')
        total_parts = 0
        for page in paginator.paginate(Bucket=bucket, Key=key, UploadId=upload_id):
            total_parts += len(page.get('Parts', []))
        return total_parts
    except ClientError as e:
        print(f"Error getting parts for {key}: {e}", file=sys.stderr)
        return 0

def init_worker(endpoint_url):
    """
    Initializer for each worker process.
    We create a session/client HERE so each process has its own isolated connection pool.
    """
    global s3_client
    # Optimized config: Retries off (fail fast), max pool match worker (though 1 per process is fine)
    boto_config = Config(
        max_pool_connections=10,
        retries={'max_attempts': 2, 'mode': 'standard'}
    )
    # Re-create the client inside the process to ensure thread/fork safety
    s3_client = boto3.client('s3', endpoint_url=endpoint_url, config=boto_config)

def abort_upload_task(bucket, key, upload_id):
    """
    The actual task run by the worker. Uses the global client created in init_worker.
    """
    try:
        s3_client.abort_multipart_upload(
            Bucket=bucket,
            Key=key,
            UploadId=upload_id
        )
        return True
    except ClientError as e:
        return f"Error: {e}"

def main():
    args = get_args()

    # 1. SCAN PHASE (Single Threaded)
    # We use a single client here just to list the work.
    main_s3 = boto3.client('s3', endpoint_url=args.endpoint_url)
    now_utc = datetime.now(timezone.utc)
    cutoff_date = now_utc - timedelta(minutes=args.minutes)

    mode_str = "Scanning (INFO MODE)" if args.info else "Scanning"
    print(f"--- {mode_str} bucket '{args.bucket}' ---")
    paginator = main_s3.get_paginator('list_multipart_uploads')

    tasks = []
    try:
        for page in paginator.paginate(Bucket=args.bucket):
            if 'Uploads' in page:
                for upload in page['Uploads']:
                    if upload['Initiated'] < cutoff_date:
                        if args.info:
                            age_minutes = int((now_utc - upload['Initiated']).total_seconds() / 60)
                            part_count = count_parts(main_s3, args.bucket, upload['Key'], upload['UploadId'])
                            print(f"Key: {upload['Key']} | Age: {age_minutes} mins | Parts uploaded: {part_count}")
                        else:
                            tasks.append((args.bucket, upload['Key'], upload['UploadId']))
    except ClientError as e:
        print(f"Error accessing bucket: {e}")
        sys.exit(1)

    if args.info:
        print("\n--- Info mode finished. No uploads were deleted. ---")
        sys.exit(0)

    total_tasks = len(tasks)
    print(f"Found {total_tasks} incomplete uploads to abort.")

    if total_tasks == 0:
        sys.exit(0)

    # 2. DELETE PHASE (Multi-Process)
    print(f"Starting deletion with {args.workers} processes...")
    start_time = time.time()

    # ProcessPoolExecutor creates completely separate python processes
    # max_workers should typically be <= (CPU Cores * 2)
    with ProcessPoolExecutor(max_workers=args.workers, initializer=init_worker, initargs=(args.endpoint_url,)) as executor:
        futures = [executor.submit(abort_upload_task, *task) for task in tasks]

        success_count = 0
        for future in as_completed(futures):
            result = future.result()
            if result is True:
                success_count += 1

            if success_count % 100 == 0:
                print(f"Progress: {success_count}/{total_tasks}...", end='\r')

    duration = time.time() - start_time
    print(f"\n--- Done in {duration:.2f}s ({success_count / duration:.1f} deletes/sec) ---")

if __name__ == "__main__":
    main()

