import os
import time
import numpy as np
import ray

ray.init(object_store_memory=200 * 1024 * 1024)  # 200MB plasma store

with open("/workspace/parent.pid", "w") as f:
    f.write(str(os.getpid()))

# put a big array in the object store (plasma, backed by /dev/shm)
arr = np.ones((20_000_000,), dtype=np.uint8)  # ~20MB
ref = ray.put(arr)
print(f"[Driver PID {os.getpid()}] put object {ref}, sleeping...", flush=True)

while True:
    time.sleep(1)
