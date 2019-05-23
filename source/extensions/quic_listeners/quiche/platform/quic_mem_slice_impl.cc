// NOLINT(namespace-envoy)
//
// This file is part of the QUICHE platform implementation, and is not to be
// consumed or referenced directly by other Envoy code. It serves purely as a
// porting layer for QUICHE.

#include "extensions/quic_listeners/quiche/platform/quic_mem_slice_impl.h"

#include "common/common/assert.h"

namespace quic {

namespace {
// Used to align both fragment and buffer at max aligned address.
struct BufferFragmentBundle : public Envoy::Buffer::BufferFragmentImpl {
  static void releasor(const void*, size_t, const Envoy::Buffer::BufferFragmentImpl* fragment) {
    delete fragment;
  }

  explicit BufferFragmentBundle(size_t length)
      : Envoy::Buffer::BufferFragmentImpl(buffer_, length, releasor) {}

  // TODO(danzh) this is not aligned in to page boundary.
  // https://stackoverflow.com/questions/54049474/does-aligning-memory-on-particular-address-boundaries-in-c-c-still-improve-x86/54049733#54049733
  // suggests that on some processors, page-boundary alignment may improve performance.
  // Envoy::Buffer::BufferFragmentImpl fragment_;
  char buffer_[];
};

} // namespace

Envoy::Buffer::BufferFragmentImpl& QuicMemSliceImpl::allocateBufferAndFragment(size_t length) {
  BufferFragmentBundle* bundle = new BufferFragmentBundle(length); // self-frees.
  return *bundle;
}

QuicMemSliceImpl::QuicMemSliceImpl(QuicBufferAllocator* /*allocator*/, size_t length) {
  single_slice_buffer_.addBufferFragment(allocateBufferAndFragment(length));
}

QuicMemSliceImpl::QuicMemSliceImpl(Envoy::Buffer::Instance& buffer, size_t length) {
  ASSERT(firstSliceLength(buffer) == length);
  single_slice_buffer_.move(buffer, length);
  ASSERT(single_slice_buffer_.getRawSlices(nullptr, 0) == 1);
}

const char* QuicMemSliceImpl::data() const {
  Envoy::Buffer::RawSlice out;
  uint64_t num_slices = single_slice_buffer_.getRawSlices(&out, 1);
  ASSERT(num_slices <= 1);
  return static_cast<const char*>(out.mem_);
}

size_t QuicMemSliceImpl::firstSliceLength(Envoy::Buffer::Instance& buffer) {
  Envoy::Buffer::RawSlice slice;
  uint64_t total_num = buffer.getRawSlices(&slice, 1);
  ASSERT(total_num != 0);
  return slice.len_;
}

} // namespace quic
