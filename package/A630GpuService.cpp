// SPDX-License-Identifier: MIT
//
// Minimal freedreno/msm-compatible user client for the persistent A630 KMD.
// It deliberately exposes only the requests needed to create buffers, obtain
// their GPU addresses, submit command buffers and wait for a fence.

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "A630GpuKernel.h"

enum {
	kDrmGemClose = 0x09,
	kMsmGetParam = 0x40,
	kMsmSetParam = 0x41,
	kMsmGemNew = 0x42,
	kMsmGemInfo = 0x43,
	kMsmGemCpuPrep = 0x44,
	kMsmGemCpuFini = 0x45,
	kMsmGemSubmit = 0x46,
	kMsmWaitFence = 0x47,
	kMsmGemMadvise = 0x48,
	kMsmSubmitQueueNew = 0x4a,
	kMsmSubmitQueueClose = 0x4b,
	kMsmSubmitQueueQuery = 0x4c,
};

struct a630_timespec {
	int64_t seconds;
	int64_t nanoseconds;
};

struct a630_param {
	uint32_t pipe;
	uint32_t param;
	uint64_t value;
	uint32_t len;
	uint32_t pad;
};

struct a630_gem_new {
	uint64_t size;
	uint32_t flags;
	uint32_t handle;
};

struct a630_gem_info {
	uint32_t handle;
	uint32_t info;
	uint64_t value;
	uint32_t len;
	uint32_t pad;
};

struct a630_gem_cpu_prep {
	uint32_t handle;
	uint32_t op;
	struct a630_timespec timeout;
};

struct a630_gem_submit_cmd {
	uint32_t type;
	uint32_t submit_idx;
	uint32_t submit_offset;
	uint32_t size;
	uint32_t pad;
	uint32_t nr_relocs;
	uint64_t relocs;
};

struct a630_gem_submit_bo {
	uint32_t flags;
	uint32_t handle;
	uint64_t presumed;
};

struct a630_gem_submit {
	uint32_t flags;
	uint32_t fence;
	uint32_t nr_bos;
	uint32_t nr_cmds;
	uint64_t bos;
	uint64_t cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs;
	uint64_t out_syncobjs;
	uint32_t nr_in_syncobjs;
	uint32_t nr_out_syncobjs;
	uint32_t syncobj_stride;
	uint32_t pad;
};

struct a630_wait_fence {
	uint32_t fence;
	uint32_t flags;
	struct a630_timespec timeout;
	uint32_t queueid;
};

struct a630_gem_madvise {
	uint32_t handle;
	uint32_t madv;
	uint32_t retained;
};

struct a630_submitqueue {
	uint32_t flags;
	uint32_t prio;
	uint32_t id;
};

struct a630_submitqueue_query {
	uint64_t data;
	uint32_t id;
	uint32_t param;
	uint32_t len;
	uint32_t pad;
};

struct a630_gem_close {
	uint32_t handle;
	uint32_t pad;
};

#define A630_MSM_INFO_GET_OFFSET 0
#define A630_MSM_INFO_GET_IOVA 1
#define A630_MSM_INFO_SET_NAME 2
#define A630_MSM_INFO_GET_FLAGS 5
#define A630_MSM_BO_GPU_READONLY 0x00000002u
#define A630_MSM_PREP_NOSYNC 0x04u
#define A630_MSM_SUBMIT_CMD_BUF 1u
#define A630_MSM_SUBMIT_FENCE_FD_OUT 0x20000000u

#define A630_GPU_BUFFERS 4096u
#define A630_GPU_BUFFER_MAX (256ull << 20)
#define A630_GPU_SUBMIT_CMDS 64u
#define A630_GPU_SUBMIT_BOS 256u
#define A630_GPU_ASK_AGAIN_US 100u
#define A630_GPU_PATIENCE_MS 5000u

struct a630_user_buffer {
	IOMemoryDescriptor *memory;
	uint8_t *kernel;
	uint64_t address;
	uint64_t size;
	uint32_t flags;
	uint32_t fence;
};

static IOLock *gA630ServiceLock;
static uint32_t gA630LastFence;
static uint32_t gA630SubmitCount;
static uint32_t gA630FaultCount;

static uint64_t
a630_milliseconds_until(const struct a630_timespec *when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t)NSEC_PER_SEC + when->nanoseconds -
	    (int64_t)now;
	return left <= 0 ? 0 :
	    (uint64_t)(left / (int64_t)NSEC_PER_MSEC);
}

