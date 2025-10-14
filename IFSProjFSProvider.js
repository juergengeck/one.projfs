const EventEmitter = require('events');
const NativeProvider = require('./build/Release/ifsprojfs.node').IFSProjFSProvider;
const fs = require('fs');
const path = require('path');

// File logging - use a fixed path that works in both webpack and direct node contexts
const LOG_FILE = 'C:\\Users\\juerg\\source\\one.filer.windows\\projfs-operations.log';
let logStream = null;

function log(message) {
    // Disable console logging in test environment
    if (process.env.NODE_ENV === 'test' || process.env.JEST_WORKER_ID) {
        return;
    }
    
    const timestamp = new Date().toISOString();
    const logMessage = `[${timestamp}] ${message}\n`;
    
    console.log(message);
    
    if (!logStream) {
        try {
            // Ensure directory exists
            const dir = path.dirname(LOG_FILE);
            if (!fs.existsSync(dir)) {
                fs.mkdirSync(dir, { recursive: true });
            }
            // Create or append to log file
            logStream = fs.createWriteStream(LOG_FILE, { flags: 'a' });
        } catch (e) {
            // If still fails, continue without file logging
            console.error('Failed to create log file:', e.message);
        }
    }
    
    if (logStream) {
        logStream.write(logMessage);
    }
}

// Global state
const enumerationCount = { count: 0 };

class IFSProjFSProvider extends EventEmitter {
    constructor(options) {
        super();
        
        // Handle both string (for native module compatibility) and object parameters
        if (typeof options === 'string') {
            this.instancePath = options;
            this.virtualRoot = 'C:\\OneFiler';
            this.fileSystem = null;
            this.debug = false;
        } else {
            this.instancePath = options.instancePath;
            this.virtualRoot = options.virtualRoot || 'C:\\OneFiler';
            this.fileSystem = options.fileSystem;
            this.debug = options.debug || false;
        }
        
        log('\n========================================');
        log('IFSProjFSProvider Constructor Called');
        log(`Virtual Root: ${this.virtualRoot}`);
        log(`Instance Path: ${this.instancePath}`);
        log(`DEBUG: typeof options = ${typeof options}`);
        if (typeof options === 'object') {
            log(`DEBUG: options.instancePath = ${options.instancePath}`);
            log(`DEBUG: options.virtualRoot = ${options.virtualRoot}`);
        }
        log('========================================');

        // Create native provider with instancePath (where objects are stored)
        this.provider = new NativeProvider(this.instancePath);
        
        // Register callbacks
        this.provider.registerCallbacks({
            getFileInfo: this.getFileInfo.bind(this),
            readFile: this.readFile.bind(this),
            readDirectory: this.readDirectory.bind(this),
            createFile: this.createFile.bind(this),
            onDebugMessage: this.onDebugMessage.bind(this)
        });
    }
    
    onDebugMessage(message) {
        log(`[Native] ${message}`);
    }
    
