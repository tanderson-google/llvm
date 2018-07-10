//==- ThreadedStreamingCache.h - Cache for StreamingMemoryObject -*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef THREADEDSTREAMINGCACHE_H
#define THREADEDSTREAMINGCACHE_H

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/StreamingMemoryObject.h"

namespace llvm {

// An implementation of StreamingMemoryObject for use in multithreaded
// translation. Each thread has one of these objects, each of which has a
// pointer to a shared StreamingMemoryObject. This object is effectively
// a thread-local cache for the bitcode streamer to avoid contention, since
// bits are only read from the bitcode stream one word at a time.

class ThreadedStreamingCache : public llvm::StreamingMemoryObject {
 public:
  explicit ThreadedStreamingCache(llvm::StreamingMemoryObject *S);
  uint64_t getExtent() const override;
  uint64_t readBytes(uint8_t *Buf, uint64_t Size,
                     uint64_t Address) const override;
  const uint8_t *getPointer(uint64_t Address,
                            uint64_t Size) const override {
    // This could be fixed by ensuring the bytes are fetched and making a copy,
    // requiring that the bitcode size be known, or otherwise ensuring that
    // the memory doesn't go away/get reallocated, but it's
    // not currently necessary. Users that need the pointer don't stream.
    llvm_unreachable("getPointer in streaming memory objects not allowed");
    return NULL;
  }
  bool isValidAddress(uint64_t Address) const override;

  /// Drop s bytes from the front of the stream, pushing the positions of the
  /// remaining bytes down by s. This is used to skip past the bitcode header,
  /// since we don't know a priori if it's present, and we can't put bytes
  /// back into the stream once we've read them.
  bool dropLeadingBytes(size_t S) override;

  /// If the data object size is known in advance, many of the operations can
  /// be made more efficient, so this method should be called before reading
  /// starts (although it can be called anytime).
  void setKnownObjectSize(size_t Size) override;
 private:
  const static uint64_t kCacheSize = 4 * 4096;
  const static uint64_t kCacheSizeMask = ~(kCacheSize - 1);
  static llvm::sys::SmartMutex<false> StreamerLock;

  // Fetch up to kCacheSize worth of data starting from Address, into the
  // CacheBase, and set MinObjectSize to the new known edge.
  // If at EOF, MinObjectSize reflects the final size.
  void fetchCacheLine(uint64_t Address) const;

  llvm::StreamingMemoryObject *Streamer;
  // Cached data for addresses [CacheBase, CacheBase + kCacheSize)
  mutable std::vector<unsigned char> Cache;
  // The MemoryObject is at least this size. Used as a cache for isValidAddress.
  mutable uint64_t MinObjectSize;
  // Current base address for the cache.
  mutable uint64_t CacheBase;

  ThreadedStreamingCache(
      const ThreadedStreamingCache&) = delete;
  void operator=(const ThreadedStreamingCache&) = delete;
};

} // namespace llvm

#endif // THREADEDSTREAMINGCACHE_H