static bool
a630_wait_fence_locked(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	if (fence == 0 || native_a630_gpu_kmd_done(fence)) {
		return true;
	}
	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (;;) {
		if (native_a630_gpu_kmd_done(fence)) {
			return true;
		}
		if (mach_absolute_time() - began >= patience) {
			return false;
		}
		IOLockUnlock(gA630ServiceLock);
		delay_for_interval(A630_GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gA630ServiceLock);
	}
}

class A630GPUUserClient;

class A630GPU : public IOService {
	OSDeclareDefaultStructors(A630GPU)

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t task, void *security, UInt32 type,
	    OSDictionary *properties, IOUserClient **client)
	    APPLE_KEXT_OVERRIDE;
};

class A630GPUUserClient : public IOUserClient {
	OSDeclareDefaultStructors(A630GPUUserClient)

	task_t fTask;
	struct a630_user_buffer *fBuffers;
	uint32_t fNextBuffer;
	uint32_t fBufferCount;
	uint64_t fBufferBytes;
	uint32_t fNextQueue;
	uint32_t fSubmissions;

	struct a630_user_buffer *buffer(uint32_t handle);
	struct a630_user_buffer *newBuffer(IOMemoryDescriptor *memory,
	    uint64_t size, uint32_t flags, uint32_t *handle);
	void freeBuffer(struct a630_user_buffer *buffer);
	IOReturn gemNew(struct a630_gem_new *request);
	IOReturn gemInfo(struct a630_gem_info *request);
	IOReturn submit(struct a630_gem_submit *request);

public:
	bool initWithTask(task_t task, void *security, UInt32 type,
	    OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *arguments,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(A630GPU, IOService)
OSDefineMetaClassAndStructors(A630GPUUserClient, IOUserClient)

bool
A630GPU::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	if (gA630ServiceLock == NULL) {
		gA630ServiceLock = IOLockAlloc();
	}
	if (gA630ServiceLock == NULL || !native_a630_gpu_kmd_ready()) {
		return false;
	}
	setProperty("A630PersistentKMD", kOSBooleanTrue);
	setProperty("A630MsmABI", kOSBooleanTrue);
	registerService();
	IOLog("A630GPU: persistent KMD service published\n");
	return true;
}

void
A630GPU::stop(IOService *provider)
{
	IOService::stop(provider);
}

IOReturn
A630GPU::newUserClient(task_t task, void *security, UInt32 type,
    OSDictionary *properties, IOUserClient **client)
{
	A630GPUUserClient *user;
	bool attached = false;

	if (client == NULL || type != 0) {
		return kIOReturnBadArgument;
	}
	*client = NULL;
	user = new A630GPUUserClient;
	if (user == NULL) {
		return kIOReturnNoMemory;
	}
	if (!user->initWithTask(task, security, type, properties) ||
	    !(attached = user->attach(this)) || !user->start(this)) {
		if (attached) {
			user->detach(this);
		}
		user->release();
		return kIOReturnNotOpen;
	}
	*client = user;
	return kIOReturnSuccess;
}

bool
A630GPUUserClient::initWithTask(task_t task, void *security, UInt32 type,
    OSDictionary *properties)
{
	if (task == TASK_NULL || type != 0 ||
	    !IOUserClient::initWithTask(task, security, type, properties)) {
		return false;
	}
	fTask = task;
	return true;
}

bool
A630GPUUserClient::start(IOService *provider)
{
	if (!native_a630_gpu_kmd_ready() ||
	    !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct a630_user_buffer, A630_GPU_BUFFERS);
	if (fBuffers == NULL) {
		return false;
	}
	fNextBuffer = 0;
	fBufferCount = 0;
	fBufferBytes = 0;
	fNextQueue = 0;
	fSubmissions = 0;
	return true;
}

struct a630_user_buffer *
A630GPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > A630_GPU_BUFFERS ||
	    fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

struct a630_user_buffer *
A630GPUUserClient::newBuffer(IOMemoryDescriptor *memory, uint64_t size,
    uint32_t flags, uint32_t *handle)
{
	struct a630_user_buffer *entry = NULL;
	uint64_t address;
	bool mapped = true;

	for (uint32_t n = 0; n < A630_GPU_BUFFERS; n++) {
		uint32_t index = (fNextBuffer + n) % A630_GPU_BUFFERS;

		if (fBuffers[index].memory == NULL) {
			entry = &fBuffers[index];
			*handle = index + 1;
			fNextBuffer = (index + 1) % A630_GPU_BUFFERS;
			break;
		}
	}
	address = native_a630_gpu_kmd_address(size);
	if (entry == NULL || address == 0) {
		if (address != 0) {
			native_a630_gpu_kmd_address_free(address, size);
		}
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 0x1000) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(offset, &length,
		    kIOMemoryMapperNone);

		mapped = phys != 0 && native_a630_gpu_kmd_map(address + offset,
		    phys, (flags & A630_MSM_BO_GPU_READONLY) != 0);
	}
	native_a630_gpu_kmd_mapped();
	if (!mapped) {
		native_a630_gpu_kmd_unmap(address, size);
		native_a630_gpu_kmd_address_free(address, size);
		return NULL;
	}
	entry->memory = memory;
	entry->kernel = (uint8_t *)memory->getBytesNoCopy();
	entry->address = address;
	entry->size = size;
	entry->flags = flags;
	entry->fence = 0;
	fBufferCount++;
	fBufferBytes += size;
	__atomic_add_fetch(&native_a630_gpu_kmd_buffer_count, 1,
	    __ATOMIC_RELAXED);
	__atomic_add_fetch(&native_a630_gpu_kmd_buffer_bytes, (uint32_t)size,
	    __ATOMIC_RELAXED);
	__atomic_store_n(&native_a630_gpu_kmd_mapped_state, 1,
	    __ATOMIC_RELEASE);
	return entry;
}

