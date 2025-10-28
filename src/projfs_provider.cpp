#include "projfs_provider.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>
#include <chrono>

#pragma comment(lib, "ProjectedFSLib.lib")

namespace oneifsprojfs {

ProjFSProvider::ProjFSProvider(const std::string& instancePath)
    : storage_(std::make_unique<SyncStorage>(instancePath)),
      virtualizationContext_(nullptr),
      isRunning_(false),
      lastError_("") {
    CoCreateGuid(&virtualizationInstanceId_);
}

ProjFSProvider::~ProjFSProvider() {
    Stop();
}

bool ProjFSProvider::Start(const std::string& virtualRoot) {
    if (isRunning_) {
        return false;
    }

    virtualRoot_ = ToWide(virtualRoot);

    // Ensure the virtualization root exists
    if (!CreateDirectoryW(virtualRoot_.c_str(), nullptr)) {
        DWORD lastErr = static_cast<DWORD>(::GetLastError());
        if (lastErr != ERROR_ALREADY_EXISTS) {
            lastError_ = "Failed to create virtual root directory. Error: " + std::to_string(lastErr);
            return false;
        }
    }

    // CRITICAL FIX: Clear any stale virtualization state from previous crashed instances
    // If the directory was previously a virtualization root with a different instance ID,
    // Windows will ignore our callbacks. We MUST clear the stale state first.
    std::cout << "[ProjFS] Clearing stale virtualization state for: " << virtualRoot << std::endl;

    // Delete the virtualization marker file if it exists
    // This forces Windows to forget the old instance ID
    std::wstring markerPath = virtualRoot_ + L"\\.projfs\\placeholder";
    DeleteFileW(markerPath.c_str());

    // Also try to remove the .projfs directory
    std::wstring projfsDir = virtualRoot_ + L"\\.projfs";
    RemoveDirectoryW(projfsDir.c_str());

    // Mark the directory as the virtualization root with our NEW instance ID
    HRESULT hr = PrjMarkDirectoryAsPlaceholder(
        virtualRoot_.c_str(),
        nullptr,  // targetPathName
        nullptr,  // versionInfo
        &virtualizationInstanceId_
    );

    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_REPARSE_POINT_ENCOUNTERED)) {
        lastError_ = "PrjMarkDirectoryAsPlaceholder failed with HRESULT: " + std::to_string(hr);
        std::cout << "[ProjFS] ERROR: PrjMarkDirectoryAsPlaceholder failed: " << lastError_ << std::endl;
        return false;
    }

    std::cout << "[ProjFS] Directory marked as virtualization root successfully" << std::endl;

    // Set up callbacks
    PRJ_CALLBACKS callbacks = {};
    callbacks.GetPlaceholderInfoCallback = GetPlaceholderInfoCallback;
    callbacks.GetFileDataCallback = GetFileDataCallback;
    callbacks.QueryFileNameCallback = QueryFileNameCallback;
    callbacks.StartDirectoryEnumerationCallback = StartDirectoryEnumerationCallback;
    callbacks.GetDirectoryEnumerationCallback = GetDirectoryEnumerationCallback;
    callbacks.EndDirectoryEnumerationCallback = EndDirectoryEnumerationCallback;
    callbacks.NotificationCallback = NotificationCallback;

    // Configure notification mappings to intercept write operations
    // CRITICAL: Without this, Windows won't call NotificationCallback for file creation/modification
    // and will allow applications to create files (like tmp files) in our virtual filesystem
    PRJ_NOTIFICATION_MAPPING notificationMapping = {};
    notificationMapping.NotificationRoot = L"";  // Apply to entire virtualization root
    notificationMapping.NotificationBitMask =
        PRJ_NOTIFY_NEW_FILE_CREATED |           // Block file creation
        PRJ_NOTIFY_FILE_OVERWRITTEN |           // Block overwrites
        PRJ_NOTIFY_PRE_DELETE |                 // Block deletions
        PRJ_NOTIFY_PRE_RENAME |                 // Block renames
        PRJ_NOTIFY_PRE_SET_HARDLINK |           // Block hardlinks
        PRJ_NOTIFY_FILE_RENAMED |               // Track renames
        PRJ_NOTIFY_HARDLINK_CREATED |           // Track hardlinks
        PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_MODIFIED |  // Track modifications
        PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_DELETED;    // Track deletions

    // Start virtualization
    PRJ_STARTVIRTUALIZING_OPTIONS options = {};
    options.PoolThreadCount = 0;  // Use default
    options.ConcurrentThreadCount = 0;  // Use default
    options.NotificationMappings = &notificationMapping;
    options.NotificationMappingsCount = 1;
    // options.EnableNegativePathCache = TRUE; // May not be available in all SDK versions

    hr = PrjStartVirtualizing(
        virtualRoot_.c_str(),
        &callbacks,
        this,  // instanceContext
        &options,
        &virtualizationContext_
    );

    if (FAILED(hr)) {
        lastError_ = "PrjStartVirtualizing failed with HRESULT: " + std::to_string(hr);
        return false;
    }

    isRunning_ = true;
    return true;
}

void ProjFSProvider::Stop() {
    if (isRunning_ && virtualizationContext_) {
        PrjStopVirtualizing(virtualizationContext_);
        virtualizationContext_ = nullptr;
        isRunning_ = false;
    }
}

bool ProjFSProvider::IsRunning() const {
    return isRunning_;
}

// ProjFS Callbacks

