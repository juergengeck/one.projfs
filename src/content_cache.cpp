#include "content_cache.h"
#include <algorithm>
#include <iostream>

namespace oneifsprojfs {

ContentCache::ContentCache() {
    // Initialize cache
}

void ContentCache::SetFileInfo(const std::string& path, const FileInfo& info) {
    std::unique_lock lock(mutex_);
    fileInfoCache_[path] = {info, std::chrono::steady_clock::now()};
    
    // Periodically clean expired entries
    static size_t insertCount = 0;
    if (++insertCount % 100 == 0) {
        CleanExpiredEntries();
    }
}

std::optional<FileInfo> ContentCache::GetFileInfo(const std::string& path) const {
    std::shared_lock lock(mutex_);
    
    auto it = fileInfoCache_.find(path);
    if (it != fileInfoCache_.end() && it->second.IsValid(ttl_)) {
        hits_++;
        return it->second.data;
    }
    
    misses_++;
    return std::nullopt;
}

void ContentCache::SetDirectoryListing(const std::string& path, const DirectoryListing& listing) {
    std::unique_lock lock(mutex_);
    directoryCache_[path] = {listing, std::chrono::steady_clock::now()};
    
    std::cout << "[Cache] SetDirectoryListing: Stored " << listing.entries.size() 
              << " entries for path: '" << path << "'" << std::endl;
    std::cout << "[Cache] Directory cache now has " << directoryCache_.size() << " paths" << std::endl;
    
    // Clean if cache is getting too large
    if (directoryCache_.size() > 1000) {
        CleanExpiredEntries();
    }
}

std::optional<DirectoryListing> ContentCache::GetDirectoryListing(const std::string& path) const {
    std::shared_lock lock(mutex_);
    
    std::cout << "[Cache] GetDirectoryListing: Looking for path: '" << path << "'" << std::endl;
    std::cout << "[Cache] Cache contains " << directoryCache_.size() << " paths:" << std::endl;
    for (const auto& [key, value] : directoryCache_) {
        std::cout << "[Cache]   - '" << key << "' (" << value.data.entries.size() << " entries)" << std::endl;
    }
    
    auto it = directoryCache_.find(path);
    if (it != directoryCache_.end()) {
        if (it->second.IsValid(ttl_)) {
            hits_++;
            std::cout << "[Cache] HIT: Found " << it->second.data.entries.size() 
                      << " entries for '" << path << "'" << std::endl;
            return it->second.data;
        } else {
            std::cout << "[Cache] EXPIRED: Entry for '" << path << "' is too old" << std::endl;
        }
    } else {
        std::cout << "[Cache] MISS: No entry for '" << path << "'" << std::endl;
    }
    
    misses_++;
    return std::nullopt;
}

void ContentCache::SetFileContent(const std::string& path, const FileContent& content) {
    std::unique_lock lock(mutex_);
    
    // Only cache small files to avoid memory bloat
    if (content.data.size() <= 1024 * 1024) { // 1MB limit
        contentCache_[path] = {content, std::chrono::steady_clock::now()};
        
        // Clean if content cache is getting too large
        if (contentCache_.size() > 100) {
            CleanExpiredEntries();
        }
    }
}

std::optional<FileContent> ContentCache::GetFileContent(const std::string& path) const {
    std::shared_lock lock(mutex_);
    
    auto it = contentCache_.find(path);
    if (it != contentCache_.end() && it->second.IsValid(ttl_)) {
        hits_++;
        return it->second.data;
    }
    
    misses_++;
    return std::nullopt;
}

void ContentCache::InvalidatePath(const std::string& path) {
    std::unique_lock lock(mutex_);
    
    fileInfoCache_.erase(path);
    directoryCache_.erase(path);
    contentCache_.erase(path);
    
    // Also invalidate parent directory listing
    auto lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos) {
        std::string parentPath = path.substr(0, lastSlash);
        directoryCache_.erase(parentPath);
    }
}

void ContentCache::InvalidateAll() {
    std::unique_lock lock(mutex_);
    fileInfoCache_.clear();
    directoryCache_.clear();
    contentCache_.clear();
}

void ContentCache::SetCacheTTL(std::chrono::seconds ttl) {
    std::unique_lock lock(mutex_);
    ttl_ = ttl;
}

ContentCache::CacheStats ContentCache::GetStats() const {
    std::shared_lock lock(mutex_);
    
    size_t memoryUsage = 0;
    
    // Estimate memory usage
    for (const auto& [path, entry] : fileInfoCache_) {
        memoryUsage += path.size() + sizeof(FileInfo) + entry.data.name.size() + entry.data.hash.size();
    }
    
    for (const auto& [path, entry] : directoryCache_) {
        memoryUsage += path.size() + sizeof(DirectoryListing);
        for (const auto& file : entry.data.entries) {
            memoryUsage += sizeof(FileInfo) + file.name.size() + file.hash.size();
        }
    }
    
    for (const auto& [path, entry] : contentCache_) {
        memoryUsage += path.size() + entry.data.data.size();
    }
    
    return {
        hits_,
        misses_,
        fileInfoCache_.size() + directoryCache_.size() + contentCache_.size(),
        memoryUsage
    };
}

void ContentCache::CleanExpiredEntries() {
    auto now = std::chrono::steady_clock::now();
    
    // Clean file info cache
    for (auto it = fileInfoCache_.begin(); it != fileInfoCache_.end();) {
        if (!it->second.IsValid(ttl_)) {
            it = fileInfoCache_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Clean directory cache
    for (auto it = directoryCache_.begin(); it != directoryCache_.end();) {
        if (!it->second.IsValid(ttl_)) {
            it = directoryCache_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Clean content cache
    for (auto it = contentCache_.begin(); it != contentCache_.end();) {
        if (!it->second.IsValid(ttl_)) {
            it = contentCache_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace oneifsprojfs