# ERROR_IO_PENDING Implementation

This document describes the implementation of the ERROR_IO_PENDING pattern in the one.ifsprojfs module, as specified in fix.md.

## Overview

The ERROR_IO_PENDING pattern allows ProjFS callbacks to return immediately when content isn't available, then complete the operation asynchronously once the content is fetched. This prevents blocking the Windows filesystem API while JavaScript code performs async operations.

## Implementation

### 1. GetFileDataCallback Changes

**File**: `src/projfs_provider.cpp:300-350`

When content is not found in cache:
```cpp
// Return ERROR_IO_PENDING and store command for later completion
{
    std::lock_guard<std::mutex> lock(provider->pendingRequestsMutex_);
    PendingFileRequest request;
    request.virtualPath = virtualPath;
    request.byteOffset = byteOffset;
    request.length = length;
    request.virtualizationContext = callbackData->NamespaceVirtualizationContext;
    request.dataStreamId = callbackData->DataStreamId;  // Copy GUID value
    
    provider->pendingFileRequests_[callbackData->CommandId] = request;
}

// Trigger async fetch
provider->asyncBridge_->FetchFileContent(virtualPath);

// Return ERROR_IO_PENDING so Windows knows to wait for completion
return HRESULT_FROM_WIN32(ERROR_IO_PENDING);
```

### 2. Command Tracking

**File**: `src/projfs_provider.h:149-157`

Added structure to track pending commands:
```cpp
struct PendingFileRequest {
    std::string virtualPath;
    UINT64 byteOffset;
    UINT32 length;
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT virtualizationContext;
    GUID dataStreamId;  // Copy the GUID value from PRJ_DATA_STREAM_ID
};
mutable std::unordered_map<INT32, PendingFileRequest> pendingFileRequests_;
```

### 3. Command Completion

**File**: `src/projfs_provider.cpp:360-449`

The `CompletePendingFileRequests` method completes pending commands:
```cpp
HRESULT completeHr = PrjCompleteCommand(
    request.virtualizationContext,
    commandId,
    dataHr,
    nullptr  // Extended parameters
);
```

**Key Fix**: Used correct 4-parameter signature for `PrjCompleteCommand` instead of 3 parameters.

### 4. Integration with Cache System

**File**: `src/filer/CachedProjFSProvider.ts:354-365`

When JavaScript fetches content, it automatically triggers command completion:
```typescript
// Push content to native cache immediately
if (self.nativeProvider && typeof (self.nativeProvider as any).setCachedContent === 'function') {
    const content = Buffer.isBuffer(result.content) ? result.content : Buffer.from(result.content);
    (self.nativeProvider as any).setCachedContent(path, content);
    
    // Complete any pending file requests for this path
    if (typeof (self.nativeProvider as any).completePendingFileRequests === 'function') {
        (self.nativeProvider as any).completePendingFileRequests(path);
    }
}
```

## Benefits

1. **Non-blocking**: ProjFS callbacks never wait for slow async operations
2. **Proper UX**: Windows Explorer shows loading indicator instead of "file not found"
3. **Scalable**: Multiple file requests can be pending simultaneously
4. **Cache-friendly**: Once cached, subsequent access is immediate

## User Experience

- First access to a file: Brief loading indicator, then content appears
- Subsequent access: Instant display from cache
- Large files: Progressive loading with proper Windows feedback
- Network errors: Proper error handling instead of hanging

## Technical Notes

- Commands are tracked by `CommandId` provided by Windows ProjFS
- Thread-safe implementation using mutex for pending request map  
- Graceful handling of cache misses and async fetch failures
- Proper resource cleanup when commands complete or fail

This implementation follows Microsoft's recommended pattern for handling async operations in ProjFS providers.