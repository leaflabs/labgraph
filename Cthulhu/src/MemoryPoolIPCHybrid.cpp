// Copyright 2004-present Facebook. All Rights Reserved.

#include "MemoryPoolIPCHybrid.h"

#include "MemoryPoolLocalImpl.h"

#define DEFAULT_LOG_CHANNEL "Cthulhu"
#include <logging/Log.h>

#include <cthulhu/Framework.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cthulhu {

namespace {

const char* const MEMORY_POOL_NAME = "MemoryPool";
const char* const MEMORY_POOL_GPU_NAME = "MemoryPoolGPU";
const char* const MEMORY_POOL_GPU_DEVICE_LOCAL_NAME = "MemoryPoolGPUDeviceLocal";
const char* const AUDITOR_NAME = "Auditor";

} // namespace

MemoryPoolIPCHybrid::MemoryPoolIPCHybrid(
    ManagedSHM* shm,
    size_t shmSize,
    size_t shmGPUSize,
    bool enableAuditor)
    : shmSize_(shmSize),
      shmGPUSize_(shmGPUSize),
      memoryPool_(new MemoryPool()),
      shm_(shm),
      stopSignal_{false} {
  pool_ = shm_->find_or_construct<MemoryPoolIPC>(MEMORY_POOL_NAME)(shm_->get_segment_manager());
  poolGPU_ =
      shm_->find_or_construct<MemoryPoolIPC>(MEMORY_POOL_GPU_NAME)(shm_->get_segment_manager());
  poolGPUDeviceLocal_ = shm_->find_or_construct<MemoryPoolIPC>(MEMORY_POOL_GPU_DEVICE_LOCAL_NAME)(
      shm_->get_segment_manager());
  auditor_ = shm_->find_or_construct<AuditorIPC>(AUDITOR_NAME)(shm_->get_segment_manager());

  vulkanUtil_.reset(new VulkanUtil());

  // Setup auditing
  ScopedLockIPC lock(auditor_->mutex);
  if (audit()) {
    auditor_->processes.emplace_back();
    if (enableAuditor) {
      auditorThread_ = std::thread([this]() {
        while (!stopSignal_.load()) {
          std::this_thread::yield();

          ScopedLockIPC lock(auditor_->mutex);
          if (!audit()) {
            if (!Framework::nuke()) {
              XR_LOGE("Could not nuke framework");
            }
            invalidate();
            break;
          }
        }
      });
    }
  } else {
    invalidate();
  }
}

bool MemoryPoolIPCHybrid::nuke(ManagedSHM* shm) {
  shm->destroy<MemoryPoolIPC>(MEMORY_POOL_NAME);
  shm->destroy<MemoryPoolIPC>(MEMORY_POOL_GPU_NAME);
  shm->destroy<MemoryPoolIPC>(MEMORY_POOL_GPU_DEVICE_LOCAL_NAME);
  shm->destroy<AuditorIPC>(AUDITOR_NAME);
  return true;
}

bool MemoryPoolIPCHybrid::audit() const {
  return isValid() && processesAlive();
}

bool MemoryPoolIPCHybrid::isValid() const {
  return !auditor_->invalid;
}

bool MemoryPoolIPCHybrid::processesAlive() const {
  auto& processes = auditor_->processes;
  for (auto it = processes.begin(); it != processes.end(); ++it) {
    if (!it->isAlive()) {
      return false;
    }
  }
  return true;
}

void MemoryPoolIPCHybrid::invalidate() {
  auditor_->invalid = true;
}

MemoryPoolIPCHybrid::~MemoryPoolIPCHybrid() {
  ptrs_.clear();

  // Stop the auditing thread
  stopSignal_.store(true);
  if (auditorThread_.joinable()) {
    auditorThread_.join();
  }

  ScopedLockIPC lock(auditor_->mutex);

  // Deregister our own process from the auditor
  auto& processes = auditor_->processes;
  for (auto it = processes.begin(); it != processes.end(); ++it) {
    if (it->isSelf()) {
      processes.erase(it);
      break;
    }
  }

  if (force_clean_) {
    processes.clear();
  }

  if (auditor_->processes.empty()) {
    invalidate();

    // CPU Cleanup
    ScopedLockIPC lock1(pool_->buffers_mutex);
    ScopedLockIPC lock2(pool_->sizes_mutex);
    for (auto& size : pool_->sizes) {
      pool_->allocated -= size.second;
    }
    for (auto& buffers : pool_->buffers) {
      for (auto& buffer : buffers.second) {
        shm_->destroy_ptr(shm_->get_address_from_handle(buffer));
      }
    }
    pool_->buffers.clear();
    pool_->sizes.clear();
  }

  // Release local GPU handle caches
  handlesGPU_.clear();
  gpuMappedBuffers_.clear();

  // Cleanup the GPU pools if we have any
  bool clearAllocations = auditor_->processes.empty();
  if (poolGPU_) {
    cleanPool(poolGPU_, clearAllocations);
  }
  if (poolGPUDeviceLocal_) {
    cleanPool(poolGPUDeviceLocal_, clearAllocations);
  }

  // Delete any locally duplicated GPU handles
  for (auto& handle : gpuHandleProcMap_) {
    vulkanUtil_->free(handle.second);
  }
}