HRESULT CALLBACK ProjFSProvider::GetPlaceholderInfoCallback(const PRJ_CALLBACK_DATA* callbackData) {
    auto* provider = static_cast<ProjFSProvider*>(callbackData->InstanceContext);
    provider->stats_.placeholderRequests++;

    // Convert Windows path to Unix-style path
    std::string relativePath = provider->ToUtf8(callbackData->FilePathName);
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    std::string virtualPath = relativePath.empty() ? "/" : "/" + relativePath;
    
    std::cout << "[ProjFS] GetPlaceholderInfo for: " << virtualPath << std::endl;

    // Get cache reference for both root mount point check and file info lookup
    auto cache = provider->asyncBridge_ ? provider->asyncBridge_->GetCache() : nullptr;

    // Check if this is a root-level mount point by querying the cached root directory listing
    // This avoids hardcoding directory names and automatically handles any mount points
    if (cache && virtualPath.length() > 1 && virtualPath.find('/', 1) == std::string::npos) {
        // Path is of form "/something" (single level, potential root mount point)
        auto rootListing = cache->GetDirectoryListing("/");
        if (rootListing) {
            std::string pathName = virtualPath.substr(1); // Remove leading '/'

            // Check if this path exists in the root directory listing
            for (const auto& entry : rootListing->entries) {
                if (entry.name == pathName && entry.isDirectory) {
                    std::cout << "[ProjFS] Detected root mount point: " << virtualPath << std::endl;

                    // This is a root-level mount point - return consistent directory metadata
                    ObjectMetadata dirMetadata;
                    dirMetadata.exists = true;
                    dirMetadata.isDirectory = true;
                    dirMetadata.size = 0;
                    dirMetadata.type = "DIRECTORY";

                    PRJ_PLACEHOLDER_INFO placeholderInfo = {};
                    placeholderInfo.FileBasicInfo = provider->CreateFileBasicInfo(dirMetadata);

                    return PrjWritePlaceholderInfo(
                        callbackData->NamespaceVirtualizationContext,
                        callbackData->FilePathName,
                        &placeholderInfo,
                        sizeof(placeholderInfo)
                    );
                }
            }
        }
    }

    // Try to get file info from cache
    if (cache) {
        // First check if we have specific file info
        auto fileInfo = cache->GetFileInfo(virtualPath);
        if (fileInfo) {
            // Convert cached FileInfo to ObjectMetadata
            ObjectMetadata metadata;
            metadata.exists = true;
            metadata.isDirectory = fileInfo->isDirectory;
            metadata.size = fileInfo->size;
            metadata.type = fileInfo->isDirectory ? "DIRECTORY" : "FILE";
            
            PRJ_PLACEHOLDER_INFO placeholderInfo = {};
            placeholderInfo.FileBasicInfo = provider->CreateFileBasicInfo(metadata);

            provider->stats_.cacheHits++;
            std::cout << "[ProjFS] GetPlaceholderInfo: Found FileInfo in cache for " << virtualPath
                      << " (size: " << metadata.size << ")" << std::endl;
            return PrjWritePlaceholderInfo(
                callbackData->NamespaceVirtualizationContext,
                callbackData->FilePathName,
                &placeholderInfo,
                sizeof(placeholderInfo)
            );
        }
        
        // Check if file exists in parent directory listing
        size_t lastSlash = virtualPath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            std::string parentPath = virtualPath.substr(0, lastSlash);
            std::string fileName = virtualPath.substr(lastSlash + 1);
            
            if (parentPath.empty()) parentPath = "/";
            
            auto dirListing = cache->GetDirectoryListing(parentPath);
            if (dirListing) {
                // Look for this file in the directory listing
                for (const auto& entry : dirListing->entries) {
                    if (entry.name == fileName) {
                        // Found it! Create placeholder info from directory entry
                        ObjectMetadata metadata;
                        metadata.exists = true;
                        metadata.isDirectory = entry.isDirectory;
                        metadata.size = entry.size;
                        metadata.type = entry.isDirectory ? "DIRECTORY" : "FILE";
                        
                        PRJ_PLACEHOLDER_INFO placeholderInfo = {};
                        placeholderInfo.FileBasicInfo = provider->CreateFileBasicInfo(metadata);

                        provider->stats_.cacheHits++;
                        std::cout << "[ProjFS] GetPlaceholderInfo: Found in parent directory listing: "
                                  << virtualPath << " (size: " << entry.size << ")" << std::endl;
                        return PrjWritePlaceholderInfo(
                            callbackData->NamespaceVirtualizationContext,
                            callbackData->FilePathName,
                            &placeholderInfo,
                            sizeof(placeholderInfo)
                        );
                    }
                }
            }
        }
        
        provider->stats_.cacheMisses++;
        std::cout << "[ProjFS] GetPlaceholderInfo: Cache miss for " << virtualPath << std::endl;
    }
    
    // Fall back to disk storage for BLOB/CLOB if it's in /objects path
    if (virtualPath.compare(0, 9, "/objects/") == 0) {
        ObjectMetadata metadata = provider->storage_->GetVirtualPathMetadata(virtualPath);
        if (metadata.exists) {
            PRJ_PLACEHOLDER_INFO placeholderInfo = {};
            placeholderInfo.FileBasicInfo = provider->CreateFileBasicInfo(metadata);

            // File size is handled by FileBasicInfo for ProjFS

            return PrjWritePlaceholderInfo(
                callbackData->NamespaceVirtualizationContext,
                callbackData->FilePathName,
                &placeholderInfo,
                sizeof(placeholderInfo)
            );
        }
    }
    
    // Request async fetch from JavaScript
    if (provider->asyncBridge_) {
        provider->asyncBridge_->FetchFileInfo(virtualPath);
        // Return file not found for now - the cache will be populated for next access
        // Returning ERROR_IO_PENDING can cause Explorer to hang
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT CALLBACK ProjFSProvider::GetFileDataCallback(
    const PRJ_CALLBACK_DATA* callbackData,
    UINT64 byteOffset,
    UINT32 length) {
    
    std::cout << "[ProjFS] GetFileDataCallback CALLED! Path: " << callbackData->FilePathName 
              << " Offset: " << byteOffset << " Length: " << length << std::endl;
    
    auto* provider = static_cast<ProjFSProvider*>(callbackData->InstanceContext);
    provider->stats_.fileDataRequests++;

    // Convert Windows path to Unix-style path
    std::string relativePath = provider->ToUtf8(callbackData->FilePathName);
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    std::string virtualPath = relativePath.empty() ? "/" : "/" + relativePath;
    
    std::cout << "[ProjFS] GetFileDataCallback virtualPath: " << virtualPath << std::endl;
    
    // Check cache first
    auto cache = provider->asyncBridge_ ? provider->asyncBridge_->GetCache() : nullptr;
    if (cache) {
        std::cout << "[ProjFS] GetFileData: Checking cache for " << virtualPath << std::endl;
        auto content = cache->GetFileContent(virtualPath);
        if (content && !content->data.empty()) {
            std::cout << "[ProjFS] GetFileData: Found cached content, size: " << content->data.size() << std::endl;
            // Use cached content
            size_t contentSize = content->data.size();
            if (byteOffset >= contentSize) {
                return S_OK;  // Nothing to write
            }

            size_t bytesToWrite = (std::min)((size_t)length, contentSize - (size_t)byteOffset);
            provider->stats_.bytesRead += bytesToWrite;
            provider->stats_.cacheHits++;

            // Allocate aligned buffer for ProjFS
            void* buffer = PrjAllocateAlignedBuffer(
                callbackData->NamespaceVirtualizationContext,
                bytesToWrite
            );

            if (!buffer) {
                return E_OUTOFMEMORY;
            }

            // Copy data to buffer
            memcpy(buffer, content->data.data() + byteOffset, bytesToWrite);

            // Write data
            HRESULT hr = PrjWriteFileData(
                callbackData->NamespaceVirtualizationContext,
                &callbackData->DataStreamId,
                buffer,
                byteOffset,
                bytesToWrite
            );

            PrjFreeAlignedBuffer(buffer);
            std::cout << "[ProjFS] GetFileData: Successfully served " << bytesToWrite << " bytes from cache" << std::endl;
            return hr;
        } else {
            std::cout << "[ProjFS] GetFileData: No cached content found for " << virtualPath << std::endl;
        }
        provider->stats_.cacheMisses++;
    }
    
    // For /objects paths, try direct disk access for BLOB/CLOB
    if (virtualPath.compare(0, 9, "/objects/") == 0) {
        auto content = provider->storage_->ReadVirtualPath(virtualPath);
        if (content) {
            size_t contentSize = content->size();
            if (byteOffset >= contentSize) {
                return S_OK;  // Nothing to write
            }

            size_t bytesToWrite = (std::min)((size_t)length, contentSize - (size_t)byteOffset);
            provider->stats_.bytesRead += bytesToWrite;

            void* buffer = PrjAllocateAlignedBuffer(
                callbackData->NamespaceVirtualizationContext,
                bytesToWrite
            );

            if (!buffer) {
                return E_OUTOFMEMORY;
            }

            memcpy(buffer, content->data() + byteOffset, bytesToWrite);

            HRESULT hr = PrjWriteFileData(
                callbackData->NamespaceVirtualizationContext,
                &callbackData->DataStreamId,
                buffer,
                byteOffset,
                bytesToWrite
            );

            PrjFreeAlignedBuffer(buffer);
            return hr;
        }
    }

    
    // For other paths, content should have been prefetched
    // If content is not in cache, return ERROR_IO_PENDING and fetch async
    std::cout << "[ProjFS] Content not cached for " << virtualPath 
              << " - returning ERROR_IO_PENDING and fetching" << std::endl;
    
    if (provider->asyncBridge_) {
        // Store pending request for later completion
        {
            std::lock_guard<std::mutex> lock(provider->pendingRequestsMutex_);
            ProjFSProvider::PendingFileRequest request;
            request.virtualPath = virtualPath;
            request.byteOffset = byteOffset;
            request.length = length;
            request.virtualizationContext = callbackData->NamespaceVirtualizationContext;
            request.dataStreamId = callbackData->DataStreamId;
            
            provider->pendingFileRequests_[callbackData->CommandId] = request;
            std::cout << "[ProjFS] Stored pending request for CommandId: " << callbackData->CommandId
                      << ", path: " << virtualPath << std::endl;
        }
        
        // Trigger async fetch
        std::cout << "[ProjFS] GetFileData: Triggering background fetch for " << virtualPath << std::endl;
        provider->asyncBridge_->FetchFileContent(virtualPath);
        
        // Return ERROR_IO_PENDING so Windows knows to wait for completion
        return HRESULT_FROM_WIN32(ERROR_IO_PENDING);
    }
    
    // Fallback if no async bridge
    std::cout << "[ProjFS] No async bridge available for " << virtualPath << std::endl;
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

void ProjFSProvider::CompletePendingFileRequests(const std::string& virtualPath) {
    std::lock_guard<std::mutex> lock(pendingRequestsMutex_);

    // Normalize the incoming path for comparison
    std::string normalizedIncomingPath = virtualPath;
    std::replace(normalizedIncomingPath.begin(), normalizedIncomingPath.end(), '\\', '/');
    if (!normalizedIncomingPath.empty() && normalizedIncomingPath[0] != '/') {
        normalizedIncomingPath = "/" + normalizedIncomingPath;
    }

    // Find all pending requests for this path
    std::vector<INT32> completedCommands;

    for (auto& [commandId, request] : pendingFileRequests_) {
        // Normalize the stored path for comparison
        std::string normalizedStoredPath = request.virtualPath;
        std::replace(normalizedStoredPath.begin(), normalizedStoredPath.end(), '\\', '/');
        if (!normalizedStoredPath.empty() && normalizedStoredPath[0] != '/') {
            normalizedStoredPath = "/" + normalizedStoredPath;
        }

        if (normalizedStoredPath == normalizedIncomingPath) {
            std::cout << "[ProjFS] Completing pending request for CommandId: " << commandId
                      << ", stored path: " << request.virtualPath
                      << ", incoming path: " << virtualPath
                      << ", normalized: " << normalizedIncomingPath << std::endl;
            
            // Get the cached content now that it should be available
            auto cache = asyncBridge_ ? asyncBridge_->GetCache() : nullptr;
            if (cache) {
                auto content = cache->GetFileContent(virtualPath);
                if (content && !content->data.empty()) {
                    // Prepare the data
                    size_t contentSize = content->data.size();
                    UINT64 byteOffset = request.byteOffset;
                    UINT32 length = request.length;
                    
                    if (byteOffset < contentSize) {
                        size_t bytesToWrite = (std::min)((size_t)length, contentSize - (size_t)byteOffset);
                        
                        // Allocate aligned buffer for ProjFS
                        void* buffer = PrjAllocateAlignedBuffer(
                            request.virtualizationContext,
                            bytesToWrite
                        );
                        
                        if (buffer) {
                            // Copy data to buffer
                            memcpy(buffer, content->data.data() + byteOffset, bytesToWrite);
                            
                            // Write data
                            HRESULT dataHr = PrjWriteFileData(
                                request.virtualizationContext,
                                &request.dataStreamId,
                                buffer,
                                byteOffset,
                                bytesToWrite
                            );
                            
                            PrjFreeAlignedBuffer(buffer);
                            
                            // Complete the command
                            HRESULT completeHr = PrjCompleteCommand(
                                request.virtualizationContext,
                                commandId,
                                dataHr,
                                nullptr
                            );
                            
                            std::cout << "[ProjFS] Completed command " << commandId 
                                      << " with " << bytesToWrite << " bytes, dataHr=" << std::hex << dataHr 
                                      << ", completeHr=" << std::hex << completeHr << std::endl;
                            
                            stats_.bytesRead += bytesToWrite;
                            stats_.cacheHits++;
                        } else {
                            // Complete with error
                            PrjCompleteCommand(request.virtualizationContext, commandId, E_OUTOFMEMORY, nullptr);
                            std::cout << "[ProjFS] Completed command " << commandId << " with E_OUTOFMEMORY" << std::endl;
                        }
                    } else {
                        // Complete with success but no data
                        PrjCompleteCommand(request.virtualizationContext, commandId, S_OK, nullptr);
                        std::cout << "[ProjFS] Completed command " << commandId << " with S_OK (no data)" << std::endl;
                    }
                } else {
                    // Complete with file not found
                    PrjCompleteCommand(request.virtualizationContext, commandId, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), nullptr);
                    std::cout << "[ProjFS] Completed command " << commandId << " with ERROR_FILE_NOT_FOUND" << std::endl;
                }
            } else {
                // Complete with error
                PrjCompleteCommand(request.virtualizationContext, commandId, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), nullptr);
                std::cout << "[ProjFS] Completed command " << commandId << " with ERROR_FILE_NOT_FOUND (no cache)" << std::endl;
            }
            
            completedCommands.push_back(commandId);
        }
    }
    
    // Remove completed requests
    for (INT32 commandId : completedCommands) {
        pendingFileRequests_.erase(commandId);
    }
    
    std::cout << "[ProjFS] Completed " << completedCommands.size() << " pending requests for " << virtualPath << std::endl;
}

