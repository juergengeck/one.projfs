#include "async_bridge.h"
#include <thread>
#include <iostream>

namespace oneifsprojfs {

AsyncBridge::AsyncBridge(Napi::Env env) 
    : cache_(std::make_shared<ContentCache>()) {
}

AsyncBridge::~AsyncBridge() {
    Stop();
}

void AsyncBridge::EmitDebugMessage(const std::string& message) {
    if (!onDebugMessageCallback_) return;
    
    onDebugMessageCallback_.NonBlockingCall([message](Napi::Env env, Napi::Function jsCallback) {
        jsCallback.Call({Napi::String::New(env, message)});
    });
}

void AsyncBridge::RegisterCallbacks(const Napi::Object& callbacks) {
    auto env = callbacks.Env();
    
    // Register file info callback
    if (callbacks.Has("getFileInfo")) {
        auto getFileInfo = callbacks.Get("getFileInfo").As<Napi::Function>();
        getFileInfoCallback_ = Napi::ThreadSafeFunction::New(
            env,
            getFileInfo,
            "getFileInfo",
            0,
            1
        );
    }
    
    // Register read file callback
    if (callbacks.Has("readFile")) {
        auto readFile = callbacks.Get("readFile").As<Napi::Function>();
        readFileCallback_ = Napi::ThreadSafeFunction::New(
            env,
            readFile,
            "readFile",
            0,
            1
        );
    }
    
    // Register read directory callback
    if (callbacks.Has("readDirectory")) {
        auto readDirectory = callbacks.Get("readDirectory").As<Napi::Function>();
        readDirectoryCallback_ = Napi::ThreadSafeFunction::New(
            env,
            readDirectory,
            "readDirectory",
            0,
            1
        );
    }
    
    // Register write callbacks
    if (callbacks.Has("createFile")) {
        auto createFile = callbacks.Get("createFile").As<Napi::Function>();
        createFileCallback_ = Napi::ThreadSafeFunction::New(
            env,
            createFile,
            "createFile",
            0,
            1
        );
    }
    
    // Register debug message callback
    if (callbacks.Has("onDebugMessage")) {
        auto onDebugMessage = callbacks.Get("onDebugMessage").As<Napi::Function>();
        onDebugMessageCallback_ = Napi::ThreadSafeFunction::New(
            env,
            onDebugMessage,
            "onDebugMessage",
            0,
            1
        );
    }
}

void AsyncBridge::FetchFileInfo(const std::string& path) {
    if (!getFileInfoCallback_) return;
    
    // Call JavaScript async function
    getFileInfoCallback_.NonBlockingCall([this, path](Napi::Env env, Napi::Function jsCallback) {
        // Call the JavaScript function with path
        auto result = jsCallback.Call({Napi::String::New(env, path)});
        
        // Handle the promise
        if (result.IsPromise()) {
            auto promise = result.As<Napi::Promise>();
            auto thenFunc = promise.Get("then").As<Napi::Function>();
            
            // Create callback for promise resolution
            auto onResolve = Napi::Function::New(env, [this, path](const Napi::CallbackInfo& info) {
                auto env = info.Env();
                if (info.Length() > 0 && info[0].IsObject()) {
                    auto fileInfo = ParseFileInfo(info[0].As<Napi::Object>());
                    cache_->SetFileInfo(path, fileInfo);
                }
                return env.Undefined();
            });
            
            thenFunc.Call(promise, {onResolve});
        }
    });
}

void AsyncBridge::FetchDirectoryListing(const std::string& path) {
    std::cout << "[AsyncBridge-DEBUG] FetchDirectoryListing ENTRY for path: " << path << std::endl;

    if (!readDirectoryCallback_) {
        std::cout << "[AsyncBridge-DEBUG] ERROR: No readDirectoryCallback registered!" << std::endl;
        EmitDebugMessage("[AsyncBridge] FetchDirectoryListing called but no callback registered for path: " + path);
        return;
    }

    std::cout << "[AsyncBridge-DEBUG] readDirectoryCallback is registered, calling NonBlockingCall" << std::endl;
    EmitDebugMessage("[AsyncBridge] FetchDirectoryListing called for path: " + path);

    readDirectoryCallback_.NonBlockingCall([this, path](Napi::Env env, Napi::Function jsCallback) {
        std::cout << "[AsyncBridge-DEBUG] Inside NonBlockingCall lambda for path: " << path << std::endl;
        EmitDebugMessage("[AsyncBridge] Calling JavaScript readDirectory for path: " + path);
        std::cout << "[AsyncBridge-DEBUG] About to call jsCallback.Call" << std::endl;
        auto result = jsCallback.Call({Napi::String::New(env, path)});
        std::cout << "[AsyncBridge-DEBUG] jsCallback.Call returned" << std::endl;

        if (result.IsPromise()) {
            std::cout << "[AsyncBridge-DEBUG] Result is a Promise" << std::endl;
            auto promise = result.As<Napi::Promise>();
            auto thenFunc = promise.Get("then").As<Napi::Function>();
            
            auto onResolve = Napi::Function::New(env, [this, path](const Napi::CallbackInfo& info) {
                auto env = info.Env();
                if (info.Length() > 0 && info[0].IsArray()) {
                    // DISABLED: Let JavaScript handle caching via setCachedDirectory
                    // This prevents double-caching and data corruption
                    // auto listing = ParseDirectoryListing(info[0].As<Napi::Array>());
                    // cache_->SetDirectoryListing(path, listing);
                    
                    // Still notify the provider that directory listing has been updated
                    if (directoryListingUpdatedCallback_) {
                        directoryListingUpdatedCallback_(path);
                    }
                }
                return env.Undefined();
            });
            
            thenFunc.Call(promise, {onResolve});
        }
    });
}

void AsyncBridge::FetchFileContent(const std::string& path) {
    std::cout << "[TEST-1.1] FetchFileContent ENTRY: path='" << path << "'" << std::endl;

    if (!readFileCallback_) {
        std::cout << "[TEST-1.1] ERROR: readFileCallback_ is NULL!" << std::endl;
        return;
    }
    std::cout << "[TEST-1.1] readFileCallback_ is valid" << std::endl;

    readFileCallback_.NonBlockingCall([this, path](Napi::Env env, Napi::Function jsCallback) {
        std::cout << "[TEST-1.2] Lambda ENTRY: path='" << path << "'" << std::endl;
        std::cout << "[TEST-1.2] Creating Napi::String from path..." << std::endl;

        auto pathString = Napi::String::New(env, path);

        std::cout << "[TEST-1.3] Napi::String created, value: '"
                  << pathString.Utf8Value() << "'" << std::endl;
        std::cout << "[TEST-1.4] jsCallback.IsFunction(): "
                  << (jsCallback.IsFunction() ? "true" : "false") << std::endl;
        std::cout << "[TEST-1.4] Calling jsCallback with 1 argument..." << std::endl;

        auto result = jsCallback.Call({pathString});

        std::cout << "[TEST-1.4] jsCallback returned successfully" << std::endl;
        std::cout << "[TEST-1.4] result.IsPromise(): "
                  << (result.IsPromise() ? "true" : "false") << std::endl;
        
        if (result.IsPromise()) {
            auto promise = result.As<Napi::Promise>();
            auto thenFunc = promise.Get("then").As<Napi::Function>();
            
            auto onResolve = Napi::Function::New(env, [this, path](const Napi::CallbackInfo& info) {
                auto env = info.Env();
                if (info.Length() > 0 && info[0].IsBuffer()) {
                    auto buffer = info[0].As<Napi::Buffer<uint8_t>>();
                    std::vector<uint8_t> data(buffer.Data(), buffer.Data() + buffer.Length());
                    
                    FileContent content;
                    content.data = std::move(data);
                    cache_->SetFileContent(path, content);
                }
                return env.Undefined();
            });
            
            thenFunc.Call(promise, {onResolve});
        }
    });
}

void AsyncBridge::QueueCreateFile(const std::string& path, const std::vector<uint8_t>& content) {
    std::lock_guard<std::mutex> lock(writeQueueMutex_);
    writeQueue_.push({WriteOperation::CREATE, path, content});
}

void AsyncBridge::QueueUpdateFile(const std::string& path, const std::vector<uint8_t>& content) {
    std::lock_guard<std::mutex> lock(writeQueueMutex_);
    writeQueue_.push({WriteOperation::UPDATE, path, content});
}

void AsyncBridge::QueueDeleteFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(writeQueueMutex_);
    writeQueue_.push({WriteOperation::DELETE_FILE, path, {}});
}