CpuBuffer MemoryPoolIPCHybrid::getBufferFromPool(const StreamIDView& id, size_t nrBytes) {
  if ((activatedStreams_.find(id) == activatedStreams_.end()) ||
      (activatedStreams_.find(id) != activatedStreams_.end() && activatedStreams_[id])) {
    auto shm = requestSHM(nrBytes);
    if (!shm) {
      XR_LOGE_EVERY_N(
          100,
          "MemoryPoolIPCHybrid - Failed to get shared memory buffer for [{}] bytes. Allocated locally.",
          nrBytes);
      return memoryPool_->request(nrBytes);
    }
    return shm;
  }
  return memoryPool_->request(nrBytes);
}

bool MemoryPoolIPCHybrid::findBuffer(
    size_t nrBytes,
    boost::interprocess::offset_ptr<MemoryPoolIPC> pool,
    std::ptrdiff_t& offset_ptr_out,
    GpuBufferDataWithPID*& ptr_out) {
  ScopedLockIPC lock(pool->buffers_mutex);
  auto buffer_it = pool->buffers.find(nrBytes);
  if (buffer_it == pool->buffers.cend()) {
    buffer_it = pool->buffers
                    .emplace(
                        nrBytes,
                        MemoryPoolIPC::PtrVectorType(
                            MemoryPoolIPC::PtrVectorAllocType(shm_->get_segment_manager())))
                    .first;
  }

  auto& ptrlist = buffer_it->second;
  // Iterate through the list until we find one that originated from our process.
  // Note: It may be useful to restructure our GPU pool to make it faster to find
  // buffers originating from our process.
#ifdef _WIN32
  uint64_t ourPid = (uint64_t)GetCurrentProcessId();
#else
  uint64_t ourPid = (uint64_t)getpid();
#endif
  auto it = ptrlist.begin();
  while (it != ptrlist.end()) {
    auto bufferPtr = reinterpret_cast<GpuBufferDataWithPID*>(shm_->get_address_from_handle(*it));
    if (bufferPtr->second == ourPid) {
      ptr_out = bufferPtr;
      offset_ptr_out = *it;
      it = ptrlist.erase(it);
      return true;
    } else {
      ++it;
    }
  }
  return false;
}

GpuBuffer MemoryPoolIPCHybrid::getGpuBufferFromPool(size_t nrBytes, bool deviceLocal) {
  if (!vulkanUtil_->isActive()) {
    XR_LOGW("Failed to generate GPU Buffer. Vulkan is not active.");
    return GpuBuffer();
  }

  std::ptrdiff_t offset_ptr = 0;
  GpuBufferDataWithPID* ptr = nullptr;

  // Check to see if we already have a buffer of this size
  boost::interprocess::offset_ptr<MemoryPoolIPC> pool =
      deviceLocal ? poolGPUDeviceLocal_ : poolGPU_;
  if (!findBuffer(nrBytes, pool, offset_ptr, ptr)) {
    // Make a new buffer
    ScopedLockIPC lock(pool->sizes_mutex);
    if (deviceLocal) {
      XR_LOGT_EVERY_N(
          10, "MemoryPoolIPCHybrid - Num GPU Device Local bytes allocated: ", pool->allocated);
    } else {
      XR_LOGT_EVERY_N(10, "MemoryPoolIPCHybrid - Num GPU bytes allocated: ", pool->allocated);
    }
    if (pool->allocated + nrBytes < shmGPUSize_) {
      auto vulkanAllocation = vulkanUtil_->allocate(nrBytes, deviceLocal);
      if (vulkanAllocation.first == 0) {
        XR_LOGW("Failed to allocate vulkan buffer of size {}.", nrBytes);
        return GpuBuffer();
      }
      // Store a local map of the external memory, which adds a reference for the local process
      gpuMappedBuffers_[vulkanAllocation.first] =
          vulkanUtil_->map(vulkanAllocation.first, nrBytes, vulkanAllocation.second);
#ifdef _WIN32
      uint64_t pid = (uint64_t)GetCurrentProcessId();
#else
      uint64_t pid = (uint64_t)getpid();
#endif
      // Put the handle in shared memory
      ptr = shm_->construct<GpuBufferDataWithPID>(boost::interprocess::anonymous_instance)();
      ptr->first.handle = vulkanAllocation.first;
      ptr->first.size = nrBytes;
      ptr->first.memoryTypeIndex = vulkanAllocation.second;
      ptr->second = pid;
      offset_ptr = shm_->get_handle_from_address(ptr);
      pool->allocated += nrBytes;
      pool->sizes.emplace(offset_ptr, nrBytes);
    } else {
      XR_LOGW(
          "Failed to allocate GPU buffer of size {}. Max GPU memory size {} reached.",
          nrBytes,
          shmGPUSize_);
      return GpuBuffer();
    }
  }

  std::lock_guard<std::mutex> lock(memoryMutex_);

  // Construct the shared shared pointer
  SharedPtrGPUIPC& buffer =
      *shm_->construct<SharedPtrGPUIPC>(boost::interprocess::anonymous_instance)(
          ptr, PtrAllocatorIPC(shm_->get_segment_manager()), ReclaimerGPUIPC(pool, offset_ptr));

  // Store the mapping to it
  handlesGPU_.emplace(ptr->first.handle, buffer);

  shm_->destroy_ptr(&buffer);

  // Return a local pointer
  return GpuBuffer(
      &ptr->first,
      [this](GpuBufferData* handlePtr) { this->destroyLocal(handlePtr); },
      deviceLocal ? CpuBuffer() : gpuMappedBuffers_[ptr->first.handle]);
}

