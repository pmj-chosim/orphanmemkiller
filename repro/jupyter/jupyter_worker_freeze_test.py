# Real jupyter_client.KernelManager E2E test: starts an actual kernel, runs
# a real PyTorch DataLoader (persistent_workers=True, num_workers=4) inside
# it, freezes one worker with SIGSTOP, then kills the kernel with a direct
# SIGKILL to its pid -- NOT via km.restart_kernel(), which calls killpg() on
# the kernel's whole process group and would kill the frozen worker directly,
# skipping orphan-guard entirely. A direct SIGKILL to one pid is what a real
# OOM-killer (or crash) actually does. See docs/cfp/jupyter-demo-storyboard.md.
import os
import time
import subprocess
from jupyter_client import KernelManager

km = KernelManager(kernel_name="python3")
km.start_kernel()
kc = km.client()
kc.start_channels()
kc.wait_for_ready(timeout=30)

kernel_pid = km.provisioner.process.pid
print(f"[harness] kernel process pid = {kernel_pid}", flush=True)
pgid = subprocess.check_output(f"ps -o pgid= -p {kernel_pid}", shell=True).decode().strip()
print(f"[harness] kernel process group id = {pgid} (jupyter_client starts kernels as their "
      f"own session/process-group leader -- relevant because killpg() on this group would "
      f"clean up children too; a real external OOM-kill targets ONE pid, not the group)", flush=True)

setup_code = """
import torch, os
from torch.utils.data import DataLoader, Dataset

class DS(Dataset):
    def __len__(self): return 10000
    def __getitem__(self, i): return torch.ones(65536)

loader = DataLoader(DS(), batch_size=64, num_workers=4, persistent_workers=True)
it = iter(loader)
for _ in range(3):
    b = next(it)
print(f"KERNEL_PID={os.getpid()}", flush=True)
"""

msg_id = kc.execute(setup_code)
while True:
    msg = kc.get_iopub_msg(timeout=30)
    if msg["msg_type"] == "stream":
        print("[kernel stdout]", msg["content"]["text"].strip(), flush=True)
    if msg["msg_type"] == "status" and msg["content"]["execution_state"] == "idle":
        break

workers = subprocess.check_output(f"ps --ppid {kernel_pid} -o pid=", shell=True).split()
workers = [int(w) for w in workers]
print(f"[harness] worker pids: {workers}", flush=True)

victim = workers[0]
os.kill(victim, 19)  # SIGSTOP
print(f"[harness] froze worker {victim} with SIGSTOP", flush=True)

print("[harness] BEFORE: process tree + shm", flush=True)
subprocess.run("ps -eo pid,ppid,stat,args | grep -v grep", shell=True)
subprocess.run("df -h /dev/shm", shell=True)

print(f"[harness] sending SIGKILL directly to kernel pid {kernel_pid} ONLY -- "
      f"this is what a real Linux OOM-killer does: targets one specific pid's "
      f"task_struct, does NOT know or care about process groups. NOT going "
      f"through jupyter_client's own restart_kernel()/killpg path at all.", flush=True)
os.kill(kernel_pid, 9)

time.sleep(1)
print("[harness] AFTER (t+1s): process tree + shm", flush=True)
subprocess.run("ps -eo pid,ppid,stat,args | grep -v grep", shell=True)
subprocess.run("df -h /dev/shm", shell=True)

for t in [2, 4, 6, 8]:
    time.sleep(2)
    print(f"[harness] AFTER (t+{t+1}s): process tree + shm", flush=True)
    subprocess.run("ps -eo pid,ppid,stat,args | grep -v grep | grep -v defunct", shell=True)
    subprocess.run("df -h /dev/shm", shell=True)
