Mass Entity Swarm Manager (Flowmap & Boids)

Context: Built to handle thousands of swarming entities efficiently in Unreal Engine 5.

Core Logic: Bypasses standard AActor ticking and NavMesh by utilizing a UInstancedStaticMeshComponent for rendering. Pathing is handled via a custom grid-based Flowmap generator with Gaussian blurring for smooth directional vectors.

Optimization: Implements spatial bucketing (hashing) to calculate boid separation forces, preventing O(N²) distance checks, and ships a single transform array to the GPU via BatchUpdateInstancesTransforms.