HRESULT CALLBACK ProjFSProvider::QueryFileNameCallback(const PRJ_CALLBACK_DATA* callbackData) {
    // We don't support case-insensitive matching
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT CALLBACK ProjFSProvider::StartDirectoryEnumerationCallback(
    const PRJ_CALLBACK_DATA* callbackData,
    const GUID* enumerationId) {
    auto* provider = static_cast<ProjFSProvider*>(callbackData->InstanceContext);
    
    // Track active enumerations
    provider->stats_.activeEnumerations++;
    
    // Debug: Log enumeration start
    char guidStr[40];
    sprintf_s(guidStr, sizeof(guidStr), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        enumerationId->Data1, enumerationId->Data2, enumerationId->Data3,
        enumerationId->Data4[0], enumerationId->Data4[1], enumerationId->Data4[2], enumerationId->Data4[3],
        enumerationId->Data4[4], enumerationId->Data4[5], enumerationId->Data4[6], enumerationId->Data4[7]);
    
    // Convert Windows path to Unix-style path
    std::string relativePath = provider->ToUtf8(callbackData->FilePathName);
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    std::string path = relativePath.empty() ? "[ROOT]" : "/" + relativePath;
    
    std::cout << "[ProjFS] START ENUM " << guidStr << " for path: " << path
        << " (active: " << provider->stats_.activeEnumerations << ")" << std::endl;

    if (provider->asyncBridge_) {
        std::stringstream msg;
        msg << "[ProjFS] START ENUM " << guidStr << " for path: " << path
            << " (active: " << provider->stats_.activeEnumerations << ")";
        provider->asyncBridge_->EmitDebugMessage(msg.str());
    }
    
    // Reset enumeration state for this session
    std::lock_guard<std::mutex> lock(provider->enumerationMutex_);
    
    // Check if this enumeration already exists (shouldn't happen normally)
    auto it = provider->enumerationStates_.find(*enumerationId);
    if (it != provider->enumerationStates_.end()) {
        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] WARNING: Enumeration already exists - this might cause issues!";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
    }
    
    provider->enumerationStates_[*enumerationId] = EnumerationState{};
    
    return S_OK;
}

