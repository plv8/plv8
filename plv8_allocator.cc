#include "plv8_allocator.h"

#define RECHECK_INCREMENT 1_MB

size_t operator""_MB( unsigned long long const x ) noexcept { return 1024L * 1024L * x; }

ArrayAllocator::ArrayAllocator(size_t limit) : heap_limit(limit),
											   heap_size(RECHECK_INCREMENT),
											   next_size(RECHECK_INCREMENT),
											   allocated(0) {}

bool ArrayAllocator::check(const size_t length) {
	if (heap_size + allocated + length > next_size) {
		v8::Isolate* isolate = v8::Isolate::GetCurrent();
		v8::HeapStatistics heap_statistics;
		isolate->GetHeapStatistics(&heap_statistics);
		heap_size = heap_statistics.used_heap_size();
		if (heap_size + allocated + length > heap_limit) {
			return false;
			// we need to force GC here,
			// otherwise the next allocation will fail even if there is a space for it
			isolate->LowMemoryNotification();
			isolate->GetHeapStatistics(&heap_statistics);
			heap_size = heap_statistics.used_heap_size();
			if (heap_size + allocated + length > heap_limit) {
				return false;
			}
		}
		next_size = heap_size + allocated + length + RECHECK_INCREMENT;
	}
	return heap_size + allocated + length <= heap_limit;
}

void* ArrayAllocator::Allocate(size_t length) {
	if (check(length)) {
		allocated += length;
		return std::calloc(length, 1);
	} else {
		return nullptr;
	}
}

void* ArrayAllocator::AllocateUninitialized(size_t length) {
	if (check(length)) {
		allocated += length;
		return std::malloc(length);
	} else {
		return nullptr;
	}
}

void ArrayAllocator::Free(void* data, size_t length) {
	allocated -= length;
	next_size -= length;
	std::free(data);
}

void* ArrayAllocator::Reallocate(void *data, size_t old_length, size_t new_length) {
	ssize_t delta = static_cast<ssize_t>(new_length) - static_cast<ssize_t>(old_length);
	if (delta > 0) {
		if (!check(delta)) {
			return nullptr;
		}
	}
	return v8::ArrayBuffer::Allocator::Reallocate(data, old_length, new_length);
}