FileInfo AsyncBridge::ParseFileInfo(const Napi::Object& jsObject) {
    FileInfo info;
    
    if (jsObject.Has("name")) {
        info.name = jsObject.Get("name").As<Napi::String>().Utf8Value();
    }
    if (jsObject.Has("hash")) {
        info.hash = jsObject.Get("hash").As<Napi::String>().Utf8Value();
    }
    if (jsObject.Has("size")) {
        info.size = jsObject.Get("size").As<Napi::Number>().Uint32Value();
    }
    if (jsObject.Has("isDirectory")) {
        info.isDirectory = jsObject.Get("isDirectory").As<Napi::Boolean>().Value();
    }
    if (jsObject.Has("isBlobOrClob")) {
        info.isBlobOrClob = jsObject.Get("isBlobOrClob").As<Napi::Boolean>().Value();
    }
    if (jsObject.Has("mode")) {
        info.mode = jsObject.Get("mode").As<Napi::Number>().Uint32Value();
    }
    
    return info;
}

DirectoryListing AsyncBridge::ParseDirectoryListing(const Napi::Array& jsArray) {
    DirectoryListing listing;
    
    for (uint32_t i = 0; i < jsArray.Length(); i++) {
        if (jsArray.Get(i).IsObject()) {
            auto fileInfo = ParseFileInfo(jsArray.Get(i).As<Napi::Object>());
            listing.entries.push_back(fileInfo);
        }
    }
    
    return listing;
}