HRESULT CALLBACK ProjFSProvider::GetDirectoryEnumerationCallback(
    const PRJ_CALLBACK_DATA* callbackData,
    const GUID* enumerationId,
    PCWSTR searchExpression,
    PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle) {
    
    auto* provider = static_cast<ProjFSProvider*>(callbackData->InstanceContext);
    provider->stats_.directoryEnumerations++;
    provider->stats_.enumerationCallbacks++;

    // Convert Windows path to Unix-style path
    std::string relativePath = provider->ToUtf8(callbackData->FilePathName);
    
    // Replace backslashes with forward slashes
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    
    // Ensure path starts with / for IFileSystem
    std::string virtualPath = relativePath.empty() ? "/" : "/" + relativePath;
    
    // Log the path transformation
    std::cout << "[ProjFS] GetDirEnum - FilePathName: '" << provider->ToUtf8(callbackData->FilePathName) 
              << "' -> virtualPath: '" << virtualPath << "'" << std::endl;
    
    // Log search expression
    std::wstring searchExpr = searchExpression ? searchExpression : L"*";
    std::cout << "[ProjFS] searchExpr: " << provider->ToUtf8(searchExpr) << std::endl;
    
    // Get or create enumeration state
    std::unique_lock<std::mutex> lock(provider->enumerationMutex_);
    
    // Check if this enumeration exists
    auto it = provider->enumerationStates_.find(*enumerationId);
    if (it == provider->enumerationStates_.end()) {
        // This shouldn't happen - StartDirectoryEnumerationCallback should have created it
        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] WARNING: Enumeration ID not found for " << virtualPath 
                << " - creating new state (this might indicate a bug!)";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
        // Create a new state
        provider->enumerationStates_[*enumerationId] = EnumerationState{};
    }
    
    auto& enumState = provider->enumerationStates_[*enumerationId];
    
    // CRITICAL: Log the current state before any modifications
    {
        std::stringstream msg;
        msg << "[ProjFS] ENUM STATE BEFORE for " << virtualPath 
            << " - entries.size: " << enumState.entries.size()
            << ", nextIndex: " << enumState.nextIndex
            << ", isComplete: " << enumState.isComplete
            << ", callCount: " << enumState.callCount;
        std::cout << msg.str() << std::endl;  // Direct console output
        if (provider->asyncBridge_) {
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
    }
    
    // Check for restart scan flag
    if (callbackData->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) {
        // Explorer wants to restart the enumeration from the beginning
        enumState.nextIndex = 0;
        enumState.callCount = 0;  // Reset call count on restart
        enumState.entries.clear();  // Clear entries to force re-fetch
        enumState.isComplete = false;  // Reset completion state
        enumState.isLoading = false;  // Reset loading state
        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] RESTART SCAN requested for " << virtualPath << " - clearing state";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
    }
    
    // Safety check to prevent infinite loops
    enumState.callCount++;
    if (enumState.callCount > EnumerationState::MAX_CALLS_PER_ENUM) {
        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] ERROR: Enumeration loop detected for " << virtualPath 
                << " - aborting after " << enumState.callCount << " calls";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
        return S_OK;  // Return empty to break the loop
    }
    
    // Debug: log enumeration info
    char guidStr[40];
    sprintf_s(guidStr, sizeof(guidStr), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        enumerationId->Data1, enumerationId->Data2, enumerationId->Data3,
        enumerationId->Data4[0], enumerationId->Data4[1], enumerationId->Data4[2], enumerationId->Data4[3],
        enumerationId->Data4[4], enumerationId->Data4[5], enumerationId->Data4[6], enumerationId->Data4[7]);
    if (provider->asyncBridge_) {
        std::stringstream msg;
        msg << "[ProjFS] GetDirEnum for " << virtualPath << " enum: " << guidStr 
            << " nextIndex: " << enumState.nextIndex 
            << " entries: " << enumState.entries.size()
            << " isLoading: " << enumState.isLoading
            << " isComplete: " << enumState.isComplete;
        provider->asyncBridge_->EmitDebugMessage(msg.str());
    }
    
    // If this is the first call for this enumeration, populate the entries
    if (enumState.entries.empty() && !enumState.isComplete) {
        // Check if already loading to prevent duplicate fetches
        if (enumState.isLoading) {
            // Another thread is loading, wait for it to complete
            provider->enumerationCv_.wait(lock, [&enumState] {
                return !enumState.isLoading;
            });
            // After waiting, check if we now have entries
            if (!enumState.entries.empty() || enumState.isComplete) {
                // Entries are now available, continue with enumeration
                goto process_entries;
            }
            // If still no entries, something went wrong, return empty
            return S_OK;
        }
        
        enumState.isLoading = true;
        lock.unlock(); // Release lock while getting entries
        
        // Try cache first
        auto cache = provider->asyncBridge_ ? provider->asyncBridge_->GetCache() : nullptr;
        bool gotEntries = false;
        
        if (cache) {
            auto listing = cache->GetDirectoryListing(virtualPath);
            if (listing) {
                provider->stats_.cacheHits++;
                // Store full FileInfo, not just names
                enumState.entries = listing->entries;
                gotEntries = true;
            } else {
                provider->stats_.cacheMisses++;
            }
        }
        
        // If not in cache, use storage ONLY for /objects paths
        if (!gotEntries && (virtualPath == "/objects" || virtualPath.compare(0, 9, "/objects/") == 0)) {
            auto storageEntries = provider->storage_->ListDirectory(virtualPath);
            // Convert string names to FileInfo structures
            for (const auto& name : storageEntries) {
                FileInfo info;
                info.name = name;
                info.isDirectory = false;  // Objects are files
                info.size = 0;  // Will be determined on demand
                info.isBlobOrClob = true;
                info.mode = 0;
                enumState.entries.push_back(info);
            }
            gotEntries = true;

            // Debug: log what we got from storage
            if (provider->asyncBridge_) {
                std::stringstream msg;
                msg << "[ProjFS] Got " << enumState.entries.size() << " entries for path: " << virtualPath;
                provider->asyncBridge_->EmitDebugMessage(msg.str());
                for (const auto& e : enumState.entries) {
                    msg.str("");
                    msg << "[ProjFS]   - " << e.name;
                    provider->asyncBridge_->EmitDebugMessage(msg.str());
                }
            }
        }
        
        // Get root directory from cache/filesystem like any other directory
        // No hardcoding - but we need to handle the initial case
        
        // For all paths including root, request async fetch if not in cache
        if (!gotEntries && provider->asyncBridge_) {
            // Request the directory listing first
            provider->asyncBridge_->FetchDirectoryListing(virtualPath);
            
            // Wait for the async operation to complete with timeout
            auto timeout = std::chrono::milliseconds(5000); // 5 second timeout
            auto startTime = std::chrono::steady_clock::now();
            
            // Poll for the listing to appear in cache
            while (std::chrono::steady_clock::now() - startTime < timeout) {
                // Check cache for the listing (no lock needed for cache reads)
                if (cache) {
                    auto listing = cache->GetDirectoryListing(virtualPath);
                    if (listing) {
                        provider->stats_.cacheHits++;
                        // Store full FileInfo, not just names
                        enumState.entries = listing->entries;
                        gotEntries = true;

                        if (provider->asyncBridge_) {
                            std::stringstream msg;
                            msg << "[ProjFS] Async load completed for " << virtualPath
                                << " - got " << enumState.entries.size() << " entries from cache";
                            provider->asyncBridge_->EmitDebugMessage(msg.str());
                        }
                        break;
                    }
                }
                
                // Wait a bit before checking again
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            if (!gotEntries) {
                // Timeout - log warning
                if (provider->asyncBridge_) {
                    std::stringstream msg;
                    msg << "[ProjFS] WARNING: Timeout waiting for directory listing for " << virtualPath;
                    provider->asyncBridge_->EmitDebugMessage(msg.str());
                }
            }
        }
        
        // Re-acquire lock before modifying enumeration state
        lock.lock();
        
        // Only mark as complete if we're not loading entries
        enumState.isLoading = false;
        enumState.isComplete = true;
        
        // Notify any waiting threads that loading is complete
        provider->enumerationCv_.notify_all();
    }
    
process_entries:
    
    // Sanity check: ensure nextIndex is valid
    if (enumState.nextIndex >= enumState.entries.size()) {
        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] ENUMERATION COMPLETE for " << virtualPath 
                << " - all " << enumState.entries.size() << " entries returned";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
        // Mark enumeration as truly complete
        enumState.isComplete = true;
        return S_OK;  // No more entries to return
    }
    
    // Return entries starting from nextIndex
    size_t entriesAdded = 0;
    size_t totalEntries = enumState.entries.size();
    
    // Debug log at start of enumeration
    std::cout << "[ProjFS] Starting enumeration return for " << virtualPath 
              << " - nextIndex: " << enumState.nextIndex 
              << ", totalEntries: " << totalEntries << std::endl;
    if (provider->asyncBridge_) {
        std::stringstream msg;
        msg << "[ProjFS] Starting enumeration return for " << virtualPath 
            << " - nextIndex: " << enumState.nextIndex 
            << ", totalEntries: " << totalEntries;
        provider->asyncBridge_->EmitDebugMessage(msg.str());
    }
    
    // Convert search expression to string for filtering
    std::wstring searchPattern = searchExpression ? searchExpression : L"*";
    
    while (enumState.nextIndex < enumState.entries.size()) {
        const auto& entryInfo = enumState.entries[enumState.nextIndex];

        // Skip empty entries (shouldn't happen but be safe)
        if (entryInfo.name.empty()) {
            enumState.nextIndex++;
            continue;
        }

        // Check if entry matches search pattern
        std::wstring wideEntry = provider->ToWide(entryInfo.name);
        if (!PrjFileNameMatch(wideEntry.c_str(), searchPattern.c_str())) {
            // Entry doesn't match search pattern, skip it
            enumState.nextIndex++;
            continue;
        }

        // Convert FileInfo to ObjectMetadata
        ObjectMetadata entryMeta;
        entryMeta.exists = true;
        entryMeta.isDirectory = entryInfo.isDirectory;
        entryMeta.size = entryInfo.size;
        entryMeta.type = entryInfo.isDirectory ? "DIRECTORY" : "FILE";
        
        PRJ_FILE_BASIC_INFO fileInfo = provider->CreateFileBasicInfo(entryMeta);
        // wideEntry already defined above for pattern matching

        // Debug log the file attributes being set
        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] Filling entry: " << entryInfo.name
                << " IsDirectory=" << (fileInfo.IsDirectory ? "TRUE" : "FALSE")
                << " FileSize=" << fileInfo.FileSize
                << " FileAttributes=0x" << std::hex << fileInfo.FileAttributes << std::dec
                << " (entryMeta.size=" << entryMeta.size
                << ", entryInfo.size=" << entryInfo.size << ")";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }

        HRESULT hr = PrjFillDirEntryBuffer(
            wideEntry.c_str(),
            &fileInfo,
            dirEntryBufferHandle
        );

        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            // Buffer is full, we'll continue from this index next time
            if (provider->asyncBridge_) {
                std::stringstream msg;
                msg << "[ProjFS] BUFFER FULL for " << virtualPath
                    << " after " << entriesAdded << " entries"
                    << ", nextIndex stays at " << enumState.nextIndex
                    << " (entry: " << entryInfo.name << ")";
                provider->asyncBridge_->EmitDebugMessage(msg.str());
            }
            // CRITICAL: Do NOT increment nextIndex when buffer is full
            // We need to retry this same entry next time
            break;
        }

        if (FAILED(hr)) {
            // Other error - log it and skip this entry
            if (provider->asyncBridge_) {
                std::stringstream msg;
                msg << "[ProjFS] ERROR: PrjFillDirEntryBuffer failed for " << entryInfo.name
                    << " in " << virtualPath << " with HRESULT 0x"
                    << std::hex << hr << std::dec
                    << " (isDirectory=" << entryInfo.isDirectory
                    << ", size=" << entryInfo.size << ")";
                provider->asyncBridge_->EmitDebugMessage(msg.str());
            }
            // Skip this entry and continue
            enumState.nextIndex++;
            continue;
        }

        // Successfully added entry, move to next
        enumState.nextIndex++;
        entriesAdded++;

        if (provider->asyncBridge_) {
            std::stringstream msg;
            msg << "[ProjFS] Added entry #" << entriesAdded << ": " << entryInfo.name
                << " (nextIndex now: " << enumState.nextIndex << ")";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
    }
    
    if (provider->asyncBridge_) {
        std::stringstream msg;
        msg << "[ProjFS] ENUM CALLBACK COMPLETE for " << virtualPath 
            << ": returned " << entriesAdded << " entries"
            << ", nextIndex=" << enumState.nextIndex
            << ", total=" << enumState.entries.size()
            << ", hasMore=" << (enumState.nextIndex < enumState.entries.size())
            << ", totalCallbacks=" << provider->stats_.enumerationCallbacks;
        provider->asyncBridge_->EmitDebugMessage(msg.str());
    }
    
    return S_OK;
}

