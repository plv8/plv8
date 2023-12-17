#ifndef PLV8_PLV8_ALLOCATOR_H
#define PLV8_PLV8_ALLOCATOR_H

#include <v8.h>
#include "plv8.h"

size_t operator""_MB( unsigned long long x );

class ArrayAllocator : public v8::ArrayBuffer::Allocator {
private:
	size_t heap_limit;
	size_t heap_size;
	std::atomic<size_t> next_size;
	std::atomic<size_t> allocated;
	v8::ArrayBuffer::Allocator* allocator;

	bool check(size_t length);

public:
	explicit ArrayAllocator(size_t limit);
	~ArrayAllocator();
	void* Allocate(size_t length) final;
	void* AllocateUninitialized(size_t length) final;
	void Free(void* data, size_t length) final;
	void* Reallocate(void *data, size_t old_length, size_t new_length) final;
};

#endif //PLV8_PLV8_ALLOCATOR_H
