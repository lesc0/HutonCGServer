#pragma once
// /dev/dma_heap 에서 할당한 dmabuf (RGA 가 fd 로 직접 읽고 쓰는 버퍼)
// 기본 힙은 system-dma32: RGA2 코어가 32bit 주소만 다루므로 4GB 이하 메모리에서 할당.
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>

struct DmaHeapBuf {
  int fd = -1;
  size_t size = 0;
  void* ptr = nullptr;   // Map() 후 CPU 주소

  bool Alloc(size_t n, const char* heap = "/dev/dma_heap/system-dma32") {
    Free();
    int hfd = open(heap, O_RDWR | O_CLOEXEC);
    if (hfd < 0) { perror(heap); return false; }
    dma_heap_allocation_data d{};
    d.len = n;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    int r = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &d);
    close(hfd);
    if (r < 0) { perror("DMA_HEAP_IOCTL_ALLOC"); return false; }
    fd = (int)d.fd;
    size = n;
    return true;
  }
  void* Map() {
    if (!ptr && fd >= 0) {
      ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (ptr == MAP_FAILED) { perror("mmap dmabuf"); ptr = nullptr; }
    }
    return ptr;
  }
  // CPU 가 쓰기 전/후 캐시 동기화 (cached 힙)
  void BeginCpuWrite() { dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE}; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); }
  void EndCpuWrite() { dma_buf_sync s{DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE}; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); }
  void Free() {
    if (ptr) { munmap(ptr, size); ptr = nullptr; }
    if (fd >= 0) { close(fd); fd = -1; }
    size = 0;
  }
  ~DmaHeapBuf() { Free(); }
};
