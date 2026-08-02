import time
import torch
from torch.utils.data import DataLoader, Dataset


class DummyDataset(Dataset):
    def __init__(self, size=20000, tensor_size=65536):
        self.size = size
        self.tensor_size = tensor_size

    def __len__(self):
        return self.size

    def __getitem__(self, idx):
        return torch.ones(self.tensor_size)


def main():
    ds = DummyDataset()
    loader = DataLoader(ds, batch_size=64, num_workers=4, persistent_workers=True)
    it = iter(loader)

    # warmup (worker spawn + first-batch overhead shouldn't count)
    for _ in range(10):
        next(it)

    N = 200
    total_images = 0
    t0 = time.perf_counter()
    for _ in range(N):
        batch = next(it)
        total_images += batch.shape[0]
    t1 = time.perf_counter()

    elapsed = t1 - t0
    print(f"RESULT batches={N} images={total_images} elapsed_s={elapsed:.4f} "
          f"images_per_sec={total_images/elapsed:.1f} batches_per_sec={N/elapsed:.2f}")


if __name__ == "__main__":
    main()