    normalizePath(inputPath) {
        if (!inputPath || typeof inputPath !== 'string') return '/';
        
        let normalized = inputPath
            .replace(/\\/g, '/')
            .replace(/^[A-Z]:/i, '')
            .replace(/^\/*OneFiler/i, '')
            .replace(/\/+/g, '/');
            
        if (!normalized.startsWith('/')) {
            normalized = '/' + normalized;
        }
        
        if (normalized.length > 1 && normalized.endsWith('/')) {
            normalized = normalized.slice(0, -1);
        }
        
        return normalized || '/';
    }
    
    async getFileInfo(path) {
        const normalizedPath = this.normalizePath(path);
        log(`getFileInfo: "${path}" -> "${normalizedPath}"`);
        
        try {
            const name = path.split(/[\\\/]/).filter(p => p).pop() || '';
            
            if (normalizedPath === '/') {
                return {
                    name: '',
                    hash: '',
                    size: 0,
                    isDirectory: true,
                    isBlobOrClob: false,
                    mode: 16877
                };
            }
            
            const info = await this.fileSystem.stat(normalizedPath);
            // Determine if it's a directory from either isDirectory flag or mode field
            const isDir = info.isDirectory || (info.mode && (info.mode & 0o040000) === 0o040000);
            return {
                name: name,
                hash: info.hash || '',
                size: info.size || 0,
                isDirectory: isDir,
                isBlobOrClob: false,
                mode: info.mode || (isDir ? 16877 : 33188)
            };
        } catch (error) {
            log(`getFileInfo ERROR: ${error.message}`);
            return null;
        }
    }
    
    async readFile(path) {
        log('[TEST-3.1] readFile CALLED');
        log(`[TEST-3.1] arguments.length: ${arguments.length}`);
        log(`[TEST-3.1] typeof path: ${typeof path}`);
        log(`[TEST-3.1] path instanceof String: ${path instanceof String}`);
        log(`[TEST-3.1] path constructor: ${path?.constructor?.name}`);
        log(`[TEST-3.1] path value: ${path}`);
        log(`[TEST-3.1] JSON.stringify(path): ${JSON.stringify(path)}`);

        // If path is an object, log its properties
        if (typeof path === 'object' && path !== null) {
            log(`[TEST-3.1] Object.keys(path): ${JSON.stringify(Object.keys(path))}`);
            log(`[TEST-3.1] Object.getOwnPropertyNames(path): ${JSON.stringify(Object.getOwnPropertyNames(path))}`);
        }

        const normalizedPath = this.normalizePath(path);
        log(`readFile: "${path}" -> "${normalizedPath}"`);
        
        if (normalizedPath.startsWith('/objects/')) {
            log(`  Skipping /objects path`);
            return null;
        }
        
        try {
            const file = await this.fileSystem.readFile(normalizedPath);
            if (file && file.content) {
                // Make sure we return a Buffer
                let buffer;
                if (Buffer.isBuffer(file.content)) {
                    buffer = file.content;
                } else if (file.content instanceof ArrayBuffer) {
                    // Handle ArrayBuffer (common for binary files like PNGs)
                    buffer = Buffer.from(file.content);
                    log(`  Converted ArrayBuffer to Buffer for ${normalizedPath}`);
                } else if (file.content.buffer && file.content.buffer instanceof ArrayBuffer) {
                    // Handle TypedArray views (Uint8Array, etc.)
                    buffer = Buffer.from(file.content.buffer, file.content.byteOffset, file.content.byteLength);
                    log(`  Converted TypedArray to Buffer for ${normalizedPath}`);
                } else {
                    // Fallback for other types
                    buffer = Buffer.from(file.content);
                }
                
                log(`  readFile SUCCESS: Got ${buffer.length} bytes for ${normalizedPath}`);

                // Log additional debug info for PNG files
                if (normalizedPath.endsWith('.png')) {
                    log(`  PNG file detected: ${normalizedPath}`);
                    // Check PNG signature (89 50 4E 47)
                    if (buffer.length >= 4) {
                        const isPNG = buffer[0] === 0x89 && buffer[1] === 0x50 &&
                                     buffer[2] === 0x4E && buffer[3] === 0x47;
                        log(`  PNG signature valid: ${isPNG}`);
                    }
                }

                // IMPORTANT: Cache the content for ProjFS to serve
                // This is critical for ALL files including invites
                // Even if files are "dynamic", they need to be cached momentarily for ProjFS to serve them
                if (this.provider && typeof this.provider.setCachedContent === 'function') {
                    log(`  Caching content for ${normalizedPath} (${buffer.length} bytes)`);
                    this.provider.setCachedContent(normalizedPath, buffer);

                    // Complete any pending file requests for this path
                    if (typeof this.provider.completePendingFileRequests === 'function') {
                        log(`  Completing pending requests for ${normalizedPath}`);
                        this.provider.completePendingFileRequests(normalizedPath);
                    }
                }
                
                return buffer;
            } else {
                log(`  readFile: No content returned from filesystem`);
                return null;
            }
        } catch (error) {
            log(`  readFile ERROR: ${error.message}`);
            return null;
        }
    }
    
    async readDirectory(path) {
        enumerationCount.count++;
        
        const normalizedPath = this.normalizePath(path);
        log(`\nreadDirectory #${enumerationCount.count}: "${path}" -> "${normalizedPath}"`);
        
        // Let the filesystem provide the root directory listing
        // No hardcoding - get real data
        
        try {
            const dir = await this.fileSystem.readDir(normalizedPath);
            if (!dir?.children || !Array.isArray(dir.children)) {
                log('  No children');
                return [];
            }
            
            log(`  Raw children: ${dir.children.length} items`);
            log(`  First 3: ${JSON.stringify(dir.children.slice(0, 3))}`);
            
            const entries = [];
            const seen = new Set();
            
            for (const child of dir.children) {
                let name = child;
                
                if (typeof child === 'string') {
                    const parts = child.split(/[\/\\]/).filter(p => p);
                    name = parts[parts.length - 1] || child;
                } else if (child && typeof child === 'object') {
                    name = child.name || '';
                }
                
                name = String(name).trim();
                
                if (!name || name.includes('/') || name.includes('\\') || seen.has(name)) {
                    continue;
                }
                
                seen.add(name);
                
                try {
                    const childPath = normalizedPath + '/' + name;
                    const info = await this.fileSystem.stat(childPath);

                    // Determine if it's a directory from either isDirectory flag or mode field
                    let isDir = info.isDirectory || (info.mode && (info.mode & 0o040000) === 0o040000);

                    // WORKAROUND: Known filesystem mount points are always directories
                    // The TemporaryFileSystem doesn't correctly identify mounted filesystems as directories
                    if (normalizedPath === '/' && (name === 'chats' || name === 'debug' || name === 'invites' ||
                                                   name === 'objects' || name === 'types' || name === 'test-data')) {
                        isDir = true;
                        log(`  FORCED directory flag for mount point: ${name}`);
                    }

                    const entry = {
                        name: name,
                        hash: info.hash || '',
                        size: info.size || 0,
                        isDirectory: isDir,
                        isBlobOrClob: false,
                        mode: info.mode || (isDir ? 16877 : 33188)
                    };
                    
                    entries.push(entry);
                    
                    // Push individual file metadata to native cache for GetPlaceholderInfo
                    if (!entry.isDirectory && entry.size > 0) {
                        log(`  Caching file metadata: ${childPath} (size: ${entry.size})`);
                        // The native cache will use this for GetPlaceholderInfoCallback
                    }
                } catch (e) {
                    log(`  Failed to stat ${normalizedPath}/${name}: ${e.message}`);
                }
            }
            
            // Validate entries before caching - ensure all have names
            const validEntries = entries.filter(entry => {
                if (!entry.name || entry.name.length === 0) {
                    log(`  WARNING: Skipping entry with empty name`);
                    return false;
                }
                return true;
            });

            // Push complete directory listing to native cache immediately
            // For /invites, we cache the directory listing so Explorer can see the files
            // The content will be fetched on-demand when files are accessed
            this.setCachedDirectory(normalizedPath, validEntries);
            log(`  Pushed ${validEntries.length} valid entries to native cache (${entries.length} total)`);

            // Also cache individual FileInfo for each entry so GetPlaceholderInfo can find them
            for (const entry of validEntries) {
                const entryPath = normalizedPath + '/' + entry.name;
                this.setCachedFileInfo(entryPath, entry);
                log(`  Cached FileInfo for ${entryPath}`);
            }

            // NOTE: /invites files are NOT pre-fetched or cached
            // They are dynamically generated and must always be read fresh from the filesystem

            log(`  Entries: [${validEntries.map(e => `${e.name}(${e.isDirectory ? 'dir' : e.size + 'b'})`).join(', ')}]`);
            return entries;
            
        } catch (error) {
            log(`  ERROR: ${error.message}`);
            return [];
        }
    }
    
    async createFile(path, content) {
        const normalizedPath = this.normalizePath(path);
        log(`createFile: "${path}" -> "${normalizedPath}"`);
        await this.fileSystem.writeFile(normalizedPath, content);
    }
    
    getCacheStats() {
        // Return basic stats from native cache if available
        if (this.provider && typeof this.provider.getCacheStats === 'function') {
            return this.provider.getCacheStats();
        }
        return {
            fileInfoCount: 0,
            directoryCount: 0,
            contentCount: 0
        };
    }
    
    setCachedContent(path, content) {
        const normalizedPath = this.normalizePath(path);
        log(`setCachedContent: Caching ${content ? content.length : 0} bytes for "${normalizedPath}"`);
        
        // Store content in native cache if available
        if (this.provider && typeof this.provider.setCachedContent === 'function') {
            log(`  Calling native setCachedContent for ${normalizedPath}`);
            this.provider.setCachedContent(normalizedPath, content);
            log(`  Native setCachedContent completed`);
        } else {
            log(`  WARNING: Native setCachedContent not available`);
        }
    }
    
    setCachedDirectory(path, entries) {
        const normalizedPath = this.normalizePath(path);
        log(`setCachedDirectory: Caching ${entries ? entries.length : 0} entries for "${normalizedPath}"`);

        // Store directory listing in native cache if available
        if (this.provider && typeof this.provider.setCachedDirectory === 'function') {
            log(`  Calling native setCachedDirectory for ${normalizedPath}`);
            this.provider.setCachedDirectory(normalizedPath, entries);
            log(`  Native setCachedDirectory completed`);
        } else {
            log(`  WARNING: Native setCachedDirectory not available`);
        }
    }

    setCachedFileInfo(path, fileInfo) {
        const normalizedPath = this.normalizePath(path);
        log(`setCachedFileInfo: Caching file info for "${normalizedPath}"`);

        // Store file info in native cache if available
        if (this.provider && typeof this.provider.setCachedFileInfo === 'function') {
            log(`  Calling native setCachedFileInfo for ${normalizedPath}`);
            this.provider.setCachedFileInfo(normalizedPath, fileInfo);
            log(`  Native setCachedFileInfo completed`);
        } else {
            log(`  WARNING: Native setCachedFileInfo not available`);
        }
    }
    
    async mount() {
        log('\n=== MOUNT CALLED ===');
        log(`Mounting at: ${this.virtualRoot}`);
        enumerationCount.count = 0;

        // Pre-populate root directory cache before starting provider
        if (this.fileSystem) {
            log('Pre-populating directory caches...');
            try {
                // Cache root directory
                const rootEntries = await this.readDirectory('/');
                log(`Pre-populated root with ${rootEntries.length} entries`);

                // Cache critical subdirectories INCLUDING invites
                // We MUST cache invites BEFORE mount to avoid timeout issues
                const criticalDirs = ['/debug', '/chats', '/invites'];

                // Process all critical directories
                for (const dir of criticalDirs) {
                    try {
                        log(`Pre-populating ${dir}...`);
                        const entries = await this.readDirectory(dir);
                        log(`Pre-populated ${dir} with ${entries.length} entries`);
                    } catch (e) {
                        log(`Could not pre-populate ${dir}: ${e.message}`);
                    }
                }

                // NOTE: /invites directory structure is pre-populated, but file CONTENT
                // is dynamically generated and must always be read fresh from the filesystem
                // Pre-populating the directory listing allows Windows to see the files exist
            } catch (error) {
                log(`Failed to pre-populate root: ${error.message}`);
            }
        }

        log('>>> CHECKPOINT 1: After critical dirs loop');

        // Pre-populate /invites BEFORE starting provider
        // This is critical because invite files must be visible immediately
        if (this.fileSystem) {
            log('>>> CHECKPOINT 2: fileSystem exists, about to pre-populate /invites');
            try {
                log('Pre-populating /invites...');
                const entries = await this.readDirectory('/invites');
                log(`Pre-populated /invites with ${entries.length} entries`);
            } catch (e) {
                log(`Could not pre-populate /invites: ${e.message}`);
            }
        } else {
            log('>>> CHECKPOINT 2-FAIL: fileSystem is null/undefined');
        }

        log('>>> CHECKPOINT 3: About to start provider');
        await this.provider.start(this.virtualRoot);
        log('Mount completed');

        // CRITICAL: Monitor /invites directory and auto-regenerate deleted files
        // Windows ProjFS allows placeholder deletion without notifications, and hydration
        // from the same process causes deadlock. Solution: Watch for deletions and recreate.
        log('>>> POST-MOUNT: Setting up invite file monitoring and auto-regeneration');
        try {
            const fs = require('fs');
            const path = require('path');
            const invitesPath = path.join(this.virtualRoot, 'invites');

            // Verify enumeration works
            let files = [];
            for (let i = 0; i < 10; i++) {
                try {
                    files = fs.readdirSync(invitesPath);
                    if (files.length > 0) {
                        log(`>>> ✓ Windows enumerated /invites: ${files.length} files - [${files.join(', ')}]`);
                        break;
                    }
                } catch (e) {
                    log(`  Retry ${i + 1}: ${e.message}`);
                }
                await new Promise(resolve => setTimeout(resolve, 100));
            }

            if (files.length === 0) {
                log(`>>> ⚠️  WARNING: /invites enumeration returned 0 files after retries`);
            }

            // Set up filesystem watcher for the invites directory
            // This will detect when files are deleted and trigger cache invalidation
            const watcher = fs.watch(invitesPath, (eventType, filename) => {
                if (eventType === 'rename' && filename) {
                    // 'rename' event fires for both creation and deletion
                    // Check if file exists to determine if it was deleted
                    const filePath = path.join(invitesPath, filename);
                    const exists = fs.existsSync(filePath);

                    if (!exists) {
                        log(`>>> ⚠️  INVITE FILE DELETED: ${filename}`);
                        log(`>>> 🔄 Auto-regeneration: Clearing cache and invalidating tombstone`);

                        // Clear ProjFS native cache for this file
                        // This ensures the next access will call readFile() to regenerate
                        const virtualPath = `/invites/${filename}`;

                        if (this.provider && typeof this.provider.clearCachedFileInfo === 'function') {
                            this.provider.clearCachedFileInfo(virtualPath);
                            log(`  ✓ Cleared FileInfo cache for ${virtualPath}`);
                        }

                        if (this.provider && typeof this.provider.clearCachedContent === 'function') {
                            this.provider.clearCachedContent(virtualPath);
                            log(`  ✓ Cleared Content cache for ${virtualPath}`);
                        }

                        // CRITICAL: Invalidate Windows tombstone cache so file can reappear
                        // Without this, Windows remembers the file was deleted and won't show it again
                        if (this.provider && typeof this.provider.invalidateTombstone === 'function') {
                            const success = this.provider.invalidateTombstone(virtualPath);
                            if (success) {
                                log(`  ✓ Invalidated Windows tombstone cache for ${virtualPath}`);
                            } else {
                                log(`  ⚠️  Failed to invalidate tombstone for ${virtualPath}`);
                            }
                        } else {
                            log(`  ⚠️  invalidateTombstone not available on provider`);
                        }

                        log(`>>> File will be auto-regenerated on next access`);
                    } else {
                        log(`>>> ✓ Invite file created/restored: ${filename}`);
                    }
                }
            });

            // Store watcher for cleanup on unmount
            this.inviteWatcher = watcher;

            log(`>>> POST-MOUNT: File monitoring active - invites will auto-regenerate if deleted`);
        } catch (error) {
            log(`>>> ⚠️ Failed to set up invite monitoring: ${error.message}`);
        }
    }
    
    async start(virtualRoot) {
        this.virtualRoot = virtualRoot || this.virtualRoot;
        return this.mount();
    }
    
    async unmount() {
        log('\n=== UNMOUNT CALLED ===');
        log(`Total enumerations: ${enumerationCount.count}`);

        // Clean up invite file watcher
        if (this.inviteWatcher) {
            try {
                this.inviteWatcher.close();
                log('>>> Invite file watcher closed');
            } catch (e) {
                log(`>>> Failed to close invite watcher: ${e.message}`);
            }
            this.inviteWatcher = null;
        }

        await this.provider.stop();
        if (logStream) {
            logStream.end();
            logStream = null;
        }
    }
    
    async stop() {
        return this.unmount();
    }
    
    isRunning() {
        return this.provider && typeof this.provider.isRunning === 'function' 
            ? this.provider.isRunning() 
            : false;
    }
    
    getStats() {
        return this.provider && typeof this.provider.getStats === 'function'
            ? this.provider.getStats()
            : {};
    }
    
    registerCallbacks(callbacks) {
        // Store callbacks for use by the existing methods
        if (callbacks.readDirectory) {
            this._readDirectoryCallback = callbacks.readDirectory;
        }
        if (callbacks.getFileInfo) {
            this._getFileInfoCallback = callbacks.getFileInfo;
        }
        if (callbacks.readFile) {
            this._readFileCallback = callbacks.readFile;
        }
        
        // Re-register with native provider if it supports it
        if (this.provider && typeof this.provider.registerCallbacks === 'function') {
            this.provider.registerCallbacks({
                getFileInfo: this.getFileInfo.bind(this),
                readFile: this.readFile.bind(this),
                readDirectory: this.readDirectory.bind(this),
                createFile: this.createFile.bind(this),
                onDebugMessage: this.onDebugMessage.bind(this)
            });
        }
    }
    
    completePendingFileRequests(path) {
        if (this.provider && typeof this.provider.completePendingFileRequests === 'function') {
            this.provider.completePendingFileRequests(path);
        }
    }
}

module.exports = IFSProjFSProvider;