# nv-resource-controller

Since NVIDIA has released its [GPU kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules). Sharing GPU between different containers has a easier way than before. The project is still base on [GaiaGPU: Sharing GPUs in Container Clouds](https://ieeexplore.ieee.org/abstract/document/8672318)

# Build

```
mkdir -p build
cd build && cmake ..
make
```

# How to use it

1. set LD_PRELOAD environemnt 

`export LD_PRELOAD=libcuda_hook.so`

1.1 for gpu memory limitation:

`export CUDA_MEM_LIMIT=<device index>=<memory limitation>`

1.2 for sm utilization limitation:

`export CUDA_CORE_LIMIT=<device index>=<core limitation>`

If you have sm utilization limit enabled, you must start a `server_monitor` to control the utilization `./server_monitor <device idx> <cgroup id> <core limit>`

