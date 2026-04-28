# **Using elbencho with Kubernetes/K8s**

This guide explains how to use elbencho for a coordinated, multi-node storage benchmark on a Kubernetes cluster.

By running elbencho in service mode across multiple pods, we can use one "coordinator" pod to trigger a synchronized benchmark run against a shared storage target, e.g. a path to an arbitrary shared filesystem from the host or an NFS server.

## **Prerequisites**

* A working Kubernetes cluster.
* kubectl configured to access your cluster.
* The [multi-node-elbencho.yaml](k8s/multi-node-elbencho.yaml) and (optional) [nfs-pv-pvc.yaml](k8s/nfs-pv-pvc.yaml) files.

## **Step 1: Configure Your Storage Target**

Edit the downloaded `multi-node-elbencho.yaml` and choose **one** of the storage option examples in the volumes section by commenting/uncommenting the appropriate blocks:

* **Option 1 (hostPath):** Good for using existing shared filesystem mount from host.
* **Option 2 (Basic NFS):** Good for standard NFS mounts without custom mount options.
* **Option 3 (PersistentVolumeClaim):** **Required** if you want to use custom NFS mount options like `nconnect`.

*Note*: Many storage providers offer special CSI drivers for Kubernetes, which you would normally want to use for your PersistentVolumeClaim. See their documentation for details on how to make a PersistentVolumeClaim.

## **Step 2: PV & PVC Config** - Only for Storage Target Option 3

*Skip this step if you are not chosing Option 3 (PVC) to use custom mount options (e.g. `nconnect=4`).*

For storage target Option 3 you must first deploy the Persistent Volume (PV) and Persistent Volume Claim (PVC).

Edit the downloaded `nfs-pv-pvc.yaml` and update the server IP and path to match your NFS server.

```
# After your PV & PVC are configured in the YAML, apply the storage configuration:
kubectl apply -f nfs-pv-pvc.yaml

# Verify the claim is bound:
kubectl get pvc elbencho-nfs-pvc
```

## **Step 3: Deploy the elbencho Cluster**

```
# Once your storage is configured in the YAML, deploy the elbencho pods:
kubectl apply -f multi-node-elbencho.yaml

# Wait for all the pods to reach the Running state:
kubectl rollout status deployment/elbencho-cluster
```

Now your elbencho service instances are running and waiting for you to send a benchmark command.

## **Step 4: Prepare the Benchmark Hosts File**

To run a coordinated test, the coordinator pod needs a file containing the IP addresses of all participating pods.

```
# 1. Extract the Pod IPs into a local file on your machine:
kubectl get pods -l app=elbencho -o jsonpath='{range .items[*]}{.status.podIP}{"\n"}{end}' > /tmp/elbencho-hosts.txt

# 2. Identify your Coordinator Pod. We just pick the first one in the list:
COORDINATOR_POD=$(kubectl get pods -l app=elbencho -o jsonpath='{.items[0].metadata.name}')
echo "Coordinator pod is: $COORDINATOR_POD"

# 3. Copy the hosts file directly into the coordinator pod:
kubectl cp /tmp/elbencho-hosts.txt $COORDINATOR_POD:/tmp/hosts.txt
```

## **Step 5: Execute the Benchmark**

Now use `kubectl exec` to trigger the benchmark from the coordinator pod.

The target path is `/benchmark-data`, which we mapped to the shared volume in Step 1.

```
kubectl exec -it $COORDINATOR_POD -- elbencho --hostsfile /tmp/hosts.txt --threads 8 --size 1g --block 1m --write --direct --files 1 --dirs 0 /benchmark-data/
```

Adjust the elbencho parameters as needed for your specific test scenario. The example above uses 8 threads per pod. Each thread will write a separate 1GiB-sized file in 1MiB blocks using direct IO (so 8GiB per pod).

If you want to keep the test results in a file, consider adding `--resfile /benchmark-data/results.txt` (or `--csvfile <PATH>` or `--jsonfile <PATH>`). Optionally, you can also use `kubectl cp` to copy such result files from the coordinator pod back to the physical host.

## **Step 6: Cleanup**

When you are finished benchmarking, you can cleanly remove all resources from your cluster:

```
# Delete the pods and deployment:
kubectl delete -f multi-node-elbencho.yaml

# If you used Option 3 (PVC), delete the storage claim and volume:
kubectl delete -f nfs-pv-pvc.yaml
```
