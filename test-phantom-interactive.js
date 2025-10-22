/**
 * Interactive test for Phantom Placeholder Bug
 *
 * This test mounts a filesystem and then waits for you to manually
 * trigger PowerShell command resolution by running invalid commands.
 *
 * This simulates the actual bug condition more accurately.
 */

import IFSProjFSProvider from './IFSProjFSProvider.js';
import fs from 'fs';
import path from 'path';
import os from 'os';
import readline from 'readline';

// Create a simple in-memory filesystem
class SimpleTestFileSystem {
    constructor() {
        this.files = {
            '/': {
                type: 'directory',
                children: ['file1.txt', 'file2.txt', 'subdir']
            },
            '/file1.txt': {
                type: 'file',
                content: Buffer.from('Hello from file1'),
                size: 17
            },
            '/file2.txt': {
                type: 'file',
                content: Buffer.from('Hello from file2'),
                size: 17
            },
            '/subdir': {
                type: 'directory',
                children: ['nested.txt']
            },
            '/subdir/nested.txt': {
                type: 'file',
                content: Buffer.from('Nested file content'),
                size: 19
            }
        };
    }

    async stat(filePath) {
        console.log(`[TestFS] stat('${filePath}')`);
        const entry = this.files[filePath];
        if (!entry) {
            console.log(`[TestFS] stat: ENOENT`);
            throw new Error(`ENOENT: ${filePath}`);
        }

        return {
            isDirectory: entry.type === 'directory',
            size: entry.size || 0,
            mode: entry.type === 'directory' ? 16877 : 33188
        };
    }

    async readDir(dirPath) {
        console.log(`[TestFS] readDir('${dirPath}')`);
        const entry = this.files[dirPath];
        if (!entry || entry.type !== 'directory') {
            throw new Error(`ENOTDIR: ${dirPath}`);
        }

        return {
            children: entry.children || []
        };
    }

    async readFile(filePath) {
        console.log(`[TestFS] readFile('${filePath}')`);
        const entry = this.files[filePath];
        if (!entry || entry.type !== 'file') {
            throw new Error(`ENOENT: ${filePath}`);
        }

        return {
            content: entry.content
        };
    }
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function runTest() {
    const mountPoint = path.join(os.tmpdir(), 'PhantomTest-Interactive');
    let provider = null;

    try {
        console.log('\n' + '='.repeat(70));
        console.log('INTERACTIVE PHANTOM PLACEHOLDER TEST');
        console.log('='.repeat(70));

        // Create mount point
        console.log(`\n📁 Creating mount point: ${mountPoint}`);
        if (fs.existsSync(mountPoint)) {
            try {
                fs.rmdirSync(mountPoint, { recursive: true });
            } catch (e) {
                console.log(`   Warning: ${e.message}`);
            }
        }
        fs.mkdirSync(mountPoint, { recursive: true });

        // Create filesystem and mount
        console.log('\n🔧 Creating test filesystem...');
        const filesystem = new SimpleTestFileSystem();

        console.log('\n🚀 Mounting ProjFS...');
        provider = new IFSProjFSProvider({
            instancePath: '',
            virtualRoot: mountPoint,
            fileSystem: filesystem,
            debug: true
        });

        await provider.mount();
        console.log('✓ Mounted successfully\n');

        await sleep(1000);

        // Check initial state
        console.log('📋 Initial directory listing:');
        let files = fs.readdirSync(mountPoint);
        console.log('   Files:', files);
        console.log(`   Count: ${files.length}`);

        // Instructions
        console.log('\n' + '='.repeat(70));
        console.log('MANUAL TEST INSTRUCTIONS');
        console.log('='.repeat(70));
        console.log(`
1. Open PowerShell in a new window
2. Navigate to: ${mountPoint}
3. Run invalid commands to trigger command resolution:

   PowerShell commands to try:
   ----------------------------
   cd "${mountPoint}"
   bla
   nonexistent
   phantom-test
   fake-command
   dir

4. Return to this window and press ENTER to check for phantom placeholders

Watch the logs below - you should see GetPlaceholderInfoCallback calls
as PowerShell searches for bla, bla.exe, bla.bat, bla.cmd, etc.
`);

        console.log('='.repeat(70));
        console.log('\n⏳ Waiting for you to run PowerShell commands...');
        console.log('   (Press ENTER when done)\n');

        // Wait for user
        const rl = readline.createInterface({
            input: process.stdin,
            output: process.stdout
        });

        await new Promise(resolve => {
            rl.question('Press ENTER to check for phantom placeholders: ', () => {
                rl.close();
                resolve();
            });
        });

        // Check for phantoms
        console.log('\n🔍 Checking for phantom placeholders...');
        await sleep(1000);

        files = fs.readdirSync(mountPoint);
        console.log('\n📋 Final directory listing:');
        console.log('   Files:', files);
        console.log(`   Count: ${files.length}`);

        // Analyze
        const expectedFiles = ['file1.txt', 'file2.txt', 'subdir', '.projfs'];
        const phantomFiles = files.filter(f => !expectedFiles.includes(f));

        console.log('\n' + '='.repeat(70));
        console.log('RESULTS');
        console.log('='.repeat(70));

        if (phantomFiles.length === 0) {
            console.log('\n✅ PASS: No phantom placeholders detected!');
        } else {
            console.log('\n❌ FAIL: Phantom placeholders detected!');
            console.log(`\n   Found ${phantomFiles.length} phantom entries:`);
            phantomFiles.forEach(f => {
                const fullPath = path.join(mountPoint, f);
                let info = '';
                try {
                    const stats = fs.statSync(fullPath);
                    info = stats.isDirectory() ? ' [DIR]' : ' [FILE]';
                } catch (e) {
                    info = ' [ERROR]';
                }
                console.log(`   ✗ ${f}${info}`);
            });

            console.log('\n📝 Analysis:');
            console.log('   These entries were created by Windows in response to');
            console.log('   PowerShell command resolution queries, even though our');
            console.log('   GetPlaceholderInfoCallback correctly returned ERROR_FILE_NOT_FOUND.');

            console.log('\n🔧 Next debugging steps:');
            console.log('   1. Review the GetPlaceholderInfoCallback logs above');
            console.log('   2. Check if PRJ_FLAG_USE_NEGATIVE_PATH_CACHE was logged');
            console.log('   3. Verify which paths triggered GetPlaceholderInfoCallback');
            console.log('   4. Consider additional ProjFS configuration options');
        }

        console.log('\n' + '='.repeat(70));
        console.log('\n⏸️  Leaving mount active for further testing...');
        console.log('   Press ENTER to unmount and cleanup');

        await new Promise(resolve => {
            const rl2 = readline.createInterface({
                input: process.stdin,
                output: process.stdout
            });
            rl2.question('', () => {
                rl2.close();
                resolve();
            });
        });

    } catch (error) {
        console.error('\n❌ ERROR:', error);
    } finally {
        // Cleanup
        console.log('\n🧹 Cleaning up...');

        if (provider) {
            try {
                await provider.unmount();
                console.log('✓ Unmounted');
            } catch (e) {
                console.log(`Warning: ${e.message}`);
            }
        }

        await sleep(2000);

        try {
            if (fs.existsSync(mountPoint)) {
                fs.rmdirSync(mountPoint, { recursive: true, force: true });
                console.log('✓ Mount point removed');
            }
        } catch (e) {
            console.log(`Warning: ${e.message}`);
            console.log(`Please manually remove: ${mountPoint}`);
        }

        console.log('\n✅ Test complete\n');
    }
}

// Run the test
runTest().catch(error => {
    console.error('Unhandled error:', error);
    process.exit(1);
});