void
A630GPUUserClient::freeBuffer(struct a630_user_buffer *entry)
{
	if (entry == NULL || entry->memory == NULL) {
		return;
	}
	if (entry->fence != 0 &&
	    !a630_wait_fence_locked(entry->fence, A630_GPU_PATIENCE_MS)) {
		gA630FaultCount++;
		native_a630_gpu_kmd_fault++;
		native_a630_gpu_kmd_report("buffer close timed out");
	}
	native_a630_gpu_kmd_unmap(entry->address, entry->size);
	native_a630_gpu_kmd_address_free(entry->address, entry->size);
	fBufferCount--;
	fBufferBytes -= entry->size;
	__atomic_sub_fetch(&native_a630_gpu_kmd_buffer_count, 1,
	    __ATOMIC_RELAXED);
	__atomic_sub_fetch(&native_a630_gpu_kmd_buffer_bytes, (uint32_t)entry->size,
	    __ATOMIC_RELAXED);
	if (fBufferCount == 0) {
		__atomic_store_n(&native_a630_gpu_kmd_mapped_state, 0,
		    __ATOMIC_RELEASE);
	}
	entry->memory->release();
	bzero(entry, sizeof(*entry));
}

IOReturn
A630GPUUserClient::gemNew(struct a630_gem_new *request)
{
	IOBufferMemoryDescriptor *memory;
	struct a630_user_buffer *entry;
	uint64_t size = round_page_64(request->size);

	if (size == 0 || size > A630_GPU_BUFFER_MAX ||
	    size > UINT32_MAX) {
		return kIOReturnBadArgument;
	}
	IOOptionBits options = (IOOptionBits)kIODirectionInOut |
	    (IOOptionBits)kIOMemoryKernelUserShared |
	    (IOOptionBits)kIOMapWriteCombineCache;
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    options, (vm_size_t)size, PAGE_SIZE);
	if (memory == NULL) {
		return kIOReturnNoMemory;
	}
	IOLockLock(gA630ServiceLock);
	entry = newBuffer(memory, size, request->flags, &request->handle);
	IOLockUnlock(gA630ServiceLock);
	if (entry == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	IOLog("A630GPU: buffer %u: %#llx bytes at GPU %#llx\n",
	    request->handle, (unsigned long long)size,
	    (unsigned long long)entry->address);
	return kIOReturnSuccess;
}

IOReturn
A630GPUUserClient::gemInfo(struct a630_gem_info *request)
{
	struct a630_user_buffer *entry;
	IOReturn status = kIOReturnSuccess;

	IOLockLock(gA630ServiceLock);
	entry = buffer(request->handle);
	if (entry == NULL) {
		status = kIOReturnBadArgument;
	} else if (request->info == A630_MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t)request->handle << 32;
	} else if (request->info == A630_MSM_INFO_GET_IOVA) {
		request->value = entry->address;
	} else if (request->info == A630_MSM_INFO_GET_FLAGS) {
		request->value = entry->flags;
	} else if (request->info != A630_MSM_INFO_SET_NAME) {
		status = kIOReturnUnsupported;
	}
	IOLockUnlock(gA630ServiceLock);
	return status;
}