CpuBuffer MemoryPoolIPCHybrid::requestSHM(size_t nrBytes) {
  std::ptrdiff_t offset_ptr = 0;
  uint8_t* ptr = nullptr;

  // Check to see if we already have a buffer of this size
  {
    ScopedLockIPC lock(pool_->buffers_mutex);
    auto buffer_it = pool_->buffers.find(nrBytes);
    if (buffer_it == pool_->buffers.cend()) {
      buffer_it = pool_->buffers
                      .emplace(
                          nrBytes,
                          MemoryPoolIPC::PtrVectorType(
                              MemoryPoolIPC::PtrVectorAllocType(shm_->get_segment_manager())))
                      .first;
    }

    auto& ptrlist = buffer_it->second;
    if (!ptrlist.empty()) {
      offset_ptr = ptrlist.back();
      ptr = reinterpret_cast<uint8_t*>(shm_->get_address_from_handle(offset_ptr));
      ptrlist.pop_back();
    }
  }

  // Make a new buffer if needed
  if (!ptr) {
    ScopedLockIPC lock(pool_->sizes_mutex);
    XR_LOGT_EVERY_N(100, "MemoryPoolIPCHybrid - Num shared bytes allocated: {}", pool_->allocated);
    if (pool_->allocated + nrBytes < shmSize_ * MAX_SHM_USAGE_FRAC) {
      ptr = shm_->construct<uint8_t>(boost::interprocess::anonymous_instance)[nrBytes]();
      offset_ptr = shm_->get_handle_from_address(ptr);
      pool_->allocated += nrBytes;
      pool_->sizes.emplace(offset_ptr, nrBytes);
    } else {
      return std::shared_ptr<uint8_t>();
    }
  }

  std::lock_guard<std::mutex> lock(memoryMutex_);

  // Construct the shared shared pointer
  SharedPtrIPC& buffer = *shm_->construct<SharedPtrIPC>(boost::interprocess::anonymous_instance)(
      ptr, PtrAllocatorIPC(shm_->get_segment_manager()), ReclaimerIPC(pool_, offset_ptr));

  // Store the mapping to it
  ptrs_.emplace(ptr, buffer);

  shm_->destroy_ptr(&buffer);

  // Return a local pointer
  return CpuBuffer(ptr, [this](uint8_t* ptr) { this->destroyLocal(ptr); });
}

void MemoryPoolIPCHybrid::activateStream(const StreamIDView& streamID, bool active) {
  activatedStreams_[streamID] = active;
}

SharedPtrIPC MemoryPoolIPCHybrid::convert(const CpuBuffer& ptr) const {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  if (ptrs_.find(ptr.get()) != ptrs_.end()) {
    return ptrs_.at(ptr.get());
  }
  return SharedPtrIPC();
}

SharedPtrGPUIPC MemoryPoolIPCHybrid::convert(const GpuBuffer& ptr) const {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  if (handlesGPU_.find(ptr->handle) != handlesGPU_.end()) {
    return handlesGPU_.at(ptr->handle);
  }
  return SharedPtrGPUIPC();
}

