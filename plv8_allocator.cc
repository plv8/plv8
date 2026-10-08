#include "plv8_allocator.h"

#include <cstdlib>

#define RECHECK_INCREMENT 1_MB

size_t operator""_MB(unsigned long long const x) { return 1024ULL * 1024ULL * x; }

ArrayAllocator::ArrayAllocator(size_t limit)
	: heap_limit(limit),
	  heap_size(RECHECK_INCREMENT),
	  next_size(RECHECK_INCREMENT),
	  allocated(0),
	  oom_exceeded(false),
	  allocator(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {}

ArrayAllocator::~ArrayAllocator() {
	delete this->allocator;
}

bool ArrayAllocator::checkAndReserve(const size_t length) {
	if (length == 0)
		return true;

	size_t cur_alloc = allocated.load(std::memory_order_relaxed);
	size_t cur_heap = heap_size.load(std::memory_order_relaxed);

	if (length > heap_limit || cur_alloc > heap_limit - length) {
		oom_exceeded.store(true, std::memory_order_relaxed);
		return false;
	}

	if (cur_heap + cur_alloc + length > next_size.load(std::memory_order_relaxed)) {
		if (is_main_pg_thread()) {
			v8::Isolate* isolate = v8::Isolate::GetCurrent();
			if (isolate != nullptr) {
				v8::HeapStatistics heap_statistics;
				isolate->GetHeapStatistics(&heap_statistics);
				cur_heap = heap_statistics.used_heap_size();
				heap_size.store(cur_heap, std::memory_order_relaxed);
			}
		}
		if (cur_heap > heap_limit || cur_alloc > heap_limit - cur_heap ||
			length > (heap_limit - cur_heap - cur_alloc)) {
			oom_exceeded.store(true, std::memory_order_relaxed);
			return false;
		}
		next_size.store(cur_heap + cur_alloc + length + RECHECK_INCREMENT,
						std::memory_order_relaxed);
	}

	size_t cur = allocated.load(std::memory_order_relaxed);
	while (true) {
		size_t ch = heap_size.load(std::memory_order_relaxed);
		if (length > heap_limit || ch > heap_limit ||
			cur > heap_limit - ch || length > (heap_limit - ch - cur)) {
			oom_exceeded.store(true, std::memory_order_relaxed);
			return false;
		}
		if (allocated.compare_exchange_weak(
				cur, cur + length,
				std::memory_order_acq_rel,
				std::memory_order_relaxed)) {
			oom_exceeded.store(false, std::memory_order_relaxed);
			return true;
		}
	}
}

void* ArrayAllocator::Allocate(size_t length) {
	if (!checkAndReserve(length)) {
		return nullptr;
	}
	void* ptr = this->allocator->Allocate(length);
	if (ptr == nullptr && length > 0) {
		allocated.fetch_sub(length, std::memory_order_acq_rel);
		oom_exceeded.store(true, std::memory_order_relaxed);
	}
	return ptr;
}

void* ArrayAllocator::AllocateUninitialized(size_t length) {
	if (!checkAndReserve(length)) {
		return nullptr;
	}
	void* ptr = this->allocator->AllocateUninitialized(length);
	if (ptr == nullptr && length > 0) {
		allocated.fetch_sub(length, std::memory_order_acq_rel);
		oom_exceeded.store(true, std::memory_order_relaxed);
	}
	return ptr;
}

void ArrayAllocator::Free(void* data, size_t length) {
	if (data == nullptr)
		return;
	this->allocator->Free(data, length);
	if (length > 0) {
		size_t cur = allocated.load(std::memory_order_relaxed);
		while (true) {
			size_t next = (cur >= length) ? (cur - length) : 0;
			if (allocated.compare_exchange_weak(
					cur, next,
					std::memory_order_acq_rel,
					std::memory_order_relaxed)) {
				break;
			}
		}
		size_t ns = next_size.load(std::memory_order_relaxed);
		while (true) {
			size_t next_ns = (ns >= length) ? (ns - length) : 0;
			if (next_size.compare_exchange_weak(
					ns, next_ns,
					std::memory_order_acq_rel,
					std::memory_order_relaxed)) {
				break;
			}
		}
	}
}

