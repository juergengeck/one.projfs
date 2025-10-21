/**
 * Example: Mount ChatFileSystem via ProjFS
 *
 * This example shows how to expose ONE's chat content through Windows Explorer.
 *
 * NOTE: This is a TypeScript reference example only. The one.projfs module itself
 * is written in JavaScript and C++. To use this example:
 * 1. Copy the code structure to your TypeScript project
 * 2. Compile with your project's TypeScript configuration
 * 3. Ensure @refinio/one.projfs, @refinio/one.models, and @refinio/one.core are installed
 *
 * For a working JavaScript example, see the integration test:
 *   test/integration/connection-test.js
 *
 * Or the production integration in refinio.api:
 *   ../refinio.api/src/index.ts (lines 199-237)
 */

import { mountIFileSystem } from '@refinio/one.projfs';
import { ChatFileSystem } from '@refinio/one.models/lib/fileSystems/ChatFileSystem.js';
import { createDefaultInstance } from '@refinio/one.core/lib/instance.js';

async function main() {
    // Initialize ONE instance
    const instance = await createDefaultInstance({
        name: 'one-projfs-example',
        directory: 'C:/data/one-instance'
    });
    
    // Get necessary models
    const { leuteModel, topicModel, channelManager } = instance;
    
    // Create ChatFileSystem
    const chatFS = new ChatFileSystem(leuteModel, topicModel, channelManager);
    
    // Mount it via ProjFS
    console.log('Mounting ChatFileSystem to C:\\OneFilerChats...');
    const provider = mountIFileSystem(
        chatFS,
        instance.directory,  // Instance path for direct BLOB access
        'C:\\OneFilerChats'   // Virtual mount point
    );
    
    await provider.mount();
    console.log('✓ Mounted successfully!');
    console.log('');
    console.log('You can now browse your chats in Windows Explorer at:');
    console.log('C:\\OneFilerChats');
    console.log('');
    console.log('Directory structure:');
    console.log('C:\\OneFilerChats\\');
    console.log('├── person1@example.com\\');
    console.log('│   └── general\\');
    console.log('│       ├── message1.txt');
    console.log('│       └── message2.txt');
    console.log('└── person2@example.com\\');
    console.log('    └── projects\\');
    console.log('        └── discussion.txt');
    console.log('');
    console.log('Press Ctrl+C to unmount...');
    
    // Keep running until interrupted
    process.on('SIGINT', async () => {
        console.log('\\nUnmounting...');
        await provider.unmount();
        console.log('✓ Unmounted successfully');
        process.exit(0);
    });
    
    // Monitor stats every 10 seconds
    setInterval(() => {
        const stats = provider.getStats();
        console.log(`Stats: ${stats.fileDataRequests} files, ${stats.directoryEnumerations} dirs, ${stats.cacheHits} cache hits`);
    }, 10000);
}

main().catch(console.error);