IOReturn
A630GPUUserClient::submit(struct a630_gem_submit *request)
{
	struct a630_gem_submit_bo *bos = NULL;
	struct a630_gem_submit_cmd *cmds = NULL;
	uint64_t addresses[A630_GPU_SUBMIT_CMDS];
	uint32_t words[A630_GPU_SUBMIT_CMDS];
	uint32_t count = 0;
	IOReturn status = kIOReturnBadArgument;

	if (request->nr_bos == 0 || request->nr_bos > A630_GPU_SUBMIT_BOS ||
	    request->nr_cmds == 0 || request->nr_cmds > A630_GPU_SUBMIT_CMDS) {
		return kIOReturnBadArgument;
	}
	bos = IONew(struct a630_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct a630_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		status = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t)request->bos, bos,
	    request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t)request->cmds, cmds,
	    request->nr_cmds * sizeof(*cmds)) != 0) {
		status = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gA630ServiceLock);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct a630_user_buffer *entry = buffer(bos[i].handle);

		if (entry != NULL && entry->kernel != NULL) {
			native_a630_gpu_kmd_to_memory(entry->kernel, entry->size);
		}
	}
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct a630_user_buffer *entry;

		if (cmds[i].type != A630_MSM_SUBMIT_CMD_BUF ||
		    cmds[i].nr_relocs != 0 || cmds[i].submit_idx >=
		    request->nr_bos || (cmds[i].size & 3) ||
		    (cmds[i].submit_offset & 3)) {
			IOLockUnlock(gA630ServiceLock);
			status = kIOReturnUnsupported;
			goto out;
		}
		entry = buffer(bos[cmds[i].submit_idx].handle);
		if (entry == NULL ||
		    (uint64_t)cmds[i].submit_offset + cmds[i].size >
		    entry->size || cmds[i].size == 0) {
			IOLockUnlock(gA630ServiceLock);
			status = kIOReturnBadArgument;
			goto out;
		}
		addresses[count] = entry->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = native_a630_gpu_kmd_submit(addresses, words, count);
	if (request->fence != 0) {
		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct a630_user_buffer *entry = buffer(bos[i].handle);

			if (entry != NULL) {
				entry->fence = request->fence;
			}
		}
		gA630LastFence = request->fence;
		gA630SubmitCount++;
		if (request->flags & A630_MSM_SUBMIT_FENCE_FD_OUT) {
			request->fence_fd = -1;
		}
		status = kIOReturnSuccess;
	} else {
		gA630FaultCount++;
		native_a630_gpu_kmd_report("submission refused");
		status = kIOReturnBusy;
	}
	IOLockUnlock(gA630ServiceLock);
out:
	if (bos != NULL) {
		IODelete(bos, struct a630_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct a630_gem_submit_cmd, request->nr_cmds);
	}
	return status;
}

IOReturn
A630GPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
	struct a630_user_buffer *entry;
	IOReturn status = kIOReturnBadArgument;

	if (options == NULL || memory == NULL || gA630ServiceLock == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gA630ServiceLock);
	entry = buffer(type);
	if (entry != NULL) {
		entry->memory->retain();
		*memory = entry->memory;
		*options = 0;
		status = kIOReturnSuccess;
	}
	IOLockUnlock(gA630ServiceLock);
	return status;
}

