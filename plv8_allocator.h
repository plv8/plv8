#ifndef PLV8_PLV8_ALLOCATOR_H
#define PLV8_PLV8_ALLOCATOR_H

#include <atomic>
#include <cstddef>
#include <v8.h>
#include "plv8.h"

size_t operator""_MB(unsigned long long x);

class ArrayAllocator : public v8::ArrayBuffer::Allocator {
private:
	size_t heap_limit;
	std::atomic<size_t> heap_size;
	std::atomic<size_t> next_size;
	std::atomic<size_t> allocated;
	std::atomic<bool> oom_exceeded{false};
	v8::ArrayBuffer::Allocator* allocator;

	bool checkAndReserve(size_t length);

public:
	explicit ArrayAllocator(size_t limit);
	~ArrayAllocator() override;
	void* Allocate(size_t length) final;
	void* AllocateUninitialized(size_t length) final;
	void Free(void* data, size_t length) final;
	size_t getAllocated() const { return allocated.load(std::memory_order_relaxed); }
	bool hasOOM() const { return oom_exceeded.load(std::memory_order_relaxed); }
	void resetOOM() { oom_exceeded.store(false, std::memory_order_relaxed); }
};

#endif //PLV8_PLV8_ALLOCATOR_H

