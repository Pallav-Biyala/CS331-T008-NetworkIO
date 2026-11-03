# CS331-T008 Benchmarking Runbook (Direct Mode / No SSH)

This runbook gives the exact commands to run the automated echo-server benchmarking suite directly between the two laptops without needing SSH.

---

## 1. Setup & Build (Both Laptops)

Ensure both laptops have the latest code and binaries compiled:
```bash
cd ~/linux-network-io-bench
git pull origin loadgen-bench
make clean && make echo && make loadgen
```

---

## 2. Dry Run Check (Client)

Verify the 49-configuration matrix without sending traffic:
```bash
./scripts/run_experiments.sh --dry-run
```

---

## 3. How to Run Experiments (Mechanism by Mechanism)

You start the server once on the **Server (Omen)**, then run the client suite on the **Client**. The script automatically runs all matrix points (E1, E1s, E2, E3) for that mechanism, repeats them across rounds, retries on error, and appends clean CSV rows into `results/master.csv` and `results/E*.csv`.

---

### Step A: Benchmark **`epoll`**

1. **On Server (Omen):**
   ```bash
   ./scripts/run_server.sh -s epoll -p 8080 -m 512
   ```
   *(Leave this running in the terminal)*

2. **On Client:**
   ```bash
   ./scripts/run_experiments.sh --mech epoll --rounds 5
   ```
   *(Runs all tests for epoll, logs progress, and saves results)*

3. **When Client completes:** Go to Server terminal and press `Ctrl+C`.

---

### Step B: Benchmark **`select`**

1. **On Server:**
   ```bash
   ./scripts/run_server.sh -s select -p 8080 -m 512
   ```

2. **On Client:**
   ```bash
   ./scripts/run_experiments.sh --mech select --rounds 5
   ```

3. **When Client completes:** Press `Ctrl+C` on Server.

---

### Step C: Benchmark **`poll`**

1. **On Server:**
   ```bash
   ./scripts/run_server.sh -s poll -p 8080 -m 512
   ```

2. **On Client:**
   ```bash
   ./scripts/run_experiments.sh --mech poll --rounds 5
   ```

3. **When Client completes:** Press `Ctrl+C` on Server.

---

### Step D: Benchmark **`uring`**

1. **On Server:**
   ```bash
   ./scripts/run_server.sh -s uring -p 8080 -m 512
   ```

2. **On Client:**
   ```bash
   ./scripts/run_experiments.sh --mech uring --rounds 5
   ```

3. **When Client completes:** Press `Ctrl+C` on Server.

---

## 4. Resuming if Interrupted

The script automatically skips configurations and repeats that are already completed and valid.
If interrupted, simply re-run the same command on the Client:
```bash
./scripts/run_experiments.sh --mech <type> --rounds 5
```

---

## 5. Copying Results Out by USB

Once all 4 mechanisms are finished:
1. Plug your USB drive into the Client.
2. Copy the results:
   ```bash
   cp -r ~/linux-network-io-bench/results /media/$USER/*/
   sync
   ```
3. Bring the USB drive to Laptop 3 for immediate report generation and plotting!