HRESULT CALLBACK ProjFSProvider::EndDirectoryEnumerationCallback(
    const PRJ_CALLBACK_DATA* callbackData,
    const GUID* enumerationId) {
    auto* provider = static_cast<ProjFSProvider*>(callbackData->InstanceContext);
    
    // Debug: Log enumeration end
    if (provider->asyncBridge_) {
        char guidStr[40];
        sprintf_s(guidStr, sizeof(guidStr), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            enumerationId->Data1, enumerationId->Data2, enumerationId->Data3,
            enumerationId->Data4[0], enumerationId->Data4[1], enumerationId->Data4[2], enumerationId->Data4[3],
            enumerationId->Data4[4], enumerationId->Data4[5], enumerationId->Data4[6], enumerationId->Data4[7]);
        
        std::lock_guard<std::mutex> lock(provider->enumerationMutex_);
        auto it = provider->enumerationStates_.find(*enumerationId);
        if (it != provider->enumerationStates_.end()) {
            std::stringstream msg;
            msg << "[ProjFS] END ENUM " << guidStr 
                << " - processed " << it->second.nextIndex 
                << " of " << it->second.entries.size() << " entries";
            provider->asyncBridge_->EmitDebugMessage(msg.str());
        }
    }
    
    // Track active enumerations
    provider->stats_.activeEnumerations--;
    
    // Clean up enumeration state
    std::lock_guard<std::mutex> lock(provider->enumerationMutex_);
    provider->enumerationStates_.erase(*enumerationId);
    
    return S_OK;
}

