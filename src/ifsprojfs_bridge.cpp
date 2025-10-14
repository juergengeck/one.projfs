#include <napi.h>
#include "projfs_provider.h"
#include "async_bridge.h"
#include <memory>
#include <iostream>

using namespace oneifsprojfs;

class IFSProjFSBridge : public Napi::ObjectWrap<IFSProjFSBridge> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "IFSProjFSProvider", {
            InstanceMethod("registerCallbacks", &IFSProjFSBridge::RegisterCallbacks),
            InstanceMethod("start", &IFSProjFSBridge::Start),
            InstanceMethod("stop", &IFSProjFSBridge::Stop),
            InstanceMethod("isRunning", &IFSProjFSBridge::IsRunning),
            InstanceMethod("getStats", &IFSProjFSBridge::GetStats),
            InstanceMethod("setCachedDirectory", &IFSProjFSBridge::SetCachedDirectory),
            InstanceMethod("setCachedContent", &IFSProjFSBridge::SetCachedContent),
            InstanceMethod("setCachedFileInfo", &IFSProjFSBridge::SetCachedFileInfo),
            InstanceMethod("completePendingFileRequests", &IFSProjFSBridge::CompletePendingFileRequests),
            InstanceMethod("invalidateTombstone", &IFSProjFSBridge::InvalidateTombstone)
        });

        Napi::FunctionReference* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        env.SetInstanceData(constructor);

        exports.Set("IFSProjFSProvider", func);
        return exports;
    }

    IFSProjFSBridge(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IFSProjFSBridge>(info) {
        Napi::Env env = info.Env();
        std::cout << "[Native] IFSProjFSBridge constructor start" << std::endl;

        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Instance path required").ThrowAsJavaScriptException();
            return;
        }

        std::string instancePath = info[0].As<Napi::String>().Utf8Value();
        std::cout << "[Native] instancePath: " << instancePath << std::endl;

        try {
            std::cout << "[Native] Creating ProjFSProvider..." << std::endl;
            provider_ = std::make_unique<ProjFSProvider>(instancePath);
            std::cout << "[Native] ProjFSProvider created" << std::endl;
            asyncBridge_ = std::make_shared<AsyncBridge>(env);
            std::cout << "[Native] AsyncBridge created" << std::endl;
            provider_->SetAsyncBridge(asyncBridge_);
            std::cout << "[Native] AsyncBridge set" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "[Native] Exception: " << e.what() << std::endl;
            Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        }
        std::cout << "[Native] IFSProjFSBridge constructor done" << std::endl;
    }

