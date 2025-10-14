#ifndef SYNC_STORAGE_H
#define SYNC_STORAGE_H

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <unordered_map>

namespace oneifsprojfs {

struct ObjectMetadata {
    size_t size;
    std::string type;
    bool isDirectory;
    bool exists;
};

class SyncStorage {
public:
    explicit SyncStorage(const std::string& instancePath);
    ~SyncStorage() = default;

    // Direct disk operations (all synchronous)
    std::optional<std::string> ReadObject(const std::string& hash);
    std::optional<std::vector<uint8_t>> ReadObjectBinary(const std::string& hash);
    std::optional<std::string> ReadObjectSection(const std::string& hash, size_t offset, size_t length);
    
    // Directory operations
    std::vector<std::string> ListObjects();
    std::vector<std::string> ListDirectory(const std::string& virtualPath);
    
    // Metadata operations
    ObjectMetadata GetObjectMetadata(const std::string& hash);
    std::string GetObjectType(const std::string& hash);
    
    // Path utilities
    std::string ExtractHashFromPath(const std::string& virtualPath);
    bool IsObjectPath(const std::string& virtualPath);
    
    // Virtual filesystem operations
    ObjectMetadata GetVirtualPathMetadata(const std::string& virtualPath);
    std::optional<std::string> ReadVirtualPath(const std::string& virtualPath);

private:
    std::filesystem::path instancePath_;
    std::filesystem::path objectsPath_;
    std::filesystem::path vheadsPath_;
    std::filesystem::path rmapsPath_;
    
    // Helper methods
    std::string ReadFirst100Bytes(const std::string& objectPath);
    std::string ExtractTypeFromMicrodata(const std::string& microdata);
    std::string ParseVirtualPath(const std::string& virtualPath);
    
    // Cache for frequently accessed metadata
    mutable std::unordered_map<std::string, ObjectMetadata> metadataCache_;
    mutable std::unordered_map<std::string, std::string> typeCache_;
};

} // namespace oneifsprojfs

#endif // SYNC_STORAGE_H