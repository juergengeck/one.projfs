#ifndef ASYNC_BRIDGE_H
#define ASYNC_BRIDGE_H

#include <napi.h>
#include <string>
#include <memory>
#include <queue>
#include <mutex>
#include <functional>
#include "content_cache.h"

namespace oneifsprojfs {

class AsyncBridge {
public:
    AsyncBridge(Napi::Env env);
    ~AsyncBridge();

    // Register JavaScript callbacks
    void RegisterCallbacks(const Napi::Object& callbacks);
    
    // Emit debug message
    void EmitDebugMessage(const std::string& message);
    
    // Async operations that update cache
    void FetchFileInfo(const std::string& path);
    void FetchDirectoryListing(const std::string& path);
    void FetchFileContent(const std::string& path);
    
    // Write operations (queued for async processing)
    void QueueCreateFile(const std::string& path, const std::vector<uint8_t>& content);
    void QueueUpdateFile(const std::string& path, const std::vector<uint8_t>& content);
    void QueueDeleteFile(const std::string& path);
    
    // Get cache reference
    std::shared_ptr<ContentCache> GetCache() { return cache_; }
    
    // Set callback for when directory listing is updated
    void SetDirectoryListingUpdatedCallback(std::function<void(const std::string&)> callback) {
        directoryListingUpdatedCallback_ = callback;
    }
    
    // Start/stop background processing
    void Start();
    void Stop();

private:
    // Callbacks from JavaScript
    Napi::ThreadSafeFunction getFileInfoCallback_;
    Napi::ThreadSafeFunction readFileCallback_;
    Napi::ThreadSafeFunction readDirectoryCallback_;
    Napi::ThreadSafeFunction createFileCallback_;
    Napi::ThreadSafeFunction updateFileCallback_;
    Napi::ThreadSafeFunction deleteFileCallback_;
    Napi::ThreadSafeFunction onDebugMessageCallback_;
    
    // Content cache
    std::shared_ptr<ContentCache> cache_;
    
    // Write queue
    struct WriteOperation {
        enum Type { CREATE, UPDATE, DELETE_FILE };
        Type type;
        std::string path;
        std::vector<uint8_t> content;
    };
    std::queue<WriteOperation> writeQueue_;
    std::mutex writeQueueMutex_;
    
    // Helper methods
    void ProcessWriteQueue();
    FileInfo ParseFileInfo(const Napi::Object& jsObject);
    DirectoryListing ParseDirectoryListing(const Napi::Array& jsArray);
    
    // Background thread management
    bool running_ = false;
    
    // Callback when directory listing is updated
    std::function<void(const std::string&)> directoryListingUpdatedCallback_;
};

} // namespace oneifsprojfs

#endif // ASYNC_BRIDGE_H