HRESULT CALLBACK ProjFSProvider::NotificationCallback(
    const PRJ_CALLBACK_DATA* callbackData,
    BOOLEAN isDirectory,
    PRJ_NOTIFICATION notification,
    PCWSTR destinationFileName,
    PRJ_NOTIFICATION_PARAMETERS* operationParameters) {

    auto* provider = static_cast<ProjFSProvider*>(callbackData->InstanceContext);
    std::string relativePath = provider->ToUtf8(callbackData->FilePathName);
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    std::string virtualPath = relativePath.empty() ? "/" : "/" + relativePath;

    // Log the notification for debugging
    const char* notificationName = "UNKNOWN";
    switch (notification) {
        case PRJ_NOTIFICATION_FILE_OPENED: notificationName = "FILE_OPENED"; break;
        case PRJ_NOTIFICATION_NEW_FILE_CREATED: notificationName = "NEW_FILE_CREATED"; break;
        case PRJ_NOTIFICATION_FILE_OVERWRITTEN: notificationName = "FILE_OVERWRITTEN"; break;
        case PRJ_NOTIFICATION_PRE_DELETE: notificationName = "PRE_DELETE"; break;
        case PRJ_NOTIFICATION_PRE_RENAME: notificationName = "PRE_RENAME"; break;
        case PRJ_NOTIFICATION_PRE_SET_HARDLINK: notificationName = "PRE_SET_HARDLINK"; break;
        case PRJ_NOTIFICATION_FILE_RENAMED: notificationName = "FILE_RENAMED"; break;
        case PRJ_NOTIFICATION_HARDLINK_CREATED: notificationName = "HARDLINK_CREATED"; break;
        case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_NO_MODIFICATION: notificationName = "FILE_CLOSED_NO_MOD"; break;
        case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED: notificationName = "FILE_CLOSED_MODIFIED"; break;
        case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED: notificationName = "FILE_CLOSED_DELETED"; break;
        case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL: notificationName = "PRE_CONVERT_TO_FULL"; break;
    }

    std::cout << "[ProjFS] NOTIFICATION: " << notificationName
              << " for path: " << virtualPath
              << " (isDirectory: " << (isDirectory ? "TRUE" : "FALSE") << ")" << std::endl;

    // For read-only virtual filesystem, deny all modifications
    switch (notification) {
        case PRJ_NOTIFICATION_FILE_OPENED:
        case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_NO_MODIFICATION:
        case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL:
            // These are informational only - allow
            return S_OK;

        case PRJ_NOTIFICATION_NEW_FILE_CREATED:
        case PRJ_NOTIFICATION_FILE_OVERWRITTEN:
        case PRJ_NOTIFICATION_PRE_DELETE:
        case PRJ_NOTIFICATION_PRE_RENAME:
        case PRJ_NOTIFICATION_PRE_SET_HARDLINK:
            // DENY all write operations with detailed logging
            std::cout << "[ProjFS] BLOCKED " << notificationName << " for: " << virtualPath << std::endl;
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);

        case PRJ_NOTIFICATION_FILE_RENAMED:
        case PRJ_NOTIFICATION_HARDLINK_CREATED:
        case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED:
        case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED:
            // Post-operation notifications - just log them
            std::cout << "[ProjFS] POST-OP notification: " << notificationName << " for: " << virtualPath << std::endl;
            return S_OK;

        default:
            // Unknown notification - deny it to be safe
            std::cout << "[ProjFS] BLOCKED unknown notification 0x" << std::hex << notification
                      << std::dec << " for: " << virtualPath << std::endl;
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
}

