#include <fstream>
#include <vector>
#include <cuda_runtime.h>

using namespace std;

static const int BLOCK_SIZE = 256;

void readInput(const char *path, int &N, int &K,
               vector<int> &x, vector<int> &y,
               vector<int> &cx, vector<int> &cy)
{
    ifstream in(path);
    in >> N >> K;
    x.resize(N);
    y.resize(N);
    cx.resize(K);
    cy.resize(K);
    for (int i = 0; i < N; ++i)
        in >> x[i];
    for (int i = 0; i < N; ++i)
        in >> y[i];
    for (int j = 0; j < K; ++j)
        in >> cx[j];
    for (int j = 0; j < K; ++j)
        in >> cy[j];
}

void writeOutput(const char *path, const vector<int> &cluster,
                 const vector<int> &cx, const vector<int> &cy)
{
    ofstream out(path);
    for (int i = 0; i < (int)cluster.size(); ++i)
        out << (i ? " " : "") << cluster[i];
    out << '\n';
    for (int j = 0; j < (int)cx.size(); ++j)
        out << cx[j] << ' ' << cy[j] << '\n';
}

__device__ int roundMean(int sum, int count)
{
    int num = 2 * sum - count;
    int den = 2 * count;
    return num < 0 ? num / den : (num + den - 1) / den;
}

__global__ void assignPoints(const int *x, const int *y,
                             const int *cx, const int *cy,
                             int *cluster, int N, int K)
{
    // Assign each point to its nearest centroid by smallest index.
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    int px = x[i];
    int py = y[i];
    int best_k = 0;
    long long dx0 = px - cx[0];
    long long dy0 = py - cy[0];
    long long min_dist = dx0 * dx0 + dy0 * dy0;

    for (int k = 1; k < K; ++k) {
        long long dx = px - cx[k];
        long long dy = py - cy[k];
        long long dist = dx * dx + dy * dy;
        if (dist < min_dist) {
            min_dist = dist;
            best_k = k;
        }
    }
    cluster[i] = best_k;
}

__global__ void accumulateTotals(const int *x, const int *y, const int *cluster,
                           int *sumX, int *sumY, int *count, int N)
{
    // Accumulate sum_x, sum_y, and count for each assigned cluster.
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    int k = cluster[i];
    atomicAdd(&sumX[k], x[i]);
    atomicAdd(&sumY[k], y[i]);
    atomicAdd(&count[k], 1);
}

__global__ void updateCentroids(int *cx, int *cy,
                                const int *sumX, const int *sumY,
                                const int *count, int K)
{
    // Update non-empty centroids with roundMean; leave empty unchanged.
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k >= K) return;

    if (count[k] > 0) {
        cx[k] = roundMean(sumX[k], count[k]);
        cy[k] = roundMean(sumY[k], count[k]);
    }
}

int main(int, char **argv)
{
    int N, K;
    vector<int> x, y, cx, cy, cluster;
    readInput(argv[1], N, K, x, y, cx, cy);
    cluster.resize(N);

    // Allocate device arrays for points, centroids, and totals.
    int *d_x = nullptr;
    int *d_y = nullptr;
    int *d_cx = nullptr;
    int *d_cy = nullptr;
    int *d_cluster = nullptr;
    int *d_sumX = nullptr;
    int *d_sumY = nullptr;
    int *d_count = nullptr;

    cudaMalloc((void **)&d_x, N * sizeof(int));
    cudaMalloc((void **)&d_y, N * sizeof(int));
    cudaMalloc((void **)&d_cx, K * sizeof(int));
    cudaMalloc((void **)&d_cy, K * sizeof(int));
    cudaMalloc((void **)&d_cluster, N * sizeof(int));
    cudaMalloc((void **)&d_sumX, K * sizeof(int));
    cudaMalloc((void **)&d_sumY, K * sizeof(int));
    cudaMalloc((void **)&d_count, K * sizeof(int));

    // Copy the input points and initial centroids to the device.
    cudaMemcpy(d_x, x.data(), N * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_y, y.data(), N * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_cx, cx.data(), K * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_cy, cy.data(), K * sizeof(int), cudaMemcpyHostToDevice);

    int blocksN = (N + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int blocksK = (K + BLOCK_SIZE - 1) / BLOCK_SIZE;

    vector<int> new_cx(K), new_cy(K);

    // Repeat assign, accumulate, and update until centroids converge.
    while (true) {
        assignPoints<<<blocksN, BLOCK_SIZE>>>(d_x, d_y, d_cx, d_cy, d_cluster, N, K);

        cudaMemset(d_sumX, 0, K * sizeof(int));
        cudaMemset(d_sumY, 0, K * sizeof(int));
        cudaMemset(d_count, 0, K * sizeof(int));

        accumulateTotals<<<blocksN, BLOCK_SIZE>>>(d_x, d_y, d_cluster, d_sumX, d_sumY, d_count, N);

        updateCentroids<<<blocksK, BLOCK_SIZE>>>(d_cx, d_cy, d_sumX, d_sumY, d_count, K);

        cudaMemcpy(new_cx.data(), d_cx, K * sizeof(int), cudaMemcpyDeviceToHost);
        cudaMemcpy(new_cy.data(), d_cy, K * sizeof(int), cudaMemcpyDeviceToHost);

        bool converged = true;
        for (int k = 0; k < K; ++k) {
            if (new_cx[k] != cx[k] || new_cy[k] != cy[k]) {
                converged = false;
                break;
            }
        }

        if (converged) {
            break;
        }

        cx = new_cx;
        cy = new_cy;
    }

    // Copy the final cluster assignment indices back to the host.
    cudaMemcpy(cluster.data(), d_cluster, N * sizeof(int), cudaMemcpyDeviceToHost);

    writeOutput(argv[2], cluster, cx, cy);

    // Free every device array allocated during the computation.
    cudaFree(d_x);
    cudaFree(d_y);
    cudaFree(d_cx);
    cudaFree(d_cy);
    cudaFree(d_cluster);
    cudaFree(d_sumX);
    cudaFree(d_sumY);
    cudaFree(d_count);

    return 0;
}
