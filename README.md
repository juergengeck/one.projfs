# one.projfs

Native IFileSystem to ProjFS bridge for one.filer - provides high-performance Windows filesystem integration for ONE content.

## Overview

This N-API module is the core component that enables one.filer to expose ONE database content as a virtual Windows drive. It replaces the complex 7-layer abstraction stack with a clean 2-layer architecture:

**Before**: Windows Explorer → ProjFS → projfs-fuse.one → FUSE emulation → multiple adapters → IFileSystem

**After**: Windows Explorer → ProjFS → one.projfs → IFileSystem

This provides the same user experience (browsing ONE content in Windows Explorer) with 10-100x better performance.

## Key Features

- **Hybrid approach** - Direct disk access for BLOBs/CLOBs (images, PDFs), JavaScript callbacks for metadata
- **Intelligent caching** - Fast synchronous ProjFS responses with async background updates
- **ONE integration** - Designed specifically for one.filer's FilerWithProjFS
- **Type-safe** - Full TypeScript support with proper IFileSystem types
- **High performance** - 10-100x faster than the previous 7-layer stack

## Installation

```bash
npm install @refinio/one.projfs
```

Note: This package requires Windows 10 version 1809 or later with ProjFS enabled.

## Role in one.filer

This module is used by one.filer to provide the virtual filesystem that users interact with:

1. **User Experience**: Users see a virtual drive (e.g., `C:\OneFiler`) in Windows Explorer
2. **Content Access**: Browse ONE content like regular files:
   - `C:\OneFiler\chats\person@example.com\general\message.txt`
   - `C:\OneFiler\debug\connections.json`
   - `C:\OneFiler\objects\[hash]\content`
3. **Performance**: Near-native filesystem performance for all operations

## Usage

### Integration with one.filer

```javascript
// In one.filer/src/filer/FilerWithProjFS.ts
import { IFSProjFSProvider } from '@refinio/one.projfs';
import { CombinedFileSystem } from '@refinio/one.models/lib/fileSystems/CombinedFileSystem.js';

export class FilerWithProjFS {
    async initProjFS(): Promise<void> {
        // Create combined filesystem with all components
        const fileSystems = [
            new ChatFileSystem(...),
            new ObjectsFileSystem(...),
            new DebugFileSystem(...),
            new TypesFileSystem(...)
        ];

        const rootFS = new CombinedFileSystem(fileSystems);

        // Use one.projfs instead of the 7-layer stack
        this.projfsProvider = new IFSProjFSProvider({
            instancePath: this.instanceDirectory,
            virtualRoot: this.config.projfsRoot || 'C:\\OneFiler',
            fileSystem: rootFS,
            cacheTTL: 30
        });

        await this.projfsProvider.mount();
        // Users can now access ONE content at C:\OneFiler!
    }
}
```

### TypeScript Integration

```typescript
import { mountIFileSystem } from '@refinio/one.projfs';
import { ChatFileSystem } from '@refinio/one.models/lib/fileSystems/ChatFileSystem.js';

// Create your filesystem
const chatFS = new ChatFileSystem(leuteModel, topicModel, channelManager);

// Mount it via ProjFS
const provider = mountIFileSystem(
    chatFS,
    'C:/data/[instance-hash]',
    'C:/OneFiler'
);

await provider.mount();

// Now Windows Explorer shows your chat content!
```

## What Users See

When one.filer uses this module, users get a virtual Windows drive with their ONE content:

```
C:/OneFiler/
├── chats/                      # From ChatFileSystem
│   ├── person@example.com/
│   │   └── general/
│   │       ├── message1.txt
│   │       └── message2.txt
├── files/                      # From FilesFileSystem  
│   └── documents/
│       ├── report.pdf         # Direct BLOB access
│       └── image.jpg          # Direct BLOB access
├── debug/                      # Debug information
└── types/                      # Type definitions
```

BLOB and CLOB files (images, PDFs, etc.) are served directly from disk for maximum performance, while metadata and directory structure come from your IFileSystem implementation.

## Technical Architecture

### How It Solves the Sync/Async Problem

Windows ProjFS requires immediate synchronous responses, but ONE's APIs are asynchronous. This module solves this with:

1. **ContentCache** - Pre-fetched metadata for instant responses
2. **AsyncBridge** - Background JavaScript calls to update cache
3. **SyncStorage** - Direct disk reads for BLOB/CLOB content
4. **ProjFSProvider** - Native Windows integration

### Replacing the 7-Layer Stack

```
OLD:  Explorer → ProjFS → projfs-fuse → FUSE API → Adapter → Adapter → IFileSystem
NEW:  Explorer → ProjFS → one.projfs → IFileSystem
```

### Cache Flow Example

```
1. User opens C:\OneFiler\chats in Explorer
2. ProjFS calls GetDirectoryEnumeration (sync)
3. one.projfs checks cache:
   - Hit? Return immediately
   - Miss? Return placeholder, queue async update
4. Background: Call IFileSystem.readDir("/chats")
5. Update cache for next access
```

## Building from Source

```bash
npm install
npm run build
```

Requirements:
- Node.js 14+
- Windows SDK with ProjFS headers
- Visual Studio 2019 or later

## Performance

The hybrid approach provides excellent performance:

| Operation | Old 7-Layer Stack | one.projfs | Improvement |
|-----------|-------------------|------------|-------------|
| BLOB read | 5-20ms | <1ms | 10-20x |
| Directory list | 10-50ms | 1-5ms | 5-10x |
| Metadata (cached) | 5-15ms | <0.1ms | 50-150x |
| File open | 20-100ms | 5-10ms | 4-10x |