// Helper methods

std::wstring ProjFSProvider::ToWide(const std::string& str) {
    if (str.empty()) return L"";
    
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring result(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, result.data(), size);
    return result;
}

std::string ProjFSProvider::ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

PRJ_FILE_BASIC_INFO ProjFSProvider::CreateFileBasicInfo(const ObjectMetadata& metadata) {
    PRJ_FILE_BASIC_INFO info = {};
    
    if (metadata.isDirectory) {
        info.IsDirectory = TRUE;
        info.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info.IsDirectory = FALSE;
        info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        info.FileSize = metadata.size;
    }
    
    // Set timestamps to current time
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    info.CreationTime.LowPart = ft.dwLowDateTime;
    info.CreationTime.HighPart = ft.dwHighDateTime;
    info.LastWriteTime = info.CreationTime;
    info.LastAccessTime = info.CreationTime;
    info.ChangeTime = info.CreationTime;
    
    return info;
}

void ProjFSProvider::OnDirectoryListingUpdated(const std::string& path) {
    // When a directory listing is updated in the cache, notify any waiting enumeration threads
    std::lock_guard<std::mutex> lock(enumerationMutex_);

    // Find any enumeration states that might be waiting for this path
    for (auto& [guid, enumState] : enumerationStates_) {
        if (enumState.isLoading) {
            // Mark as no longer loading since the data is now in cache
            enumState.isLoading = false;
        }
    }

    // Notify all waiting threads to check the cache again
    enumerationCv_.notify_all();

    if (asyncBridge_) {
        std::stringstream msg;
        msg << "[ProjFS] Directory listing updated for path: " << path;
        asyncBridge_->EmitDebugMessage(msg.str());
    }
}