void AsyncBridge::Start() {
    running_ = true;
    
    // Start background thread for write queue processing
    std::thread([this]() {
        while (running_) {
            ProcessWriteQueue();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

void AsyncBridge::Stop() {
    running_ = false;
    
    // Release callbacks
    if (getFileInfoCallback_) {
        getFileInfoCallback_.Release();
    }
    if (readFileCallback_) {
        readFileCallback_.Release();
    }
    if (readDirectoryCallback_) {
        readDirectoryCallback_.Release();
    }
    if (createFileCallback_) {
        createFileCallback_.Release();
    }
    if (onDebugMessageCallback_) {
        onDebugMessageCallback_.Release();
    }
}

void AsyncBridge::ProcessWriteQueue() {
    std::queue<WriteOperation> toProcess;
    
    {
        std::lock_guard<std::mutex> lock(writeQueueMutex_);
        std::swap(toProcess, writeQueue_);
    }
    
    while (!toProcess.empty()) {
        auto& op = toProcess.front();
        
        switch (op.type) {
            case WriteOperation::CREATE:
                if (createFileCallback_) {
                    createFileCallback_.NonBlockingCall([op](Napi::Env env, Napi::Function jsCallback) {
                        auto path = Napi::String::New(env, op.path);
                        auto content = Napi::Buffer<uint8_t>::Copy(env, op.content.data(), op.content.size());
                        jsCallback.Call({path, content});
                    });
                }
                break;
                
            case WriteOperation::UPDATE:
                // Similar for update
                break;
                
            case WriteOperation::DELETE_FILE:
                // Similar for delete
                break;
        }
        
        toProcess.pop();
    }
}

} // namespace oneifsprojfs