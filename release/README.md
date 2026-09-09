# LISA — Low-level Intelligent Search Accelerator

**LISA** is a pure ARM64 assembly, NEON-optimized *exact*-search engine for vector similarity.  
Single binary. Zero dependencies. No cloud.

[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ARM64-brightgreen)]()

---

## How it works

LISA computes exact nearest-neighbor search (brute-force, not approximate) using hand-written ARM64 NEON SIMD kernels for L2 distance computation. Because it's exact, recall is always 1.0 — there's no index to tune and no approximation error.

The NEON kernel processes 4 floats per instruction, uses fused multiply-add (`fmla`), manual prefetching, and loop unrolling to saturate the M2's SIMD units. Vectors are stored in row-major order in a memory-mapped file — no heap allocation, zero-copy I/O.

**Distance metric:** L2 (Euclidean) squared distance — exact, no approximations.

---

## 🚀 Install

### Prebuilt binary (macOS ARM64)

```bash
curl -L https://github.com/shivkarankagne/lisa/releases/latest/download/lisa-v1.0-macos-arm64.tar.gz -o lisa.tar.gz
tar -xzf lisa.tar.gz
./install.sh
Platform support: macOS ARM64 (Apple Silicon) — M1/M2/M3/M4/M5. Linux ARM64 support is planned for v1.1.

Build from source
bash
git clone https://github.com/shivkarankagne/lisa.git
cd lisa
as -o lisa.o lisa.s
gcc -O3 -o lisa main.c lisa.o
Requirements: ARM64 CPU with NEON, macOS 13+ or Linux ARM64, GNU assembler (as), GCC or Clang.

Usage
CLI
bash
lisa --index vectors.bin --dim 768 --topk 5 --query query.txt
Input format
Vector file: Binary header (n, dim) as two 32-bit integers, followed by n * dim 32-bit floats in row-major order.

Query file: Space-separated or comma-separated floats, one per line.

Max dimensions: 4096 (tested); any multiple of 4 works.

Data type: float32 only.

📊 Benchmarks
System	Vectors	Dim	Time / query	Recall@5
LISA (exact)	10,000	768	1.474 ms	1.0
Qdrant (HNSW, ef_search=128)	10,000	768	4.687 ms	~0.96
LISA (exact)	100,000	768	~14.7 ms	1.0
LISA (exact)	1,000,000	768	~147 ms	1.0
LISA scales linearly — 10x vectors = 10x time. Qdrant's HNSW scales sub-linearly and will pull ahead at ~1M vectors.

Methodology: Apple M2, 16GB RAM, macOS 15, single-threaded, warm cache, L2 distance, Qdrant v1.12 with default HNSW settings. Random 768‑dim embeddings. Full benchmark script in benchmark_qdrant.py.

⚠️ Limitations & Roadmap
Exact search is O(n) — no indexing means no sublinear scaling. At large collections (millions of vectors), ANN methods like HNSW will win on latency.

Single-threaded — multi-threading planned for v1.1.

No filtering/metadata — pure vector search only.

No persistence — vectors are memory-mapped from disk; index build is trivial (just write the binary file).

Roadmap (v1.1+):

Multi-threading (OpenMP / GCD)

Linux ARM64 support

AVX2 x86_64 support

Optional ANN mode (HNSW)

Validator tool

Contributing
Issues and PRs welcome. Open an issue to discuss before submitting a large PR.

License
MIT — free for all.

Built by LISA — the binary RAG engine.