bool ProjFSProvider::InvalidateTombstone(const std::string& virtualPath) {
    if (!isRunning_ || !virtualizationContext_) {
        std::cout << "[ProjFS] Cannot invalidate tombstone - provider not running" << std::endl;
        return false;
    }

    // Convert to Windows path (remove leading slash)
    std::string windowsPath = virtualPath;
    if (!windowsPath.empty() && windowsPath[0] == '/') {
        windowsPath = windowsPath.substr(1);
    }

    // Replace forward slashes with backslashes for Windows
    std::replace(windowsPath.begin(), windowsPath.end(), '/', '\\');

    std::wstring widePath = ToWide(windowsPath);

    std::cout << "[ProjFS] Invalidating tombstone for: " << virtualPath
              << " (Windows path: " << windowsPath << ")" << std::endl;

    // Use PrjDeleteFile to remove the tombstone cache entry
    // This tells Windows to forget that the file was deleted
    HRESULT hr = PrjDeleteFile(
        virtualizationContext_,
        widePath.c_str(),
        PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_TOMBSTONE,
        nullptr  // failureReason
    );

    if (SUCCEEDED(hr)) {
        std::cout << "[ProjFS] Successfully invalidated tombstone for: " << virtualPath << std::endl;

        // Clear our internal caches too
        if (cache_) {
            cache_->InvalidatePath(virtualPath);
        }

        return true;
    } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        // File not in tombstone cache - this is fine, it means there's no tombstone to clear
        std::cout << "[ProjFS] No tombstone to invalidate for: " << virtualPath << std::endl;
        return true;
    } else {
        std::cout << "[ProjFS] Failed to invalidate tombstone for: " << virtualPath
                  << " HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
}

} // namespace oneifsprojfs