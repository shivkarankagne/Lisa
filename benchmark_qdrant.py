import time
import numpy as np
from qdrant_client import QdrantClient
from qdrant_client.http.models import Distance, VectorParams, PointStruct

# Generate data
n = 10000
dim = 768
vectors = np.random.rand(n, dim).astype(np.float32)
queries = np.random.rand(1000, dim).astype(np.float32)

# Connect to Qdrant
client = QdrantClient(host="localhost", port=6333)

# Create collection
try:
    client.delete_collection("test")
except:
    pass

client.create_collection(
    collection_name="test",
    vectors_config=VectorParams(size=dim, distance=Distance.COSINE)
)

# Upload vectors in batches of 1000
batch_size = 1000
for i in range(0, n, batch_size):
    batch_points = [
        PointStruct(id=j, vector=vectors[j].tolist())
        for j in range(i, min(i + batch_size, n))
    ]
    client.upsert("test", batch_points)
    print(f"Uploaded {min(i + batch_size, n)} vectors")

# Warmup
for q in queries[:10]:
    client.query_points(
        collection_name="test",
        query=q.tolist(),
        limit=5
    )

# Benchmark — measure wall-clock time for all queries
start = time.perf_counter()
for q in queries:
    client.query_points(
        collection_name="test",
        query=q.tolist(),
        limit=5
    )
elapsed = time.perf_counter() - start

# Compute average per query in milliseconds
avg_ms = (elapsed / len(queries)) * 1000

print(f"Total time: {elapsed:.3f} sec for {len(queries)} queries")
print(f"Qdrant: {avg_ms:.3f} ms per query")
