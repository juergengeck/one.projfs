#ifndef CONTENT_CACHE_H
#define CONTENT_CACHE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <optional>
#include <variant>

namespace oneifsprojfs {

struct FileInfo {
    std::string name;
    std::string hash;
    size_t size;
    bool isDirectory;
    bool isBlobOrClob;  // If true, can be read directly from disk
    uint32_t mode;
};

struct DirectoryListing {
    std::vector<FileInfo> entries;
};

struct FileContent {
    std::vector<uint8_t> data;
    std::string hash;  // For direct disk access if BLOB/CLOB
};

class ContentCache {
public:
    ContentCache();
    ~ContentCache() = default;

    // Cache operations
    void SetFileInfo(const std::string& path, const FileInfo& info);
    std::optional<FileInfo> GetFileInfo(const std::string& path) const;
    
    void SetDirectoryListing(const std::string& path, const DirectoryListing& listing);
    std::optional<DirectoryListing> GetDirectoryListing(const std::string& path) const;
    
    void SetFileContent(const std::string& path, const FileContent& content);
    std::optional<FileContent> GetFileContent(const std::string& path) const;
    
    // Cache management
    void InvalidatePath(const std::string& path);
    void InvalidateAll();
    void SetCacheTTL(std::chrono::seconds ttl);
    
    // Statistics
    struct CacheStats {
        size_t hits;
        size_t misses;
        size_t entries;
        size_t memoryUsage;
    };
    CacheStats GetStats() const;

private:
    template<typename T>
    struct CacheEntry {
        T data;
        std::chrono::steady_clock::time_point timestamp;
        
        bool IsValid(std::chrono::seconds ttl) const {
            auto age = std::chrono::steady_clock::now() - timestamp;
            return age < ttl;
        }
    };
    
    mutable std::shared_mutex mutex_;
    std::chrono::seconds ttl_ = std::chrono::seconds(3600); // 1 hour TTL
    
    // Separate caches for different data types
    std::unordered_map<std::string, CacheEntry<FileInfo>> fileInfoCache_;
    std::unordered_map<std::string, CacheEntry<DirectoryListing>> directoryCache_;
    std::unordered_map<std::string, CacheEntry<FileContent>> contentCache_;
    
    // Statistics
    mutable size_t hits_ = 0;
    mutable size_t misses_ = 0;
    
    // Helper to clean expired entries
    void CleanExpiredEntries();
};

} // namespace oneifsprojfs

#endif // CONTENT_CACHE_H