IOReturn
A630GPUUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *dispatch, OSObject *target,
    void *reference)
{
	union {
		struct a630_param param;
		struct a630_gem_new gemNew;
		struct a630_gem_info gemInfo;
		struct a630_gem_cpu_prep cpuPrep;
		struct a630_gem_submit submit;
		struct a630_wait_fence waitFence;
		struct a630_gem_madvise madvise;
		struct a630_submitqueue queue;
		struct a630_submitqueue_query query;
		struct a630_gem_close close;
	} record;
	size_t size;
	IOReturn status = kIOReturnSuccess;

	(void)dispatch;
	(void)target;
	(void)reference;
	if (arguments == NULL || arguments->structureInput == NULL ||
	    arguments->structureInputSize > sizeof(record) ||
	    fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!native_a630_gpu_kmd_ready()) {
		return kIOReturnNotReady;
	}
	size = arguments->structureInputSize;
	bzero(&record, sizeof(record));
	memcpy(&record, arguments->structureInput, size);
	switch (selector) {
	case kMsmGetParam:
		status = native_a630_gpu_kmd_parameter(record.param.param,
		    &record.param.value) ? kIOReturnSuccess :
		    kIOReturnBadArgument;
		break;
	case kMsmSetParam:
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		status = gemNew(&record.gemNew);
		break;
	case kMsmGemInfo:
		status = gemInfo(&record.gemInfo);
		break;
	case kMsmGemCpuPrep: {
		struct a630_user_buffer *entry;
		uint32_t fence;
		uint64_t patience;

		IOLockLock(gA630ServiceLock);
		entry = buffer(record.cpuPrep.handle);
		fence = entry != NULL && entry->fence != 0 ?
		    entry->fence : gA630LastFence;
		patience = (record.cpuPrep.op & A630_MSM_PREP_NOSYNC) ?
		    0 : a630_milliseconds_until(&record.cpuPrep.timeout);
		if (!a630_wait_fence_locked(fence, patience)) {
			status = kIOReturnBusy;
		}
		IOLockUnlock(gA630ServiceLock);
		break;
	}
	case kMsmGemSubmit:
		status = submit(&record.submit);
		break;
	case kMsmWaitFence: {
		uint64_t patience = a630_milliseconds_until(
		    &record.waitFence.timeout);
		IOLockLock(gA630ServiceLock);
		if (!a630_wait_fence_locked(record.waitFence.fence,
		    patience)) {
			status = kIOReturnTimeout;
		}
		IOLockUnlock(gA630ServiceLock);
		break;
	}
	case kMsmGemMadvise:
		record.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		record.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery: {
		uint64_t faults = gA630FaultCount;

		if (record.query.data != 0 &&
		    record.query.len >= sizeof(faults) &&
		    copyout(&faults, (user_addr_t)record.query.data,
		    sizeof(faults)) != 0) {
			status = kIOReturnVMError;
		}
		break;
	}
	case kDrmGemClose: {
		struct a630_user_buffer *entry;

		IOLockLock(gA630ServiceLock);
		entry = buffer(record.close.handle);
		if (entry == NULL) {
			status = kIOReturnBadArgument;
		} else {
			freeBuffer(entry);
		}
		IOLockUnlock(gA630ServiceLock);
		break;
	}
	default:
		IOLog("A630GPU: unsupported request %#x (%zu bytes)\n",
		    selector, size);
		status = kIOReturnUnsupported;
		break;
	}
	if (status == kIOReturnSuccess &&
	    arguments->structureOutput != NULL) {
		if (arguments->structureOutputSize > size) {
			arguments->structureOutputSize = (uint32_t)size;
		}
		memcpy(arguments->structureOutput, &record,
		    arguments->structureOutputSize);
	}
	return status;
}

IOReturn
A630GPUUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

void
A630GPUUserClient::stop(IOService *provider)
{
	if (fBuffers != NULL && gA630ServiceLock != NULL) {
		IOLockLock(gA630ServiceLock);
		for (uint32_t i = 0; i < A630_GPU_BUFFERS; i++) {
			freeBuffer(&fBuffers[i]);
		}
		IOLockUnlock(gA630ServiceLock);
	}
	IOUserClient::stop(provider);
}

void
A630GPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct a630_user_buffer, A630_GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

extern "C" void
native_a630_gpu_service_publish(void)
{
	A630GPU *gpu;
	IOService *parent;
	bool attached = false;

	if (!native_a630_gpu_kmd_ready()) {
		return;
	}
	parent = IOService::getResourceService();
	if (parent == NULL) {
		IOLog("A630GPU: no resource service\n");
		return;
	}
	gpu = new A630GPU;
	if (gpu == NULL || !gpu->init()) {
		OSSafeReleaseNULL(gpu);
		return;
	}
	gpu->setName("A630GPU");
	if (!(attached = gpu->attach(parent)) || !gpu->start(parent)) {
		if (attached) {
			gpu->detach(parent);
		}
		gpu->release();
		IOLog("A630GPU: service publish failed\n");
		return;
	}
	gpu->release();
}

extern "C" void
native_a630_gpu_service_stop(void)
{
	native_a630_gpu_kmd_stop();
}

extern "C" void
native_a630_gpu_static_init(void)
    __asm__("__GLOBAL__sub_I_A630GpuService.cpp");