CpuBuffer MemoryPoolIPCHybrid::createLocal(const SharedPtrIPC& buffer) {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  auto pointer = buffer.get().get();
  ptrs_[pointer] = buffer;
  return CpuBuffer(pointer, [this](uint8_t* ptr) { this->destroyLocal(ptr); });
}

void MemoryPoolIPCHybrid::destroyLocal(uint8_t* ptr) {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  ptrs_.erase(ptr);
}

GpuBuffer MemoryPoolIPCHybrid::createLocal(const SharedPtrGPUIPC& buffer) {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  auto pointer = buffer.get().get();
  auto handle = pointer->first.handle;

  uint64_t newHandle = 0;

  if (gpuHandleProcMap_.find(handle) == gpuHandleProcMap_.end()) {
    // Clone the handle into our process
#ifdef _WIN32
    uint64_t ourPID = (uint64_t)GetCurrentProcessId();
    HANDLE currentProcHandle = OpenProcess(PROCESS_ALL_ACCESS, true, ourPID);
    HANDLE otherProcHandle = OpenProcess(PROCESS_DUP_HANDLE, false, pointer->second);
    HANDLE tempHandle;
    auto dupResult = DuplicateHandle(
        otherProcHandle,
        (HANDLE)handle,
        currentProcHandle,
        &tempHandle,
        0,
        false,
        DUPLICATE_SAME_ACCESS);
    CloseHandle(currentProcHandle);
    CloseHandle(otherProcHandle);
    if (!dupResult) {
      XR_LOGW(
          "Failed to duplicate handle {} to process {}. GPU buffer failed to load to this process.",
          handle,
          ourPID);
      return GpuBuffer();
    }
    newHandle = (uint64_t)tempHandle;
#else
    char fdPath[64]; // actual maximal length: 37 for 64bit systems
    snprintf(fdPath, sizeof(fdPath), "/proc/%d/fd/%d", (int)pointer->second, (int)handle);
    newHandle = open(fdPath, O_RDWR); // TBD: Are these sufficient permissions?
#endif
  } else {
    newHandle = gpuHandleProcMap_[handle];
  }

  gpuHandleProcMap_[handle] = newHandle;

  handlesGPU_[newHandle] = buffer;

  // Map to the CPU in this process if we haven't seen this before
  if (gpuMappedBuffers_.find(newHandle) == gpuMappedBuffers_.end()) {
    gpuMappedBuffers_[newHandle] =
        vulkanUtil_->map(newHandle, pointer->first.size, pointer->first.memoryTypeIndex);
  }

  return GpuBuffer(
      new GpuBufferData{newHandle, pointer->first.size, pointer->first.memoryTypeIndex},
      [this](GpuBufferData* ptr) {
        this->destroyLocal(ptr);
        delete ptr;
      },
      gpuMappedBuffers_[newHandle]);
}

void MemoryPoolIPCHybrid::destroyLocal(GpuBufferData* handlePtr) {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  handlesGPU_.erase(handlePtr->handle);
}

SharedPtrIPC MemoryPoolIPCHybrid::getBufferFromSharedPoolDirect(size_t nrBytes) {
  return convert(requestSHM(nrBytes));
}

bool MemoryPoolIPCHybrid::isBufferFromPool(const AnyBuffer& buf) const {
  return convert(buf);
}

void MemoryPoolIPCHybrid::cleanPool(
    boost::interprocess::offset_ptr<MemoryPoolIPC> pool,
    bool clearAllocations) {
  ScopedLockIPC lock1(pool->buffers_mutex);
  ScopedLockIPC lock2(pool->sizes_mutex);

  // Regardless of reference count, clear out all buffers originating from this process.
  // No-one else will try to recycle these buffers, and the underlying resource
  // of any in-flight buffers will be preserved through their lifetimes.
  if (vulkanUtil_->isActive()) {
    AuditorIPC::Process ownProc;
    uint64_t ourPid = ownProc.pid();
    for (auto& buffers : pool->buffers) {
      for (auto& buffer : buffers.second) {
        GpuBufferDataWithPID data =
            *reinterpret_cast<GpuBufferDataWithPID*>(shm_->get_address_from_handle(buffer));
        if (data.second == ourPid) {
          vulkanUtil_->free(data.first.handle);
          shm_->destroy_ptr(shm_->get_address_from_handle(buffer));
        }
      }
    }
  }
  pool->buffers.clear();

  if (clearAllocations) {
    for (auto& size : pool->sizes) {
      pool->allocated -= size.second;
    }
    pool->sizes.clear();
  }
}

} // namespace cthulhu
