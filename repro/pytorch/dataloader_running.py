import os
import time
import torch
from torch.utils.data import DataLoader, Dataset


class DummyDataset(Dataset):
    def __init__(self, size=100000, tensor_size=65536):
        self.size = size
        self.tensor_size = tensor_size

    def __len__(self):
        return self.size

    def __getitem__(self, idx):
        return torch.ones(self.tensor_size)


def main():
    with open("/workspace/parent.pid", "w") as f:
        f.write(str(os.getpid()))

    ds = DummyDataset()
    loader = DataLoader(ds, batch_size=64, num_workers=4, persistent_workers=True)
    it = iter(loader)

    i = 0
    while True:
        batch = next(it)
        print(f"[Main PID {os.getpid()}] batch {i}, shape={tuple(batch.shape)}", flush=True)
        i += 1
        time.sleep(1)


if __name__ == "__main__":
    main()