## Caching Strategy

### Why It's Faster

1. **No FUSE Emulation**: Direct Windows ProjFS integration
2. **Fewer Layers**: 2 instead of 7
3. **Direct Disk Access**: BLOBs read without abstraction
4. **Smart Caching**: Metadata served from memory
5. **Native Code**: C++ for sync operations

## Asynchronous Content Delivery

This module implements proper asynchronous content delivery using Windows ProjFS ERROR_IO_PENDING pattern:

### How It Works

1. **Cache Miss Handling**: When GetFileDataCallback can't find content in cache, it returns `ERROR_IO_PENDING` and stores the command details
2. **Background Fetch**: AsyncBridge triggers JavaScript IFileSystem to fetch content asynchronously 
3. **Command Completion**: When content is available, `PrjCompleteCommand` resumes the pending operation
4. **User Experience**: Windows Explorer shows loading indicator while content loads, then displays immediately

### Benefits

- **No Blocking**: ProjFS callbacks never block waiting for async operations
- **Proper UX**: Windows shows loading state instead of errors
- **Scalable**: Handles multiple concurrent file requests efficiently
- **Cache-First**: Subsequent access to same files is instant from cache

### Implementation Details

The ERROR_IO_PENDING flow is implemented in:
- `GetFileDataCallback`: Returns ERROR_IO_PENDING on cache miss
- `CompletePendingFileRequests`: Completes commands when content arrives
- `AsyncBridge`: Coordinates between C++ callbacks and JavaScript IFileSystem

## Building and Testing

### Build Commands

```bash
npm install          # Builds native module automatically
npm run build        # Rebuild native module
npm run rebuild      # Clean rebuild
npm run clean        # Clean build artifacts
```

### Testing

```bash
npm test             # Run integration tests
npm run test:clean   # Clean test directories and run tests
```

**IMPORTANT**: The integration tests require clean state. If tests fail with connection errors, run:
```bash
npm run clean:test   # Clean cached instance directories
npm test             # Run tests again
```

## Troubleshooting

### ProjFS Native Module Not Loading

**Error**: `The specified module could not be found: build\Release\ifsprojfs.node`

**Cause**: ProjFS Windows feature not enabled or native dependencies missing.

**Solution**:
```powershell
# Enable ProjFS (Run PowerShell as Administrator)
Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS

# Restart your computer
# Then rebuild the module
npm rebuild
```

**Requirements**:
- Windows 10 version 1809 (October 2018 Update) or later
- ProjFS feature enabled
- Visual Studio 2019+ with C++ build tools
- Windows SDK with ProjFS headers

### Integration Test Connection Failures

**Error**: `No listening connection for the specified publicKey`

**Cause**: Cached instance state from previous test runs prevents proper initialization.

**Solution**:
```bash
# Clean ALL test state
npm run clean:test

# Run tests with fresh state
npm test
```

**What gets cleaned**:
- `C:\Temp\refinio-api-server-instance` - Server ONE instance cache
- `C:\Temp\refinio-api-client-instance` - Client ONE instance cache
- `C:\OneFiler-Test` - Test mount point

**Why this happens**: ONE instances cache cryptographic keys and connection state. Stale cache causes the ConnectionsModel to not properly register with the CommServer, resulting in connection failures.

### System Clock Issues

**Error**: Connection failures or "token expired" errors

**Cause**: System clock significantly off from actual time.

**Solution**: Ensure your system clock is accurate. The CommServer and invitation tokens use timestamps for validation.

### Invitation Token Reuse

**Symptom**: New invitation created on every file read

**Cause**: Bug in `PairingFileSystem.getAndRefreshIopInviteIfNoneExists()` not checking if invitation exists.

**Fixed in**: `@refinio/one.models` - The method now properly caches and reuses invitations.

**Impact**: Without this fix, each file read creates a new token, but only the latest token is valid on the CommServer, causing "No listening connection" errors.

### Mount Point Already in Use

**Error**: `Mount point already exists` or ProjFS mount fails

**Cause**: Previous mount not properly unmounted.

**Solution**:
```bash
# On Windows, check if directory is a reparse point
dir C:\OneFiler-Test

# If it shows <JUNCTION> or <SYMLINKD>, remove it
rmdir C:\OneFiler-Test

# Or use the cleanup script
npm run clean:test
```

### Build Failures

**Error**: `error MSB8036: The Windows SDK version X was not found`

**Solution**: Install the correct Windows SDK or update `binding.gyp` to match your SDK version.

**Error**: `Cannot find module 'node-gyp'`

**Solution**:
```bash
npm install --save-dev node-gyp
npm rebuild
```

### Performance Issues

If you experience slow file access:

1. **Check cache TTL**: Default is 30 seconds. Adjust in your IFSProjFSProvider config:
   ```javascript
   new IFSProjFSProvider({
       cacheTTL: 60  // Increase to 60 seconds
   })
   ```

2. **Monitor cache hits**: Look for `[Cache] HIT` vs `[Cache] MISS` in debug logs

3. **BLOB location**: Ensure BLOBs are on fast storage (SSD recommended)

### Debug Logging

Enable detailed logging by setting environment variables before starting:

```bash
# Windows CMD
set NODE_DEBUG=one.projfs
npm test

# PowerShell
$env:NODE_DEBUG="one.projfs"
npm test
```

Look for log prefixes:
- `[ProjFS]` - Native ProjFS operations
- `[Cache]` - Cache hits/misses
- `[Native]` - C++ bridge operations
- `[PairingFileSystem]` - Invitation creation

## License

MIT