private:
    Napi::Value RegisterCallbacks(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsObject()) {
            Napi::TypeError::New(env, "Callbacks object required").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        Napi::Object callbacks = info[0].As<Napi::Object>();
        asyncBridge_->RegisterCallbacks(callbacks);

        return env.Undefined();
    }

    Napi::Value Start(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Virtual root path required").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string virtualRoot = info[0].As<Napi::String>().Utf8Value();

        // Start async bridge first
        asyncBridge_->Start();
        
        // Then start ProjFS provider
        bool success = provider_->Start(virtualRoot);
        if (!success) {
            asyncBridge_->Stop();
            std::string error = "Failed to start ProjFS provider: " + provider_->GetLastError();
            Napi::Error::New(env, error).ThrowAsJavaScriptException();
        }
        
        return Napi::Boolean::New(env, success);
    }

    Napi::Value Stop(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        provider_->Stop();
        asyncBridge_->Stop();
        return Napi::Boolean::New(env, true);
    }

    Napi::Value IsRunning(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), provider_->IsRunning());
    }

    Napi::Value GetStats(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Object stats = Napi::Object::New(env);

        const ProviderStats& providerStats = provider_->GetStats();

        stats.Set("placeholderRequests", Napi::Number::New(env, providerStats.placeholderRequests.load()));
        stats.Set("fileDataRequests", Napi::Number::New(env, providerStats.fileDataRequests.load()));
        stats.Set("directoryEnumerations", Napi::Number::New(env, providerStats.directoryEnumerations.load()));
        stats.Set("bytesRead", Napi::BigInt::New(env, providerStats.bytesRead.load()));
        stats.Set("cacheHits", Napi::Number::New(env, providerStats.cacheHits.load()));
        stats.Set("cacheMisses", Napi::Number::New(env, providerStats.cacheMisses.load()));

        return stats;
    }
    
    Napi::Value SetCachedDirectory(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
            Napi::TypeError::New(env, "Path string and entries array required").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        
        std::string path = info[0].As<Napi::String>().Utf8Value();
        Napi::Array entries = info[1].As<Napi::Array>();
        
        // Convert JavaScript array to DirectoryListing
        DirectoryListing listing;
        for (uint32_t i = 0; i < entries.Length(); i++) {
            if (entries.Get(i).IsObject()) {
                Napi::Object entry = entries.Get(i).As<Napi::Object>();
                FileInfo fileInfo = {};  // Zero-initialize all fields

                if (entry.Has("name")) {
                    fileInfo.name = entry.Get("name").As<Napi::String>().Utf8Value();
                }
                if (entry.Has("hash")) {
                    fileInfo.hash = entry.Get("hash").As<Napi::String>().Utf8Value();
                }
                if (entry.Has("size")) {
                    fileInfo.size = entry.Get("size").As<Napi::Number>().Int64Value();  // Use Int64Value for size_t
                }
                if (entry.Has("isDirectory")) {
                    fileInfo.isDirectory = entry.Get("isDirectory").As<Napi::Boolean>().Value();
                }
                if (entry.Has("isBlobOrClob")) {
                    fileInfo.isBlobOrClob = entry.Get("isBlobOrClob").As<Napi::Boolean>().Value();
                }
                if (entry.Has("mode")) {
                    fileInfo.mode = entry.Get("mode").As<Napi::Number>().Uint32Value();
                }
                
                listing.entries.push_back(fileInfo);
            }
        }
        
        // Store in cache
        if (asyncBridge_ && asyncBridge_->GetCache()) {
            asyncBridge_->GetCache()->SetDirectoryListing(path, listing);
        }
        
        return env.Undefined();
    }
    
    Napi::Value SetCachedContent(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBuffer()) {
            Napi::TypeError::New(env, "Path string and content buffer required").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string path = info[0].As<Napi::String>().Utf8Value();
        Napi::Buffer<uint8_t> buffer = info[1].As<Napi::Buffer<uint8_t>>();

        // Store content in cache
        if (asyncBridge_ && asyncBridge_->GetCache()) {
            FileContent content;
            content.data = std::vector<uint8_t>(buffer.Data(), buffer.Data() + buffer.Length());
            asyncBridge_->GetCache()->SetFileContent(path, content);
        }

        return env.Undefined();
    }

    Napi::Value SetCachedFileInfo(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsObject()) {
            Napi::TypeError::New(env, "Path string and file info object required").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string path = info[0].As<Napi::String>().Utf8Value();
        Napi::Object obj = info[1].As<Napi::Object>();

        FileInfo fileInfo;
        if (obj.Has("name")) {
            fileInfo.name = obj.Get("name").As<Napi::String>().Utf8Value();
        }
        if (obj.Has("hash")) {
            fileInfo.hash = obj.Get("hash").As<Napi::String>().Utf8Value();
        }
        if (obj.Has("size")) {
            fileInfo.size = obj.Get("size").As<Napi::Number>().Uint32Value();
        }
        if (obj.Has("isDirectory")) {
            fileInfo.isDirectory = obj.Get("isDirectory").As<Napi::Boolean>().Value();
        }
        if (obj.Has("isBlobOrClob")) {
            fileInfo.isBlobOrClob = obj.Get("isBlobOrClob").As<Napi::Boolean>().Value();
        }
        if (obj.Has("mode")) {
            fileInfo.mode = obj.Get("mode").As<Napi::Number>().Uint32Value();
        }

        // Store file info in cache
        if (asyncBridge_ && asyncBridge_->GetCache()) {
            asyncBridge_->GetCache()->SetFileInfo(path, fileInfo);
        }

        return env.Undefined();
    }
    
    Napi::Value CompletePendingFileRequests(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Path string required").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string path = info[0].As<Napi::String>().Utf8Value();

        if (provider_) {
            provider_->CompletePendingFileRequests(path);
        }

        return env.Undefined();
    }

    Napi::Value InvalidateTombstone(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Path string required").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string path = info[0].As<Napi::String>().Utf8Value();

        bool success = false;
        if (provider_) {
            success = provider_->InvalidateTombstone(path);
        }

        return Napi::Boolean::New(env, success);
    }

    std::unique_ptr<ProjFSProvider> provider_;
    std::shared_ptr<AsyncBridge> asyncBridge_;
};

// Module initialization
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    return IFSProjFSBridge::Init(env, exports);
}

NODE_API_MODULE(ifsprojfs, Init)